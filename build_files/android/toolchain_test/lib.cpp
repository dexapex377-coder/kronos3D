// SPDX-FileCopyrightText: 2026 Blender Authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Proves we can link against the Android system libs the GHOST backend needs:
// - <android/native_window.h> : ANativeWindow* for the Vulkan surface
// - <android/log.h>           : logging bridge
#include <android/native_window.h>
#include <android/log.h>

extern "C" int blender_android_probe(ANativeWindow *win) {
  __android_log_print(ANDROID_LOG_INFO, "blender", "probe win=%p", (void *)win);
  return win ? ANativeWindow_getWidth(win) : -1;
}
