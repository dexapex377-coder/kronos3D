// SPDX-FileCopyrightText: 2026 Kronos3D Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

package com.kronos3d.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.PixelFormat
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import androidx.lifecycle.ProcessLifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistry
import androidx.savedstate.SavedStateRegistryController
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import androidx.core.os.bundleOf
import kotlin.math.roundToInt

/**
 * The floating shortcuts panel: a draggable window that lives above Blender's viewport.
 *
 * ## Why a WindowManager window and not addContentView
 *
 * [BlenderActivity] extends `NativeActivity`, which hands its entire surface to
 * `android_main`. A ComposeView added to its content view therefore sits *under* the
 * Vulkan output, and a full-screen one would additionally take every touch away from
 * the viewport: Compose returns true from onTouchEvent on ACTION_DOWN and then owns
 * the whole gesture. A `WindowManager` window is a separate window entirely, so it
 * floats above the native surface and, with [WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL],
 * leaves touches outside its own bounds to the windows underneath.
 *
 * ## The lifecycle wiring is not optional
 *
 * A ComposeView created by hand has no LifecycleOwner and no SavedStateRegistryOwner,
 * and it crashes on first composition without them. [ProcessLifecycleOwner] plus a
 * local [SavedStateRegistryOwner] is the smallest thing that works; the panel is a
 * window, not an Activity, so there is no ComponentActivity to inherit from.
 */
object ShortcutsOverlay {

    @Volatile
    private var panelView: View? = null

    @Volatile
    private var keyboardView: View? = null

    @Volatile
    private var triggerView: View? = null

    /** True while either window is on screen. */
    val isShowing: Boolean
        get() = panelView != null || keyboardView != null

    @JvmStatic
    fun toggle(activity: BlenderActivity) {
        if (isShowing) {
            hide()
        } else {
            show(activity)
        }
    }

    /**
     * Put the small always-on button that opens the panel.
     *
     * This is a WindowManager window rather than a child of the activity's content
     * view for the same reason the panel is: on a NativeActivity the native surface
     * is drawn over the content view, so a view added there is invisible. Blender's
     * own viewport already has a controls strip along the bottom and a header along
     * the top, so this sits on the right edge, out of both.
     *
     * [BlenderActivity] calls this once the surface exists and drops it again when the
     * activity goes away, so the button does not outlive the window it belongs to.
     */
    @JvmStatic
    fun installTrigger(activity: BlenderActivity) {
        if (triggerView != null) {
            return
        }
        val windowManager =
            activity.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val density = activity.resources.displayMetrics.density
        val sizePx = (44 * density).roundToInt()

        val view = ComposeView(activity).apply {
            setViewTreeLifecycleOwner(ProcessLifecycleOwner.get())
            setContent {
                MaterialTheme {
                    Box(
                        modifier = Modifier
                            .size(sizePx.dp)
                            .background(PanelBackground, RoundedCornerShape(10.dp))
                            .border(1.dp, PanelBorder, RoundedCornerShape(10.dp))
                            .clickable { toggle(activity) },
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            "K3D",
                            color = PanelHeader,
                            fontSize = 11.sp,
                            fontWeight = FontWeight.Bold,
                        )
                    }
                }
            }
        }

        val params = WindowManager.LayoutParams(
            sizePx,
            sizePx,
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

        runCatching { windowManager.addView(view, params) }
            .onSuccess { triggerView = view }
    }

    @JvmStatic
    fun uninstallTrigger() {
        removeWindow(triggerView)
        triggerView = null
    }

    @JvmStatic
    fun show(activity: BlenderActivity) {
        if (isShowing) {
            return
        }
        val windowManager =
            activity.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        panelView = addComposeWindow(activity, windowManager) {
            ShortcutsPanel(
                onDismiss = { hide() },
                onSendKey = { keyCode, meta -> activity.sendShortcutKey(keyCode, meta) },
                onToggleKeyboard = { showKeyboard(activity) },
            )
        }
    }

    @JvmStatic
    fun hide() {
        removeWindow(panelView)
        panelView = null
        removeWindow(keyboardView)
        keyboardView = null
    }

