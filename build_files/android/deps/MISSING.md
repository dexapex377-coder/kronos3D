# Android dependency gap analysis

Blender has 97 dep recipes; 46 are cross-compiled (see STATUS.md). The 53
"missing" break down as follows.

## Already satisfied (naming / vendored — no work)
- `ffi` → built as `libffi`; `ssl` → built as `openssl`.
- `vulkan-memory-allocator`, `volk` → vendored in `extern/`.
- `vulkan` headers → provided by the NDK sysroot.

## Must build for a minimal Blender (small / required)
- **eigen** — required across Blender core (header-only).
- **sse2neon** — x86 SIMD→NEON shim so SSE code compiles on arm64 (header-only).
- **fribidi** — bidirectional text for the UI (autotools).
- **abseil** — pulled in via OIIO/USD headers at Blender link time.
- **spirv-reflect** — Vulkan backend (small).
- **shaderc** — Vulkan backend runtime shader compile (heavy: glslang+SPIRV-Tools);
  build only if the Vulkan backend configure demands it.

## Deferred-but-wanted — now resolved
- **lzma, bzip2** — ✅ built; Python rebuilt so `_lzma`/`_bz2` modules exist.
- **xml2** — ✅ built (libxml2 2.14.6).

## Resolved since
- **openimagedenoise** — ✅ built statically for arm64 (CPU device, ISPC NEON
  kernels), enabled in the `full` profile. **ispc** comes along as a host
  compiler. See `OPENIMAGEDENOISE.md`.

## Still deferred (genuinely hard / optional, not blocking Blender launch)
- **numpy** — full cross-build (meson + BLAS + C extensions); addons want it,
  core runs without. Add via cross-build or bundled wheel later.
- **cython, nanobind, pybind11** — only needed to re-enable Python bindings in
  OCIO/OIIO/USD; header/tool deps, low priority.
- **osl** — needs a HOST clang matching LLVM 20 (another large host LLVM build)
  to generate shader bitcode. WITH_CYCLES_OSL stays off until then.

## Disabled via WITH_* on Android (desktop-only / optional / not needed)
ceres (libmv off), draco + meshoptimizer (glTF), gmp (exact boolean),
fftw, flac + sndfile + rubberband + openal + sdl (audio codecs),
libheif, openjph, openpgl, manifold, thorvg, tracy, haru,
epoxy (desktop GL), wayland + wayland_protocols + wayland_weston, xr_openxr,
spnav (NDOF), zstandard.

## Build-system helpers (not real libraries — skip)
check_compilers, check_software, setup_msys2, package_python,
python_site_packages, ocloc, shaderc_deps, pthreads (Windows).
