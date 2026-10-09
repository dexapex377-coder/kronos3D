# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Minimal project to verify the Android NDK CMake toolchain works end-to-end,
# independent of Blender's (not-yet-ported) dependency stack.
#
# Configure + build via:  build_files/android/configure.sh test

cmake_minimum_required(VERSION 3.20)
project(blender_android_toolchain_test CXX)

message(STATUS "System name    : ${CMAKE_SYSTEM_NAME}")
message(STATUS "Android ABI    : ${CMAKE_ANDROID_ARCH_ABI}")
message(STATUS "Android API    : ${CMAKE_SYSTEM_VERSION}")
message(STATUS "C++ compiler   : ${CMAKE_CXX_COMPILER}")

if(NOT ANDROID)
  message(FATAL_ERROR "This project must be configured with the NDK android.toolchain.cmake")
endif()

# A native executable (mirrors a headless GHOST test target).
add_executable(toolchain_test main.cpp)

# A shared library — this is the shape Blender ships on Android (libblender.so
# loaded by a NativeActivity), so prove we can link one against the Android
# system libs we will need: Vulkan, native window, logging, android glue.
add_library(toolchain_test_lib SHARED lib.cpp)
find_library(ANDROID_LOG_LIB log)
find_library(ANDROID_LIB android)
target_link_libraries(toolchain_test_lib PRIVATE ${ANDROID_LOG_LIB} ${ANDROID_LIB})