    /**
     * The visual keyboard, in its own window so that it can sit at the bottom of the
     * screen while the panel stays where the user dragged it.
     */
    private fun showKeyboard(activity: BlenderActivity) {
        if (keyboardView != null) {
            removeWindow(keyboardView)
            keyboardView = null
            return
        }
        val windowManager =
            activity.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        keyboardView = addComposeWindow(
            activity,
            windowManager,
            gravity = Gravity.BOTTOM,
            heightDp = 260,
        ) {
            VisualKeyboard(
                onDismiss = {
                    removeWindow(keyboardView)
                    keyboardView = null
                },
                onKey = { keyCode, meta ->
                    activity.sendShortcutKey(keyCode, meta)
                },
            )
        }
    }

    /**
     * Wrap [content] in a ComposeView and put it in a floating window.
     *
     * [heightDp] null means "wrap content", which is what the panel wants: its window
     * has to be exactly as tall as the panel or the transparent window would eat
     * touches below it.
     */
    private fun addComposeWindow(
        activity: BlenderActivity,
        windowManager: WindowManager,
        gravity: Int = Gravity.TOP or Gravity.START,
        heightDp: Int? = null,
        content: @Composable () -> Unit,
    ): View {
        val density = activity.resources.displayMetrics.density
        val widthPx = (PANEL_WIDTH_DP * density).roundToInt()
        val heightPx = heightDp?.let { (it * density).roundToInt() }

        val lifecycleOwner = OverlayLifecycleOwner()
        lifecycleOwner.dispatch(Lifecycle.Event.ON_CREATE)

        val view = ComposeView(activity).apply {
            setViewTreeLifecycleOwner(lifecycleOwner)
            setViewTreeSavedStateRegistryOwner(lifecycleOwner)
            // Touch handling is per-element inside the composables; the window itself
            // must not be clickable or it swallows events it does not draw.
            isClickable = false
            setBackgroundColor(PixelFormat.TRANSPARENT)
            setContent {
                MaterialTheme {
                    content()
                }
            }
        }

        val params = WindowManager.LayoutParams(
            widthPx,
            heightPx ?: WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
            // NOT_TOUCH_MODAL is the load-bearing flag: without it this window would
            // take every touch on the screen, and the viewport would go dead.
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSPARENT,
        ).apply {
            x = if (gravity and Gravity.START != 0) {
                // Default to the right edge with a margin, out of the way of the
                // touch controls Blender puts on the left.
                val screenWidth = activity.resources.displayMetrics.widthPixels
                screenWidth - widthPx - (16 * density).roundToInt()
            } else {
                0
            }
            y = (56 * density).roundToInt()
            this.gravity = gravity
            softInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING
        }

        windowManager.addView(view, params)
        return view
    }

    private fun removeWindow(view: View?) {
        if (view == null) {
            return
        }
        val windowManager =
            view.context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        try {
            windowManager.removeViewImmediate(view)
        } catch (_: IllegalArgumentException) {
            // Already detached; nothing to do.
        }
    }

    /**
     * Minimal LifecycleOwner + SavedStateRegistryOwner for a ComposeView that has no
     * Activity behind it.
     *
     * The saved-state registry is created but never actually used, because the panel
     * holds no state that must survive a process death; it exists only because Compose
     * reads it during composition.
     */
    private class OverlayLifecycleOwner : LifecycleOwner, SavedStateRegistryOwner {
        private val lifecycleRegistry = LifecycleRegistry(this)
        private val savedStateController = SavedStateRegistryController.create(this)

        init {
            savedStateController.performAttach()
            savedStateController.performRestore(bundleOf())
        }

        override val lifecycle: Lifecycle get() = lifecycleRegistry

        override val savedStateRegistry: SavedStateRegistry
            get() = savedStateController.savedStateRegistry

        fun dispatch(event: Lifecycle.Event) {
            lifecycleRegistry.handleLifecycleEvent(event)
        }
    }
}

