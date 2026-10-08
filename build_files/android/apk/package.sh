#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Package the cross-compiled Blender into an installable APK (no gradle).
# Gathers libblender.so + its transitive .so deps, compiles BlenderActivity,
# and assembles a debug-signed APK with the SDK build-tools.
#
# With --stage-only, stop after staging: $STAGE/lib/arm64-v8a holds the stripped
# native libraries and $STAGE/assets the runtime payload, which is exactly the
# input app/build.gradle declares as jniLibs.srcDirs / assets.srcDirs. Gradle then
# owns resource merging, Kotlin/Compose compilation and packaging, which the
# javac+d8 path below cannot do. Everything above that line is unchanged.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=/dev/null
source "$REPO_ROOT/build_files/android/env.sh"

CONFIG="${BLENDER_ANDROID_CONFIG:-full}"
STAGE_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --stage-only) STAGE_ONLY=1 ;;
    *) echo "usage: $(basename "$0") [--stage-only]" >&2; exit 2 ;;
  esac
done
# Canonical path: CMake records a resolved one, and comparing an unresolved
# path against it made drop_stale_cache wipe the build dir on every run.
BUILD_BASE="${BUILD_BASE:-$(cd "$REPO_ROOT/.." && pwd)/blender_build_android}"
: "${LIBDIR:=$BUILD_BASE/lib/android_arm64}"
BUILD="${BUILD:-$BUILD_BASE/build_android_$CONFIG}"
BT="$ANDROID_HOME/build-tools/35.0.1"
ANDROID_JAR="$ANDROID_HOME/platforms/android-35/android.jar"
# Flavours differ only in what gets packaged, so they share a build tree but
# need their own stage and output name.
FLAVOUR="${BLENDER_ANDROID_FLAVOUR:-}"
STAGE="$BUILD_BASE/android_apk_stage_$CONFIG$FLAVOUR"
JNI="$STAGE/lib/arm64-v8a"
OUT="$STAGE/blender-$CONFIG$FLAVOUR.apk"

rm -rf "$STAGE"; mkdir -p "$JNI"

echo "[apk] gathering native libraries"
cp "$BUILD/lib/libblender.so" "$JNI/"
cp "$ANDROID_SYSROOT/usr/lib/aarch64-linux-android/libc++_shared.so" "$JNI/"

# libadrenotools loads its hooks by name from the native library directory, so they
# have to ship in the APK for the Turnip driver path to work.
# Mesa Turnip, loaded from the APK so a release build can use it too. Android only
# extracts native libraries named lib*.so, and the soname must match the file name.
TURNIP="$LIBDIR/turnip/lib/libvulkan_turnip.so"
# Shipping the driver is what turns Turnip on at runtime, so only the Turnip
# flavour of the APK may carry it.
if [ "${BLENDER_ANDROID_TURNIP:-0}" = "1" ] && [ -f "$TURNIP" ]; then
  cp "$TURNIP" "$JNI/"
  patchelf --set-soname libvulkan_turnip.so "$JNI/libvulkan_turnip.so"

  # The driver links two private platform libraries that an app namespace does
  # not expose, so it cannot be dlopened without stand-ins alongside it.
  SHIM_SRC="$SCRIPT_DIR/../turnip_shim/shim.c"
  CLANG="$ANDROID_LLVM_BIN/aarch64-linux-android$ANDROID_API-clang"
  for part in CUTILS:libcutils.so HARDWARE:libhardware.so; do
    out="${part#*:}"
    "$CLANG" -shared -fPIC -O2 -DSHIM_"${part%%:*}" -o "$JNI/$out" "$SHIM_SRC" \
      -Wl,-soname,"$out"
  done
  echo "[apk] bundled Mesa Turnip driver + platform shims"
fi

