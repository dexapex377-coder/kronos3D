/* SPDX-FileCopyrightText: 2007 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * This file contains the splash screen logic (the `WM_OT_splash` operator).
 *
 * - Loads the splash image.
 * - Displaying version information.
 * - Lists New Files (application templates).
 * - Lists Recent files.
 * - Links to web sites.
 */

#include <algorithm>
#include <cfloat>
#include <cstring>

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.hh"
#include "BLI_math_base_c.hh"
#include "BLI_path_utils.hh"
#include "BLI_utildefines.hh"

#include "BKE_appdir.hh"
#include "BKE_blender_version.h"
#include "BKE_context.hh"
#include "BKE_preferences.h"

#include "BLT_translation.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "ED_datafiles.h"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "wm.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Splash Screen
 * \{ */

static void wm_block_splash_close(bContext *C, ui::Block *block)
{
  wmWindow *win = CTX_wm_window(C);
  popup_block_close(C, win, block);
}

static void wm_block_splash_add_label(ui::Block *block, const char *label, int x, int y)
{
  if (!(label && label[0])) {
    return;
  }

  block_emboss_set(block, ui::EmbossType::None);

  ui::Button *but = uiDefBut(
      block, ui::ButtonType::Label, label, 0, y, x, UI_UNIT_Y, nullptr, 0, 0, std::nullopt);
  button_drawflag_disable(but, ui::BUT_TEXT_LEFT);
  button_drawflag_enable(but, ui::BUT_TEXT_RIGHT);

  /* Regardless of theme, this text should always be bright white. */
  uchar color[4] = {255, 255, 255, 255};
  button_color_set(but, color);

  block_emboss_set(block, ui::EmbossType::Emboss);
}

#ifndef WITH_HEADLESS
static void wm_block_splash_image_roundcorners_add(ImBuf *ibuf)
{
  uchar *rct = ibuf->byte_data_for_write();
  if (!rct) {
    return;
  }

  const bTheme *btheme = ui::theme::theme_get();
  const float roundness = btheme->tui.wcol_menu_back.roundness * UI_SCALE_FAC;
  const int size = roundness * 20;

  if (size < ibuf->x && size < ibuf->y) {
    /* Y-axis initial offset. */
    rct += 4 * (ibuf->y - size) * ibuf->x;

    for (int y = 0; y < size; y++) {
      for (int x = 0; x < size; x++, rct += 4) {
        const float pixel = 1.0 / size;
        const float u = pixel * x;
        const float v = pixel * y;
        const float distance = sqrt(u * u + v * v);

        /* Pointer offset to the alpha value of pixel. */
        /* NOTE: the left corner is flipped in the X-axis. */
        const int offset_l = 4 * (size - x - x - 1) + 3;
        const int offset_r = 4 * (ibuf->x - size) + 3;

        if (distance > 1.0) {
          rct[offset_l] = 0;
          rct[offset_r] = 0;
        }
        else {
          /* Create a single pixel wide transition for anti-aliasing.
           * Invert the distance and map its range [0, 1] to [0, pixel]. */
          const float fac = (1.0 - distance) * size;

          if (fac > 1.0) {
            continue;
          }

          const uchar alpha = unit_float_to_uchar_clamp(fac);
          rct[offset_l] = alpha;
          rct[offset_r] = alpha;
        }
      }

      /* X-axis offset to the next row. */
      rct += 4 * (ibuf->x - size);
    }
  }
}
#endif /* !WITH_HEADLESS */

static ImBuf *wm_block_splash_image(int width, int *r_height)
{
  ImBuf *ibuf = nullptr;
  int height = 0;
#ifndef WITH_HEADLESS
  if (U.app_template[0] != '\0') {
    char splash_filepath[FILE_MAX];
    char template_directory[FILE_MAX];
    if (BKE_appdir_app_template_id_search(
            U.app_template, template_directory, sizeof(template_directory)))
    {
      BLI_path_join(splash_filepath, sizeof(splash_filepath), template_directory, "splash.png");
      ibuf = IMB_load_image_from_filepath(splash_filepath, ImBufFlags::ByteData);
    }
  }

  if (ibuf == nullptr) {
    const char *custom_splash_path = BLI_getenv("BLENDER_CUSTOM_SPLASH");
    if (custom_splash_path) {
      ibuf = IMB_load_image_from_filepath(custom_splash_path, ImBufFlags::ByteData);
    }
  }

  if (ibuf == nullptr) {
    const uchar *splash_data = reinterpret_cast<const uchar *>(datatoc_splash_png);
    size_t splash_data_size = datatoc_splash_png_size;
    ibuf = IMB_load_image_from_memory(
        splash_data, splash_data_size, ImBufFlags::ByteData, "<splash screen>");
  }

  if (ibuf) {
    ibuf->color_mode = ImColorMode::RGBA; /* The image might not have an alpha channel. */
    height = (width * ibuf->y) / ibuf->x;
    if (width != ibuf->x || height != ibuf->y) {
      IMB_scale(ibuf, width, height, IMBScaleFilter::Box, false);
    }

    wm_block_splash_image_roundcorners_add(ibuf);
    IMB_premultiply_alpha(ibuf);
  }

#else
  UNUSED_VARS(width);
#endif
  *r_height = height;
  return ibuf;
}

static ImBuf *wm_block_splash_banner_image(int *r_width,
                                           int *r_height,
                                           int max_width,
                                           int max_height)
{
  ImBuf *ibuf = nullptr;
  int height = 0;
  int width = max_width;
#ifndef WITH_HEADLESS

  const char *custom_splash_path = BLI_getenv("BLENDER_CUSTOM_SPLASH_BANNER");
  if (custom_splash_path) {
    ibuf = IMB_load_image_from_filepath(custom_splash_path, ImBufFlags::ByteData);
  }

  if (!ibuf) {
    return nullptr;
  }

  ibuf->color_mode = ImColorMode::RGBA; /* The image might not have an alpha channel. */

  width = ibuf->x;
  height = ibuf->y;
  if (width > 0 && height > 0 && (width > max_width || height > max_height)) {
    const float splash_ratio = max_width / float(max_height);
    const float banner_ratio = ibuf->x / float(ibuf->y);

    if (banner_ratio > splash_ratio) {
      /* The banner is wider than the splash image. */
      width = max_width;
      height = max_width / banner_ratio;
    }
    else if (banner_ratio < splash_ratio) {
      /* The banner is taller than the splash image. */
      height = max_height;
      width = max_height * banner_ratio;
    }
    else {
      width = max_width;
      height = max_height;
    }
    if (width != ibuf->x || height != ibuf->y) {
      IMB_scale(ibuf, width, height, IMBScaleFilter::Box, false);
    }
  }

  IMB_premultiply_alpha(ibuf);

#else
  UNUSED_VARS(max_height);
#endif
  *r_height = height;
  *r_width = width;
  return ibuf;
}

/**
 * Close the splash when opening a file-selector.
 */
static void wm_block_splash_close_on_fileselect(bContext *C, void *arg1, void * /*arg2*/)
{
  wmWindow *win = CTX_wm_window(C);
  if (!win) {
    return;
  }

  /* Check for the event as this will run before the new window/area has been created. */
  bool has_fileselect = false;
  for (const wmEvent &event : win->runtime->event_queue) {
    if (event.type == EVT_FILESELECT) {
      has_fileselect = true;
      break;
    }
  }

  if (has_fileselect) {
    wm_block_splash_close(C, static_cast<ui::Block *>(arg1));
  }
}

#if defined(__APPLE__)
/* Check if Blender is running under Rosetta for the purpose of displaying a splash screen warning.
 * From Apple's WWDC 2020 Session - Explore the new system architecture of Apple Silicon Macs.
 * Time code: 14:31 - https://developer.apple.com/videos/play/wwdc2020/10686/ */

#  include <sys/sysctl.h>

static int is_using_macos_rosetta()
{
  int ret = 0;
  size_t size = sizeof(ret);

  if (sysctlbyname("sysctl.proc_translated", &ret, &size, nullptr, 0) != -1) {
    return ret;
  }
  /* If "sysctl.proc_translated" is not present then must be native. */
  if (errno == ENOENT) {
    return 0;
  }
  return -1;
}
#endif /* __APPLE__ */

/**
 * Touch: how wide the splash and the About box are allowed to be, in pixels.
 *
 * Blender asks for a box 45 em wide and clamps it to a fraction of the window. On a desktop the
 * clamp never bites; on a phone it always did, and that is the whole defect. The clamp moved the
 * box and left the text where it was: 11 pt at UI_SCALE_FAC 1.833 wants 907 px of width, the
 * clamp gave it 756, and the result was a splash whose type was a sixth too large for the space
 * it had -- the rows crowding each other, nothing like the desktop the design came from.
 *
 * Measured on a Galaxy S24 Ultra, 1080x2243 upright. Landscape never clamped: 0.7 of 2244 is
 * 1570, well past what the box asks for, which is why only the upright splash looked wrong.
 *
 * The fraction is higher than upstream's 0.7 on purpose. A phone held upright has width to spare
 * beside a 45 em box and nothing else to spend it on, so the box is allowed to grow into it
 * rather than the type being shrunk to fit a narrower one, and the margins are still wide enough
 * that the artwork does not run to the edges. #wm_splash_fit_factor() handles the case where even
 * this is not enough.
 */
static float wm_splash_max_width(const wmWindow *win)
{
  if (win == nullptr) {
    return FLT_MAX;
  }
  return WM_window_native_pixel_x(win) * 0.86f;
}

/**
 * Touch: the scale the splash and the About box are drawn at, so the box and its contents agree.
 *
 * Returns 1.0 when the box fits as designed, which is the desktop case and the landscape phone
 * case. When it does not fit, the contents are scaled down by the same fraction the box is,
 * rather than the box alone being squeezed -- which is what produced the cramped upright splash
 * this exists to fix. Everything inside is derived from UI_SCALE_FAC, so scaling that scales the
 * type, the icons and the row spacing together and the proportions are the ones upstream drew.
 *
 * The height is bounded too, by the same factor, so a short window cannot push the artwork off
 * the top and bottom. The block's height is not known until it has been laid out, so it is
 * estimated as a multiple of its width: #SPLASH_HEIGHT_PER_WIDTH, which is the artwork's aspect
 * plus the rows beneath it. Estimating high costs nothing here -- a splash slightly smaller than
 * it had to be -- while estimating low is the bug being prevented.
 */
static float wm_splash_fit_factor(const bContext *C)
{
  const wmWindow *win = CTX_wm_window(C);
  if (win == nullptr) {
    return 1.0f;
  }

  const uiStyle *style = ui::style_get_dpi();
  const float natural_width = style->widget.points * 45 * UI_SCALE_FAC;
  if (natural_width <= 0.0f) {
    return 1.0f;
  }

  /* The artwork is a little over half as tall as it is wide, and the rows beneath it come to
   * about two thirds of a width more now they are drawn at the menu pitch.
   *
   * Measured from the drawn block rather than derived, because the block is not known until it
   * has been laid out: 1.02 before the rows were raised, then 1.20 upright and 1.26 turned. The
   * two disagree by more than rounding -- the rows have minimum heights that do not scale all the
   * way down -- so the constant is the larger of them with room on top. Estimating high costs a
   * splash slightly smaller than it had to be; estimating low is the artwork running off the top
   * of a landscape phone, which is the whole thing being prevented. */
  constexpr float SPLASH_HEIGHT_PER_WIDTH = 1.32f;

  const float width_limit = wm_splash_max_width(win);
  const float height_limit = WM_window_native_pixel_y(win) * 0.95f / SPLASH_HEIGHT_PER_WIDTH;

  return std::min(1.0f, std::min(width_limit, height_limit) / natural_width);
}

static ui::Block *wm_block_splash_create(bContext *C, ARegion *region, void * /*arg*/)
{
  /* Touch: the guard comes first, and every size below is read inside it. The factor itself has
   * to be computed before it, from the unscaled values -- it is the answer to "by how much is
   * this too big", which cannot be asked once the answer has been applied. */
  const ScopedMenuScale splash_scale(wm_splash_fit_factor(C));

  const uiStyle *style = ui::style_get_dpi();

  ui::Block *block = block_begin(C, region, "splash", ui::EmbossType::Emboss);

  /* Note on #BLOCK_NO_WIN_CLIP, the window size is not always synchronized
   * with the OS when the splash shows, window clipping in this case gives
   * ugly results and clipping the splash isn't useful anyway, just disable it #32938. */
  block_flag_enable(block, ui::BLOCK_LOOP | ui::BLOCK_KEEP_OPEN | ui::BLOCK_NO_WIN_CLIP);
  block_theme_style_set(block, ui::BLOCK_THEME_STYLE_POPUP);

  int splash_width = style->widget.points * 45 * UI_SCALE_FAC;
  CLAMP_MAX(splash_width, wm_splash_max_width(CTX_wm_window(C)));
  int splash_height;

  /* Would be nice to support caching this, so it only has to be re-read (and likely resized) on
   * first draw or if the image changed. */
  ImBuf *ibuf = wm_block_splash_image(splash_width, &splash_height);
  /* This should never happen, if it does - don't crash. */
  if (ibuf) [[likely]] {
    ui::Button *but = uiDefButImage(
        block, ibuf, 0, 0.5f * U.widget_unit, splash_width, splash_height, nullptr);

    button_func_set(but, [block](bContext &C) { wm_block_splash_close(&C, block); });

    wm_block_splash_add_label(block,
                              BKE_blender_version_string(),
                              splash_width - 8.0 * UI_SCALE_FAC,
                              splash_height - 13.0 * UI_SCALE_FAC);
  }

  /* Banner image passed through the environment, to overlay on the splash and
   * indicate a custom Blender version. Transparency can be used. To replace the
   * full splash screen, see BLENDER_CUSTOM_SPLASH. */
  int banner_width = 0;
  int banner_height = 0;
  ImBuf *bannerbuf = wm_block_splash_banner_image(
      &banner_width, &banner_height, splash_width, splash_height);
  if (bannerbuf) {
    ui::Button *banner_but = uiDefButImage(
        block, bannerbuf, 0, 0.5f * U.widget_unit, banner_width, banner_height, nullptr);

    button_func_set(banner_but, [block](bContext &C) { wm_block_splash_close(&C, block); });
  }

  const int layout_margin_x = UI_SCALE_FAC * 26;
  ui::Layout &layout = ui::block_layout(block,
                                        ui::LayoutDirection::Vertical,
                                        ui::LayoutType::Panel,
                                        layout_margin_x,
                                        0,
                                        splash_width - (layout_margin_x * 2),
                                        UI_SCALE_FAC * 110,
                                        0,
                                        style);

  /* Touch: the rows, at the pitch the rest of the interface uses.
   *
   * The block itself stays out of the menu scale -- see wm_splash_invoke() -- because it is a
   * fixed-proportion image and scaling it only pushes the artwork off the screen. Its list is not
   * artwork though, and left at 1.0 it was the one list in the program drawn tighter than every
   * menu beside it: measured on the same screen, 40 px per row here against 54 in the top bar
   * menu, which is what reads as no padding.
   *
   * scale_y rather than a scale guard, and that is the point of doing it this way: it gives the
   * rows their height without touching the type, so the file names in the right-hand column are
   * no more truncated than they were. A guard would have scaled the text too, inside a box whose
   * width is already decided, and truncated them further. */
  layout.scale_y_set(ED_ui_menu_scale());

  MenuType *mt;

  /* Draw setup screen if no preferences have been saved yet. */
  if (!bke::preferences::exists()) {
    mt = WM_menutype_find("WM_MT_splash_quick_setup", true);

    /* The #BLOCK_QUICK_SETUP flag prevents the button text from being left-aligned,
     * as it is for all menus due to the #BLOCK_LOOP flag, see in #ui_def_but. */
    block_flag_enable(block, ui::BLOCK_QUICK_SETUP);
  }
  else {
    mt = WM_menutype_find("WM_MT_splash", true);
  }

  block_func_set(block, wm_block_splash_close_on_fileselect, block, nullptr);

  if (mt) {
    ui::menutype_draw(C, mt, &layout);
  }

/* Displays a warning if blender is being emulated via Rosetta (macOS) or XTA (Windows) */
#if defined(__APPLE__) || defined(_M_X64)
#  if defined(__APPLE__)
  if (is_using_macos_rosetta() > 0)
#  elif defined(_M_X64)
  const char *proc_id = BLI_getenv("PROCESSOR_IDENTIFIER");
  if (proc_id && strncmp(proc_id, "ARM", 3) == 0)
#  endif
  {
    layout.separator(2.0f, ui::LayoutSeparatorType::Line);

    ui::Layout &split = layout.split(0.725, true);
    ui::Layout &row1 = split.row(true);
    ui::Layout &row2 = split.row(true);

    row1.label(RPT_("Intel binary detected. Expect reduced performance."),
               ICON_STATUS_WARNING_FILLED);

    PointerRNA op_ptr = row2.op("WM_OT_url_open",
                                CTX_IFACE_(BLT_I18NCONTEXT_OPERATOR_DEFAULT, "Learn More"),
                                ICON_URL,
                                wm::OpCallContext::InvokeDefault,
                                UI_ITEM_NONE);
#  if defined(__APPLE__)
    RNA_string_set(
        &op_ptr,
        "url",
        "https://docs.blender.org/manual/en/latest/getting_started/installing/macos.html");
#  elif defined(_M_X64)
    RNA_string_set(
        &op_ptr,
        "url",
        "https://docs.blender.org/manual/en/latest/getting_started/installing/windows.html");
#  endif

    layout.separator();
  }
#endif

  block_bounds_set_centered(block, 0);

  return block;
}

static wmOperatorStatus wm_splash_invoke(bContext *C,
                                         wmOperator * /*op*/,
                                         const wmEvent * /*event*/)
{
  /* Touch: opted out of the menu scale. The splash is a fixed-proportion image whose width is
   * already clamped to the window but whose height is not, so extra scale does not make it easier
   * to read -- it pushes the artwork off the top and bottom of a landscape phone. The About box
   * is the same block laid out the same way. Neither is something a finger aims at. */
  ui::popup_block_invoke(C, wm_block_splash_create, nullptr, nullptr, nullptr, false);

  return OPERATOR_FINISHED;
}

void WM_OT_splash(wmOperatorType *ot)
{
  ot->name = "Splash Screen";
  ot->idname = "WM_OT_splash";
  ot->description = "Open the splash screen with release info";

  ot->invoke = wm_splash_invoke;
  ot->poll = WM_operator_winactive;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Splash Screen: About
 * \{ */

static ui::Block *wm_block_about_create(bContext *C, ARegion *region, void * /*arg*/)
{
  /* Touch: the same fit as the splash; see wm_splash_fit_factor(). This box carried no clamp at
   * all upstream, so on a narrow enough phone it ran off both sides. */
  const ScopedMenuScale about_scale(wm_splash_fit_factor(C));

  const uiStyle *style = ui::style_get_dpi();
  int dialog_width = style->widget.points * 42 * UI_SCALE_FAC;
  CLAMP_MAX(dialog_width, wm_splash_max_width(CTX_wm_window(C)));

  ui::Block *block = block_begin(C, region, "about", ui::EmbossType::Emboss);

  block_flag_enable(block, ui::BLOCK_KEEP_OPEN | ui::BLOCK_LOOP | ui::BLOCK_NO_WIN_CLIP);
  block_theme_style_set(block, ui::BLOCK_THEME_STYLE_POPUP);

  ui::Layout &layout = ui::block_layout(block,
                                        ui::LayoutDirection::Vertical,
                                        ui::LayoutType::Panel,
                                        0,
                                        0,
                                        dialog_width,
                                        0,
                                        0,
                                        style);

/* Blender logo. */
#ifndef WITH_HEADLESS
  constexpr bool show_color = false;
  const float size = 0.2f * dialog_width;

  ImBuf *ibuf = ui::svg_icon_bitmap(ICON_BLENDER_LOGO_LARGE, size, show_color);

  if (ibuf) {
    const bTheme *btheme = ui::theme::theme_get();
    const uchar *color = btheme->tui.wcol_menu_back.text_sel;

    /* The top margin. */
    layout.row(false).separator(0.2f);

    /* The logo image. */
    layout.row(false).alignment_set(ui::LayoutAlign::Left);
    uiDefButImage(block, ibuf, 0, U.widget_unit, ibuf->x, ibuf->y, show_color ? nullptr : color);

    /* Padding below the logo. */
    layout.row(false).separator(2.7f);
  }
#endif /* !WITH_HEADLESS */

  ui::Layout &col = layout.column(true);

  /* Touch: the same row pitch as the splash and every other menu; see wm_block_splash_create().
   *
   * On this column and not on the layout above it, which is the whole point. The logo is added
   * with uiDefButImage() while that layout is open, so it is a layout item like any other and
   * scale_y stretched it vertically -- a Blender logo squeezed narrow, which no scale should ever
   * produce. The splash gets away with scaling its outer layout only because its artwork is
   * created before the layout exists. Here the text and the buttons live in this column, added
   * after the logo, so this is where the rows can be raised without touching the artwork. */
  col.scale_y_set(ED_ui_menu_scale());

  uiItemL_ex(&col, IFACE_("Blender"), ICON_NONE, true, false);

  MenuType *mt = WM_menutype_find("WM_MT_splash_about", true);
  if (mt) {
    ui::menutype_draw(C, mt, &col);
  }

  block_bounds_set_centered(block, 22 * UI_SCALE_FAC);

  return block;
}

static wmOperatorStatus wm_splash_about_invoke(bContext *C,
                                               wmOperator * /*op*/,
                                               const wmEvent * /*event*/)
{
  /* Touch: not at the menu scale, for the same reason as the splash above. */
  ui::popup_block_invoke(C, wm_block_about_create, nullptr, nullptr, nullptr, false);

  return OPERATOR_FINISHED;
}

void WM_OT_splash_about(wmOperatorType *ot)
{
  ot->name = "About Blender";
  ot->idname = "WM_OT_splash_about";
  ot->description = "Open a window with information about Blender";

  ot->invoke = wm_splash_about_invoke;
  ot->poll = WM_operator_winactive;
}

/** \} */

}  // namespace blender