// ---------------------------------------------------------------------------
// Colours. Match the launcher's palette so the two screens do not look like
// different apps.
// ---------------------------------------------------------------------------

/** Panel width. Short enough to keep the viewport readable. */
private const val PANEL_WIDTH_DP = 230

private val PanelBackground = Color(0xF2181B20)
private val PanelBorder = Color(0xFF2E3543)
private val PanelHeader = Color(0xFF8A99AD)
private val AccentBlue = Color(0xFF4C8DFF)
private val DangerRed = Color(0xFFFF5252)
private val KeyFace = Color(0xFF262C36)
private val KeyFacePressed = Color(0xFF3A4657)
private val KeyLabel = Color(0xFFDCE3EC)

// ---------------------------------------------------------------------------
// Shortcut model
// ---------------------------------------------------------------------------

/**
 * One entry in the grid.
 *
 * [id] is a stable string rather than a random UUID: a default of
 * UUID.randomUUID() makes every recomposition produce a new key, so Compose throws
 * the whole list away and re-creates it, and any edit is lost.
 */
private data class Shortcut(
    val id: String,
    val label: String,
    val keyHint: String,
    /** Android keycode; dispatched through BlenderActivity.sendShortcutKey. */
    val keyCode: Int,
    /** KeyEvent metaState bits, so ctrl/alt shortcuts are distinguishable. */
    val metaState: Int = 0,
)

/** The default grid: things that are awkward with one finger on a phone. */
private val DEFAULT_SHORTCUTS: List<Shortcut> = listOf(
    Shortcut("undo", "Undo", "Z", KeyEvent.KEYCODE_Z),
    Shortcut("redo", "Redo", "Shift+Z", KeyEvent.KEYCODE_Z, KeyEvent.META_SHIFT_ON),
    Shortcut("delete", "Delete", "Del", KeyEvent.KEYCODE_FORWARD_DEL),
    Shortcut("duplicate", "Duplicate", "Shift+D", KeyEvent.KEYCODE_D, KeyEvent.META_SHIFT_ON),
    Shortcut("play", "Play", "Space", KeyEvent.KEYCODE_SPACE),
    Shortcut("frame_next", "Next Frame", "Down", KeyEvent.KEYCODE_DPAD_DOWN),
    Shortcut("frame_prev", "Prev Frame", "Up", KeyEvent.KEYCODE_DPAD_UP),
    Shortcut("view_selected", "Frame Sel", "Numpad .", KeyEvent.KEYCODE_NUMPAD_DOT),
    Shortcut("top_view", "Top", "Numpad 7", KeyEvent.KEYCODE_NUMPAD_7),
    Shortcut("front_view", "Front", "Numpad 1", KeyEvent.KEYCODE_NUMPAD_1),
    Shortcut("right_view", "Right", "Numpad 3", KeyEvent.KEYCODE_NUMPAD_3),
    Shortcut("toggle_xray", "X-Ray", "Alt+Z", KeyEvent.KEYCODE_Z, KeyEvent.META_ALT_ON),
)

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