if [ -d "$LIBDIR/adrenotools/hooks" ]; then
  cp "$LIBDIR"/adrenotools/hooks/*.so "$JNI/"
  echo "[apk] bundled adrenotools hooks (Turnip support)"
fi

# The Python interpreter, shipped so `sys.executable` is real. Blender's
# extension system runs its CLI as a subprocess (bl_extension_utils.py builds
# `[sys.executable, ..., blender_ext.py]`), so browsing or installing an online
# extension needs an interpreter it can actually execute.
#
# It has to live here rather than in the runtime payload: the payload is
# unpacked into the app's data directory, which is mounted noexec from API 29
# on, while this directory is extracted read-only by the package manager and
# stays executable. The lib*.so name is what makes the installer extract it at
# all -- it is an ELF executable, not a library, and nothing dlopens it.
# BlenderActivity symlinks python/bin/python3.13 in the payload to this file so
# BKE_appdir_program_python_search finds it where Blender already looks.
PY_BIN="$LIBDIR/python/bin/python3.13"
if [ -f "$PY_BIN" ]; then
  cp "$PY_BIN" "$JNI/libpython3_13_bin.so"
  chmod 755 "$JNI/libpython3_13_bin.so"
  # A plain exec'd child does not inherit the app's library namespace, so the
  # interpreter has to find libpython3.13.so itself. $ORIGIN is this same
  # directory, which is where the packager puts it.
  patchelf --set-rpath '$ORIGIN' "$JNI/libpython3_13_bin.so"
  echo "[apk] bundled Python interpreter for sys.executable"
else
  echo "[apk] WARNING: $PY_BIN missing; online extensions will not work" >&2
fi

readelf_needed() {
  "$ANDROID_LLVM_BIN/llvm-readelf" -d "$1" 2>/dev/null |
    sed -nE 's/.*\(NEEDED\).*\[(.*)\]/\1/p'
}
# Android requires unversioned sonames (libX.so, never libX.so.N).
unversion() { echo "${1%%.so*}.so"; }
searchdirs=$(ls -d "$LIBDIR"/*/lib 2>/dev/null)

# Assemble the runtime payload first. Python is optional in the bootstrap
# profile; when enabled, its extension modules also seed dependency resolution
# (their NEEDED libs — libffi, ssl, sqlite… — must ship in jniLibs).
echo "[apk] assembling runtime payload (scripts + datafiles)"
ASSETS="$STAGE/assets"
PAYLOAD="$STAGE/payload"
mkdir -p "$ASSETS" "$PAYLOAD"
cp -R "$REPO_ROOT/release/datafiles" "$PAYLOAD/datafiles"
cp -R "$REPO_ROOT/scripts" "$PAYLOAD/scripts"

# The essentials asset library. Since 4.3 a brush is an asset rather than code,
# so without this there is not one brush in sculpt, texture paint, vertex paint,
# weight paint, grease pencil or curves, the asset browser reports "No asset
# catalogs", and none of the bundled compositor, geometry or shader node groups
# exist. Blender looks for it at BLENDER_SYSTEM_DATAFILES/assets
# (essentials_directory_path()); the desktop install target puts it there from
# the same source directory.
cp -R "$REPO_ROOT/assets" "$PAYLOAD/datafiles/assets"
echo "[apk] bundled the essentials asset library (brushes, node groups)"

