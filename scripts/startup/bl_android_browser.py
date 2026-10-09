# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Touch: give Python's ``webbrowser`` a browser to open on Android.

Every link in the program -- the About box, "Online Manual" on a tool's context menu, the
manual and community buttons in Preferences -- goes through ``wm.url_open``, which calls
``webbrowser.open()``. That module builds its list of browsers by hunting for ``xdg-open``,
``gio``, ``x-www-browser`` and friends with ``shutil.which()``. Android has none of them,
so the list comes out empty and ``open()`` returns False: no browser, no exception, no log
line. Every one of those links did nothing at all, and did it silently.

Registering here rather than editing ``wm.url_open`` is deliberate. ``webbrowser`` is what
add-ons use, and what ``_bpy_internal.system_info.url_prefill_startup`` uses to open a
pre-filled bug report, so fixing the module fixes all of them at once and leaves the
operator alone.

The work itself is ``wm.platform_url_open``, a native operator that hands the URL to
``BlenderActivity.openUrl()`` over JNI, which starts an ``ACTION_VIEW`` intent.
"""

import sys

__all__ = (
    "register",
    "unregister",
)

_NAME = "blender-android"


class _AndroidBrowser:
    """A ``webbrowser`` controller that hands the URL to the platform.

    The three methods and the two attributes are what ``webbrowser`` expects of a
    controller; it has no base class that has to be inherited.
    """

    name = _NAME
    basename = _NAME

    def open(self, url, new=0, autoraise=True):
        import bpy
        # The operator reports its own error, so a False here is not silent.
        return bpy.ops.wm.platform_url_open(url=url) == {'FINISHED'}

    def open_new(self, url):
        # Android decides for itself whether a link lands in a new tab or window.
        return self.open(url)

    def open_new_tab(self, url):
        return self.open(url)


def register():
    # CPython's own marker for an Android build of the interpreter. Nothing here
    # applies to any other platform, where webbrowser already works.
    if not hasattr(sys, "getandroidapilevel"):
        return

    import webbrowser

    # Reloading scripts runs this again, and webbrowser.register() appends without
    # checking, so a second copy would pile up in _tryorder on every reload.
    if _NAME in getattr(webbrowser, "_browsers", {}):
        return

    webbrowser.register(_NAME, None, _AndroidBrowser(), preferred=True)


def unregister():
    # Left registered on purpose: webbrowser has no way to take one back out, and
    # a stale entry would be worse than a live one that still works.
    pass
