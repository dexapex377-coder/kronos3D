// SPDX-FileCopyrightText: 2026 Kronos3D Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

package com.kronos3d.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.PixelFormat
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.LazyGridState
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Star
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Divider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.roundToPx
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleRegistry
import androidx.lifecycle.ProcessLifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistry
import androidx.savedstate.SavedStateRegistryController
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import kotlin.math.ceil
import kotlin.math.roundToInt
import androidx.compose.ui.unit.IntOffset
import androidx.compose.runtime.mutableFloatStateOf

/**
 * The floating shortcuts panel, its trigger button, the key-combo keyboard and the
 * shortcut composer, all living in WindowManager windows above Blender's viewport.
 *
 * ## Why a WindowManager window and not addContentView
 *
 * [BlenderActivity] extends `NativeActivity`, which hands its whole surface to
 * `android_main`. A view in its content view is drawn *under* the Vulkan output and
 * never appears. A `WindowManager` window is a separate window, floats above the
 * native surface, and with [WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL] leaves
 * every touch outside its own bounds to Blender -- which is what keeps the viewport
 * usable with the panel open.
 *
 * ## Why the panel cannot offset itself
 *
 * Dragging adjusts [WindowManager.LayoutParams] x/y, never a `Modifier.offset`. The
 * panel's window is only as wide as the panel, so a screen-space offset inside it
 * pushes the content outside its own bounds and it renders as a blank rectangle.
 *
 * ## The lifecycle wiring is not optional
 *
 * A hand-built ComposeView has no LifecycleOwner and no SavedStateRegistryOwner.
 * Omitting the first crashes on first composition; omitting the second throws
 * IllegalStateException from onAttachedToWindow. See [newComposeView].
 */
object ShortcutsOverlay {

    /** Panel width. Short enough to keep the viewport readable. */
    private const val PANEL_WIDTH_DP = 230

    @Volatile private var panelView: View? = null
    @Volatile private var keyboardView: View? = null
    @Volatile private var triggerView: View? = null

    val isShowing: Boolean get() = panelView != null

    @JvmStatic
    fun toggle(activity: BlenderActivity) {
        if (!isEnabled) return
        if (isShowing) hide() else show(activity)
    }

    @JvmStatic
    fun show(activity: BlenderActivity) {
        if (isShowing) return
        val wm = activity.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        panelView = addComposeWindow(activity, wm) { onDrag ->
            ShortcutsPanel(
                onDrag = onDrag,
                onSendCombo = { combo ->
                    val key = resolveCombo(combo)
                    activity.sendShortcutKey(key.keyCode, key.meta)
                },
                onDismiss = { hide() },
            )
        }
    }

    @JvmStatic
    fun hide() {
        removeWindow(panelView); panelView = null
        removeWindow(keyboardView); keyboardView = null
    }

    /**
     * Put the small always-on button that opens the panel. Also a WindowManager
     * window, for the same reason the panel is one: on a NativeActivity the native
     * surface covers the content view.
     */
    /**
     * Opt-in switch, so the overlay can be taken out of the picture without a rebuild.
     *
     *   adb shell setprop debug.blender.shortcuts 1     # enable
     *   adb shell setprop debug.blender.shortcuts 0     # disable
     *
     * Read on every call rather than cached, so `adb shell setprop` takes effect on
     * the next focus change without restarting the app.
     */
    private val isEnabled: Boolean
        get() = readFlag("debug.blender.shortcuts", true)

    private fun readFlag(name: String, fallback: Boolean): Boolean =
        BlenderActivity.systemPropertyInt(name, if (fallback) 1 else 0) != 0

    @JvmStatic
    fun installTrigger(activity: BlenderActivity) {
        if (!isEnabled) return
        if (triggerView != null) return
        val wm = activity.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val density = activity.resources.displayMetrics.density
        val sizePx = (44 * density).roundToInt()

        val view = newComposeView(activity) { _ ->
            Box(
                modifier = Modifier
                    .size(sizePx.dp)
                    .background(PanelBackground, RoundedCornerShape(10.dp))
                    .border(1.dp, PanelBorder, RoundedCornerShape(10.dp))
                    .clickable { toggle(activity) },
                contentAlignment = Alignment.Center,
            ) {
                Text("K3D", color = PanelHeader, fontSize = 11.sp, fontWeight = FontWeight.Bold)
            }
        }

        val params = WindowManager.LayoutParams(
            sizePx, sizePx,
            WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSPARENT,
        ).apply {
            gravity = Gravity.END or Gravity.TOP
            x = (8 * density).roundToInt()
            y = (120 * density).roundToInt()
            softInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING
        }

        runCatching { wm.addView(view, params) }.onSuccess { triggerView = view }
    }

    @JvmStatic
    fun uninstallTrigger() {
        removeWindow(triggerView); triggerView = null
    }

    private fun addComposeWindow(
        activity: BlenderActivity,
        windowManager: WindowManager,
        gravity: Int = Gravity.TOP or Gravity.START,
        widthDp: Int = PANEL_WIDTH_DP,
        heightDp: Int? = null,
        content: @Composable (onDrag: (dx: Float, dy: Float) -> Unit) -> Unit,
    ): View {
        val density = activity.resources.displayMetrics.density
        val widthPx = (widthDp * density).roundToInt()
        val heightPx = heightDp?.let { (it * density).roundToInt() }

        val params = WindowManager.LayoutParams(
            widthPx,
            heightPx ?: WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
            // NOT_TOUCH_MODAL is load-bearing: without it this window takes every
            // touch on the screen and the viewport goes dead.
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSPARENT,
        ).apply {
            x = if (gravity and Gravity.START != 0) {
                val screenWidth = activity.resources.displayMetrics.widthPixels
                screenWidth - widthPx - (16 * density).roundToInt()
            } else {
                0
            }
            y = (56 * density).roundToInt()
            this.gravity = gravity
            softInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING
        }

        // lateinit: the drag handler must reach the very view it is drawn into, and
        // that view only exists once newComposeView returns. The closure runs on a
        // later user drag, by which point it is assigned.
        lateinit var view: View
        view = newComposeView(activity) { onDrag ->
            content { dx, dy ->
                runCatching {
                    params.x += dx.roundToInt()
                    params.y += dy.roundToInt()
                    windowManager.updateViewLayout(view, params)
                }
            }
        }

        windowManager.addView(view, params)
        return view
    }