# Interface translations. WITH_INTERNATIONAL is on in the target build, so
# Blender uses these when they are present and otherwise leaves the Language
# menu empty. Only the compiled catalogues ship: the .po sources are 84 MB and
# have no business in an APK. msgfmt is Blender's own, already built alongside
# the host code generators.
MSGFMT="$BUILD_BASE/build_host_tools_$CONFIG/bin/msgfmt"
if [ -x "$MSGFMT" ] && [ -d "$REPO_ROOT/locale/po" ]; then
  LOCALE_DIR="$PAYLOAD/datafiles/locale"
  mkdir -p "$LOCALE_DIR"
  cp "$REPO_ROOT/locale/languages" "$LOCALE_DIR/"
  locale_count=0
  for po in "$REPO_ROOT"/locale/po/*.po; do
    lang="$(basename "$po" .po)"
    mkdir -p "$LOCALE_DIR/$lang/LC_MESSAGES"
    "$MSGFMT" "$po" "$LOCALE_DIR/$lang/LC_MESSAGES/blender.mo"
    locale_count=$((locale_count + 1))
  done
  echo "[apk] bundled $locale_count interface translations"
else
  echo "[apk] WARNING: no msgfmt or locale/po; the interface will be English only" >&2
fi

# The glTF add-on dlopens this for meshopt-compressed meshes, from
# `resource_path('SYSTEM_LIBS')/scripts/addons_core/io_scene_gltf2` -- see
# `dll_path()` in the add-on's `io/com/library.py`. GHOST_SystemPathsAndroid
# points SYSTEM_LIBS at the payload root, which is what makes that resolve.
# Plain glTF works without it; only compression needs the bridge.
for bridge in meshopt draco; do
  so="$BUILD/lib/libbf_intern_${bridge}_bridge.so"
  if [ -f "$so" ]; then
    dest="$PAYLOAD/scripts/addons_core/io_scene_gltf2/libbf_intern_${bridge}_bridge.so"
    cp "$so" "$dest"
    # Draco is linked statically and arrives around 25 MB, nearly all of it
    # debug information that no on-device workflow can use.
    "$ANDROID_LLVM_BIN/llvm-strip" --strip-unneeded "$dest"
    echo "[apk] bundled the glTF $bridge bridge ($(du -h "$dest" | cut -f1))"
  fi
done

# OIDN is linked statically into libblender.so (deps/build.sh::build_oidn sets
# -DOIDN_STATIC_LIB=ON, so no libOpenImageDenoise*.so exists to stage and the
# dlopen()ed device_cpu module is removed from the picture entirely). If a
# stray .a-less, shared build ever returns, fail loudly instead of shipping an
# APK whose denoiser dies at runtime with "Failed to create OIDN CPU device".
if [ "$(find "$LIBDIR/openimagedenoise/lib" -name 'libOpenImageDenoise*.so*' 2>/dev/null | wc -l)" -gt 0 ]; then
  echo "ERROR: OIDN built SHARED; static link required (build.sh build_oidn)." >&2
  exit 1
fi

# USD finds its file-format plugins through the plugInfo.json files here, so
# without them the importers and exporters are built but never register.
if [ -d "$LIBDIR/usd/plugin/usd" ]; then
  mkdir -p "$PAYLOAD/datafiles/usd"
  cp -R "$LIBDIR/usd/plugin/usd/." "$PAYLOAD/datafiles/usd/"
  echo "[apk] bundled USD plugin resources"
fi

# Cycles registers itself from Python, and that half lives outside scripts/ --
# CMake only puts it in place during install, which this packaging path skips.
# Without it the engine is linked in but never appears in the render engine list.
if grep -q "set(WITH_CYCLES ON" "$REPO_ROOT/build_files/android/android_features_$CONFIG.cmake"; then
  cp -R "$REPO_ROOT/intern/cycles/blender/addon" "$PAYLOAD/scripts/addons_core/cycles"
  echo "[apk] bundled the Cycles add-on"
fi
# A package-only run restores the native build's output from an artifact, and
# deliberately does not restore CMakeCache.txt: the absolute build path recorded
# in it would make build_apk.sh's drop_stale_cache wipe the build dir (and the
# libblender.so with it) before we get here. The native job therefore stamps the
# decision it actually made into a marker file next to the .so.
WITH_PYTHON_MARKER="$BUILD/with_python"
if [ -f "$WITH_PYTHON_MARKER" ]; then
  with_python=$(tr -d '[:space:]' < "$WITH_PYTHON_MARKER")
elif grep -q '^WITH_PYTHON:BOOL=ON$' "$BUILD/CMakeCache.txt" 2>/dev/null; then
  with_python=ON
else
  with_python=OFF
fi
if [ "$with_python" = "ON" ]; then
  mkdir -p "$PAYLOAD/python/lib"
  cp -R "$LIBDIR/python/lib/python3.13" "$PAYLOAD/python/lib/python3.13"
  echo "[apk] bundled Python runtime"
else
  echo "[apk] Python disabled for bootstrap build"
fi
find "$PAYLOAD" -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true
# CPython ships extension modules for every platform its build recorded
# (Mach-O `*-darwin.so`, `.dylib`); only the Android ELF ones are useful here
# and anything else would ship twice (runtime payload + jniLibs).
find "$PAYLOAD/python" \( -name '*-darwin*' -o -name '*.dylib' -o -name '*.pyd' \) -delete 2>/dev/null || true

echo "[apk] gathering native libraries (unversioned)"
# Seed queue: libblender + c++_shared + every Python extension module, when
# Python is part of this profile.
if [ -d "$PAYLOAD/python" ]; then
  for so in $(find "$PAYLOAD/python" -name '*.so'); do
    b="$(unversion "$(basename "$so")")"
    [ -f "$JNI/$b" ] || cp "$so" "$JNI/$b"
  done
fi
changed=1
while [ "$changed" = 1 ]; do
  changed=0
  for cur in "$JNI"/*.so; do
    for need in $(readelf_needed "$cur"); do
      case "$need" in lib*.so|lib*.so.*) ;; *) continue;; esac
      base="$(unversion "$need")"
      [ -f "$JNI/$base" ] && continue
      case "$base" in
        libc.so|libm.so|libdl.so|liblog.so|libandroid.so|libGLESv1_CM.so|\
        libGLESv2.so|libGLESv3.so|libEGL.so|libvulkan.so|libOpenSLES.so|\
        libjnigraphics.so|libz.so) continue;;
      esac
      for d in $searchdirs; do
        if [ -f "$d/$base" ]; then cp "$d/$base" "$JNI/$base"; changed=1; break; fi
        cand=$(ls "$d/$base".* 2>/dev/null | head -1 || true)
        if [ -n "$cand" ]; then cp "$cand" "$JNI/$base"; changed=1; break; fi
      done
    done
  done
done

echo "[apk] rewriting sonames + NEEDED to unversioned"
patch_unversion() {
  patchelf --set-soname "$(basename "$1")" "$1" 2>/dev/null || true
  for need in $(readelf_needed "$1"); do
    case "$need" in
      *.so.*) patchelf --replace-needed "$need" "$(unversion "$need")" "$1" 2>/dev/null || true;;
    esac
  done
}
for so in "$JNI"/*.so; do patch_unversion "$so"; done
# Python extension modules load from filesDir but resolve NEEDED via jniLibs.
if [ -d "$PAYLOAD/python" ]; then
  for so in $(find "$PAYLOAD/python" -name '*.so'); do
    patch_unversion "$so"
  done
fi

echo "[apk] stripping native libraries"
for so in "$JNI"/*.so; do
  "$ANDROID_LLVM_BIN/llvm-strip" --strip-unneeded "$so" 2>/dev/null || true
done
echo "[apk] bundled $(ls "$JNI" | wc -l | tr -d ' ') native libraries ($(du -sh "$JNI" | cut -f1))"

( cd "$PAYLOAD" && zip -qr -X "$ASSETS/blender_runtime.zip" . )
echo "[apk] runtime payload: $(du -sh "$ASSETS/blender_runtime.zip" | cut -f1)"
# The device unpacks the payload once and keys that on a revision string. Deriving it
# from the archive is what makes a runtime change actually reach the device: a Python
# script, a datafile or an asset edit now invalidates the marker on its own, instead of
# being silently ignored until someone remembers to bump a constant by hand.
sha256sum "$ASSETS/blender_runtime.zip" | cut -c1-16 > "$ASSETS/blender_runtime.rev"
echo "[apk] runtime revision: $(cat "$ASSETS/blender_runtime.rev")"

if [ "$STAGE_ONLY" = 1 ]; then
  echo "[apk] stage-only: staged $JNI and $ASSETS, stopping before aapt2"
  du -sh "$STAGE"
  exit 0
fi

echo "[apk] compiling resources"
RES_SRC="$SCRIPT_DIR/app/src/main/res"
mkdir -p "$STAGE/rescompiled" "$STAGE/rjava"
"$BT/aapt2" compile --dir "$RES_SRC" -o "$STAGE/rescompiled/res.zip"

echo "[apk] linking resources"
# The manifest is not debuggable, so a distributed APK never is by accident.
# Debug builds ask for it here; needed to attach validation layers or a debugger.
AAPT_DEBUG=""
if [ -n "${BLENDER_ANDROID_DEBUGGABLE:-}" ]; then
  AAPT_DEBUG="--debug-mode"
  echo "[apk] debuggable build"
fi
AAPT_PACKAGE_ARGS=()
if [ -n "${BLENDER_ANDROID_APPLICATION_ID:-}" ]; then
  AAPT_PACKAGE_ARGS=(--rename-manifest-package "$BLENDER_ANDROID_APPLICATION_ID")
  echo "[apk] application id: $BLENDER_ANDROID_APPLICATION_ID"
fi
# --java emits the R class, and it has to run before javac: the layouts are
# addressed by R.layout/R.id, so R is a compile-time dependency of the sources.
# This is why linking used to sit after the dex step -- it did not have to care
# about the code, and the old single activity never named a resource. Now that
# the code does, link first and the dex goes into the APK it produced.
"$BT/aapt2" link -o "$STAGE/base.apk" -I "$ANDROID_JAR" $AAPT_DEBUG \
  "${AAPT_PACKAGE_ARGS[@]}" \
  --manifest "$SCRIPT_DIR/app/src/main/AndroidManifest.xml" \
  --java "$STAGE/rjava" \
  `# These are the app's own resources, not an overlay. Passing them with -R` \
  `# made aapt2 demand that each one override something that already exists,` \
  `# which file resources like the icon get away with but a new colour or` \
  `# style does not: "does not override an existing resource".` \
  "$STAGE/rescompiled/res.zip" \
  -A "$ASSETS" -0 zip \
  --min-sdk-version "$ANDROID_API" --target-sdk-version "$ANDROID_TARGET_API"

echo "[apk] compiling activities"
mkdir -p "$STAGE/javac" "$STAGE/dex"
# The whole source tree plus the generated R, not a named file: there is more
# than one activity now (LauncherActivity plus BlenderActivity) and an explicit
# list is one more thing to forget when the next one is added.
"$JAVA_HOME/bin/javac" -classpath "$ANDROID_JAR" -source 17 -target 17 \
  -d "$STAGE/javac" \
  $(find "$SCRIPT_DIR/app/src/main/java" "$STAGE/rjava" -name '*.java')
"$BT/d8" --min-api "$ANDROID_API" --output "$STAGE/dex" \
  $(find "$STAGE/javac" -name '*.class')

echo "[apk] assembling"
cp "$STAGE/base.apk" "$OUT"
( cd "$STAGE/dex" && zip -q "$OUT" classes.dex )
( cd "$STAGE" && zip -qr "$OUT" lib )

echo "[apk] signing"
KS="$BUILD_BASE/android-debug.keystore"  # persistent: stable signature across runs
if [ ! -f "$KS" ]; then
  "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KS" -storepass android \
    -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
"$BT/zipalign" -f -p 4 "$OUT" "$STAGE/blender-aligned.apk"
mv "$STAGE/blender-aligned.apk" "$OUT"
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android "$OUT"

echo "[apk] done -> $OUT"
ls -lh "$OUT"
