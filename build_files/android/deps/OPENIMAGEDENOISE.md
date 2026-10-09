# Intel Open Image Denoise on Android

Status: **enabled in the `full` profile**, CPU device, arm64, statically linked.

## 1. Why it was not working

Nothing was broken at runtime — the denoiser was never compiled in.
`build_files/android/android_features_common.cmake` carried

```cmake
set(WITH_OPENIMAGEDENOISE OFF CACHE BOOL "" FORCE)
```

and `deps/MISSING.md` listed `openimagedenoise` among the libraries "disabled
via `WITH_*` on Android". With that flag off:

- `intern/cycles/util/openimagedenoise.h` compiles `openimagedenoise_supported()`
  down to `return false`, so `device/cpu/device.cpp` never sets
  `DENOISER_OPENIMAGEDENOISE` in `info.denoisers` and Cycles reports no
  denoiser for the CPU device;
- the compositor's Denoise node compiles its `#ifndef WITH_OPENIMAGEDENOISE`
  branch, which passes the image through unchanged.

The Cycles side needed no porting at all. `openimagedenoise_supported()`
already returns `true` for `__aarch64__`, and neither `denoiser_oidn.cpp` nor
`denoiser_oidn_base.cpp` contains x86-specific code. The whole problem was
building the library itself for Android.

## 2. What OIDN 2.5 needs on ARM64

OIDN has supported ARM64 since 2.2 (Linux, Windows, macOS), so upstream already
covers everything architectural. Four things stood between that and Android.

### 2.1 ISPC is a hard requirement, and it is a host compiler

The CPU device's kernels (`devices/cpu/*.ispc`) have no C++ fallback: without
ISPC there is no CPU device, and with no device OIDN is useless. ISPC is not
cross-compiled for the target — it runs on the build machine and *emits* target
code. The official x86-64 build already knows how:

```console
$ ispc --support-matrix | grep '^neon'
neon-i8x16   | aarch64 | arm, aarch64 | ... | Android: arm, aarch64 | ...
neon-i32x8   | aarch64 | arm, aarch64 | ... | Android: arm, aarch64 | ...
neon-i16x16  | aarch64 | arm, aarch64 | ... | Android: arm, aarch64 | ...
```

So a prebuilt ISPC release is enough; no source build, no host LLVM. Verified
by compiling OIDN's own kernels out of tree — `cpu_conv.ispc`,
`cpu_image_copy.ispc` and `cpu_pool_f16.ispc` for both `neon-i32x8` and
`neon-i16x16` — and checking the results are `ELF64 / EM_AARCH64` objects.

`oidn_ispc.cmake` never passes `--target-os`, so it would produce
`aarch64-unknown-linux-gnu` objects. Rather than patch the macro, the target OS
and the ISA baseline ride along in `ISPC_FLAGS_RELEASE`, which the macro
already splices into the command line verbatim — the same hook Blender's
desktop recipe uses for `--cpu=cortex-a78`:

```
-DISPC_FLAGS_RELEASE=-O3 --target-os=android --cpu=cortex-a55
```

`cortex-a55` is the ARMv8.2-A LITTLE core of every big.LITTLE SoC in scope
(Exynos 1380 is A78 + A55). ARMv8.2-A is what supplies the half-float and
dot-product instructions OIDN's ARM64 kernels assume unconditionally — there is
no runtime feature check on ARM, so an ARMv8.0 device (Cortex-A53 era) would
`SIGILL`. Code built for A55 runs unchanged on the big cores. Override with
`OIDN_ISPC_CPU=` if you ever target something else.

Checked against the reference device (SM-S928B, Snapdragon 8 Gen 3,
`ro.board.platform=pineapple`), `/proc/cpuinfo` reports `fphp asimdhp` (FEAT_FP16
scalar and SIMD), `asimddp` (dot product), plus `i8mm` and `bf16` — comfortably
above the ARMv8.2-A baseline this build assumes.

### 2.2 The default shared layout cannot survive an APK

