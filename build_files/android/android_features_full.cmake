# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# FULL Android config — everything, for flagship devices (modern Qualcomm etc.).
# Cycles path tracer, video (ffmpeg), USD/OpenVDB/Alembic/MaterialX, LLVM
# (OSL to be enabled once its host-clang bitcode step is wired up).

include(${CMAKE_CURRENT_LIST_DIR}/android_features_common.cmake)

set(WITH_CYCLES ON CACHE BOOL "" FORCE)
set(WITH_CYCLES_EMBREE ON CACHE BOOL "" FORCE)
set(WITH_CODEC_FFMPEG ON CACHE BOOL "" FORCE)
set(WITH_USD ON CACHE BOOL "" FORCE)
set(WITH_MATERIALX ON CACHE BOOL "" FORCE)
set(WITH_OPENVDB ON CACHE BOOL "" FORCE)
set(WITH_ALEMBIC ON CACHE BOOL "" FORCE)
set(WITH_LLVM ON CACHE BOOL "" FORCE)
set(WITH_RUBBERBAND ON CACHE BOOL "" FORCE)

# CPU denoiser for Cycles and the compositor's Denoise node. Built statically
# for arm64 by build_files/android/deps/build.sh (ispc + oidn).
set(WITH_OPENIMAGEDENOISE ON CACHE BOOL "" FORCE)

# Draco-compressed glTF. The add-on loads bf_intern_draco_bridge for it; most
# .glb files published in the wild are compressed this way.
set(WITH_DRACO ON CACHE BOOL "" FORCE)

# The exact boolean solver. Without GMP only the fast float solver exists,
# which is the one that leaves holes on awkward intersections.
set(WITH_GMP ON CACHE BOOL "" FORCE)

# Mantaflow smoke and fluid simulation. No dependency to cross-compile: the
# solver is vendored at extern/mantaflow and ships preprocessed, so there is no
# code-generation step either. CMakeLists.txt asks only for WITH_PYTHON and
# WITH_TBB, which this config already has.
set(WITH_MOD_FLUID ON CACHE BOOL "" FORCE)

# Everything below is enabled by a dependency that deps/build.sh now
# cross-compiles. See ANDROID_MISSING_FEATURES.md for what each one restores.

# The newer boolean backend. Upstream leaves this ON and registers the solver in
# the RNA enum with no #ifdef, so it was listed in the Boolean modifier all along
# without a backend behind it -- which is why choosing it behaved oddly.
set(WITH_MANIFOLD ON CACHE BOOL "" FORCE)

# Camera and object motion tracking, and the Movie Clip editor. gflags and glog
# are vendored in extern/; ceres was the only missing piece.
set(WITH_LIBMV ON CACHE BOOL "" FORCE)

# PDF export from Grease Pencil.
set(WITH_HARU ON CACHE BOOL "" FORCE)

# Ocean modifier. Unlike the fluid solver this one really does need FFTW.
set(WITH_FFTW3 ON CACHE BOOL "" FORCE)
set(WITH_MOD_OCEANSIM ON CACHE BOOL "" FORCE)

# Path guiding in Cycles.
set(WITH_CYCLES_PATH_GUIDING ON CACHE BOOL "" FORCE)