    /**
     * Build a ComposeView that is safe to hand to the WindowManager.
     *
     * Both view-tree owners are mandatory and neither may be dropped: Compose throws
     * from onAttachedToWindow without the saved-state one, and crashes on first
     * composition without the lifecycle one. One place for both is deliberate --
     * wiring them by hand per call site is how the trigger shipped with only one.
     */
    private fun newComposeView(
        activity: BlenderActivity,
        content: @Composable (onDrag: (dx: Float, dy: Float) -> Unit) -> Unit,
    ): ComposeView {
        // Two owners, and they are deliberately different. LifecycleOwner is
        // ProcessLifecycleOwner, which is already RESUMED while the app is in the
        // foreground and drops on its own when the app is backgrounded. A hand-rolled
        // registry is the trap: a bare LifecycleRegistry that is only ever sent ON_CREATE
        // leaves Compose's Recomposer paused below STARTED, so every MutableState still
        // mutates and nothing ever redraws -- the panel looks alive and responds to
        // nothing. The saved-state side has no such requirement and gets the trivial
        // SimpleSavedStateRegistryOwner.
        val savedStateOwner = OverlaySavedStateOwner()
        return ComposeView(activity).apply {
            setViewTreeLifecycleOwner(ProcessLifecycleOwner.get())
            setViewTreeSavedStateRegistryOwner(savedStateOwner)
            // Touch handling is per-element inside the composables; the window itself
            // must not be clickable or it swallows events it does not draw.
            isClickable = false
            // android.graphics.Color.TRANSPARENT, not PixelFormat.TRANSPARENT: this
            // setter takes an ARGB colour, and PixelFormat.TRANSPARENT is -1-coded as
            // 0xFFFFFFFD, which paints the view near-white. The panel is a rounded
            // Surface over this background, so the rounding let the white show through
            // as four corner marks.
            setBackgroundColor(android.graphics.Color.TRANSPARENT)
            // The drag handler is a no-op on purpose: newComposeView takes no
            // dragHandler of its own. addComposeWindow, the only caller that drags,
            // ignores this one and substitutes its own because it needs the
            // LayoutParams and the view; the trigger button genuinely does not drag.
            setContent { MaterialTheme { content { _, _ -> } } }
        }
    }

    /**
     * Saved-state owner for a ComposeView that has no Activity behind it.
     *
     * Compose reads this during composition but the panel keeps nothing that has to
     * survive process death, so it only has to exist and be attached.
     * androidx.savedstate has a ready-made SimpleSavedStateRegistryOwner for exactly
     * this, but it is not on the classpath here.
     */
    private class OverlaySavedStateOwner : SavedStateRegistryOwner {
        /**
         * Its own LifecycleRegistry, NOT ProcessLifecycleOwner.
         *
         * SavedStateRegistryController.create() builds a Restarter, and a Restarter is
         * only legal while the owner's lifecycle is between INITIALIZED and CREATED.
         * ProcessLifecycleOwner is RESUMED whenever the app is foregrounded, so
         * delegating to it made create() throw
         *
         *   IllegalStateException: Restarter must be created only during owner's
         *   initialization stage
         *
         * and killed the app on the first frame, from onWindowFocusChanged.
         *
         * The two lifecycles are separate on purpose. ProcessLifecycleOwner goes to
         * the view tree, where Compose's Recomposer needs it RESUMED to recompose at
         * all; this one only has to reach CREATED so the registry can be restored.
         * Sharing one object cannot satisfy both.
         */
        override val lifecycle: Lifecycle = LifecycleRegistry(this)

        private val controller = SavedStateRegistryController.create(this)

        init {
            controller.performAttach()
            controller.performRestore(null)
            (lifecycle as LifecycleRegistry).handleLifecycleEvent(Lifecycle.Event.ON_CREATE)
        }

        override val savedStateRegistry: SavedStateRegistry
            get() = controller.savedStateRegistry
    }

    private fun removeWindow(view: View?) {
        if (view == null) return
        val wm = view.context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        try {
            wm.removeViewImmediate(view)
        } catch (_: IllegalArgumentException) {
            // Already detached.
        } catch (_: WindowManager.BadTokenException) {
            // The activity's window token died first, which is the normal case when a
            // rotation or a backgrounding took the window away. The view goes with it.
        }
    }

}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

private val PanelBackground = Color(0xF2181B20)
private val PanelBorder = Color(0xFF2E3543)
private val PanelHeader = Color(0xFF8A99AD)
private val CardFace = Color(0xFF222731)
private val CardBorder = Color(0xFF333D4D)
private val ChromeFace = Color(0xFF1E293B)
private val KeyLabel = Color(0xFFDCE3EC)
private val Teal = Color(0xFF00838F)
private val Amber = Color(0xFFFF9100)
private val DangerRed = Color(0xFFFF5252)

// ---------------------------------------------------------------------------
// Key model: a keyboard key, and what it sends to Blender
// ---------------------------------------------------------------------------

enum class KeyType { NORMAL, MODIFIER, ACTION, NUMPAD }

/**
 * What a key sends: an Android keycode plus a metaState.
 *
 * A MODIFIER contributes only [meta]; every other key contributes its [keyCode].
 */
private data class AndroidKey(val keyCode: Int = 0, val meta: Int = 0) {
    val isModifier: Boolean get() = meta != 0
}

private data class KeyItem(
    val id: String,
    val label: String,
    val weight: Float = 1f,
    val type: KeyType = KeyType.NORMAL,
)

/**
 * Every key the composer can offer, and the Android keycode it maps to.
 *
 * These reach Blender through [BlenderActivity.sendShortcutKey], which forwards to
 * GHOST_SystemAndroid::handleJavaKeyEvent and becomes a real GHOST key event. So a
 * combo is not a private side-channel that merely looks like a shortcut: it lands in
 * Blender's own keymap, honours Blender's modifier state, and can be rebound from
 * Preferences. Android keycodes rather than GLFW because that is what the existing
 * JNI entry point already converts.
 */