By default OIDN builds `libOpenImageDenoise.so` plus a *separate*
`libOpenImageDenoise_device_cpu.so.2.5.0` that `core/module.cpp` opens with
`dlopen()` at an absolute path derived from its own via `dladdr()`. Three things
go wrong on Android:

1. the package manager only installs files from `lib/<abi>/` whose name matches
   `lib*.so` — a `.so.2.5.0` suffix is silently dropped;
2. `apk/package.sh` collects transitive libraries by walking `DT_NEEDED`, and a
   `dlopen`'ed module has no `NEEDED` entry, so it would never be staged.

Either one is decisive on its own. A third failure mode does *not* apply here
but is worth knowing about: with `extractNativeLibs=false` the path `dladdr()`
reports is `/data/app/.../base.apk!/lib/arm64-v8a/libX.so`, which `dlopen()`
will not accept for a sibling file. This port sets
`android:extractNativeLibs="true"` in `apk/app/src/main/AndroidManifest.xml`,
and a check on a real install confirms the libraries land unpacked in
`.../lib/arm64/`, so that path never comes up. It would if the manifest ever
changed.

`OIDN_STATIC_LIB=ON` removes the module loader entirely: `core/context.h`
switches from `modules.load("device_cpu")` to a direct `init_device_cpu()` call
and everything ends up inside `libblender.so`. No packaging change was needed.

Because that flips OIDN into a set of archives whose link order matters
(`OpenImageDenoise`, `_core`, `_device_cpu`, `_common`, `_weights`), Blender's
own `FindOpenImageDenoise.cmake` — which looks for one shared library — is not
usable here. `platform_android.cmake` calls `find_package(OpenImageDenoise
CONFIG REQUIRED)` instead and feeds the imported target into the
`OPENIMAGEDENOISE_*` variables the rest of the build reads.

### 2.3 Thread affinity does not link on bionic

`core/thread.cpp` takes the `__linux__` branch on Android and calls
`pthread_getaffinity_np` / `pthread_setaffinity_np`, which bionic only exposes
from API level 36 — we build against 31. `build_files/build_environment/patches/oidn_android.diff`
redirects both to `sched_getaffinity` / `sched_setaffinity` with pid `0`, which
means "the calling thread" and has existed in every API level. `PinningObserver`
runs `set()`/`restore()` on the very thread it is pinning, so the two are the
same operation.

The shim is required regardless of how new the *device* is: the reference phone
runs Android 16 (API 36) and would have the symbols at runtime, but the NDK
stubs we link against are the API 31 set, so the link fails without it.

If pinning ever misbehaves on a big.LITTLE scheduler, `OIDN_SET_AFFINITY=0` in
the environment disables it without a rebuild.

### 2.4 Weight size

The RT weights compile into the binary as C arrays and total ~45 MB, of which
23 MB is the three "large" models used only by the High quality preset. On a
phone CPU that preset is not usable anyway (roughly 4x the weights, and OIDN
turns off fast math for it), and `UNetFilter::getWeights()` already degrades to
the base model when `large` is null:

```cpp
case Quality::High:
  weightsBlob = model->large ? model->large : model->base;
```

So `build_oidn()` drops them by default; High silently resolves to the same
network Balanced uses. `OIDN_ANDROID_LARGE_WEIGHTS=1` keeps them.
`OIDN_FILTER_RTLIGHTMAP=OFF` (another 3.6 MB) matches what Blender does on
desktop. Net cost to `libblender.so`: about 22 MB.

## 3. What changed

| File | Change |
| --- | --- |
| `deps/build.sh` | `build_ispc()` (host tool), `build_oidn()`, both added to `ALL_DEPS` |
| `build_environment/patches/oidn_android.diff` | bionic affinity shim |
| `cmake/platform/platform_android.cmake` | `OpenImageDenoise_ROOT` + `find_package(... CONFIG)` |
| `android_features_common.cmake` | OIDN off by default, with the reason |
| `android_features_full.cmake` | `WITH_OPENIMAGEDENOISE ON` |

