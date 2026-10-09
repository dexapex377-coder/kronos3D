/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Search Box Region & Interaction
 */

#include "MEM_guardedalloc.h"

#include <climits>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include "DNA_userdef_types.h"

#include "BLI_listbase.hh"
#include "BLI_math_base_c.hh"
#include "BLI_rect.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.hh"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"

#include "UI_interface_icons.hh"
#include "UI_view2d.hh"

#include "BLT_translation.hh"

#include "ED_screen.hh"

#include "BLF_api.hh"

#include "GPU_state.hh"
#include "interface_intern.hh"
#include "interface_regions_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Search Box Creation
 * \{ */

struct SearchItems {
  int maxitem, totitem, maxstrlen;

  int offset, offset_i; /* offset for inserting in array */
  int more;             /* flag indicating there are more items */

  char **names;
  void **pointers;
  int *icons;
  int64_t *but_flags;
  uint8_t *name_prefix_offsets;

  /** Is there any item with an icon? */
  bool has_icon;

  AutoComplete *autocpl;
  void *active;
};

struct uiSearchboxData {
  rcti bbox;
  uiFontStyle fstyle;
  /** Region zoom level. */
  float zoom;
  SearchItems items;
  bool size_set;
  ARegion *butregion;
  ButtonSearch *search_but;
  /** index in items array */
  int active;
  /** when menu opened with enough space for this */
  bool noback;
  /** draw thumbnail previews, rather than list */
  bool preview;
  /** Use the #UI_SEP_CHAR char for splitting shortcuts (good for operators, bad for data). */
  bool use_shortcut_sep;
  int prv_rows, prv_cols;
  /**
   * Touch: how many result rows this box actually holds, which is not always #SEARCH_ITEMS.
   *
   * A phone can leave the box less room than ten rows want -- the on-screen keyboard takes the
   * bottom of the screen, and a window in Android's split screen takes half of what is left. The
   * row height is the box divided by this, so a fixed ten in a short box is ten squeezed rows
   * rather than a few readable ones. Everything else follows from #SearchItems::maxitem being set
   * to match: the gather stops there, so `totitem` never exceeds it.
   */
  int rows;
  /**
   * Row height and the band at each end, in region pixels, settled once by the layout.
   *
   * Kept rather than recomputed because #searchbox_butrect is called from both sides of the touch
   * menu scale: the layout and the draw hold it, the event handling does not. Both `UI_UNIT_Y` and
   * `UI_SEARCHBOX_TRIA_H` are scale-dependent, so working them out again at hit-test time measured
   * rows at less than half the height they were drawn at -- a tap landed three rows below the one
   * it was aimed at. Pixels settled once cannot disagree with themselves.
   */
  int row_h;
  int tria_h;
  /**
   * Touch: the press a drag belongs to, and how many rows it has stepped so far.
   *
   * A finger or a stylus scrolls this list by dragging it, which arrives as plain motion with a
   * button held -- the press and release themselves go to the button, not here. The release is
   * what resets the run, through #searchbox_drag_consume_release, so a new drag is told from the
   * continuation of the last one without ever needing to see a press.
   */
  /**
   * Where the last motion left the hand, or #INT_MIN between drags.
   *
   * Incremental on purpose. Measuring instead from the press, through `prev_press_xy`, put the
   * list a screenful away on the very first event: that field can still hold the *previous* press
   * when the first motion of a new one arrives, and a distance measured from there is applied all
   * at once. Following the movement between one motion event and the next cannot jump, whatever
   * the press says.
   */
  int drag_last_y;
  /** Movement not yet worth a row, and total travel, which is what tells a drag from a tap. */
  int drag_accum;
  int drag_travel;
  /** Whether the hand has moved far enough to mean it, whether or not the list could move. */
  bool drag_active;
  /**
   * Whether a button is down inside the list, which is the only state a drag can begin from.
   *
   * Set by the press and cleared by the release, both of which reach the button rather than this
   * region -- hence #searchbox_drag_press and #searchbox_drag_consume_release. Asking the event
   * instead, through `prev_press_type`, does not work: that field names the last press there ever
   * was, so after any click it keeps saying LEFTMOUSE, and a mouse merely crossing the list or a
   * stylus merely hovering over it scrolled as though it were being dragged.
   */
  bool drag_armed;
  /**
   * Show the active icon and text after the last instance of this string.
   * Used so we can show leading text to menu items less prominently (not related to 'use_sep').
   */
  const char *sep_string;

  /* Owned by ButtonSearch */
  void *search_arg;
  ButtonSearchListenFn search_listener;
};

#define SEARCH_ITEMS 10
/**
 * Touch: the fewest rows worth showing when the room is short.
 *
 * Below two the box stops being a list and becomes a single answer with no context, at which
 * point the search field alone would serve better. Two is also what keeps the arrows meaningful:
 * one row cannot be scrolled past.
 */
#define SEARCH_ROWS_MIN 2

/** How many whole rows fit in `height`, within the range worth drawing. */
static int searchbox_rows_for_height(const int height)
{
  const int rows = (height - 2 * UI_SEARCHBOX_TRIA_H) / UI_UNIT_Y;
  return std::clamp(rows, SEARCH_ROWS_MIN, SEARCH_ITEMS);
}


bool search_item_add(SearchItems *items,
                     const StringRef name,
                     void *poin,
                     int iconid,
                     const int64_t but_flag,
                     const uint8_t name_prefix_offset)
{
  /* hijack for autocomplete */
  if (items->autocpl) {
    autocomplete_update_name(items->autocpl, name.drop_prefix(name_prefix_offset));
    return true;
  }

  if (iconid) {
    items->has_icon = true;
  }

  /* hijack for finding active item */
  if (items->active) {
    if (poin == items->active) {
      items->offset_i = items->totitem;
    }
    items->totitem++;
    return true;
  }

  if (items->totitem >= items->maxitem) {
    items->more = 1;
    return false;
  }

  /* skip first items in list */
  if (items->offset_i > 0) {
    items->offset_i--;
    return true;
  }

  if (items->names) {
    name.copy_utf8_truncated(items->names[items->totitem], items->maxstrlen);
  }
  if (items->pointers) {
    items->pointers[items->totitem] = poin;
  }
  if (items->icons) {
    items->icons[items->totitem] = iconid;
  }

  if (name_prefix_offset != 0) {
    /* Lazy initialize, as this isn't used often. */
    if (items->name_prefix_offsets == nullptr) {
      items->name_prefix_offsets = MEM_new_array_zeroed<uint8_t>(items->maxitem, __func__);
    }
    items->name_prefix_offsets[items->totitem] = name_prefix_offset;
  }

  /* Limit flags that can be set so flags such as 'UI_SELECT' aren't accidentally set
   * which will cause problems, add others as needed. */
  BLI_assert((but_flag & ~(BUT_DISABLED | BUT_INACTIVE | BUT_REDALERT | BUT_HAS_SEP_CHAR)) == 0);
  if (items->but_flags) {
    items->but_flags[items->totitem] = but_flag;
  }

  items->totitem++;

  return true;
}

int searchbox_size_y()
{
  return SEARCH_ITEMS * UI_UNIT_Y + 2 * UI_SEARCHBOX_TRIA_H;
}

int searchbox_size_y_fit(const int height_max)
{
  return searchbox_rows_for_height(height_max) * UI_UNIT_Y + 2 * UI_SEARCHBOX_TRIA_H;
}

int searchbox_size_x()
{
  return 12 * UI_UNIT_X;
}

static int searchbox_size_x_from_items(const SearchItems &items)
{
  /* Compute the width of each item. */
  Array<int> item_widths(items.totitem);
  threading::parallel_for(item_widths.index_range(), 256, [&](const IndexRange range) {
    for (const int i : range) {
      const StringRefNull name = items.names[i];
      const int icon = items.icons ? items.icons[i] : ICON_NONE;
      const float text_width = BLF_width(BLF_default(), name.c_str(), name.size(), nullptr);
      const float icon_with_padding = icon == ICON_NONE ? 0.0f : UI_ICON_SIZE + UI_UNIT_X;
      const float padding = UI_UNIT_X;
      item_widths[i] = int(text_width + padding + icon_with_padding);
    }
  });

  /* Compute the final width of the search box. */
  int box_width = searchbox_size_x();
  for (const int width : item_widths) {
    box_width = std::max(box_width, width);
  }
  /* Avoid extremely wide boxes. */
  box_width = std::min(box_width, searchbox_size_x() * 5);
  return box_width;
}

int searchbox_size_x_guess(const bContext *C, const ButtonSearchUpdateFn update_fn, void *arg)
{
  SearchItems items{};
  /* Upper bound on the number of item names that are checked. */
  items.maxitem = 1000;
  items.maxstrlen = 256;

  /* Prepare name buffers. */
  Array<char> names_buffer(items.maxitem * items.maxstrlen);
  Array<char *> names(items.maxitem);
  Array<int> icons(items.maxitem);
  items.names = names.data();
  items.icons = icons.data();
  for (int i : IndexRange(items.maxitem)) {
    names[i] = names_buffer.data() + i * items.maxstrlen;
  }

  /* Gather the items shown in the search box. */
  update_fn(C, arg, "", &items, true);

  /* This is lazy-initialized in #search_item_add. */
  MEM_SAFE_DELETE(items.name_prefix_offsets);

  return searchbox_size_x_from_items(items);
}

int search_items_find_index(const SearchItems *items, const char *name)
{
  if (items->name_prefix_offsets != nullptr) {
    for (int i = 0; i < items->totitem; i++) {
      if (STREQ(name, items->names[i] + items->name_prefix_offsets[i])) {
        return i;
      }
    }
  }
  else {
    for (int i = 0; i < items->totitem; i++) {
      if (STREQ(name, items->names[i])) {
        return i;
      }
    }
  }
  return -1;
}

/* region is the search box itself */
static void searchbox_select(bContext *C, ARegion *region, Button *but, int step)
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);

  /* apply step */
  data->active += step;

  if (data->items.totitem == 0) {
    data->active = -1;
  }
  else if (data->active >= data->items.totitem) {
    if (data->items.more) {
      data->items.offset++;
      data->active = data->items.totitem - 1;
      searchbox_update(C, region, but, false);
    }
    else {
      data->active = data->items.totitem - 1;
    }
  }
  else if (data->active < 0) {
    if (data->items.offset) {
      data->items.offset--;
      data->active = 0;
      searchbox_update(C, region, but, false);
    }
    else {
      /* only let users step into an 'unset' state for unlink buttons */
      data->active = (but->flag & BUT_VALUE_CLEAR) ? -1 : 0;
    }
  }

  ED_region_tag_redraw(region);
}