private val KEY_TABLE: Map<String, AndroidKey> = buildMap {
    fun put(id: String, code: Int) = put(id, AndroidKey(code))
    fun mod(id: String, meta: Int) = put(id, AndroidKey(meta = meta))

    put("esc", KeyEvent.KEYCODE_ESCAPE)
    put("bksp", KeyEvent.KEYCODE_DEL)
    put("del", KeyEvent.KEYCODE_FORWARD_DEL)
    put("tab", KeyEvent.KEYCODE_TAB)
    put("enter", KeyEvent.KEYCODE_ENTER)
    put("caps", KeyEvent.KEYCODE_CAPS_LOCK)
    put("space", KeyEvent.KEYCODE_SPACE)
    put("backtick", KeyEvent.KEYCODE_GRAVE)
    put("minus", KeyEvent.KEYCODE_MINUS)
    put("equals", KeyEvent.KEYCODE_EQUALS)
    put("bracket_l", KeyEvent.KEYCODE_LEFT_BRACKET)
    put("bracket_r", KeyEvent.KEYCODE_RIGHT_BRACKET)
    put("backslash", KeyEvent.KEYCODE_BACKSLASH)
    put("semicolon", KeyEvent.KEYCODE_SEMICOLON)
    put("quote", KeyEvent.KEYCODE_APOSTROPHE)
    put("comma", KeyEvent.KEYCODE_COMMA)
    put("dot", KeyEvent.KEYCODE_PERIOD)
    put("slash", KeyEvent.KEYCODE_SLASH)
    put("left", KeyEvent.KEYCODE_DPAD_LEFT)
    put("up", KeyEvent.KEYCODE_DPAD_UP)
    put("down", KeyEvent.KEYCODE_DPAD_DOWN)
    put("right", KeyEvent.KEYCODE_DPAD_RIGHT)

    for (i in 0..9) put("$i", KeyEvent.KEYCODE_0 + i)
    for (i in 1..9) put("f$i", KeyEvent.KEYCODE_F1 + (i - 1))

    for (c in 'a'..'z') put(c.toString(), KeyEvent.KEYCODE_A + (c - 'a'))

    // Numpad. Blender's view shortcuts live here (Numpad 1/3/7, Numpad period), which
    // is the whole point of showing a numpad on the composer.
    put("np_0", KeyEvent.KEYCODE_NUMPAD_0)
    for (i in 1..9) put("np_$i", KeyEvent.KEYCODE_NUMPAD_0 + i)
    put("np_div", KeyEvent.KEYCODE_NUMPAD_DIVIDE)
    put("np_mul", KeyEvent.KEYCODE_NUMPAD_MULTIPLY)
    put("np_sub", KeyEvent.KEYCODE_NUMPAD_SUBTRACT)
    put("np_add", KeyEvent.KEYCODE_NUMPAD_ADD)
    put("np_dot", KeyEvent.KEYCODE_NUMPAD_DOT)
    put("np_enter", KeyEvent.KEYCODE_NUMPAD_ENTER)

    mod("shift_l", KeyEvent.META_SHIFT_ON)
    mod("shift_r", KeyEvent.META_SHIFT_ON)
    mod("ctrl_l", KeyEvent.META_CTRL_ON)
    mod("ctrl_r", KeyEvent.META_CTRL_ON)
    mod("alt_l", KeyEvent.META_ALT_ON)
    mod("alt_r", KeyEvent.META_ALT_ON)
}

/** Fold a chosen key combination into the single keycode + metaState pair to send. */
private fun resolveCombo(ids: List<String>): AndroidKey {
    var meta = 0
    var code = 0
    for (id in ids) {
        val key = KEY_TABLE[id] ?: continue
        meta = meta or key.meta
        // First non-modifier wins; Blender cannot receive two keydowns at once.
        if (code == 0 && key.keyCode != 0) code = key.keyCode
    }
    return AndroidKey(code, meta)
}

// ---------------------------------------------------------------------------
// Shortcut model and storage
// ---------------------------------------------------------------------------

/**
 * One entry in the grid.
 *
 * [id] is derived from the key combination rather than being a random UUID: a UUID
 * default makes every recomposition produce a new key, so Compose discards and
 * recreates the whole list and any edit is lost.
 */
private data class GridShortcut(
    val label: String,
    val combo: List<String>,
    val icon: ImageVector = Icons.Default.Star,
) {
    /**
     * Stable and unique.
     *
     * The combo alone is not a key: two shortcuts saved with the same combination
     * produce the same string, and LazyVerticalGrid throws
     *
     *   IllegalArgumentException: Key "shift_l+a" was already used
     *
     * on the second one. The name disambiguates the common case, and the position
     * finishes it so a duplicate name with a duplicate combo is still legal.
     */
    val id: String get() = label + "\u0000" + combo.joinToString("+")

    /**
     * Human-readable combo, e.g. "Shift+A".
     *
     * Built from the keyboard layout's labels rather than from the key ids: those are
     * internal names, so uppercasing them rendered "SHIFT_L+A", "NP_0", "BACKTICK".
     * The two shift keys share the label "Shift" and the two alt/ctrl pairs likewise,
     * so duplicates collapse and the hint reads "Shift+A" rather than "Shift+Shift+A".
     */
    val keyHint: String
        get() = combo.mapNotNull { keyLabel(it) }
            .distinct()
            .joinToString("+")
            .ifEmpty { "sin tecla" }
}

/**
 * Keys that act as held modifiers rather than as the key a shortcut triggers.
 *
 * Used by the composer's "Mantener" toggle: latching works on these, since a modifier
 * has no meaning on its own. Order matches the precedence the native side applies when
 * it packs several modifiers into one sequence.
 */
private val MODIFIER_KEY_IDS = listOf("shift_l", "ctrl_l", "alt_l", "shift_r", "ctrl_r", "alt_r")

/** Label a key carries on the composer keyboard, e.g. shift_l -> "Shift". */
private fun keyLabel(id: String): String? =
    ALL_KEY_ITEMS.firstOrNull { it.id == id }?.label

/**
 * Shortcut list, persisted in SharedPreferences.
 *
 * Held here rather than in a composable so closing and reopening the panel does not
 * throw the user's work away, which is what would happen with a bare
 * `remember { mutableStateListOf() }` inside the panel.
 */
