# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# LITE Android config — first-frame bootstrap for phones. Keep the viewport,
# modeling, image/color and font stack, while excluding optional subsystems that
# either add a large dependency chain or run before the first frame. Embedded
# Python stays enabled because Blender's editor headers, menus and panels are
# registered by the bundled startup scripts. NumPy is not needed for that UI.

include(${CMAKE_CURRENT_LIST_DIR}/android_features_common.cmake)

set(WITH_CYCLES OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES_EMBREE OFF CACHE BOOL "" FORCE)
set(WITH_CODEC_FFMPEG OFF CACHE BOOL "" FORCE)
set(WITH_USD OFF CACHE BOOL "" FORCE)
set(WITH_MATERIALX OFF CACHE BOOL "" FORCE)
set(WITH_OPENVDB OFF CACHE BOOL "" FORCE)
set(WITH_ALEMBIC OFF CACHE BOOL "" FORCE)
set(WITH_LLVM OFF CACHE BOOL "" FORCE)
set(WITH_RUBBERBAND OFF CACHE BOOL "" FORCE)
set(WITH_PYTHON ON CACHE BOOL "" FORCE)
set(WITH_PYTHON_NUMPY OFF CACHE BOOL "" FORCE)
set(WITH_FREESTYLE OFF CACHE BOOL "" FORCE)
set(WITH_AUDASPACE OFF CACHE BOOL "" FORCE)
set(WITH_OPENSUBDIV OFF CACHE BOOL "" FORCE)
set(WITH_POTRACE OFF CACHE BOOL "" FORCE)
set(WITH_TBB OFF CACHE BOOL "" FORCE)
# These all want TBB or Cycles, which this config drops, or are simply more than
# a bootstrap profile needs. Stated rather than left to the dependency warnings,
# so the lite feature set reads as a set of decisions.
set(WITH_MOD_FLUID OFF CACHE BOOL "" FORCE)
set(WITH_MANIFOLD OFF CACHE BOOL "" FORCE)
set(WITH_LIBMV OFF CACHE BOOL "" FORCE)
set(WITH_HARU OFF CACHE BOOL "" FORCE)
set(WITH_FFTW3 OFF CACHE BOOL "" FORCE)
set(WITH_MOD_OCEANSIM OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES_PATH_GUIDING OFF CACHE BOOL "" FORCE)
set(WITH_TBB_MALLOC_PROXY OFF CACHE BOOL "" FORCE)