static void searchbox_butrect(rcti *r_rect, uiSearchboxData *data, int itemnr)
{
  /* Touch: the settled pixels, not the constants they came from. This runs from both sides of the
   * menu scale -- the draw holds it, the hit test on a release does not -- and both `UI_UNIT_Y`
   * and `UI_SEARCHBOX_TRIA_H` move with it. See #uiSearchboxData::row_h. */
  const float tria_h = data->tria_h;

  /* thumbnail preview */
  if (data->preview) {
    const int butw = BLI_rcti_size_x(&data->bbox) / data->prv_cols;
    const int buth = (BLI_rcti_size_y(&data->bbox) - 2.0f * tria_h) / data->prv_rows;
    int row, col;

    *r_rect = data->bbox;

    col = itemnr % data->prv_cols;
    row = itemnr / data->prv_cols;

    r_rect->xmin += col * butw;
    r_rect->xmax = r_rect->xmin + butw;

    r_rect->ymax -= tria_h + row * buth;
    r_rect->ymin = r_rect->ymax - buth;
  }
  /* list view */
  else {
    const float buth = float(data->row_h);

    *r_rect = data->bbox;

    r_rect->xmin = data->bbox.xmin;
    r_rect->xmax = data->bbox.xmax;

    r_rect->ymax = data->bbox.ymax - tria_h - itemnr * buth;
    r_rect->ymin = r_rect->ymax - buth;
  }
}

int searchbox_find_index(ARegion *region, const char *name)
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);
  return search_items_find_index(&data->items, name);
}

bool searchbox_inside(ARegion *region, const int xy[2])
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);

  return BLI_rcti_isect_pt(&data->bbox, xy[0] - region->winrct.xmin, xy[1] - region->winrct.ymin);
}

bool searchbox_apply(Button *but, ARegion *region)
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);
  ButtonSearch *search_but = static_cast<ButtonSearch *>(but);

  BLI_assert(but->type == ButtonType::SearchMenu);

  search_but->item_active = nullptr;

  if (data->active != -1) {
    const char *name = data->items.names[data->active] +
                       /* Never include the prefix in the button. */
                       (data->items.name_prefix_offsets ?
                            data->items.name_prefix_offsets[data->active] :
                            0);

    const char *name_sep = data->use_shortcut_sep ? strrchr(name, UI_SEP_CHAR) : nullptr;

    /* Search button with dynamic string properties may have their own method of applying
     * the search results, so only copy the result if there is a proper space for it. */
    if (but->hardmax != 0) {
      BLI_strncpy(but->editstr, name, name_sep ? (name_sep - name) + 1 : data->items.maxstrlen);
    }

    search_but->item_active = data->items.pointers[data->active];
    MEM_SAFE_DELETE(search_but->item_active_str);
    search_but->item_active_str = BLI_strdup(data->items.names[data->active]);

    return true;
  }
  return false;
}