private object ShortcutStore {
    private const val PREFS = "kronos3d.shortcuts"
    private const val KEY = "list"

    fun load(context: Context): MutableList<GridShortcut> {
        val raw = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(KEY, "") ?: return mutableListOf()
        return raw.split(';')
            .filter { it.isNotBlank() }
            .mapNotNull { entry ->
                val parts = entry.split('|')
                if (parts.size != 2) return@mapNotNull null
                GridShortcut(label = parts[0], combo = parts[1].split('+').filter { it.isNotBlank() })
            }
            .toMutableList()
    }

    fun save(context: Context, shortcuts: List<GridShortcut>) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY, shortcuts.joinToString(";") { "${it.label}|${it.combo.joinToString("+")}" })
            .apply()
    }
}

/**
 * Write the shortcut list next to the app's external files, one `name=combo` per line.
 *
 * Deliberately a plain text file in getExternalFilesDir: that path is readable from a
 * file manager and pullable over adb, so a list can be moved between devices without
 * adding a file picker or a storage permission to the manifest.
 */
private fun exportShortcuts(context: Context) {
    val list = ShortcutStore.load(context)
    val text = list.joinToString("\n") { s -> s.label + "=" + s.combo.joinToString("+") }
    val file = java.io.File(context.getExternalFilesDir(null), EXPORT_FILE)
    runCatching { file.writeText(text) }
        .onSuccess { android.widget.Toast.makeText(context, "Exportado: ${file.name}", android.widget.Toast.LENGTH_SHORT).show() }
        .onFailure { android.widget.Toast.makeText(context, "No se pudo exportar", android.widget.Toast.LENGTH_SHORT).show() }
}

/** Read back whatever exportShortcuts wrote, replacing the current list. */
private fun importShortcuts(context: Context, into: MutableList<GridShortcut>) {
    val file = java.io.File(context.getExternalFilesDir(null), EXPORT_FILE)
    val ok = runCatching {
        val parsed = file.readLines()
            .filter { it.isNotBlank() && it.contains('=') }
            .map { line ->
                val name = line.substringBefore('=')
                val combo = line.substringAfter('=').split('+').filter { it.isNotBlank() }
                GridShortcut(label = name, combo = combo)
            }
            .filter { it.combo.isNotEmpty() && it.combo.all { k -> KEY_TABLE.containsKey(k) } }
        if (parsed.isNotEmpty()) {
            into.clear()
            into.addAll(parsed)
        }
        parsed.isNotEmpty()
    }.getOrDefault(false)
    android.widget.Toast.makeText(
        context,
        if (ok) "Atajos importados" else "No hay archivo para importar",
        android.widget.Toast.LENGTH_SHORT,
    ).show()
}

private const val EXPORT_FILE = "kronos3d_shortcuts.txt"

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

