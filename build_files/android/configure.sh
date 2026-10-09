#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Configure an Android cross-compile build with the NDK CMake toolchain.
#
# Usage:
#   build_files/android/configure.sh test     # configure+build the toolchain sanity project
#   build_files/android/configure.sh ghost    # (future) configure the GHOST module for Android
#
# All target/ABI/API knobs come from build_files/android/env.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/env.sh"

if [ ! -f "$ANDROID_TOOLCHAIN_FILE" ]; then
  echo "ERROR: NDK toolchain file not found: $ANDROID_TOOLCHAIN_FILE" >&2
  exit 1
fi

TARGET="${1:-test}"

configure() {
  local src_dir="$1"
  local build_dir="$2"
  echo "[configure] src=$src_dir build=$build_dir"
  cmake -S "$src_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DCMAKE_BUILD_TYPE=Release
}

case "$TARGET" in
  test)
    SRC="$SCRIPT_DIR/toolchain_test"
    BUILD="$REPO_ROOT/../blender_build_android/build_android_toolchain_test"
    configure "$SRC" "$BUILD"
    cmake --build "$BUILD" -v
    echo
    echo "=== artifacts ==="
    file "$BUILD"/toolchain_test "$BUILD"/libtoolchain_test_lib.so
    ;;
  ghost)
    echo "Not wired yet — the GHOST Android backend sources land in the next step." >&2
    exit 2
    ;;
  *)
    echo "Unknown target '$TARGET' (expected: test | ghost)" >&2
    exit 1
    ;;
esac