@Composable
private fun ShortcutsPanel(
    onDismiss: () -> Unit,
    onSendKey: (Int, Int) -> Unit,
    onToggleKeyboard: () -> Unit,
) {
    val configuration = LocalConfiguration.current
    val density = LocalDensity.current
    val panelWidthPx = with(density) { PANEL_WIDTH_DP.dp.toPx() }
    val screenWidthPx = with(density) { configuration.screenWidthDp.dp.toPx() }
    val marginPx = with(density) { 16.dp.toPx() }

    var offsetX by remember { mutableStateOf(screenWidthPx - panelWidthPx - marginPx) }
    var offsetY by remember { mutableStateOf(with(density) { 64.dp.toPx() }) }
    var isMinimized by remember { mutableStateOf(false) }
    val shortcuts = remember { mutableStateListOf<Shortcut>().apply { addAll(DEFAULT_SHORTCUTS) } }

    Box(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier
                .offset { IntOffset(offsetX.roundToInt(), offsetY.roundToInt()) }
                .width(PANEL_WIDTH_DP.dp)
                .background(PanelBackground, RoundedCornerShape(12.dp))
                .border(1.dp, PanelBorder, RoundedCornerShape(12.dp))
                .padding(8.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            // Header doubles as the drag handle. Only this row consumes the gesture,
            // so dragging the panel cannot fight the buttons below it.
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .pointerInput(Unit) {
                        detectDragGestures { change, drag ->
                            change.consume()
                            offsetX = (offsetX + drag.x)
                                .coerceIn(marginPx, screenWidthPx - panelWidthPx - marginPx)
                            offsetY = (offsetY + drag.y)
                                .coerceIn(marginPx, with(density) { configuration.screenHeightDp.dp.toPx() } - 150f)
                        }
                    },
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = if (isMinimized) "K3D" else "KRONOS SHORTCUTS",
                    color = PanelHeader,
                    fontSize = 9.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.clickable { isMinimized = !isMinimized },
                )
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    HeaderButton("KB") { onToggleKeyboard() }
                    HeaderButton("✕") { onDismiss() }
                }
            }

            if (!isMinimized) {
                LazyVerticalGrid(
                    // 2 columns: at 230dp, 3 would leave each card ~66dp, too narrow
                    // for the labels.
                    columns = GridCells.Fixed(2),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                    modifier = Modifier.height(340.dp),
                ) {
                    items(shortcuts, key = { it.id }) { shortcut ->
                        ShortcutCard(shortcut) {
                            onSendKey(shortcut.keyCode, shortcut.metaState)
                        }
                    }
                }

                Text(
                    text = "Teclas reales del keymap: se pueden reasignar en Preferencias.",
                    color = PanelHeader,
                    fontSize = 8.sp,
                    modifier = Modifier.padding(top = 2.dp),
                )
            }
        }
    }
}

@Composable
private fun HeaderButton(label: String, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .size(width = 26.dp, height = 18.dp)
            .background(KeyFace, RoundedCornerShape(4.dp))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = KeyLabel, fontSize = 9.sp, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun ShortcutCard(shortcut: Shortcut, onClick: () -> Unit) {
    Column(
        modifier = Modifier
            .background(KeyFace, RoundedCornerShape(8.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 6.dp, vertical = 8.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            text = shortcut.label,
            color = KeyLabel,
            fontSize = 10.sp,
            fontWeight = FontWeight.Medium,
            maxLines = 1,
        )
        Spacer(Modifier.height(2.dp))
        Text(text = shortcut.keyHint, color = AccentBlue, fontSize = 8.sp, maxLines = 1)
    }
}

// ---------------------------------------------------------------------------
// Visual keyboard
// ---------------------------------------------------------------------------

/**
 * A compact QWERTY block plus the modifier row.
 *
 * Every key is dispatched as an Android keycode through
 * BlenderActivity.sendShortcutKey, so it lands in Blender's keymap and modifier
 * combinations come for free: the modifier row is a held-state latch in this
 * composable, not an Android meta-state modifier, because a floating window cannot
 * receive key events to derive metaState from.
 */
@Composable
private fun VisualKeyboard(
    onDismiss: () -> Unit,
    onKey: (Int, Int) -> Unit,
) {
    var shiftLatch by remember { mutableStateOf(false) }
    var ctrlLatch by remember { mutableStateOf(false) }
    var altLatch by remember { mutableStateOf(false) }

    fun metaState(): Int {
        var meta = 0
        if (ctrlLatch) meta = meta or KeyEvent.META_CTRL_ON
        if (altLatch) meta = meta or KeyEvent.META_ALT_ON
        return meta
    }

    fun press(keyCode: Int) {
        onKey(keyCode, metaState())
        if (shiftLatch) {
            shiftLatch = false
        }
    }

    Column(
        modifier = Modifier
            .background(PanelBackground, RoundedCornerShape(topStart = 14.dp, topEnd = 14.dp))
            .border(1.dp, PanelBorder, RoundedCornerShape(topStart = 14.dp, topEnd = 14.dp))
            .padding(6.dp),
        verticalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(3.dp),
        ) {
            ModifierKey("Shift", shiftLatch) { shiftLatch = !shiftLatch }
            ModifierKey("Ctrl", ctrlLatch) { ctrlLatch = !ctrlLatch }
            ModifierKey("Alt", altLatch) { altLatch = !altLatch }
            Spacer(Modifier.weight(1f))
            ModifierKey("Hide", false, onDismiss)
        }

        KEY_ROWS.forEach { row ->
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(3.dp),
            ) {
                row.forEach { (label, keyCode) ->
                    KeyCap(label) { press(keyCode) }
                }
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(3.dp),
        ) {
            KeyCap("⌫", Modifier.width(48.dp)) { press(KeyEvent.KEYCODE_DEL) }
            KeyCap("Tab", Modifier.weight(1f)) { press(KeyEvent.KEYCODE_TAB) }
            KeyCap("⏎", Modifier.weight(1f)) { press(KeyEvent.KEYCODE_ENTER) }
        }
    }
}

@Composable
private fun RowScope.ModifierKey(
    label: String,
    active: Boolean,
    onClick: () -> Unit,
) {
    Box(
        modifier = Modifier
            .weight(1f)
            .height(26.dp)
            .background(if (active) AccentBlue else KeyFace, RoundedCornerShape(5.dp))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            label,
            color = if (active) Color.White else KeyLabel,
            fontSize = 9.sp,
            fontWeight = FontWeight.SemiBold,
        )
    }
}