static ARegion *wm_searchbox_tooltip_init(
    bContext *C, ARegion *region, int * /*r_pass*/, double * /*pass_delay*/, bool *r_exit_on_event)
{
  *r_exit_on_event = true;

  for (Block &block : region->runtime->uiblocks) {
    for (Button &but : block.buttons()) {
      if (but.type != ButtonType::SearchMenu) {
        continue;
      }

      ButtonSearch *search_but = static_cast<ButtonSearch *>(&but);
      if (!search_but->item_tooltip_fn) {
        continue;
      }

      ARegion *searchbox_region = region_searchbox_region_get(region);
      uiSearchboxData *data = static_cast<uiSearchboxData *>(searchbox_region->regiondata);

      BLI_assert(data->items.pointers[data->active] == search_but->item_active);

      rcti rect;
      searchbox_butrect(&rect, data, data->active);

      return search_but->item_tooltip_fn(
          C, region, &rect, search_but->arg, search_but->item_active);
    }
  }
  return nullptr;
}

/**
 * Touch: scroll the list by dragging it, and say whether the drag took the event.
 *
 * The press and the release belong to the button -- they are what picks an item -- so this only
 * ever sees motion, and reads `prev_press_xy` to know where the finger took hold. A drag that has
 * not yet crossed a row is not a drag at all, which is what leaves a tap free to select.
 *
 * It moves the view and leaves the selection alone, which is the difference between this and the
 * wheel. #searchbox_select() steps the highlight and only shifts the list once that highlight has
 * run into an end, so driving a drag through it made the selection race down the list while the
 * list itself sat still and then lurched -- fast, and impossible to aim. Moving `items.offset` by
 * hand is one row of list per row of finger, which is what a drag is supposed to be.
 *
 * The ends are `offset` and `more`: the list never knows how many results there are in total, only
 * whether there is another one past the last it fetched.
 */
static bool searchbox_touch_scroll(
    bContext *C, ARegion *region, Button *but, uiSearchboxData *data, const wmEvent *event)
{
  /* Only ever from a button actually held down inside the list: see #drag_armed. A mouse crossing
   * the list and a stylus hovering over it both arrive as plain motion, and neither is a drag. */
  if (!data->drag_armed || data->preview) {
    return false;
  }

  /* First motion of a new hold. The release clears this back to the sentinel, which is a more
   * reliable mark than the press position: it always arrives, and it arrives exactly once. */
  if (data->drag_last_y == INT_MIN) {
    data->drag_last_y = event->xy[1];
    data->drag_accum = 0;
    data->drag_travel = 0;
    data->drag_active = false;
    return false;
  }

  const int delta = event->xy[1] - data->drag_last_y;
  data->drag_last_y = event->xy[1];
  data->drag_travel += abs(delta);

  rcti row;
  searchbox_butrect(&row, data, 0);
  const int row_h = BLI_rcti_size_y(&row);
  if (row_h <= 0) {
    return false;
  }

  if (!data->drag_active) {
    /* A tap has to reach the item the way it always did, so nothing moves until the hand has
     * travelled far enough to mean it. Counted from what has actually been seen rather than from
     * the press, so a stale press position cannot start a drag the user never made.
     *
     * Half a row rather than the drag threshold, which is a few pixels: a finger always wobbles
     * on the way down, and a wobble that counted as a drag ate the release -- the tap then chose
     * nothing at all, which is the worst of the three things a tap can do. */
    if (data->drag_travel < std::max(WM_event_drag_threshold(event), row_h / 2)) {
      return false;
    }
    data->drag_active = true;
    data->drag_accum = 0;
  }

  /* The content follows the finger, the same way and the same sign as the pull-down menus:
   * window coordinates put y upwards, so dragging up raises the offset and brings the later
   * results into view, as though the list itself were being pulled. */
  data->drag_accum += delta;
  while (abs(data->drag_accum) >= row_h) {
    const int direction = (data->drag_accum > 0) ? 1 : -1;
    if (direction > 0) {
      if (!data->items.more) {
        /* The last result is showing. Draining the movement rather than keeping it means the
         * hand does not have to give back everything it pushed past the end. */
        data->drag_accum = 0;
        break;
      }
      data->items.offset++;
    }
    else {
      if (data->items.offset == 0) {
        data->drag_accum = 0;
        break;
      }
      data->items.offset--;
    }
    data->drag_accum -= direction * row_h;
    searchbox_update(C, region, but, false);
  }

  /* The window of results moved under it, so keep the highlight on something that exists. */
  if (data->items.totitem == 0) {
    data->active = -1;
  }
  else {
    data->active = std::clamp(data->active, 0, data->items.totitem - 1);
  }

  ED_region_tag_redraw(region);
  return true;
}

bool searchbox_select_at(ARegion *region, const int xy[2])
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);
  if (data == nullptr || data->preview) {
    return false;
  }
  const int items_shown = std::min(data->items.totitem, data->rows);
  for (int a = 0; a < items_shown; a++) {
    rcti rect;
    searchbox_butrect(&rect, data, a);
    if (BLI_rcti_isect_pt(&rect, xy[0] - region->winrct.xmin, xy[1] - region->winrct.ymin)) {
      data->active = a;
      return true;
    }
  }
  return false;
}

void searchbox_drag_press(ARegion *region)
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);
  if (data == nullptr) {
    return;
  }
  data->drag_armed = true;
  data->drag_active = false;
  data->drag_accum = 0;
  data->drag_travel = 0;
  data->drag_last_y = INT_MIN;
}

bool searchbox_drag_consume_release(ARegion *region)
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);
  if (data == nullptr) {
    return false;
  }
  const bool dragged = data->drag_active;
  data->drag_armed = false;
  data->drag_active = false;
  data->drag_accum = 0;
  data->drag_travel = 0;
  data->drag_last_y = INT_MIN;
  return dragged;
}

