# Android dependency build — status

Run: `build_files/android/deps/build.sh <dep>`. Installs into
`lib/android_arm64/<dep>` (a sibling of the repo). Versions come from
`build_files/build_environment/cmake/versions.cmake`.

## Done (cross-compiled arm64, verified elf64-littleaarch64)

zlib · zstd · libdeflate · Imath · fmt · oneTBB · OpenEXR · libpng · pugixml ·
libjpeg-turbo · brotli · freetype · harfbuzz · libwebp · libtiff · openjpeg ·
expat · yaml-cpp · c-blosc · pystring · minizip-ng · OpenColorIO ·
OpenSubdiv (CPU) · robin-map · OpenImageIO · embree · Alembic · MaterialX ·
potrace · sqlite · libffi · OpenSSL · **Python 3.13** · OpenVDB(+NanoVDB) ·
ogg · vorbis · theora · opus · lame · aom · x265 · libvpx · x264 · **ffmpeg** ·
**OpenUSD 26.03** · **LLVM 20.1.8 + clang** ·
**OpenImageDenoise 2.5.0** (static, CPU device, ISPC/NEON)

## Main-build integration

- ✅ platform_android.cmake wires all harvested deps; `cmake` **configures and
  generates** with no errors (WITH_CYCLES CPU+embree, USD/OpenVDB/OIIO/OCIO/
  MaterialX/LLVM/Python all found).
- ✅ Non-codegen targets compile (e.g. bf_intern_guardedalloc).
- ⛔ BLOCKER: Blender's build-time codegen tools (makesdna, makesrna,
  shader_tool, datatoc, datatoc_icon, msgformat) get cross-compiled for arm64
  and can't run on the macOS host ("Unknown system error -8").
  FIX (iOS blueprint): port the `WITH_CROSSCOMPILED_TOOLS` mechanism from
  `origin/ios` (platform_apple.cmake + the tools' CMakeLists). It builds those
  tools natively for the host (`blender_cross_tools_compile`) and imports them
  as IMPORTED executables from `CROSSCOMPILE_TOOLDIR/bin`. This is generic
  cross logic, not Apple-specific — adapt it for Android.

### Build progress (cross-compile, fixed in order)
1. ✅ Host codegen tools (WITH_CROSSCOMPILED_TOOLS, native build + import).
2. ✅ Host/target feature-flag matching (android_features_full.cmake) — fixed RNA
   generation mismatch (fluid getters).
3. ✅ Cycles NEON via sse2neon (test_neon_support), not x86 -msse.
4. ✅ Host python for build-time scripts (discover_nodes.py).
5. ✅ GHOST headless VK ctor Android branch; USD libc++ (__gnu_cxx off).
6. ✅ Vulkan-Headers 1.4.341 (NDK ships 275, too old).
7. ✅ OpenSubdiv GLSL patch shader source header.

### CURRENT BLOCKER — GPU subdivision is GL-coupled
`intern/opensubdiv` GPU evaluator (gpu_compute_evaluator.cc, eval_output_gpu.h)
unconditionally includes `epoxy/gl.h` and osd `glPatchTable.h`/`glVertexBuffer.h`
— desktop GL. Android has no desktop GL, and osd's GL patch table can't build on
GLES. The capi compiles GPU eval whenever WITH_VULKAN_BACKEND||WITH_OPENGL_BACKEND,
but the impl is GL-only.
FIX (non-stub): add a WITH_OPENSUBDIV_GPU-style gate so on Android the GPU
evaluator sources + epoxy dep are excluded and the capi reports GPU subdiv
unavailable; Blender already falls back to CPU subdiv
(BKE_subsurf_modifier_can_do_gpu_subdiv → GPU_BACKEND_NONE path). Touches
intern/opensubdiv (CMake + capi) and must stay desktop-safe.

8. ✅ opensubdiv GPU-eval GL includes guarded (iOS approach) — GPU subdiv
   still works via the backend-agnostic GPU module (Vulkan).
9. ✅ USD `-DARCH_HAS_GNU_STL_EXTENSIONS` (Blender io/usd Linux/GCC ABI
   workaround) excluded on Android (libc++).

### CURRENT BLOCKER — numpy (as predicted)
audaspace's Python `aud` module (extern/audaspace/bindings/python/
PyAnimateableProperty.cpp) includes <numpy/ndarrayobject.h> unconditionally,
so building Blender's Python needs numpy 2.3.4 cross-compiled for Android.
This is a substantial focused task (meson cross-build + Cython + target-python
C extensions, like the Python/LLVM builds). Host python 3.13 exists at
android_deps_build/work/python-host-install (needs pip+cython+meson-python).
Install into $LIBDIR/python/lib/python3.13/site-packages and set
PYTHON_NUMPY_INCLUDE_DIRS / NUMPY_FOUND in platform_android.cmake.

Everything else compiles: DNA/RNA gen, USD, Cycles (NEON), OpenSubdiv,
OpenImageIO, Vulkan, MaterialX, etc. numpy is the last compile blocker before
linking libblender.so, then APK.

Old notes:
Next: OSL (needs a host clang matching LLVM to generate bitcode — thorny).

Notes:
- OpenColorIO: Python bindings off (Python not yet ported), SSE off (arm64);
  re-enable OCIO_BUILD_PYTHON once Python lands.
- OpenSubdiv: CPU only; gated OPENGLES_FOUND behind NO_OPENGL.
- OpenImageIO: Python/libheif/openjph/libraw/ffmpeg/OpenVDB/Qt/OpenGL off for
  now; re-enable as those deps land. Ptex optional (not built).
