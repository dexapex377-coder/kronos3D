/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_WindowAndroid class.
 */

#pragma once

#include "GHOST_Window.hh"

struct ANativeWindow;
class GHOST_SystemAndroid;

class GHOST_WindowAndroid : public GHOST_Window {
 public:
  GHOST_WindowAndroid(GHOST_SystemAndroid *system,
                      ANativeWindow *native_window,
                      const char *title,
                      uint32_t width,
                      uint32_t height,
                      GHOST_TWindowState state,
                      GHOST_TDrawingContextType type,
                      const GHOST_ContextParams &context_params);

  ~GHOST_WindowAndroid() override;

  bool getValid() const override;

  ANativeWindow *getNativeWindow() const
  {
    return native_window_;
  }

  /* The single fullscreen surface is replaced when Android recreates it. */
  void setNativeWindow(ANativeWindow *native_window);

  void getWindowBounds(GHOST_Rect &bounds) const override;
  void getClientBounds(GHOST_Rect &bounds) const override;

  GHOST_TSuccess setClientWidth(uint32_t width) override;
  GHOST_TSuccess setClientHeight(uint32_t height) override;
  GHOST_TSuccess setClientSize(uint32_t width, uint32_t height) override;

  void screenToClient(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const override;
  void clientToScreen(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const override;

  void setTitle(const char *title) override;
  std::string getTitle() const override;

  GHOST_TSuccess setState(GHOST_TWindowState state) override;
  GHOST_TWindowState getState() const override;

  GHOST_TSuccess setOrder(GHOST_TWindowOrder /*order*/) override
  {
    return GHOST_kSuccess;
  }

  GHOST_TSuccess invalidate() override;

  uint16_t getDPIHint() override;

 protected:
  GHOST_Context *newDrawingContext(GHOST_TDrawingContextType type) override;

  GHOST_TSuccess setWindowCursorGrab(GHOST_TGrabCursorMode /*mode*/) override
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess setWindowCursorShape(GHOST_TStandardCursor /*shape*/) override
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess hasCursorShape(GHOST_TStandardCursor /*shape*/) override
  {
    return GHOST_kFailure;
  }
  GHOST_TSuccess setWindowCursorVisibility(bool /*visible*/) override
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess setWindowCustomCursorShape(const uint8_t * /*bitmap*/,
                                            const uint8_t * /*mask*/,
                                            const int /*size*/[2],
                                            const int /*hot_size*/[2],
                                            bool /*can_invert_color*/) override
  {
    return GHOST_kFailure;
  }

 private:
  GHOST_SystemAndroid *system_;
  ANativeWindow *native_window_;
  std::string title_;
};