bool searchbox_event(
    bContext *C, ARegion *region, Button *but, ARegion *butregion, const wmEvent *event)
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);
  ButtonSearch *search_but = static_cast<ButtonSearch *>(but);
  int type = event->type, val = event->val;
  bool handled = false;
  bool tooltip_timer_started = false;

  BLI_assert(but->type == ButtonType::SearchMenu);

  if (type == MOUSEPAN) {
    pan_to_scroll(event, &type, &val);
  }

  switch (type) {
    case WHEELUPMOUSE:
    case EVT_UPARROWKEY:
      searchbox_select(C, region, but, -1);
      handled = true;
      break;
    case WHEELDOWNMOUSE:
    case EVT_DOWNARROWKEY:
      searchbox_select(C, region, but, 1);
      handled = true;
      break;
    case RIGHTMOUSE:
      if (val) {
        if (search_but->item_context_menu_fn) {
          if (data->active != -1) {
            /* Check the cursor is over the active element
             * (a little confusing if this isn't the case, although it does work). */
            rcti rect;
            searchbox_butrect(&rect, data, data->active);
            if (BLI_rcti_isect_pt(
                    &rect, event->xy[0] - region->winrct.xmin, event->xy[1] - region->winrct.ymin))
            {

              void *active = data->items.pointers[data->active];
              if (search_but->item_context_menu_fn(C, search_but->arg, active, event)) {
                handled = true;
              }
            }
          }
        }
      }
      break;
    case MOUSEMOVE: {
      /* Touch: before the hover-select below, which would otherwise fight a drag for the same
       * motion -- the finger would scroll the list and then immediately select whatever slid
       * under it. */
      if (searchbox_touch_scroll(C, region, but, data, event)) {
        handled = true;
        break;
      }

      /* Ignore the mouse event, in case the search popup is created underneath the cursor.
       * We always want the first result to be selected by default. See: #144168 */
      if (event->xy[0] == event->prev_xy[0] && event->xy[1] == event->prev_xy[1]) {
        searchbox_select(C, region, but, 0);
        handled = true;
        break;
      }

      bool is_inside = false;

      if (BLI_rcti_isect_pt(&region->winrct, event->xy[0], event->xy[1])) {
        rcti rect;
        int a;

        for (a = 0; a < data->items.totitem; a++) {
          searchbox_butrect(&rect, data, a);
          if (BLI_rcti_isect_pt(
                  &rect, event->xy[0] - region->winrct.xmin, event->xy[1] - region->winrct.ymin))
          {
            is_inside = true;
            if (data->active != a) {
              data->active = a;
              searchbox_select(C, region, but, 0);
              handled = true;
              break;
            }
          }
        }
      }

      if (U.flag & USER_TOOLTIPS) {
        if (is_inside) {
          if (data->active != -1) {
            ScrArea *area = CTX_wm_area(C);
            search_but->item_active = data->items.pointers[data->active];
            WM_tooltip_timer_init(C, CTX_wm_window(C), area, butregion, wm_searchbox_tooltip_init);
            tooltip_timer_started = true;
          }
        }
      }

      break;
    }
  }

  if (handled && (tooltip_timer_started == false)) {
    wmWindow *win = CTX_wm_window(C);
    WM_tooltip_clear(C, win);
  }

  return handled;
}

/** Wrap #ButSearchUpdateFn callback. */
static void searchbox_update_fn(bContext *C,
                                ButtonSearch *but,
                                const char *str,
                                SearchItems *items)
{
  /* While the button is in text editing mode (searchbox open), remove tooltips on every update. */
  if (but->editstr) {
    wmWindow *win = CTX_wm_window(C);
    WM_tooltip_clear(C, win);
  }
  const bool is_first_search = !but->changed;
  but->items_update_fn(C, but->arg, str, items, is_first_search);
}

void searchbox_update(bContext *C, ARegion *region, Button *but, const bool reset)
{
  ButtonSearch *search_but = static_cast<ButtonSearch *>(but);
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);

  BLI_assert(but->type == ButtonType::SearchMenu);

  /* reset vars */
  data->items.totitem = 0;
  data->items.more = 0;
  if (!reset) {
    data->items.offset_i = data->items.offset;
  }
  else {
    data->items.offset_i = data->items.offset = 0;
    data->active = -1;

    /* On init, find and center active item. */
    const bool is_first_search = !but->changed;
    if (is_first_search && search_but->items_update_fn && search_but->item_active) {
      data->items.active = search_but->item_active;
      searchbox_update_fn(C, search_but, but->editstr, &data->items);
      data->items.active = nullptr;

      /* found active item, calculate real offset by centering it */
      if (data->items.totitem) {
        /* first case, begin of list */
        if (data->items.offset_i < data->items.maxitem) {
          data->active = data->items.offset_i;
          data->items.offset_i = 0;
        }
        else {
          /* second case, end of list */
          if (data->items.totitem - data->items.offset_i <= data->items.maxitem) {
            data->active = data->items.offset_i - data->items.totitem + data->items.maxitem;
            data->items.offset_i = data->items.totitem - data->items.maxitem;
          }
          else {
            /* center active item */
            data->items.offset_i -= data->items.maxitem / 2;
            data->active = data->items.maxitem / 2;
          }
        }
      }
      data->items.offset = data->items.offset_i;
      data->items.totitem = 0;
    }
  }

  /* callback */
  if (search_but->items_update_fn) {
    searchbox_update_fn(C, search_but, but->editstr, &data->items);
  }

  /* handle case where editstr is equal to one of items */
  if (reset && data->active == -1) {
    for (int a = 0; a < data->items.totitem; a++) {
      const char *name = data->items.names[a] +
                         /* Never include the prefix in the button. */
                         (data->items.name_prefix_offsets ? data->items.name_prefix_offsets[a] :
                                                            0);
      const char *name_sep = data->use_shortcut_sep ? strrchr(name, UI_SEP_CHAR) : nullptr;
      if (STREQLEN(but->editstr, name, name_sep ? (name_sep - name) : data->items.maxstrlen)) {
        data->active = a;
        break;
      }
    }
    if (data->items.totitem == 1 && but->editstr[0]) {
      data->active = 0;
    }
  }

  /* Nothing active, check at mouse location. */
  if (data->active == -1) {
    wmWindow *win = CTX_wm_window(C);
    if (win && win->runtime && win->runtime->eventstate) {
      const int cursor_x = win->runtime->eventstate->xy[0];
      const int cursor_y = win->runtime->eventstate->xy[1];
      if (BLI_rcti_isect_pt(&region->winrct, cursor_x, cursor_y)) {
        rcti rect;
        for (int a = 0; a < data->items.totitem; a++) {
          searchbox_butrect(&rect, data, a);
          if (BLI_rcti_isect_pt(
                  &rect, cursor_x - region->winrct.xmin, cursor_y - region->winrct.ymin))
          {
            data->active = a;
            break;
          }
        }
      }
    }
  }

  /* validate selected item */
  searchbox_select(C, region, but, 0);

  ED_region_tag_redraw(region);
}

int searchbox_autocomplete(bContext *C, ARegion *region, Button *but, char *str)
{
  ButtonSearch *search_but = static_cast<ButtonSearch *>(but);
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);
  int match = AUTOCOMPLETE_NO_MATCH;

  BLI_assert(but->type == ButtonType::SearchMenu);

  if (str[0]) {
    int maxncpy = button_string_get_maxncpy(but);
    if (maxncpy == 0) {
      /* The string length is dynamic, just assume a reasonable length. */
      maxncpy = strlen(str) + 1024;
    }
    data->items.autocpl = autocomplete_begin(str, maxncpy);

    searchbox_update_fn(C, search_but, but->editstr, &data->items);

    match = autocomplete_end(data->items.autocpl, str);
    data->items.autocpl = nullptr;
  }

  return match;
}

/**
 * Draws a downwards facing triangle.
 * \param rect: Rectangle under which the triangle icon is drawn. Usually from the last result item
 *              that can be displayed.
 */