@Composable
private fun ShortcutsPanel(
    onDrag: (dx: Float, dy: Float) -> Unit,
    onSendCombo: (List<String>) -> Unit,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current

    var isMinimized by remember { mutableStateOf(false) }
    var isDeleteMode by remember { mutableStateOf(false) }
    var showMenuDropdown by remember { mutableStateOf(false) }
    var showKeyboard by remember { mutableStateOf(false) }
    var showNameDialog by remember { mutableStateOf(false) }
    var pendingCombo by remember { mutableStateOf<List<String>>(emptyList()) }

    val selectedKeyIds = remember { mutableStateListOf<String>() }
    /** Modifiers latched on via "Mantener": added to every combo built from here. */
    val latchedModifierIds = remember { mutableStateListOf<String>() }
    // No KEY_TABLE filtering here. It used to drop any shortcut containing an
    // unknown key id, so a mapping gap silently deleted saved work on reopen.
    // Shortcuts are now kept as written; GridShortcutCard flags unmappable ones.
    val shortcuts = remember {
        mutableStateListOf<GridShortcut>().apply { addAll(ShortcutStore.load(context)) }
    }
    fun persist() = ShortcutStore.save(context, shortcuts)

    Surface(
        shape = RoundedCornerShape(12.dp),
        color = PanelBackground,
        border = BorderStroke(1.dp, PanelBorder),
        tonalElevation = 8.dp,
        modifier = Modifier.width(230.dp),
    ) {
        Column(
            modifier = Modifier.padding(8.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                // The title is the drag handle, and only the title: a pointerInput on
                // the row would swallow taps on the +/delete/minimize/overflow buttons
                // sitting right next to it, and on the Surface it swallowed every tap
                // in the panel, leaving it inert.
                Text(
                    text = if (isMinimized) "KRONOS"
                    else if (isDeleteMode) "BORRAR"
                    else "KRONOS SHORTCUTS",
                    color = if (isDeleteMode) DangerRed else PanelHeader,
                    fontSize = 9.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier
                        .padding(start = 2.dp)
                        .pointerInput(Unit) {
                            detectDragGestures { change, drag ->
                                change.consume()
                                onDrag(drag.x, drag.y)
                            }
                        },
                )

                Row(
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    if (!isMinimized) {
                        Box(
                            modifier = Modifier
                                .size(26.dp)
                                .background(Teal, RoundedCornerShape(6.dp))
                                .clickable { selectedKeyIds.clear(); showKeyboard = true },
                            contentAlignment = Alignment.Center,
                        ) {
                            Icon(
                                Icons.Default.Add,
                                contentDescription = "New Shortcut",
                                tint = Color.White,
                                modifier = Modifier.size(15.dp),
                            )
                        }
                        Box(
                            modifier = Modifier
                                .size(26.dp)
                                .background(
                                    if (isDeleteMode) Color(0xFFD32F2F) else CardFace,
                                    RoundedCornerShape(6.dp),
                                )
                                .clickable(enabled = shortcuts.isNotEmpty()) { isDeleteMode = !isDeleteMode },
                            contentAlignment = Alignment.Center,
                        ) {
                            Icon(
                                Icons.Default.Delete,
                                contentDescription = "Delete Mode",
                                tint = if (isDeleteMode) Color.White else Color(0xFFFF8A80),
                                modifier = Modifier.size(14.dp),
                            )
                        }
                    }

                    Box(
                        modifier = Modifier
                            .size(26.dp)
                            .background(CardFace, RoundedCornerShape(6.dp))
                            .clickable {
                                isMinimized = !isMinimized
                                if (isMinimized) {
                                    isDeleteMode = false
                                    showMenuDropdown = false
                                }
                            },
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(
                            if (isMinimized) Icons.Default.KeyboardArrowDown
                            else Icons.Default.KeyboardArrowUp,
                            contentDescription = "Minimize / Maximize",
                            tint = Color.White,
                            modifier = Modifier.size(16.dp),
                        )
                    }

                    Box(
                        modifier = Modifier
                            .size(26.dp)
                            .background(CardFace, RoundedCornerShape(6.dp))
                            .clickable { showMenuDropdown = !showMenuDropdown },
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(
                            Icons.Default.MoreVert,
                            contentDescription = "Options",
                            tint = Color.White,
                            modifier = Modifier.size(15.dp),
                        )
                    }
                }
            }

            if (showMenuDropdown) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    MenuEntry("Export", Modifier.weight(1f)) {
                        showMenuDropdown = false
                        exportShortcuts(context)
                    }
                    MenuEntry("Import", Modifier.weight(1f)) {
                        showMenuDropdown = false
                        importShortcuts(context, shortcuts)
                        persist()
                    }
                }
            }

            if (!isMinimized) {
                Divider(color = PanelBorder, thickness = 1.dp)

                if (shortcuts.isEmpty()) {
                    Box(
                        modifier = Modifier.fillMaxWidth().height(90.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            text = "Sin atajos creados.\nToca '+' para añadir uno.",
                            color = Color(0xFF6C7D93),
                            fontSize = 11.sp,
                            textAlign = TextAlign.Center,
                        )
                    }
                } else {
                    val gridState = rememberLazyGridState()
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        LazyVerticalGrid(
                            columns = GridCells.Fixed(5),
                            state = gridState,
                            modifier = Modifier.weight(1f).heightIn(max = 210.dp),
                            horizontalArrangement = Arrangement.spacedBy(4.dp),
                            verticalArrangement = Arrangement.spacedBy(4.dp),
                            contentPadding = PaddingValues(vertical = 2.dp),
                        ) {
                            items(shortcuts, key = { it.id }) { item ->
                                GridShortcutCard(
                                    item = item,
                                    isDeleteMode = isDeleteMode,
                                    onClick = {
                                        if (isDeleteMode) {
                                            shortcuts.remove(item)
                                            if (shortcuts.isEmpty()) isDeleteMode = false
                                            persist()
                                        } else {
                                            val missing = item.combo.filter { k -> !KEY_TABLE.containsKey(k) }
                                            if (missing.isNotEmpty()) {
                                                android.widget.Toast.makeText(
                                                    context,
                                                    "Sin asignar en el teclado nativo: " +
                                                        missing.joinToString("+") { keyLabel(it) ?: it },
                                                    android.widget.Toast.LENGTH_SHORT,
                                                ).show()
                                            } else {
                                                onSendCombo(item.combo)
                                            }
                                        }
                                    },
                                )
                            }
                        }
                        // Explicit scrollbar: the grid is taller than the panel, and
                        // without a visible thumb there is no hint that the rest of
                        // the shortcuts exist.
                        GridScrollbar(
                            state = gridState,
                            itemCount = shortcuts.size,
                            modifier = Modifier.width(3.dp).height(210.dp),
                        )
                    }
                }
            }
        }
    }

    if (showKeyboard) {
        VisualKeyboard(
            selectedKeyIds = selectedKeyIds,
            onKeyToggle = { id ->
                if (selectedKeyIds.contains(id)) selectedKeyIds.remove(id)
                else selectedKeyIds.add(id)
            },
            onClearAll = { selectedKeyIds.clear(); latchedModifierIds.clear() },
            latchedModifierIds = latchedModifierIds,
            onToggleLatch = {
                /* Latch whichever modifier is currently selected, or drop all of them.
                 * Latching a modifier that is not selected would add a key the user never
                 * tapped, so the selection is what drives it. */
                val mods = selectedKeyIds.filter { MODIFIER_KEY_IDS.contains(it) }
                if (mods.isEmpty()) latchedModifierIds.clear() else latchedModifierIds.clear(); latchedModifierIds.addAll(mods)
            },
            onConfirm = { combo ->
                pendingCombo = combo.toList()
                showKeyboard = false
                showNameDialog = true
            },
            onDismiss = { showKeyboard = false },
        )
    }

    if (showNameDialog) {
        SaveShortcutNameDialog(
            keyCombo = pendingCombo.mapNotNull { keyLabel(it) }.distinct().joinToString("+"),
            onDismiss = { showNameDialog = false },
            onSave = { name ->
                // toList() here too, so a saved combo can never alias pendingCombo or
                // the keyboard's live selection: a shared list meant the next key
                // tapped in the composer retroactively edited an already-saved
                // shortcut, and the duplicate check below then matched it and
                // overwrote it instead of appending.
                val combo = pendingCombo.toList()
                val existing = shortcuts.indexOfFirst { it.combo == combo }
                val shortcut = GridShortcut(label = name, combo = combo)
                if (existing >= 0) shortcuts[existing] = shortcut else shortcuts.add(shortcut)
                // Start the next composer from empty, otherwise the previous
                // selection is still there and OK re-saves that combo instead.
                selectedKeyIds.clear()
                latchedModifierIds.clear()
                persist()
                showNameDialog = false
            },
        )
    }
}

@Composable
private fun MenuEntry(text: String, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Box(
        modifier = modifier
            .height(24.dp)
            .background(CardFace, RoundedCornerShape(5.dp))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = KeyLabel, fontSize = 10.sp, fontWeight = FontWeight.SemiBold)
    }
}

/**
 * Minimal vertical scrollbar for the shortcut grid.
 *
 * Compose has no stock scrollbar for lazy grids, and the panel is capped in height,
 * so the overflow was invisible and it read as "one shortcut replaced another".
 * The thumb length is proportional to what fraction of the rows fit on screen, and it
 * hides when everything already fits.
 */