`lite` keeps it off: that profile drops TBB, which the CPU device requires.

## 4. Building

```bash
bash build_files/android/deps/build.sh ispc oidn
```

`ispc` lands in `$BUILD_BASE/android_deps_build/host/ispc` (a host x86-64
binary, deliberately outside `lib/android_arm64` so it never reaches the
target's `CMAKE_PREFIX_PATH`); `oidn` lands in
`lib/android_arm64/openimagedenoise`. `tbb` must already be built. Then the
usual:

```bash
bash build_files/android/build_apk.sh full
```

Expect in the prefix:

```text
lib/android_arm64/openimagedenoise/
  include/OpenImageDenoise/{oidn.h,oidn.hpp,config.h}
  lib/libOpenImageDenoise.a
  lib/libOpenImageDenoise_core.a
  lib/libOpenImageDenoise_device_cpu.a
  lib/libOpenImageDenoise_common.a
  lib/cmake/OpenImageDenoise-2.5.0/OpenImageDenoiseConfig.cmake
```

## 5. Verifying on device

```bash
adb shell setprop log.tag.blender VERBOSE
adb logcat -c && adb logcat | grep -i -E 'oidn|denois'
```

1. Cycles: Render Properties > Sampling > Denoise. The panel is drawn only when
   Cycles reports a denoiser for the device, so its mere presence confirms
   `DENOISER_OPENIMAGEDENOISE` reached `info.denoisers`.
2. Compositor: add a Denoise node. Without the library it is a pass-through and
   the node reports it is unavailable.
3. `OIDN_VERBOSE=2` makes OIDN print the device it picked and the thread count
   at filter commit time.

A first denoise allocates the network and is much slower than the ones after
it. On a mid-range arm64 SoC expect single-digit seconds for a 1080p frame with
the Balanced network; memory peaks at a few hundred MB, which is the figure to
watch on a phone rather than the wall time.

## 6. Build outcome and remaining risks

The dependency builds clean. What comes out:

```text
libOpenImageDenoise.a             1.5 MB   (API shim)
libOpenImageDenoise_core.a       36.3 MB   (core + the weight blobs)
libOpenImageDenoise_device_cpu.a  4.9 MB   (ISPC NEON kernels + CPU engine)
```

All three report `file format elf64-littleaarch64`, and the ISPC objects are in
the device archive as expected — `cpu_conv.dev.o`, `color.dev.o`,
`cpu_pool_f16.dev.o`, `cpu_upsample_f16.dev.o` and the rest, the `_f16` ones
being the `neon-i16x16` targets.

Risks that were anticipated and did **not** materialise:

- **Static export of the weights object library.** `OpenImageDenoise_weights`
  and `_common` are `OBJECT` libraries exported with `install(TARGETS ...
  EXPORT ...)` and no explicit `OBJECTS DESTINATION`. In practice CMake folded
  them into `libOpenImageDenoise_core.a` (hence its 36 MB) and no separate
  archives are installed. If a future OIDN version does complain about a
  missing `OBJECTS DESTINATION`, patch that `install()` call to name
  `OBJECTS DESTINATION lib`.
- **`_FORTIFY_SOURCE=2` redefinition warning.** OIDN appends it in Release and
  the NDK already defines it. Harmless; `OIDN_WARN_AS_ERRORS` is off.
- **Unversioned-symbol linker errors.** Not expected in static mode (the
  version script is only applied when `OIDN_STATIC_LIB` is off), but if a
  shared build is ever attempted, add `-Wl,--undefined-version` the way the
  oneTBB recipe does.

## 7. Not done

- **GPU denoising.** OIDN's GPU devices are SYCL/CUDA/HIP/Metal. There is no
  Vulkan device upstream, so a phone GPU cannot run OIDN at all today. CPU is
  the only path, and this is not a gap that can be closed locally.
- **`arm32`.** Only `arm64-v8a` is built; ISPC can target `arm` but nothing
  else in this port does.