static void searchbox_draw_clip_tri_down(rcti *rect, const float zoom)
{
  const float x = BLI_rcti_cent_x(rect) - (0.5f * zoom * UI_ICON_SIZE);
  const float y = rect->ymin - (0.5f * zoom * (UI_SEARCHBOX_TRIA_H - UI_ICON_SIZE) - U.pixelsize) -
                  zoom * UI_ICON_SIZE;
  const float aspect = U.inv_scale_factor / zoom;
  GPU_blend(GPU_BLEND_ALPHA);
  icon_draw_ex(x, y, ICON_TRIA_DOWN, aspect, 1.0f, 0.0f, nullptr, false, UI_NO_ICON_OVERLAY_TEXT);
  GPU_blend(GPU_BLEND_NONE);
}

/**
 * Draws an upwards facing triangle.
 * \param rect: Rectangle above which the triangle icon is drawn. Usually from the first result
 *              item that can be displayed.
 */
static void searchbox_draw_clip_tri_up(rcti *rect, const float zoom)
{
  const float x = BLI_rcti_cent_x(rect) - (0.5f * zoom * UI_ICON_SIZE);
  const float y = rect->ymax + (0.5f * zoom * (UI_SEARCHBOX_TRIA_H - UI_ICON_SIZE) - U.pixelsize);
  const float aspect = U.inv_scale_factor / zoom;
  GPU_blend(GPU_BLEND_ALPHA);
  icon_draw_ex(x, y, ICON_TRIA_UP, aspect, 1.0f, 0.0f, nullptr, false, UI_NO_ICON_OVERLAY_TEXT);
  GPU_blend(GPU_BLEND_NONE);
}

static void searchbox_region_draw_fn(const bContext *C, ARegion *region)
{
  /* Touch: and again here, so the text and the icons in a row grow with the row. The layout above
   * sizes the box; this sizes what goes in it, and the two have to agree. */
  const ScopedMenuScale menu_scale(ED_ui_menu_scale());

  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);

  /* pixel space */
  wmOrtho2_region_pixelspace(region);

  if (data->noback == false) {
    draw_widget_menu_back(&data->bbox, true);
  }

  /* draw text */
  if (data->items.totitem) {
    rcti rect;

    if (data->preview) {
      /* draw items */
      for (int a = 0; a < data->items.totitem; a++) {
        const int64_t but_flag = ((a == data->active) ? UI_HOVER : 0) | data->items.but_flags[a];

        /* ensure icon is up-to-date */
        icon_ensure_deferred(C, data->items.icons[a], data->preview);

        searchbox_butrect(&rect, data, a);

        /* widget itself */
        draw_preview_item(&data->fstyle,
                          &rect,
                          data->zoom,
                          data->items.names[a],
                          data->items.icons[a],
                          but_flag,
                          UI_STYLE_TEXT_LEFT);
      }

      /* indicate more */
      if (data->items.more || data->items.offset) {
        rcti rect_first_item;
        searchbox_butrect(&rect_first_item, data, 0);
        rcti rect_max_item;
        searchbox_butrect(&rect_max_item, data, data->items.maxitem - 1);

        if (data->items.offset) {
          /* The first item is in the top left corner. Adjust width so the icon is centered. */
          rect_first_item.xmax = rect_max_item.xmax;
          searchbox_draw_clip_tri_up(&rect_first_item, data->zoom);
        }

        if (data->items.more) {
          /* The last item is in the bottom right corner. Adjust width so the icon is centered. */
          rect_max_item.xmin = rect_first_item.xmin;
          searchbox_draw_clip_tri_down(&rect_max_item, data->zoom);
        }
      }
    }
    else {
      const int search_sep_len = data->sep_string ? strlen(data->sep_string) : 0;
      /* Touch: never more rows than the box holds, whatever the gather came back with. Capping
       * maxitem should already have seen to it; this is the line that makes drawing outside the
       * box impossible rather than merely unlikely. */
      const int items_drawn = std::min(data->items.totitem, data->rows);
      /* draw items */
      for (int a = 0; a < items_drawn; a++) {
        const int64_t but_flag = ((a == data->active) ? UI_HOVER : 0) | data->items.but_flags[a];
        const char *name = data->items.names[a];
        int icon = data->items.icons[a];
        char *name_sep_test = nullptr;

        MenuItemSeparatorType separator_type = UI_MENU_ITEM_SEPARATOR_NONE;
        if (data->use_shortcut_sep) {
          separator_type = UI_MENU_ITEM_SEPARATOR_SHORTCUT;
        }
        /* Only set for displaying additional hint (e.g. library name of a linked data-block). */
        else if (but_flag & BUT_HAS_SEP_CHAR) {
          separator_type = UI_MENU_ITEM_SEPARATOR_HINT;
        }

        searchbox_butrect(&rect, data, a);

        /* widget itself */
        if ((search_sep_len == 0) ||
            !(name_sep_test = strstr(data->items.names[a], data->sep_string)))
        {
          if (!icon && data->items.has_icon) {
            /* If there is any icon item, make sure all items line up. */
            icon = ICON_BLANK1;
          }

          /* Simple menu item. */
          draw_menu_item(&data->fstyle,
                         &rect,
                         &rect,
                         data->zoom,
                         data->noback,
                         name,
                         icon,
                         but_flag,
                         separator_type,
                         nullptr);
        }
        else {
          /* Split menu item, faded text before the separator. */
          char *name_sep = nullptr;
          do {
            name_sep = name_sep_test;
            name_sep_test = strstr(name_sep + search_sep_len, data->sep_string);
          } while (name_sep_test != nullptr);

          name_sep += search_sep_len;
          const char name_sep_prev = *name_sep;
          *name_sep = '\0';
          int name_width = 0;
          draw_menu_item(&data->fstyle,
                         &rect,
                         &rect,
                         data->zoom,
                         data->noback,
                         name,
                         ICON_NONE,
                         but_flag | BUT_INACTIVE,
                         UI_MENU_ITEM_SEPARATOR_NONE,
                         &name_width);
          *name_sep = name_sep_prev;
          rect.xmin += name_width;
          rect.xmin += UI_UNIT_X / 4;

          if (icon == ICON_BLANK1) {
            icon = ICON_NONE;
          }
          if (icon != ICON_NONE) {
            rect.xmin += UI_UNIT_X / 8;
          }

          /* The previous menu item draws the active selection. */
          draw_menu_item(&data->fstyle,
                         &rect,
                         nullptr,
                         data->zoom,
                         data->noback,
                         name_sep,
                         icon,
                         but_flag,
                         separator_type,
                         nullptr);
        }
      }
      /* indicate more */
      if (data->items.more) {
        searchbox_butrect(&rect, data, data->items.maxitem - 1);
        searchbox_draw_clip_tri_down(&rect, data->zoom);
      }
      if (data->items.offset) {
        searchbox_butrect(&rect, data, 0);
        searchbox_draw_clip_tri_up(&rect, data->zoom);
      }
    }
  }
  else {
    rcti rect;
    searchbox_butrect(&rect, data, 0);
    draw_menu_item(&data->fstyle,
                   &rect,
                   &rect,
                   data->zoom,
                   data->noback,
                   IFACE_("No results found"),
                   0,
                   0,
                   UI_MENU_ITEM_SEPARATOR_NONE,
                   nullptr);
  }
}