@SuppressLint("ModifierParameter")
@Composable
private fun RowScope.KeyCap(
    label: String,
    modifier: Modifier = Modifier.weight(1f),
    onClick: () -> Unit,
) {
    Box(
        modifier = modifier
            .height(30.dp)
            .background(KeyFace, RoundedCornerShape(5.dp))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = KeyLabel, fontSize = 11.sp, fontWeight = FontWeight.Medium)
    }
}

/** Three rows of character keys, as Android keycodes. */
private val KEY_ROWS: List<List<Pair<String, Int>>> = listOf(
    listOf(
        "1" to KeyEvent.KEYCODE_1, "2" to KeyEvent.KEYCODE_2, "3" to KeyEvent.KEYCODE_3,
        "4" to KeyEvent.KEYCODE_4, "5" to KeyEvent.KEYCODE_5, "6" to KeyEvent.KEYCODE_6,
        "7" to KeyEvent.KEYCODE_7, "8" to KeyEvent.KEYCODE_8, "9" to KeyEvent.KEYCODE_9,
        "0" to KeyEvent.KEYCODE_0,
    ),
    listOf(
        "Q" to KeyEvent.KEYCODE_Q, "W" to KeyEvent.KEYCODE_W, "E" to KeyEvent.KEYCODE_E,
        "R" to KeyEvent.KEYCODE_R, "T" to KeyEvent.KEYCODE_T, "Y" to KeyEvent.KEYCODE_Y,
        "U" to KeyEvent.KEYCODE_U, "I" to KeyEvent.KEYCODE_I, "O" to KeyEvent.KEYCODE_O,
        "P" to KeyEvent.KEYCODE_P,
    ),
    listOf(
        "A" to KeyEvent.KEYCODE_A, "S" to KeyEvent.KEYCODE_S, "D" to KeyEvent.KEYCODE_D,
        "F" to KeyEvent.KEYCODE_F, "G" to KeyEvent.KEYCODE_G, "H" to KeyEvent.KEYCODE_H,
        "J" to KeyEvent.KEYCODE_J, "K" to KeyEvent.KEYCODE_K, "L" to KeyEvent.KEYCODE_L,
    ),
    listOf(
        "Z" to KeyEvent.KEYCODE_Z, "X" to KeyEvent.KEYCODE_X, "C" to KeyEvent.KEYCODE_C,
        "V" to KeyEvent.KEYCODE_V, "B" to KeyEvent.KEYCODE_B, "N" to KeyEvent.KEYCODE_N,
        "M" to KeyEvent.KEYCODE_M,
    ),
)