#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a full Android APK for a given config, end to end:
#   build_files/android/build_apk.sh [lite|full] [--native-only|--package-only|--stage-only]
#
# Steps: host codegen tools (config-matched) -> cross-compile libblender.so ->
# package APK. Deps must already be built (build_files/android/deps/build.sh).
#
# Modes:
#   (default)           full pipeline: host tools -> libblender.so -> APK
#   --native-only       build host tools + libblender.so, skip APK packaging
#   --package-only      skip native build, only run package.sh (requires prebuilt .so)
#   --stage-only        skip native build and stop after package.sh has staged the
#                       libs and payload; Gradle then builds the APK, which is the
#                       only path that can compile Kotlin/Compose.

set -euo pipefail
CONFIG="${1:-full}"
case "$CONFIG" in lite|full) ;; *) echo "usage: $0 [lite|full] [--native-only|--package-only|--stage-only]" >&2; exit 1;; esac
MODE="${2:-full}"
case "$MODE" in --native-only|--package-only|--stage-only|full) ;; *) echo "usage: $0 [lite|full] [--native-only|--package-only|--stage-only]" >&2; exit 1;; esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# Drop any leaked Android Studio SDK/NDK paths so env.sh picks the Homebrew ones
# (Studio's SDK lacks our build-tools/NDK version).
unset ANDROID_HOME ANDROID_NDK_ROOT ANDROID_NDK_HOME
# shellcheck source=/dev/null
source "$SCRIPT_DIR/env.sh"
cd "$REPO_ROOT"

# Canonical path: CMake records a resolved one, and comparing an unresolved
# path against it made drop_stale_cache wipe the build dir on every run.
BUILD_BASE="${BUILD_BASE:-$(cd "$REPO_ROOT/.." && pwd)/blender_build_android}"
HOST="$BUILD_BASE/build_host_tools_$CONFIG"
BUILD="$BUILD_BASE/build_android_$CONFIG"
FEATURES="build_files/android/android_features_$CONFIG.cmake"

# A CMake build dir records its own absolute path; if it was moved, cmake refuses
# to reconfigure. Wipe a relocated cache so the configure below regenerates it.
drop_stale_cache() {
  local d="$1" cache="$1/CMakeCache.txt"
  [ -f "$cache" ] || return 0
  if ! grep -q "CMAKE_CACHEFILE_DIR:INTERNAL=$d\$" "$cache" 2>/dev/null; then
    echo "[build_apk] relocated cache in $d — wiping for a clean reconfigure"
    rm -rf "$d"
  fi
}
drop_stale_cache "$HOST"
drop_stale_cache "$BUILD"

# ccache launchers: enabled when ccache is on PATH (the CI workflow sets it up
# via hendrikmuhs/ccache-action; local builds without it just skip). Both the
# host codegen tools and the target cross-compile share the same cache, and the
# prebuilt Android libs compile once-ish then hit from cache on every rerun.
CCACHE_LAUNCHER=()
if command -v ccache >/dev/null 2>&1; then
  CCACHE_LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  echo "[build_apk] ccache enabled (host tools + target)"
fi

# ==== MODE HANDLING ====
if [ "$MODE" = "--package-only" ]; then
  echo "=== [$CONFIG] package APK only (using prebuilt .so) ==="
  BLENDER_ANDROID_CONFIG="$CONFIG" BUILD="$BUILD" bash "$SCRIPT_DIR/apk/package.sh"
  exit 0
fi

if [ "$MODE" = "--stage-only" ]; then
  echo "=== [$CONFIG] stage only (libs + payload; Gradle owns the APK) ==="
  BLENDER_ANDROID_CONFIG="$CONFIG" BUILD="$BUILD" bash "$SCRIPT_DIR/apk/package.sh" --stage-only
  exit 0
fi

if [ "$MODE" = "--native-only" ]; then
  echo "=== [$CONFIG] native build only (host tools + libblender.so, no APK) ==="
else
  echo "=== [$CONFIG] full pipeline ==="
fi

# ==== HOST CODEGEN TOOLS ====
echo "=== [$CONFIG] host codegen tools ==="
# Always re-run CMake for the native code generators. This remains incremental,
# but it is essential when an Android feature profile changes: makesrna must be
# built with exactly the same WITH_* flags as the target or its generated RNA
# silently lacks registration callbacks (for example Operator/Gizmo with Python).
# Keep the GUI out since the host build only supplies generators.
cmake -S . -B "$HOST" -G Ninja -C "$FEATURES" -DWITH_CROSSCOMPILED_TOOLS=OFF \
  -DCMAKE_C_COMPILER="$ANDROID_HOST_CC" -DCMAKE_CXX_COMPILER="$ANDROID_HOST_CXX" \
  "${CCACHE_LAUNCHER[@]}" \
  -DWITH_HEADLESS=ON -DWITH_X11_XINPUT=OFF -DWITH_AUDASPACE=OFF \
  -DCMAKE_BUILD_RPATH="$REPO_ROOT/lib/linux_x64/tbb/lib"
ninja -C "$HOST" makesdna makesrna datatoc msgfmt shader_tool

echo "=== [$CONFIG] configure + build libblender.so ==="
cmake -S . -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE" \
  -DANDROID_ABI="$ANDROID_ABI" -DANDROID_PLATFORM="android-$ANDROID_API" \
  -DBUILD_BASE="$BUILD_BASE" -DCMAKE_BUILD_TYPE=Release \
  -DBLENDER_ANDROID_CONFIG="$CONFIG" \
  "${CCACHE_LAUNCHER[@]}"
# The glTF add-on dlopens the meshopt bridge at run time, so it is not a
# dependency of the blender target and would never be built otherwise.
ninja -C "$BUILD" blender bf_intern_meshopt_bridge bf_intern_draco_bridge

if [ "$MODE" = "--native-only" ]; then
  echo "=== [$CONFIG] native build complete (libblender.so ready) ==="
  exit 0
fi

echo "=== [$CONFIG] package APK ==="
BLENDER_ANDROID_CONFIG="$CONFIG" BUILD="$BUILD" bash "$SCRIPT_DIR/apk/package.sh"