static void searchbox_region_free_fn(ARegion *region)
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);

  /* free search data */
  for (int a = 0; a < data->items.maxitem; a++) {
    MEM_delete(data->items.names[a]);
  }
  MEM_delete(data->items.names);
  MEM_delete(data->items.pointers);
  MEM_delete(data->items.icons);
  MEM_delete(data->items.but_flags);

  if (data->items.name_prefix_offsets != nullptr) {
    MEM_delete(data->items.name_prefix_offsets);
  }

  MEM_delete(data);
  region->regiondata = nullptr;
}

static void searchbox_region_listen_fn(const wmRegionListenerParams *params)
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(params->region->regiondata);
  if (data->search_listener) {
    data->search_listener(params, data->search_arg);
  }
}

static void searchbox_region_layout_fn(const bContext *C, ARegion *region)
{
  /* Touch: a search box is menu chrome, and it is the one piece of it that is not built inside
   * popup_block_refresh().
   *
   * It is a temporary region of its own, laid out and drawn by these two callbacks with no scale
   * applied, so #searchbox_size_y() -- which is SEARCH_ITEMS * UI_UNIT_Y -- measured the unscaled
   * widget unit while the popup holding the search field measured the scaled one. The row count is
   * fixed, so the whole box came out at two thirds the height its rows wanted: the same squeeze,
   * and the same ratio, as the button context menus.
   *
   * Held across the whole function rather than around the size call, because the position is
   * computed from the same units. #ScopedMenuScale is re-entrant: a caller that already applied
   * the scale makes this a no-op rather than compounding it. */
  const ScopedMenuScale menu_scale(ED_ui_menu_scale());

  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);

  if (data->size_set) {
    /* Already set. */
    return;
  }

  ButtonSearch *but = data->search_but;
  ARegion *butregion = data->butregion;
  const int margin = UI_POPUP_MARGIN;
  wmWindow *win = CTX_wm_window(C);

  /* compute position */
  if (but->block->flag & BLOCK_SEARCH_MENU) {
    /* this case is search menu inside other menu */
    /* we copy region size */

    region->winrct = butregion->winrct;

    /* Align menu items with the search button. */
    const float zoom = data->zoom;
    const int padding = zoom * UI_SEARCHBOX_BOUNDS - (data->preview ? 0 : U.pixelsize);
    const int search_but_h = BLI_rctf_size_y(&but->rect) + zoom * UI_SEARCHBOX_BOUNDS;

    /* widget rect, in region coords */
    data->bbox.xmin = margin + padding;
    data->bbox.xmax = BLI_rcti_size_x(&region->winrct) - (margin + padding);
    data->bbox.ymin = margin;
    data->bbox.ymax = BLI_rcti_size_y(&region->winrct) - UI_POPUP_MENU_TOP;

    /* check if button is lower half */
    if (but->rect.ymax < BLI_rctf_cent_y(&but->block->rect)) {
      data->bbox.ymin += search_but_h;
    }
    else {
      data->bbox.ymax -= search_but_h;
    }
  }
  else {
    const int searchbox_width = searchbox_size_x_from_items(data->items);

    rctf rect_fl;
    rect_fl.xmin = but->rect.xmin;
    rect_fl.xmax = but->rect.xmax;
    rect_fl.ymax = but->rect.ymin;
    rect_fl.ymin = rect_fl.ymax - searchbox_size_y();

    const int ofsx = (but->block->panel) ? but->block->panel->ofsx : 0;
    const int ofsy = (but->block->panel) ? but->block->panel->ofsy : 0;

    BLI_rctf_translate(&rect_fl, ofsx, ofsy);

    /* minimal width */
    if (BLI_rctf_size_x(&rect_fl) < searchbox_width) {
      rect_fl.xmax = rect_fl.xmin + searchbox_width;
    }

    /* copy to int, gets projected if possible too */
    rcti rect_i;
    BLI_rcti_rctf_copy(&rect_i, &rect_fl);

    if (butregion->v2d.cur.xmin != butregion->v2d.cur.xmax) {
      view2d_view_to_region_rcti(&butregion->v2d, &rect_fl, &rect_i);
    }

    BLI_rcti_translate(&rect_i, butregion->winrct.xmin, butregion->winrct.ymin);

    int winx = WM_window_native_pixel_x(win);
    /* Touch: the height is wanted now, not left unused. See the clamp below. */
    const int winy = WM_window_native_pixel_y(win);

    if (rect_i.xmax > winx) {
      /* super size */
      if (rect_i.xmax > winx + rect_i.xmin) {
        rect_i.xmax = winx;
        rect_i.xmin = 0;
      }
      else {
        rect_i.xmin -= rect_i.xmax - winx;
        rect_i.xmax = winx;
      }
    }

    /* Touch: pick the side of the field with room, then take the height from that side's room.
     *
     * The box grows away from the field and is never moved across it. That is the whole design,
     * and it is not a detail: a box slid vertically to fit the screen can end up over the very
     * field that opened it, and then the press that opens it lands on the box instead -- the list
     * appears and vanishes in the same instant, and the field cannot be used at all.
     *
     * What was here watched two of the four edges. The right, and the bottom through a flip that
     * moves the box above the field when there is no room beneath it. Nothing watched the top, and
     * the window height was not even read -- the line fetching it was commented out as unused -- so
     * a field near the top of the Properties editor flipped upwards and ran off the ceiling. That
     * is how an IK constraint's Target list came out with its rows above the screen.
     *
     * Shrink rather than clamp: cutting the rectangle would squeeze the rows back to the
     * unreadable heights the row count exists to avoid, so the height is refitted to whole rows.
     * #searchbox_size_y_fit floors at two rows, so a field with almost no room either side gets a
     * short box hanging off the screen edge rather than nothing at all -- but it hangs off the
     * edge, never over the field.
     *
     * The floor is the on-screen keyboard when it is up, for the same reason the F3 popup keeps
     * clear of it: the two are used together, and results under the keys typing into them cannot
     * be reached. Android's own keyboard cannot be accounted for -- its insets never reach this
     * window -- so a field that opens the platform IME is still on its own. */
    {
      int floor_y = 0;
      rcti keyboard;
      if (WM_virtual_keyboard_rect_get(win, &keyboard)) {
        floor_y = keyboard.ymax;
      }

      /* The two anchors in window coordinates: the bottom of the field, and its top. */
      const int anchor_below = rect_i.ymax;
      int anchor_above = but->rect.ymax + ofsy;
      if (butregion->v2d.cur.xmin != butregion->v2d.cur.xmax) {
        anchor_above = view2d_view_to_region_y(&butregion->v2d, anchor_above);
      }
      anchor_above += butregion->winrct.ymin;

      const int room_below = anchor_below - floor_y;
      const int room_above = winy - anchor_above;
      const int wanted = BLI_rcti_size_y(&rect_i);

      /* Below unless above has more to offer, which is the same preference as before -- only now
       * it is decided by measuring both sides rather than by discovering the first one failed. */
      if (room_below >= wanted || room_below >= room_above) {
        rect_i.ymax = anchor_below;
        rect_i.ymin = anchor_below - searchbox_size_y_fit(std::min(wanted, room_below));
      }
      else {
        rect_i.ymin = anchor_above;
        rect_i.ymax = anchor_above + searchbox_size_y_fit(std::min(wanted, room_above));
      }

      /* Sideways only. Moving it vertically is exactly what would put it over the field. */
      if (rect_i.xmax > winx) {
        BLI_rcti_translate(&rect_i, winx - rect_i.xmax, 0);
      }
      if (rect_i.xmin < 0) {
        BLI_rcti_translate(&rect_i, -rect_i.xmin, 0);
      }
    }

    /* widget rect, in region coords */
    data->bbox.xmin = margin;
    data->bbox.xmax = BLI_rcti_size_x(&rect_i) + margin;
    data->bbox.ymin = margin;
    data->bbox.ymax = BLI_rcti_size_y(&rect_i) + margin;

    /* region bigger for shadow */
    region->winrct.xmin = rect_i.xmin - margin;
    region->winrct.xmax = rect_i.xmax + margin;
    region->winrct.ymin = rect_i.ymin - margin;
    region->winrct.ymax = rect_i.ymax;
  }

  region->winx = region->winrct.xmax - region->winrct.xmin + 1;
  region->winy = region->winrct.ymax - region->winrct.ymin + 1;

  /* Touch: the box has been measured, so now say how many rows it really holds -- and if the
   * gather already ran against the old count, run it again against this one.
   *
   * The order is creation, first gather, layout, draw. A count settled here alone therefore
   * arrives one step too late: the gather had already filled ten items into a box with room for
   * six, and the four extra drew below it, which is why the overflow healed itself the moment
   * anything scrolled and forced a second gather. Estimating the box at creation instead was
   * tried and came out short by a row or two, because the height the popup asks for and the box
   * this region ends up with are separated by a chain of margins that do not cancel.
   *
   * So it is measured here, where the answer is certain, and the gather is simply redone. The
   * cost is one extra pass over the results as the box opens. */
  if (!data->preview) {
    const int rows = searchbox_rows_for_height(BLI_rcti_size_y(&data->bbox));
    if (rows != data->items.maxitem) {
      data->rows = rows;
      data->items.maxitem = rows;
      searchbox_update(const_cast<bContext *>(C), region, data->search_but, false);
    }
  }

  /* Touch: settle the row geometry here, where the menu scale is held, so everything that asks
   * later -- the draw, and the hit test on a release, which runs without it -- gets the same
   * answer. Rows at their natural height and top aligned rather than the box stretched to fill:
   * the popup paints its background from the block's own height while this bbox comes from the
   * region around it, and dividing the box by the count spread that difference into the rows
   * until the last one hung below the background behind it. Slack stays empty at the bottom,
   * where nothing draws. */
  data->tria_h = int(data->zoom * UI_SEARCHBOX_TRIA_H);
  if (!data->preview) {
    const int usable = BLI_rcti_size_y(&data->bbox) - 2 * data->tria_h;
    data->row_h = std::min(int(UI_UNIT_Y), usable / std::max(data->rows, 1));
  }

  data->size_set = true;
}