- embree: NEON ISA, TBB tasking, ISPC off. Alembic: HDF5 off. MaterialX:
  core/codegen only (render off, needs X11). potrace/sqlite via autotools.
- Python: two-stage (host 3.13 → NDK cross); _ctypes/_ssl/_sqlite3/zlib built.
- OpenVDB: DELAYED_LOADING off (drops Boost), NanoVDB on, python module off.
- ffmpeg: --enable-gpl, all codecs. vpx static (target rejects shared).
  x265 asm off (bad -mcpu). pthread/rt stub libs for Android libc.
- LLVM: targets AArch64;ARM;NVPTX + clang; needs host tblgen first.
- USD: monolithic, GL/OSL/Python-bindings off for now, noboost patch,
  imaging + MaterialX + OpenVDB + OIIO plugin on.
- Host tooling installed: pkgconf (ffmpeg/USD need it).

## Cross-compile lessons baked into build.sh

- `CMAKE_POLICY_VERSION_MINIMUM=3.5` — older deps predate CMake 4's floor.
- Find roots = each `lib/android_arm64/<dep>` with modes left at `ONLY`, so
  `find_*` locate our libs but never host (Homebrew) libs. Using `BOTH` leaked
  `/opt/homebrew/lib/libwebp.a` into libtiff — avoid it.
- oneTBB: NDK sets `--no-undefined-version`; TBB def files list unbuilt symbols,
  so pass `-Wl,--undefined-version`.

## Not done yet

- **Hard tier (large, likely need patches):** Python, LLVM (→ OSL), OpenVDB
  (needs boost), MaterialX, USD, Alembic, embree (needs ISPC), ffmpeg, potrace,
  sqlite, openjph, libheif, Ptex.
- Blender cannot link/run until at least Python + the remaining scene/render
  stack are up. Python is the critical path.
- Several built libs have optional features turned off pending their deps
  (see Notes above) — revisit once Python/OpenVDB/etc. exist.

## Strategy for the hard tier (use the iOS branch as a blueprint)

The `origin/ios` branch already cross-compiled the whole hard tier to a mobile
ARM target. Reuse that work per dependency:

1. `git show origin/ios:build_files/build_environment/patches/<dep>_ios.diff`
   — see exactly what they had to change to cross-compile it.
2. Adapt, don't copy: the iOS patches target Apple's toolchain/sysroot and have
   Apple-only bits (SDK paths, `libb2_apple`, x265 Apple asm, Metal). Keep the
   generic cross-compile fixes (host-tool assumptions, failing `configure`
   checks, disabled subcomponents); drop/replace the Apple-specific parts.
3. Cross-check `origin/ios:build_files/build_environment/cmake/ios_defines.cmake`
   for the flags/vars they passed each dep, and `<dep>.cmake` for the base args.
4. Store any Android patch as `build_files/android/deps/patches/<dep>_android.diff`
   and apply it in that dep's `build_*` function before configure.

Available iOS patches: brotli, embree, ffmpeg, ispc, libb2(apple), llvm,
opencolorio, openimageio, osl, python, rubberband, usd, x265(apple).
Key proof point: Python, LLVM and USD are all in there — the mobile port is
known-possible, not speculative.

## Python (the critical path) — approach

CPython 3.13 has official Android support (PEP 738), so use it instead of the
iOS patch (which is Apple-specific: Darwin cross hacks + force-crosscompile
because iOS builds on a same-arch Mac — irrelevant here).

Plan:
1. Host interpreter must match target version. Host has 3.12; build CPython
   3.13.13 natively first (`./configure && make`) to get a `python3.13` for
   `--with-build-python` / `PYTHON_FOR_BUILD`.
2. Cross-compile with the NDK: `--host=aarch64-linux-android --build=<config.guess>`,
   `--with-build-python=<host 3.13>`, `--disable-test-modules`,
   `ac_cv_file__dev_ptmx=no ac_cv_file__dev_ptc=no`, and point `--with-openssl`,
   sqlite, zlib, libffi at the harvest prefix. CPython's `Android/android.py`
   automates most of this (configure-build-python → configure-host → make-host).
3. Needs libffi + OpenSSL cross-compiled first (add build_libffi, build_openssl).
4. Install to `$LIBDIR/python`; then re-enable Python bindings in OCIO/OIIO/USD.

This is a dedicated multi-stage effort — do it as its own focused pass.

## Next steps

1. Patch + build OpenSubdiv.
2. OpenColorIO chain (pystring, minizip-ng, then OCIO).
3. Tackle the hard tier; Python is the critical path for running Blender.
4. Point Blender's main build at `lib/android_arm64` (a `platform_android.cmake`
   under `build_files/cmake/platform/`), analogous to the desktop platforms.

## ✅ BLENDER LINKS FOR ANDROID
`bin/blender` builds as an arm64 Android ELF (full build [4577/4577], exit 0).
Final-link fixes: pthread/rt stubs, TBB::tbbmalloc, volk VK_USE_PLATFORM_ANDROID_KHR.
All deps done incl. Python+numpy, LLVM, USD(+python), OpenVDB, ffmpeg, OIIO,
Cycles(NEON), Vulkan+shaderc, MaterialX, embree, OpenSubdiv.
minSdk 31 (Android 12), targetSdk 34.
Next: build as libblender.so (NativeActivity) + APK packaging; strip binary.
