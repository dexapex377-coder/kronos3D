/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edscr
 *
 * Touch: menu chrome at its own scale.
 *
 * A phone needs bigger buttons than a mouse does, and it has less screen to spend on them than a
 * monitor does. Raising the Resolution Scale answers the first and makes the second worse: it
 * enlarges the 3D viewport grid, the node canvas, the outliner rows and every line width in the
 * program along with the header it was raised for.
 *
 * What is wanted instead is a bigger thumb target that costs no canvas, so the multiplier is
 * scoped to the chrome: headers, the top bar, the status bar, navigation and tool bars, and every
 * pop-up. Editor content keeps the size it has.
 *
 * The mechanism is small on purpose. Every size in the interface -- widget geometry, font points,
 * icon pixels, layout spacing, region heights -- is derived from `U.scale_factor` and
 * `U.widget_unit`, at around 1800 call sites. Moving those two for the length of a scope moves
 * all of them, and none of the call sites has to know. Blender already does exactly this in
 * `wm_surface_constant_dpi_set_userpref()`, which pins a VR mirror surface to a constant size the
 * same way.
 *
 * The multiplier is applied on top of the Resolution Scale rather than in place of it, so there
 * is still one scale control: the preference moves the whole interface, and this only decides
 * where the menus start from relative to it.
 *
 * See ANDROID_TOUCH_UI_SCALE_STUDY.md for the region map and the reasoning behind it.
 */

#include <algorithm>
#include <cmath>

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "ED_screen.hh"

namespace blender {

float ED_ui_menu_scale()
{
  /* The preference stores the extra fraction rather than the multiplier -- 0.5 means half again
   * as large -- so that 0 reads as "off" and the slider cannot be set to a value that would draw
   * the menus smaller than the rest of the interface. The multiplier is what every caller wants,
   * so the conversion lives here and nowhere else. */
  return 1.0f + std::clamp(U.ui_scale_menu, 0.0f, 1.0f);
}

bool ED_region_uses_menu_scale(const ARegion *region, const ScrArea *area)
{
  if (region == nullptr) {
    return false;
  }

  /* Everything in a global area is chrome, whatever its region type says it is. The top bar's
   * tool settings row registers itself as RGN_TYPE_WINDOW but draws through
   * ED_region_header_layout() like any other header, and the status bar sits in the same case.
   * Left to the switch below it would be given a header's height and a content region's contents,
   * which is a tall bar with small buttons in it. */
  if (area != nullptr && ED_area_is_global(area)) {
    return true;
  }

  switch (region->regiontype) {
    /* Every editor header, plus the top bar and the status bar, which are headers of the global
     * SPACE_TOPBAR and SPACE_STATUSBAR areas. */
    case RGN_TYPE_HEADER:
    case RGN_TYPE_TOOL_HEADER:
    case RGN_TYPE_FOOTER:
    case RGN_TYPE_ASSET_SHELF_HEADER:
    /* The time scrub bar. Its height is derived from the footer, so leaving it out would give a
     * taller strip with the old small numbers in it. Safe to scale: the frame positions along it
     * come from the View2D mapping it shares with the editor below, not from the interface scale,
     * so only the text and the playhead box grow. */
    case RGN_TYPE_SCRUBBING:
    /* The Properties editor tab column and the Preferences navigation bar: icon buttons, and the
     * ones a finger reaches for most. */
    case RGN_TYPE_NAV_BAR:
    /* Tool bars, and the tool settings beside them. */
    case RGN_TYPE_TOOLS:
    case RGN_TYPE_TOOL_PROPS:
    /* The file browser's Open/Cancel bar. A button bar, and a touch target. */
    case RGN_TYPE_EXECUTE:
    /* Pull-downs, popovers, pie menus, search boxes, context menus and tooltips. */
    case RGN_TYPE_TEMPORARY:
      return true;

    /* Deliberately not scaled.
     *
     * RGN_TYPE_WINDOW is the thing being protected: the viewport, the node canvas, the outliner,
     * the Properties panel body, the timeline.
     *
     * RGN_TYPE_UI is the N sidebar. It reads as chrome but it is panel content -- sliders and
     * values -- and scaling it would take a third of the viewport width to do it.
     *
     * RGN_TYPE_CHANNELS, the dope sheet and NLA channel lists, are content: their rows have to
     * line up with the keyframes drawn beside them.
     *
     * RGN_TYPE_HUD, the redo panel, is left alone for now: it floats over the viewport and is as
     * much content as chrome. Worth revisiting on the device. */
    default:
      return false;
  }
}

/* -------------------------------------------------------------------- */
/** \name Scoped Menu Scale
 * \{ */

/**
 * Whether a scale is applied right now, so a nested guard can decline instead of compounding.
 *
 * It has to be tracked, because the scopes genuinely do nest: #ED_area_headersize() scales itself
 * so that all 21 of its callers get a header height that matches the header, and one of those
 * callers is the region rect pass, which is itself inside a guard for the vertical bars beside
 * it. Applying twice there would give a header half again as tall as the buttons in it.
 *
 * A plain static is enough because everything that sizes or draws the interface runs on the main
 * thread. If that ever stops being true this needs to become thread-local, not a lock.
 */
static bool g_menu_scale_active = false;

ScopedMenuScale::ScopedMenuScale(const float factor)
{
  this->apply(factor);
}

ScopedMenuScale::ScopedMenuScale(const ARegion *region, const ScrArea *area)
{
  this->apply(ED_region_uses_menu_scale(region, area) ? ED_ui_menu_scale() : 1.0f);
}

void ScopedMenuScale::apply(const float factor)
{
  if (factor == 1.0f || g_menu_scale_active) {
    return;
  }

  active_ = true;
  g_menu_scale_active = true;
  dpi_ = U.dpi;
  scale_factor_ = U.scale_factor;
  inv_scale_factor_ = U.inv_scale_factor;
  widget_unit_ = U.widget_unit;

  /* Multiplied, not assigned: this is what keeps the Resolution Scale preference in charge of the
   * result. The menus land at `ui_scale * ui_scale_menu`, and raising the preference still moves
   * them. Assigning would turn the two into independent controls, which is not what is wanted. */
  U.scale_factor *= factor;
  U.inv_scale_factor = 1.0f / U.scale_factor;
  U.dpi = int(roundf(U.scale_factor * 72.0f));

  /* Mirrors WM_window_dpi_set_userdef(). `U.pixelsize` is deliberately left where it is: it is
   * the line width, and a bigger button wants the same thin outline around it, not a thicker
   * one. Leaving it alone also keeps every separator drawn at `U.pixelsize` crisp. */
  const int pixelsize = int(U.pixelsize);
  U.widget_unit = int(roundf(18.0f * U.scale_factor)) + (2 * pixelsize);
}

void ScopedMenuScale::reset()
{
  if (!active_) {
    return;
  }
  active_ = false;
  g_menu_scale_active = false;
  U.dpi = dpi_;
  U.scale_factor = scale_factor_;
  U.inv_scale_factor = inv_scale_factor_;
  U.widget_unit = widget_unit_;
}

ScopedMenuScale::~ScopedMenuScale()
{
  this->reset();
}

/** \} */

}  // namespace blender