static ARegion *searchbox_create_generic_ex(bContext *C,
                                            ARegion *butregion,
                                            ButtonSearch *but,
                                            const bool use_shortcut_sep)
{
  /* Touch: the same scale the layout and the draw hold, for the same reason and one step earlier.
   * The row count is settled in here, and it is measured in widget units -- so if this ran at the
   * unscaled unit while the layout ran at the scaled one, the two would disagree about how many
   * rows the very same box holds. */
  const ScopedMenuScale menu_scale(ED_ui_menu_scale());

  const uiStyle *style = style_get();
  const float aspect = but->block->aspect;

  /* create area region */
  ARegion *region = region_temp_add(CTX_wm_screen(C));

  static ARegionType type;
  memset(&type, 0, sizeof(ARegionType));
  type.layout = searchbox_region_layout_fn;
  type.draw = searchbox_region_draw_fn;
  type.free = searchbox_region_free_fn;
  type.listener = searchbox_region_listen_fn;
  type.regionid = RGN_TYPE_TEMPORARY;
  region->runtime->type = &type;

  /* Create search-box data. */
  uiSearchboxData *data = MEM_new<uiSearchboxData>(__func__);
  data->search_arg = but->arg;
  data->search_but = but;
  data->butregion = butregion;
  data->size_set = false;
  data->search_listener = but->listen_fn;
  data->zoom = 1.0f / aspect;

  /* Set font, get the bounding-box. */
  data->fstyle = style->widget; /* copy struct */
  fontscale(&data->fstyle.points, aspect);
  fontstyle_set(&data->fstyle);

  region->regiondata = data;

  /* Special case, hard-coded feature, not draw backdrop when called from menus,
   * assume for design that popup already added it. */
  if (but->block->flag & BLOCK_SEARCH_MENU) {
    data->noback = true;
  }

  if (but->preview_rows > 0 && but->preview_cols > 0) {
    data->preview = true;
    data->prv_rows = but->preview_rows;
    data->prv_cols = but->preview_cols;
  }

  if (but->optype != nullptr || use_shortcut_sep) {
    data->use_shortcut_sep = true;
  }
  data->sep_string = but->item_sep_string;

  /* Adds sub-window. */
  ED_region_floating_init(region);

  /* notify change and redraw */
  ED_region_tag_redraw(region);

  /* prepare search data */
  data->drag_last_y = INT_MIN;
  data->drag_accum = 0;
  data->drag_travel = 0;
  data->drag_active = false;
  data->drag_armed = false;
  /* The full count, and the buffers below are allocated for it. The layout measures the box and
   * lowers both once it knows what actually fits. */
  data->rows = SEARCH_ITEMS;
  /* Sane geometry until the layout settles it for real; this runs inside the menu scale too. */
  data->tria_h = int(data->zoom * UI_SEARCHBOX_TRIA_H);
  data->row_h = int(UI_UNIT_Y);
  if (data->preview) {
    data->items.maxitem = data->prv_rows * data->prv_cols;
  }
  else {
    data->items.maxitem = SEARCH_ITEMS;
  }
  /* In case the button's string is dynamic, make sure there are buffers available. */
  data->items.maxstrlen = but->hardmax == 0 ? UI_MAX_NAME_STR : but->hardmax;
  data->items.totitem = 0;
  data->items.names = MEM_new_array_zeroed<char *>(data->items.maxitem, __func__);
  data->items.pointers = MEM_new_array_zeroed<void *>(data->items.maxitem, __func__);
  data->items.icons = MEM_new_array_zeroed<int>(data->items.maxitem, __func__);
  data->items.but_flags = MEM_new_array_zeroed<int64_t>(data->items.maxitem, __func__);
  data->items.name_prefix_offsets = nullptr; /* Lazy initialized as needed. */
  for (int i = 0; i < data->items.maxitem; i++) {
    data->items.names[i] = MEM_new_array_zeroed<char>(data->items.maxstrlen + 1, __func__);
  }

  return region;
}