@Composable
private fun GridScrollbar(
    state: LazyGridState,
    itemCount: Int,
    modifier: Modifier = Modifier,
) {
    if (itemCount <= 0) return
    val density = LocalDensity.current
    val viewportPx = with(density) { 210.dp.roundToPx() }
    val rows = ceil(itemCount / 5f).coerceAtLeast(1f)
    val visibleRows = minOf(ceil(viewportPx / maxOf(state.layoutInfo.minorAxisItemSize, 1f)), rows)
    val fraction = (visibleRows / rows).coerceIn(0.15f, 1f)

    val thumbHeightPx = (viewportPx * fraction).roundToInt()
    val maxScroll = (state.layoutInfo.totalItemsExtent - state.layoutInfo.viewportEndOffset)
    val progress = if (maxScroll <= 0) 0f
    else (state.firstVisibleItemIndex / maxOf(itemCount - visibleRows.toInt(), 1)).coerceIn(0f, 1f)

    Box(
        modifier = modifier.background(Color(0x22FFFFFF), RoundedCornerShape(2.dp)),
        contentAlignment = Alignment.TopCenter,
    ) {
        if (maxScroll > 0) {
            Box(
                modifier = Modifier
                    .width(3.dp)
                    .height(with(density) { thumbHeightPx.toDp() })
                    .offset(y = with(density) { (progress * (viewportPx - thumbHeightPx)).toDp() })
                    .background(Color(0xFF90A4AE), RoundedCornerShape(2.dp)),
            )
        }
    }
}

