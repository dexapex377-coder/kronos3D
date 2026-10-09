/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_WindowAndroid.hh"
#include "GHOST_SystemAndroid.hh"

#ifdef WITH_VULKAN_BACKEND
#  include "GHOST_ContextVK.hh"
#endif

#include "GHOST_AndroidMemoryTier.hh"

#include <android/log.h>
#include <android/native_window.h>

/**
 * Shrink the buffers the window renders into; the display scales them back up. Every render
 * target follows the window size, so this cuts GPU memory by the square of the divisor.
 */
static void ghost_android_apply_render_scale(ANativeWindow *native_window)
{
  const uint32_t divisor = GHOST_android_render_scale_divisor();
  if (native_window == nullptr || divisor <= 1) {
    return;
  }
  const int32_t native_w = ANativeWindow_getWidth(native_window);
  const int32_t native_h = ANativeWindow_getHeight(native_window);
  if (native_w <= 0 || native_h <= 0) {
    return;
  }
  const int32_t w = std::max(1, native_w / int32_t(divisor));
  const int32_t h = std::max(1, native_h / int32_t(divisor));
  /* Format 0 keeps the window's current pixel format. */
  ANativeWindow_setBuffersGeometry(native_window, w, h, 0);
  __android_log_print(ANDROID_LOG_INFO,
                      "blender-renderscale",
                      "native=%dx%d -> buffers=%dx%d (divisor=%u)",
                      native_w,
                      native_h,
                      w,
                      h,
                      divisor);
}

GHOST_WindowAndroid::GHOST_WindowAndroid(GHOST_SystemAndroid *system,
                                         ANativeWindow *native_window,
                                         const char *title,
                                         uint32_t width,
                                         uint32_t height,
                                         GHOST_TWindowState state,
                                         GHOST_TDrawingContextType type,
                                         const GHOST_ContextParams &context_params)
    : GHOST_Window(width, height, state, context_params, false),
      system_(system),
      native_window_(native_window),
      title_(title ? title : "")
{
  if (native_window_) {
    ANativeWindow_acquire(native_window_);
    ghost_android_apply_render_scale(native_window_);
  }
  setDrawingContextType(type);
}

GHOST_WindowAndroid::~GHOST_WindowAndroid()
{
  releaseNativeHandles();
  if (native_window_) {
    ANativeWindow_release(native_window_);
    native_window_ = nullptr;
  }
}

bool GHOST_WindowAndroid::getValid() const
{
  return GHOST_Window::getValid() && native_window_ != nullptr;
}

void GHOST_WindowAndroid::setNativeWindow(ANativeWindow *native_window)
{
  if (native_window_) {
    ANativeWindow_release(native_window_);
  }
  native_window_ = native_window;
  if (native_window_) {
    ANativeWindow_acquire(native_window_);
    ghost_android_apply_render_scale(native_window_);
  }

#ifdef WITH_VULKAN_BACKEND
  /* Rebuild the Vulkan surface/swapchain for the replaced native window, else we
   * keep presenting to a dead surface (black screen after doze/wake or rotation). */
  if (GHOST_ContextVK *context = dynamic_cast<GHOST_ContextVK *>(getContext())) {
    context->setAndroidNativeWindow(native_window_);
  }
#endif
}

GHOST_Context *GHOST_WindowAndroid::newDrawingContext(GHOST_TDrawingContextType type)
{
#ifdef WITH_VULKAN_BACKEND
  if (type == GHOST_kDrawingContextTypeVulkan) {
    GHOST_Context *context = new GHOST_ContextVK(
        want_context_params_, native_window_, 1, 2, GHOST_GPUDevice{});
    if (context->initializeDrawingContext() == GHOST_kSuccess) {
      return context;
    }
    delete context;
  }
#else
  (void)type;
#endif
  return nullptr;
}

void GHOST_WindowAndroid::getWindowBounds(GHOST_Rect &bounds) const
{
  getClientBounds(bounds);
}

void GHOST_WindowAndroid::getClientBounds(GHOST_Rect &bounds) const
{
  /* ANativeWindow_getWidth/Height report the display size, not the (possibly reduced)
   * buffer size, so apply the same divisor used for the buffer geometry. */
  const uint32_t divisor = GHOST_android_render_scale_divisor();
  const int32_t w = native_window_ ? ANativeWindow_getWidth(native_window_) / int32_t(divisor) : 0;
  const int32_t h = native_window_ ? ANativeWindow_getHeight(native_window_) / int32_t(divisor) :
                                     0;
  bounds.set(0, 0, w, h);
}

GHOST_TSuccess GHOST_WindowAndroid::setClientWidth(uint32_t /*width*/)
{
  return GHOST_kFailure;
}

GHOST_TSuccess GHOST_WindowAndroid::setClientHeight(uint32_t /*height*/)
{
  return GHOST_kFailure;
}

GHOST_TSuccess GHOST_WindowAndroid::setClientSize(uint32_t /*width*/, uint32_t /*height*/)
{
  return GHOST_kFailure;
}

void GHOST_WindowAndroid::screenToClient(
    int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const
{
  outX = inX;
  outY = inY;
}

void GHOST_WindowAndroid::clientToScreen(
    int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const
{
  outX = inX;
  outY = inY;
}

void GHOST_WindowAndroid::setTitle(const char *title)
{
  title_ = title ? title : "";
}

std::string GHOST_WindowAndroid::getTitle() const
{
  return title_;
}

GHOST_TSuccess GHOST_WindowAndroid::setState(GHOST_TWindowState /*state*/)
{
  return GHOST_kSuccess;
}

GHOST_TWindowState GHOST_WindowAndroid::getState() const
{
  return GHOST_kWindowStateFullScreen;
}

GHOST_TSuccess GHOST_WindowAndroid::invalidate()
{
  return GHOST_kSuccess;
}

uint16_t GHOST_WindowAndroid::getDPIHint()
{
  return system_ ? system_->getDPIHint() : 96;
}