ARegion *searchbox_create_generic(bContext *C, ARegion *butregion, ButtonSearch *search_but)
{
  return searchbox_create_generic_ex(C, butregion, search_but, false);
}

/**
 * Similar to Python's `str.title` except...
 *
 * - we know words are upper case and ASCII only.
 * - `_` are replaced by spaces.
 */
static void str_tolower_titlecaps_ascii(char *str, const size_t len)
{
  bool prev_delim = true;

  for (size_t i = 0; (i < len) && str[i]; i++) {
    if (str[i] >= 'A' && str[i] <= 'Z') {
      if (prev_delim == false) {
        str[i] += 'a' - 'A';
      }
    }
    else if (str[i] == '_') {
      str[i] = ' ';
    }

    prev_delim = ELEM(str[i], ' ') || (str[i] >= '0' && str[i] <= '9');
  }
}

static void searchbox_region_draw_cb__operator(const bContext * /*C*/, ARegion *region)
{
  uiSearchboxData *data = static_cast<uiSearchboxData *>(region->regiondata);

  /* pixel space */
  wmOrtho2_region_pixelspace(region);

  if (data->noback == false) {
    draw_widget_menu_back(&data->bbox, true);
  }

  /* draw text */
  if (data->items.totitem) {
    rcti rect;

    /* draw items */
    for (int a = 0; a < data->items.totitem; a++) {
      rcti rect_pre, rect_post;
      searchbox_butrect(&rect, data, a);

      rect_pre = rect;
      rect_post = rect;

      rect_pre.xmax = rect_post.xmin = rect.xmin + ((rect.xmax - rect.xmin) / 4);

      /* widget itself */
      /* NOTE: i18n messages extracting tool does the same, please keep it in sync. */
      {
        const int64_t but_flag = ((a == data->active) ? UI_HOVER : 0) | data->items.but_flags[a];

        wmOperatorType *ot = static_cast<wmOperatorType *>(data->items.pointers[a]);
        char text_pre[128];
        const char *text_pre_p = strstr(ot->idname, "_OT_");
        if (text_pre_p == nullptr) {
          text_pre[0] = '\0';
        }
        else {
          int text_pre_len;
          text_pre_p += 1;
          text_pre_len = BLI_strncpy_utf8_rlen(
              text_pre, ot->idname, min_ii(sizeof(text_pre), text_pre_p - ot->idname));
          text_pre[text_pre_len] = ':';
          text_pre[text_pre_len + 1] = '\0';
          str_tolower_titlecaps_ascii(text_pre, sizeof(text_pre));
        }

        draw_menu_item(&data->fstyle,
                       &rect_pre,
                       &rect,
                       data->zoom,
                       data->noback,
                       CTX_IFACE_(BLT_I18NCONTEXT_OPERATOR_DEFAULT, text_pre),
                       data->items.icons[a],
                       but_flag,
                       UI_MENU_ITEM_SEPARATOR_NONE,
                       nullptr);
        draw_menu_item(&data->fstyle,
                       &rect_post,
                       nullptr,
                       data->zoom,
                       data->noback,
                       data->items.names[a],
                       0,
                       but_flag,
                       data->use_shortcut_sep ? UI_MENU_ITEM_SEPARATOR_SHORTCUT :
                                                UI_MENU_ITEM_SEPARATOR_NONE,
                       nullptr);
      }
    }
    /* indicate more */
    if (data->items.more) {
      searchbox_butrect(&rect, data, data->items.maxitem - 1);
      searchbox_draw_clip_tri_down(&rect, data->zoom);
    }
    if (data->items.offset) {
      searchbox_butrect(&rect, data, 0);
      searchbox_draw_clip_tri_up(&rect, data->zoom);
    }
  }
  else {
    rcti rect;
    searchbox_butrect(&rect, data, 0);
    draw_menu_item(&data->fstyle,
                   &rect,
                   &rect,
                   data->zoom,
                   data->noback,
                   IFACE_("No results found"),
                   0,
                   0,
                   UI_MENU_ITEM_SEPARATOR_NONE,
                   nullptr);
  }
}

ARegion *searchbox_create_operator(bContext *C, ARegion *butregion, ButtonSearch *search_but)
{
  ARegion *region = searchbox_create_generic_ex(C, butregion, search_but, true);

  region->runtime->type->draw = searchbox_region_draw_cb__operator;

  return region;
}

void searchbox_free(bContext *C, ARegion *region)
{
  region_temp_remove(C, CTX_wm_screen(C), region);
}

static void searchbox_region_draw_cb__menu(const bContext * /*C*/, ARegion * /*region*/)
{
  /* Currently unused. */
}

ARegion *searchbox_create_menu(bContext *C, ARegion *butregion, ButtonSearch *search_but)
{
  ARegion *region = searchbox_create_generic_ex(C, butregion, search_but, true);

  if (false) {
    region->runtime->type->draw = searchbox_region_draw_cb__menu;
  }

  return region;
}

void button_search_refresh(ButtonSearch *but)
{
  /* possibly very large lists (such as ID datablocks) only
   * only validate string RNA buts (not pointers) */
  if (but->rnaprop && RNA_property_type(but->rnaprop) != PROP_STRING) {
    return;
  }

  SearchItems *items = MEM_new<SearchItems>(__func__);

  /* setup search struct */
  items->maxitem = 10;
  items->maxstrlen = 256;
  items->names = MEM_new_array_zeroed<char *>(items->maxitem, __func__);
  for (int i = 0; i < items->maxitem; i++) {
    items->names[i] = MEM_new_array_zeroed<char>(but->hardmax + 1, __func__);
  }

  searchbox_update_fn(
      static_cast<bContext *>(but->block->evil_C), but, but->drawstr.c_str(), items);

  if (!but->results_are_suggestions) {
    /* Only red-alert when we are sure of it, this can miss cases when >10 matches. */
    if (items->totitem == 0) {
      button_flag_enable(but, BUT_REDALERT);
    }
    else if (items->more == 0) {
      if (search_items_find_index(items, but->drawstr.c_str()) == -1) {
        button_flag_enable(but, BUT_REDALERT);
      }
    }
  }

  for (int i = 0; i < items->maxitem; i++) {
    MEM_delete(items->names[i]);
  }
  MEM_delete(items->names);
  MEM_delete(items);
}

/** \} */

}  // namespace blender::ui