@Composable
private fun GridShortcutCard(item: GridShortcut, isDeleteMode: Boolean, onClick: () -> Unit) {
    // Keys with no KEY_TABLE entry cannot be sent. Previously these were dropped at
    // load time; now the card stays and says so, so the gap is visible instead of
    // silently losing the shortcut or doing nothing when tapped.
    val unmappable = item.combo.filter { k -> !KEY_TABLE.containsKey(k) }
    val broken = unmappable.isNotEmpty()

    Surface(
        shape = RoundedCornerShape(6.dp),
        color = if (isDeleteMode) Color(0xCCB71C1C) else CardFace,
        border = BorderStroke(1.dp, when {
            isDeleteMode -> DangerRed
            broken -> Color(0xFFCC8800)
            else -> CardBorder
        }),
        modifier = Modifier.fillMaxWidth().height(36.dp).clickable(onClick = onClick),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            modifier = Modifier.padding(horizontal = 8.dp),
        ) {
            Icon(
                imageVector = if (isDeleteMode) Icons.Default.Clear else item.icon,
                contentDescription = null,
                tint = Color.White,
                modifier = Modifier.size(13.dp),
            )
            Column(verticalArrangement = Arrangement.Center) {
                Text(
                    text = item.label,
                    color = Color.White,
                    fontSize = 10.sp,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    text = when {
                        isDeleteMode -> "Borrar"
                        broken -> "Sin mapear: " + unmappable.joinToString("+")
                        else -> item.keyHint
                    },
                    color = if (broken) Color(0xFFCC8800) else Color(0xFF90A4AE),
                    fontSize = 8.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Composer: pick the keys, then name the shortcut
// ---------------------------------------------------------------------------

/**
 * Key-combo composer, as a full-screen modal Dialog.
 *
 * Dialog rather than a second WindowManager window: it brings its own window, and
 * while it is up the viewport is deliberately inert, which is what you want while
 * picking a combination.
 */
@Composable
private fun VisualKeyboard(
    selectedKeyIds: List<String>,
    onKeyToggle: (String) -> Unit,
    onClearAll: () -> Unit,
    onConfirm: (List<String>) -> Unit,
    onDismiss: () -> Unit,
    /** Modifier key ids latched on: they stay in the combination until untoggled. */
    latchedModifierIds: List<String>,
    onToggleLatch: () -> Unit,
) {
    val keyLookup = remember { mutableStateListOf<Pair<String, String>>() }
    if (keyLookup.isEmpty()) {
        ALL_KEY_ITEMS.forEach { keyLookup.add(it.id to it.label) }
    }
    val lookup = keyLookup.associate { it.first to it.second }

    val comboString = selectedKeyIds.mapNotNull { lookup[it] }.distinct().joinToString("+")

    /* Drag the sheet up and down by its handle. The sheet lives in a Dialog, so it is not
     * a window that can be moved the way the panel is; translating the surface inside its
     * own bounds is what keeps the handle useful without a second window. */
    var dragOffset by remember { mutableStateOf(0f) }
    var dragStart by remember { mutableFloatStateOf(0f) }
    val density = LocalDensity.current
    val maxDrag = with(density) { 160.dp.toPx() }

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color(0xCC000000))
                .clickable(onClick = onDismiss),
            contentAlignment = Alignment.BottomCenter,
        ) {
            Surface(
                // clickable(enabled=false) keeps taps off the scrim: the outer Box
                // owns dismissal, the sheet itself must not bubble into it.
                modifier = Modifier
                    .fillMaxWidth()
                    .offset { IntOffset(0, dragOffset.roundToInt()) }
                    .clickable(enabled = false) {},
                color = Color(0xFF14171C),
                shape = RoundedCornerShape(topStart = 14.dp, topEnd = 14.dp),
                border = BorderStroke(1.dp, Color(0xFF28303D)),
            ) {
                Column(
                    modifier = Modifier.fillMaxWidth().padding(8.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Row(
                            modifier = Modifier.weight(1f),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                        ) {
                            Text(
                                "KEY COMBO:",
                                color = Color(0xFF6C7D93),
                                fontSize = 10.sp,
                                fontWeight = FontWeight.Bold,
                            )
                            if (selectedKeyIds.isEmpty()) {
                                Text(
                                    "Toca teclas para armar combinación...",
                                    color = Color(0xFF4A5568),
                                    fontSize = 11.sp,
                                )
                            } else {
                                LazyRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                    items(
                                        selectedKeyIds.mapNotNull { lookup[it] }.distinct()
                                    ) { label ->
                                        Surface(shape = RoundedCornerShape(4.dp), color = Amber) {
                                            Text(
                                                label,
                                                color = Color.Black,
                                                fontSize = 10.sp,
                                                fontWeight = FontWeight.ExtraBold,
                                                modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                            )
                                        }
                                    }
                                }
                            }
                        }

                        Row(
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            /* Drag handle: lets the sheet be pushed up or down, since a
                             * Dialog surface cannot be repositioned like the panel window. */
                            Box(
                                modifier = Modifier
                                    .width(26.dp)
                                    .height(24.dp)
                                    .pointerInput(Unit) {
                                        detectDragGestures(
                                            onDragStart = { dragStart = dragOffset },
                                            onDragEnd = { dragOffset = 0f },
                                            onDragCancel = { dragOffset = 0f },
                                        ) { change, amount ->
                                            change.consume()
                                            dragOffset = (dragStart + amount.y).coerceIn(0f, maxDrag)
                                        }
                                    },
                                contentAlignment = Alignment.Center,
                            ) {
                                Box(
                                    modifier = Modifier
                                        .width(18.dp)
                                        .height(3.dp)
                                        .background(Color(0xFF4A5568), RoundedCornerShape(2.dp)),
                                )
                            }
                            if (selectedKeyIds.isNotEmpty()) {
                                TextButton(onClick = onClearAll) {
                                    Text("Limpiar", color = DangerRed, fontSize = 10.sp)
                                }
                            }
                            /* Latching modifier. Tapping Shift normally means Shift+<next key>,
                             * but on a touch composer you often want Shift held while you pick
                             * several keys in a row. This toggles it as latched instead: it stays
                             * in the combination until tapped again, so Ctrl+Shift+A can be built
                             * as Ctrl, latch Shift, A in any order. */
                            val latched = latchedModifierIds.isNotEmpty()
                            Surface(
                                shape = RoundedCornerShape(6.dp),
                                color = if (latched) Amber else CardFace,
                                border = BorderStroke(1.dp, if (latched) Amber else CardBorder),
                            ) {
                                Box(
                                    modifier = Modifier
                                        .clickable { onToggleLatch() }
                                        .padding(horizontal = 8.dp, vertical = 6.dp),
                                    contentAlignment = Alignment.Center,
                                ) {
                                    Text(
                                        "Mantener",
                                        color = if (latched) Color.Black else Color(0xFF90A4AE),
                                        fontSize = 10.sp,
                                        fontWeight = FontWeight.Bold,
                                    )
                                }
                            }
                            Surface(
                                shape = RoundedCornerShape(6.dp),
                                color = if (selectedKeyIds.isNotEmpty()) Teal else CardFace,
                            ) {
                                Box(
                                    modifier = Modifier
                                        .clickable(enabled = selectedKeyIds.isNotEmpty()) {
                                            // toList(): this is the live keyboard
                                            // selection. Handing over the
                                            // reference made a saved shortcut
                                            // share state with the next key
                                            // tapped, so adding a shortcut
                                            // rewrote the previous one's combo
                                            // and the duplicate check below
                                            // then matched and replaced it.
                                            onConfirm(selectedKeyIds.toList() + latchedModifierIds.toList())
                                        }
                                        .padding(horizontal = 16.dp, vertical = 6.dp),
                                    contentAlignment = Alignment.Center,
                                ) {
                                    Text(
                                        "OK",
                                        color = if (selectedKeyIds.isNotEmpty()) Color.White else Color.Gray,
                                        fontSize = 11.sp,
                                        fontWeight = FontWeight.Bold,
                                    )
                                }
                            }
                            Box(
                                modifier = Modifier
                                    .size(28.dp)
                                    .background(CardFace, RoundedCornerShape(6.dp))
                                    .clickable(onClick = onDismiss),
                                contentAlignment = Alignment.Center,
                            ) {
                                Icon(
                                    Icons.Default.Close,
                                    contentDescription = "Close",
                                    tint = PanelHeader,
                                    modifier = Modifier.size(16.dp),
                                )
                            }
                        }
                    }

                    Divider(color = Color(0xFF222731), thickness = 1.dp)

                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalAlignment = Alignment.Bottom,
                    ) {
                        Column(
                            modifier = Modifier.weight(1.8f),
                            verticalArrangement = Arrangement.spacedBy(3.dp),
                        ) {
                            KEY_ROWS.forEach { row ->
                                ProportionalKeyRow(
                                    rowModifier = Modifier.fillMaxWidth(),
                                    keys = row,
                                    selectedKeyIds = selectedKeyIds,
                                    onKeyToggle = onKeyToggle,
                                )
                            }
                        }
                        Box(
                            modifier = Modifier.width(1.dp).height(195.dp).background(Color(0xFF222731))
                        )
                        Column(
                            modifier = Modifier.weight(1f),
                            verticalArrangement = Arrangement.spacedBy(3.dp),
                        ) {
                            NUMPAD_ROWS.forEach { row ->
                                ProportionalKeyRow(
                                    rowModifier = Modifier.fillMaxWidth(),
                                    keys = row,
                                    selectedKeyIds = selectedKeyIds,
                                    onKeyToggle = onKeyToggle,
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

/** Caller passes [rowModifier]; weight() only resolves inside a Row receiver. */
@Composable
private fun ProportionalKeyRow(
    rowModifier: Modifier,
    keys: List<KeyItem>,
    selectedKeyIds: List<String>,
    onKeyToggle: (String) -> Unit,
) {
    Row(
        modifier = rowModifier,
        horizontalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        keys.forEach { key ->
            val isSelected = selectedKeyIds.contains(key.id)
            val (bg, border, text) = when {
                isSelected -> Triple(Amber, Color(0xFFFFB74D), Color.Black)
                key.type == KeyType.MODIFIER -> Triple(ChromeFace, Color(0xFF334155), Color(0xFF93C5FD))
                key.type == KeyType.ACTION -> Triple(CardFace, Color(0xFF3B4454), Color(0xFFCBD5E1))
                key.type == KeyType.NUMPAD -> Triple(Color(0xFF1B2430), Color(0xFF2D3748), Color(0xFFE2E8F0))
                else -> Triple(Color(0xFF20252E), Color(0xFF2E3644), Color(0xFFE2E8F0))
            }
            Surface(
                shape = RoundedCornerShape(4.dp),
                color = bg,
                border = BorderStroke(1.dp, border),
                modifier = Modifier
                    .weight(key.weight)
                    .height(30.dp)
                    .clickable { onKeyToggle(key.id) },
            ) {
                Box(contentAlignment = Alignment.Center, modifier = Modifier.fillMaxSize()) {
                    Text(
                        text = key.label,
                        color = text,
                        fontSize = if (key.label.length > 3) 8.sp else 9.sp,
                        fontWeight = if (isSelected || key.type == KeyType.MODIFIER) {
                            FontWeight.Bold
                        } else {
                            FontWeight.Medium
                        },
                        textAlign = TextAlign.Center,
                        maxLines = 1,
                    )
                }
            }
        }
    }
}

@Composable
private fun SaveShortcutNameDialog(
    keyCombo: String,
    onDismiss: () -> Unit,
    onSave: (String) -> Unit,
) {
    var name by remember { mutableStateOf("") }

    Dialog(onDismissRequest = onDismiss) {
        Surface(
            shape = RoundedCornerShape(12.dp),
            color = PanelBackground,
            border = BorderStroke(1.dp, PanelBorder),
            modifier = Modifier.width(280.dp).padding(8.dp),
        ) {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(
                    "GUARDAR ATAJO",
                    color = Color.White,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                )

                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Text("Combinación:", color = PanelHeader, fontSize = 11.sp)
                    Surface(shape = RoundedCornerShape(4.dp), color = Amber) {
                        Text(
                            keyCombo,
                            color = Color.Black,
                            fontSize = 10.sp,
                            fontWeight = FontWeight.ExtraBold,
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                        )
                    }
                }

                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it },
                    placeholder = { Text("Ej. Extrude Face", fontSize = 11.sp, color = Color(0xFF526070)) },
                    singleLine = true,
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = Teal,
                        unfocusedBorderColor = PanelBorder,
                        focusedTextColor = Color.White,
                        unfocusedTextColor = Color.White,
                        cursorColor = Teal,
                    ),
                    modifier = Modifier.fillMaxWidth(),
                )

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    TextButton(onClick = onDismiss) {
                        Text("Cancelar", color = PanelHeader, fontSize = 11.sp)
                    }
                    Spacer(modifier = Modifier.width(4.dp))
                    Button(
                        onClick = { if (name.isNotBlank()) onSave(name) },
                        colors = ButtonDefaults.buttonColors(containerColor = Teal),
                        shape = RoundedCornerShape(6.dp),
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp),
                    ) {
                        Text("Guardar", color = Color.White, fontSize = 11.sp)
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Keyboard layout tables
// ---------------------------------------------------------------------------

private fun key(id: String, label: String, type: KeyType = KeyType.NORMAL, weight: Float = 1f) =
    KeyItem(id, label, weight, type)

private val KEY_ROWS: List<List<KeyItem>> = listOf(
    listOf(
        key("esc", "Esc", KeyType.ACTION), key("del", "Del", KeyType.ACTION),
        key("f1", "F1"), key("f2", "F2"), key("f3", "F3"), key("f4", "F4"), key("f5", "F5"),
        key("f6", "F6"), key("f7", "F7"), key("f8", "F8"), key("f9", "F9"), key("f10", "F10"),
        key("f11", "F11"), key("f12", "F12"),
    ),
    listOf(
        key("backtick", "`"), key("1", "1"), key("2", "2"), key("3", "3"), key("4", "4"),
        key("5", "5"), key("6", "6"), key("7", "7"), key("8", "8"), key("9", "9"), key("0", "0"),
        key("minus", "-"), key("equals", "="),
        key("bksp", "Bksp", KeyType.ACTION, 1.4f),
    ),
    listOf(
        key("tab", "Tab", KeyType.ACTION, 1.2f),
        key("q", "Q"), key("w", "W"), key("e", "E"), key("r", "R"), key("t", "T"), key("y", "Y"),
        key("u", "U"), key("i", "I"), key("o", "O"), key("p", "P"),
        key("bracket_l", "["), key("bracket_r", "]"), key("backslash", "\\"),
    ),
    listOf(
        key("caps", "Caps", KeyType.ACTION, 1.3f),
        key("a", "A"), key("s", "S"), key("d", "D"), key("f", "F"), key("g", "G"), key("h", "H"),
        key("j", "J"), key("k", "K"), key("l", "L"),
        key("semicolon", ";"), key("quote", "'"),
        key("enter", "Enter", KeyType.ACTION, 1.6f),
    ),
    listOf(
        key("shift_l", "Shift", KeyType.MODIFIER, 1.6f),
        key("z", "Z"), key("x", "X"), key("c", "C"), key("v", "V"), key("b", "B"),
        key("n", "N"), key("m", "M"),
        key("comma", ","), key("dot", "."), key("slash", "/"),
        key("shift_r", "Shift", KeyType.MODIFIER, 1.6f),
    ),
    listOf(
        key("ctrl_l", "Ctrl", KeyType.MODIFIER, 1.3f),
        key("alt_l", "Alt", KeyType.MODIFIER, 1.2f),
        key("space", "Space", weight = 3.5f),
        key("left", "<-"), key("up", "^"), key("down", "v"), key("right", "->"),
        key("alt_r", "Alt", KeyType.MODIFIER, 1.2f),
        key("ctrl_r", "Ctrl", KeyType.MODIFIER, 1.3f),
    ),
)

private val NUMPAD_ROWS: List<List<KeyItem>> = listOf(
    listOf(
        key("np_7", "7", KeyType.NUMPAD), key("np_8", "8", KeyType.NUMPAD),
        key("np_9", "9", KeyType.NUMPAD), key("np_div", "/", KeyType.NUMPAD),
    ),
    listOf(
        key("np_4", "4", KeyType.NUMPAD), key("np_5", "5", KeyType.NUMPAD),
        key("np_6", "6", KeyType.NUMPAD), key("np_mul", "*", KeyType.NUMPAD),
    ),
    listOf(
        key("np_1", "1", KeyType.NUMPAD), key("np_2", "2", KeyType.NUMPAD),
        key("np_3", "3", KeyType.NUMPAD), key("np_sub", "-", KeyType.NUMPAD),
    ),
    listOf(
        key("np_0", "0", KeyType.NUMPAD, 2f), key("np_dot", ".", KeyType.NUMPAD),
        key("np_add", "+", KeyType.NUMPAD),
        key("np_enter", "Enter", KeyType.ACTION, 1.3f),
    ),
)

private val ALL_KEY_ITEMS: List<KeyItem> = (KEY_ROWS + NUMPAD_ROWS).flatten()
