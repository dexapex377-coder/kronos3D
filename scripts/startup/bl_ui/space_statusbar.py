# SPDX-FileCopyrightText: 2018-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import Header


class STATUSBAR_HT_header(Header):
    bl_space_type = 'STATUSBAR'

    def draw(self, context):
        # Touch: two header regions, so the keyboard button can sit still while the rest scrolls.
        # See statusbar_create() for why. The one-region case is still handled, because a window
        # restored from an older configuration can have a status bar that predates the split.
        #
        # TODO: the on-screen keyboard button has been hidden (it is redundant with the touch
        # ball + pie). The region logic is left intact so nothing else breaks.
        # if context.region.alignment == 'LEFT':
        #     self._draw_keyboard(context)
        #     return

        # One region only, from a window that predates the split: the old order, all in this one.
        # if not any(region.alignment == 'LEFT' for region in context.area.regions):
        #     self._draw_keyboard(context)

        self._draw_info(context)

    # def _draw_keyboard(self, _context):
    #     layout = self.layout
    #
    #     # Touch: the on-screen keyboard lives at the far left of the status bar, which is always
    #     # on screen and within thumb reach whatever workspace is open. It is alone in this region
    #     # so that dragging the bar sideways to read the figures on the right cannot carry it off
    #     # the edge.
    #     layout.operator("wm.virtual_keyboard_toggle", text="KEYBOARD", icon='KEY_MENU')

    def _draw_info(self, context):
        """The hints, the messages and the figures.

        Touch: the spacers are upstream's and they stay, in the region beside the keyboard button
        as much as in the one-region fallback. They are what holds the figures against the right
        end of the bar, and -- this is the part that is easy to undo by accident -- what keeps
        them still.

        `template_input_status()` draws the mouse and modifier hints, and those exist only while a
        finger is on the screen. Anchored by a spacer of their own they appear and vanish at the
        left end and nothing else moves. Packed together behind a single leading spacer instead,
        every appearance changed the width of the whole row, and the figures jumped a hint's worth
        sideways each time the bar was touched or let go of -- most visible mid-drag, where it read
        as the scroll losing its place.

        `UILayout.alignment = 'RIGHT'` is not an alternative: a header layout is only as wide as
        its own content, so alignment has nothing to push against.
        """
        layout = self.layout

        # input status
        layout.template_input_status()

        layout.separator_spacer()

        # Messages
        layout.template_reports_banner()

        # Progress Bar
        layout.template_running_jobs()

        layout.separator_spacer()

        # Stats & Info
        layout.template_status_info()


classes = (
    STATUSBAR_HT_header,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
