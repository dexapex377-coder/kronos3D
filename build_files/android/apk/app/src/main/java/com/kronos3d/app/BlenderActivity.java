/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

package com.kronos3d.app;

import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.system.Os;
import android.text.InputType;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * NativeActivity subclass for Blender. Extracts the bundled runtime
 * (Python + scripts + datafiles) on first launch, then loads libblender.so
 * and bridges soft-keyboard IME text to native. Orientation follows the
 * device: see screenOrientation in the manifest.
 */
public class BlenderActivity extends NativeActivity {

  /* NativeActivity dlopen()s the library from native code, which never registers
   * it with the class loader, so the JNI lookup for the native methods below
   * fails with UnsatisfiedLinkError. Load it here as well to register it. */
  static {
    System.loadLibrary("blender");
  }

  /* Must match GHOST_SystemPathsAndroid: <filesDir>/blender/<version>. */
  private static final String VERSION = "5.3";
  /* Only a fallback now, for a payload built before package.sh started emitting
   * RUNTIME_REV. The real revision is a hash of the archive, so it changes by itself
   * whenever the packaged runtime does -- a hand-bumped constant was silently
   * forgotten, leaving edited Python scripts stranded in the APK while the device kept
   * running the copy it unpacked months earlier. */
  private static final String RUNTIME_REVISION = "py313-ui1-net3-pip-assets1";
  private static final String RUNTIME_ZIP = "blender_runtime.zip";
  private static final String RUNTIME_REV = "blender_runtime.rev";

  private static final String TAG = "blender";
  /* Must match build_files/android/deps/build.sh and package.sh. */
  private static final String PYTHON_VERSION = "3.13";
  /* Only used to fill in pyvenv.cfg; CPython does not validate it. */
  private static final String PYTHON_FULL_VERSION = "3.13.13";
  private static final String PYTHON_BIN_LIB = "libpython3_13_bin.so";

  private InputView inputView;

  private native void nativeOnCommitText(String text);
  private native void nativeOnKey(int keycode, int action, int metaState);
  private native void nativeOpenMainFile(String path);

  /**
   * Send a key press/release pair on behalf of the shortcuts overlay.
   *
   * <p>{@link #nativeOnKey} is private native, so this is the only door into it from
   * Kotlin. It routes through GHOST_SystemAndroid::handleJavaKeyEvent, which converts
   * the Android keycode and pushes a real GHOST_kEventKeyDown/Up: the shortcut lands
   * in Blender's own keymap, so it is subject to modifiers and can be rebound from
   * Preferences, exactly like a key on a physical keyboard.
   *
   * <p>Safe to call before the surface exists: the native side queues under
   * java_input_mutex_ and drainJavaInput() drops events with no window attached.
   */
  public void sendShortcutKey(int keyCode, int metaState) {
    if (keyCode == 0) {
      /* Modifier-only combination. Blender has no standalone modifier key to press,
       * and sending keyCode 0 would be a key event with no key. */
      return;
    }
    nativeOnKey(keyCode, KeyEvent.ACTION_DOWN, metaState);
    nativeOnKey(keyCode, KeyEvent.ACTION_UP, metaState);
  }

  /**
   * Toggle the floating shortcuts panel.
   *
   * <p>The panel is a WindowManager window rather than a child of this activity's
   * content view, because {@code NativeActivity} hands its whole surface to
   * android_main and a full-screen ComposeView on top would swallow every touch
   * meant for the viewport. A separate window with FLAG_NOT_TOUCH_MODAL keeps the
   * rest of the screen with Blender. See ShortcutsOverlay.kt.
   */
  public void toggleShortcutsOverlay() {
    ShortcutsOverlay.toggle(this);
  }

  @Override
  protected void onCreate(Bundle state) {
    /* Runtime files must exist before native Blender init reads them. */
    extractRuntimeIfNeeded();
    /* Must precede super.onCreate(): that is what starts the native thread,
     * and Blender reads both of these during its Python initialization. */
    setUpPythonInterpreter();
    publishHardwareNames();
    /* Also before super.onCreate(): the glue reads this while building argv. */
    publishLaunchFile(getIntent());
    super.onCreate(state);
    enterImmersive();
    requestAllFilesAccess();

    inputView = new InputView(this);
    addContentView(inputView, new ViewGroup.LayoutParams(1, 1));
  }

  /* Scoped storage confines the app to its sandbox, but Blender opens and saves
   * .blend files and their assets anywhere by path. Send the user to the "All
   * files access" screen once; it is a no-op after they grant it. */
  private void requestAllFilesAccess() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()) {
      return;
    }
    try {
      Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                 Uri.parse("package:" + getPackageName()));
      startActivity(intent);
    }
    catch (Exception ex) {
      /* Some devices lack the per-app screen; fall back to the global list. */
      try {
        startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
      }
      catch (Exception ignored) {
      }
    }
  }

  /* A .blend tapped in a file manager while Blender is already running.
   * launchMode="singleTask" routes it here rather than building a second
   * NativeActivity, which would start a second Blender in this process. */
  @Override
  protected void onNewIntent(Intent intent) {
    super.onNewIntent(intent);
    setIntent(intent);
    String path = resolveBlendPath(intent);
    if (path != null) {
      /* Queued in GHOST and turned into GHOST_kEventOpenMainFile on Blender's own
       * thread, which wm_window.cc already answers with WM_OT_open_mainfile -- the
       * same operator the File menu uses, so the unsaved-changes prompt and the
       * recent files list behave the way they do everywhere else. */
      nativeOpenMainFile(path);
    }
  }

  /* Cold start: the file is the startup file, so it goes in as a launch argument
   * the way a double-clicked file reaches argv[1] on macOS (GHOST_HACK_getFirstFile
   * in creator.cc). Loading the startup file first and replacing it through the
   * event above would be a visible double load, and would prompt about discarding
   * an empty scene.
   *
   * An environment variable because the native thread has not started yet and this
   * process already passes values across that way, a few lines up in
   * publishHardwareNames(). */
  private void publishLaunchFile(Intent intent) {
    String path = resolveBlendPath(intent);
    if (path == null) {
      return;
    }
    try {
      Os.setenv("BLENDER_ANDROID_OPEN_FILE", path, true);
    }
    catch (Exception ex) {
      Log.w(TAG, "cannot publish launch file", ex);
    }
  }

  private String resolveBlendPath(Intent intent) {
    if (intent == null) {
      return null;
    }
    String action = intent.getAction();
    if (!Intent.ACTION_VIEW.equals(action) && !Intent.ACTION_EDIT.equals(action)) {
      return null;
    }
    Uri uri = intent.getData();
    if (uri == null) {
      return null;
    }
    try {
      String path = resolveUriToPath(uri);
      Log.i(TAG, "open request " + uri + " -> " + path);
      return path;
    }
    catch (Exception ex) {
      Log.w(TAG, "cannot resolve " + uri, ex);
      return null;
    }
  }

  /* Blender opens files by path -- it has no notion of a stream -- and a .blend
   * opened from a copy loses the relative paths to its textures and linked
   * libraries, and saves back somewhere the user will never find it. So every rung
   * here tries to name the real file, and the copy is only what is left when
   * nothing does. */
  private String resolveUriToPath(Uri uri) throws Exception {
    if ("file".equals(uri.getScheme())) {
      String path = usable(uri.getPath());
      if (path != null) {
        return path;
      }
    }

    /* The storage document provider spells the volume and the relative path into
     * the document id: "primary:Download/scene.blend". */
    if (DocumentsContract.isDocumentUri(this, uri)
        && "com.android.externalstorage.documents".equals(uri.getAuthority()))
    {
      String[] id = DocumentsContract.getDocumentId(uri).split(":", 2);
      if (id.length == 2) {
        File root = "primary".equalsIgnoreCase(id[0]) ? Environment.getExternalStorageDirectory()
                                                      : volumeRoot(id[0]);
        if (root != null) {
          String path = usable(new File(root, id[1]).getAbsolutePath());
          if (path != null) {
            return path;
          }
        }
      }
    }

    /* MediaStore's DATA column is deprecated but still carries the real path. */
    if ("content".equals(uri.getScheme())) {
      try (Cursor c = getContentResolver().query(
               uri, new String[] {MediaStore.MediaColumns.DATA}, null, null, null)) {
        if (c != null && c.moveToFirst() && !c.isNull(0)) {
          String path = usable(c.getString(0));
          if (path != null) {
            return path;
          }
        }
      }
      catch (Exception ignored) {
        /* A provider is free to reject the column. */
      }
    }

    /* Whatever the provider is, the descriptor it hands back is usually a real
     * file and /proc/self/fd names it. This is the rung that covers the providers
     * nobody enumerated. */
    try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
      if (pfd != null) {
        String path = usable(Os.readlink("/proc/self/fd/" + pfd.getFd()));
        if (path != null) {
          return path;
        }
      }
    }
    catch (Exception ignored) {
    }

    /* Nothing real behind it, or all-files access has not been granted yet --
     * canRead() fails on the very first launch from a file manager, because
     * requestAllFilesAccess() only runs after onCreate(). Take a copy so the tap
     * still opens something; relative links inside it will not resolve. */
    return copyToCache(uri);
  }

  /* Only accept a rung's answer if it names a file this process can actually open.
   * Falling through on a failure is what makes the missing-permission case end in
   * a working copy rather than an error. */
  private static String usable(String path) {
    if (path == null) {
      return null;
    }
    File file = new File(path);
    return (file.isFile() && file.canRead()) ? file.getAbsolutePath() : null;
  }

  private File volumeRoot(String uuid) {
    try {
      StorageManager sm = (StorageManager)getSystemService(Context.STORAGE_SERVICE);
      for (StorageVolume volume : sm.getStorageVolumes()) {
        if (uuid.equalsIgnoreCase(volume.getUuid())) {
          return volume.getDirectory();
        }
      }
    }
    catch (Exception ignored) {
    }
    return null;
  }

  private String copyToCache(Uri uri) throws Exception {
    File dir = new File(getCacheDir(), "opened");
    dir.mkdirs();
    File out = new File(dir, displayName(uri));
    try (InputStream is = getContentResolver().openInputStream(uri);
         OutputStream os = new FileOutputStream(out)) {
      if (is == null) {
        return null;
      }
      byte[] buf = new byte[65536];
      int n;
      while ((n = is.read(buf)) > 0) {
        os.write(buf, 0, n);
      }
    }
    Log.w(TAG, "no path behind " + uri + "; opening a copy, relative links will not resolve");
    return out.getAbsolutePath();
  }

  /* A provider-supplied name is untrusted: it can carry separators or "..". Keep
   * the basename and nothing else. */
  private String displayName(Uri uri) {
    String name = null;
    try (Cursor c = getContentResolver().query(
             uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
      if (c != null && c.moveToFirst() && !c.isNull(0)) {
        name = c.getString(0);
      }
    }
    catch (Exception ignored) {
    }
    if (name == null || name.isEmpty()) {
      name = "opened.blend";
    }
    name = new File(name).getName().replace("..", "_");
    return name.toLowerCase().endsWith(".blend") ? name : name + ".blend";
  }

  /* Hide the status/navigation bars so they don't overlap Blender's own menus
   * (the top File/Edit/… bar and the bottom timeline). Sticky immersive lets the
   * user swipe from an edge to reveal the bars temporarily. */
  private void enterImmersive() {
    View d = getWindow().getDecorView();
    d.setSystemUiVisibility(
        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_FULLSCREEN
        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
  }

  @Override
  public void onWindowFocusChanged(boolean hasFocus) {
    super.onWindowFocusChanged(hasFocus);
    /* Immersive mode is cleared when focus returns (e.g. after the soft keyboard
     * or a system dialog); re-apply it. */
    if (hasFocus) {
      enterImmersive();
      /* The shortcuts panel and its trigger are WindowManager windows rather than
       * views of this activity, so nothing removes them for us: a window manager
       * outlives the activity that added it and its token dies with it, leaving a
       * floating button floating over whatever is next. Put them back whenever this
       * window actually has focus. */
      ShortcutsOverlay.installTrigger(this);
    }
    else {
      ShortcutsOverlay.hide();
      ShortcutsOverlay.uninstallTrigger();
    }
  }

  /**
   * Makes `sys.executable` real.
   *
   * Blender's extension system runs its CLI as a subprocess built from
   * `sys.executable`, so "Get Extensions" does nothing without an interpreter
   * Blender can execute. The interpreter ships in the native library directory
   * because the runtime payload lands in the app's data directory, which is
   * mounted noexec from API 29 on. Blender looks for it under
   * `<python>/bin/` (BKE_appdir_program_python_search), so link the two.
   *
   * The symlink is rebuilt on every launch: nativeLibraryDir contains a hash
   * that changes when the app is updated, which would leave it dangling.
   */
  private void setUpPythonInterpreter() {
    File root = new File(getFilesDir(), "blender/" + VERSION);
    File pythonHome = new File(root, "python");
    if (!pythonHome.isDirectory()) {
      /* Bootstrap profile without Python; nothing to wire up. */
      return;
    }
    try {
      File interpreter = new File(getApplicationInfo().nativeLibraryDir, PYTHON_BIN_LIB);
      if (interpreter.exists()) {
        File binDir = new File(pythonHome, "bin");
        binDir.mkdirs();
        File link = new File(binDir, "python" + PYTHON_VERSION);
        /* delete() rather than exists(): a dangling symlink reads as absent
         * but still makes symlink() fail with EEXIST. */
        link.delete();
        Os.symlink(interpreter.getAbsolutePath(), link.getAbsolutePath());
      }
      else {
        Log.w(TAG, "no bundled interpreter; online extensions will not work");
      }
      /* A child process gets none of Blender's Python configuration, so it
       * would compute its prefix from the interpreter's own location -- the
       * library directory, which holds no standard library. Blender's embedded
       * interpreter is unaffected: it runs an isolated config that ignores
       * PYTHONHOME and sets its home explicitly. */
      Os.setenv("PYTHONHOME", pythonHome.getAbsolutePath(), true);

      /* PYTHONHOME alone is not enough. bpy.app.python_args is ("-I",) unless
       * Blender was told to use the system environment, and the extension
       * system passes it, so the child starts in isolated mode -- which
       * implies -E and therefore ignores PYTHONHOME. It then resolves the
       * symlink above back to the library directory and finds no standard
       * library, dying with "Failed to import encodings module" before
       * running a line.
       *
       * pyvenv.cfg is the way out: CPython reads it as a file next to the
       * executable or one level up, which -E does not suppress -- that is
       * exactly how a virtualenv's symlinked interpreter finds its base. The
       * path is only known at runtime, so it is written here rather than
       * shipped in the payload. */
      writeText(new File(pythonHome, "pyvenv.cfg"),
          "home = " + new File(pythonHome, "bin").getAbsolutePath() + "\n"
              + "include-system-site-packages = true\n"
              + "version = " + PYTHON_FULL_VERSION + "\n");

      /* A child is a plain exec outside the app's linker namespace, so it
       * resolves "libcrypto.so" against the system paths and finds Android's
       * BoringSSL, which does not export the symbols the bundled _ssl module
       * needs -- the import then dies on OPENSSL_sk_pop_free. Naming the
       * library directory first puts the real OpenSSL ahead of it. The app's
       * own libraries are already loaded by this point (libblender.so is
       * loaded in the static initializer), so this only affects what comes
       * after: the extension system's subprocesses. */
      Os.setenv("LD_LIBRARY_PATH", getApplicationInfo().nativeLibraryDir, true);
    }
    catch (Exception ex) {
      /* Not fatal: everything except online extensions works without it. */
      Log.w(TAG, "python interpreter setup failed", ex);
    }
  }

  /* Put the names of the device and its chip where Blender's Python can read them.
   *
   * Neither is readable from the Linux side of the process. /proc/cpuinfo carries no model name
   * on arm64, /sys/devices/soc0/machine is labelled vendor_sysfs_soc and closed to apps, and
   * /proc/device-tree/model is closed to everything. Build.SOC_MODEL is the same value the
   * platform reads out of ro.soc.model, and it is free here.
   *
   * Passed as environment variables because the process already sets some, a few lines above,
   * and os.environ costs the reading end nothing -- no JNI, no new RNA, no native call. Anything
   * missing is left unset rather than set to a placeholder, so the panel can leave the line out
   * instead of printing "unknown". */
  private void publishHardwareNames() {
    try {
      /* Build.MANUFACTURER is lower case -- "samsung" -- which reads as a typo beside a chip
       * vendor that is not. */
      String device = joinNonEmpty(capitalize(Build.MANUFACTURER), Build.MODEL);
      if (!device.isEmpty()) {
        Os.setenv("BLENDER_ANDROID_DEVICE", device, true);
      }

      /* "QTI" is what Qualcomm parts report, and it is not a name anybody recognises. */
      String vendor = Build.SOC_MANUFACTURER;
      if ("QTI".equalsIgnoreCase(vendor)) {
        vendor = "Qualcomm";
      }
      String soc = joinNonEmpty(vendor, Build.SOC_MODEL);
      if (!soc.isEmpty()) {
        Os.setenv("BLENDER_ANDROID_SOC", soc, true);
      }
    }
    catch (Exception ex) {
      /* Cosmetic. The panel simply leaves out whatever did not arrive. */
      Log.w(TAG, "hardware names unavailable", ex);
    }
  }

  private static String capitalize(String text) {
    if (text == null || text.isEmpty()) {
      return text;
    }
    return Character.toUpperCase(text.charAt(0)) + text.substring(1);
  }

  /* Build fields report the literal string "unknown" when they are not set, which is worse than
   * nothing: it would be printed. */
  private static String joinNonEmpty(String a, String b) {
    StringBuilder out = new StringBuilder();
    for (String part : new String[] {a, b}) {
      if (part == null) {
        continue;
      }
      part = part.trim();
      if (part.isEmpty() || part.equalsIgnoreCase(Build.UNKNOWN)) {
        continue;
      }
      if (out.length() != 0) {
        out.append(' ');
      }
      out.append(part);
    }
    return out.toString();
  }

  private static void writeText(File out, String text) throws Exception {
    try (OutputStream os = new FileOutputStream(out)) {
      os.write(text.getBytes("UTF-8"));
    }
  }

  /** Revision of the packaged runtime: a hash of the archive, written by package.sh. */
  private String runtimeRevision() {
    try (InputStream is = getAssets().open(RUNTIME_REV)) {
      ByteArrayOutputStream buf = new ByteArrayOutputStream();
      byte[] chunk = new byte[64];
      int n;
      while ((n = is.read(chunk)) > 0) {
        buf.write(chunk, 0, n);
      }
      String rev = buf.toString("UTF-8").trim();
      if (!rev.isEmpty()) {
        return rev;
      }
    }
    catch (Exception ex) {
      /* Payload predates the revision file; fall back to the constant. */
    }
    return RUNTIME_REVISION;
  }

  private void extractRuntimeIfNeeded() {
    File root = new File(getFilesDir(), "blender/" + VERSION);
    File marker = new File(root, ".installed-" + VERSION + "-" + runtimeRevision());
    if (marker.exists()) {
      return;
    }
    root.mkdirs();
    /* Drop markers from earlier revisions, so what is on disk stays readable at a
     * glance and they do not pile up one per build. */
    File[] stale = root.listFiles((dir, name) -> name.startsWith(".installed-"));
    if (stale != null) {
      for (File old : stale) {
        old.delete();
      }
    }
    try (InputStream is = getAssets().open(RUNTIME_ZIP, AssetManager.ACCESS_STREAMING);
         ZipInputStream zis = new ZipInputStream(is)) {
      ZipEntry e;
      byte[] buf = new byte[65536];
      while ((e = zis.getNextEntry()) != null) {
        File out = new File(root, e.getName());
        if (e.isDirectory()) {
          out.mkdirs();
          continue;
        }
        File parent = out.getParentFile();
        if (parent != null) {
          parent.mkdirs();
        }
        try (OutputStream os = new FileOutputStream(out)) {
          int n;
          while ((n = zis.read(buf)) > 0) {
            os.write(buf, 0, n);
          }
        }
      }
      marker.createNewFile();
    }
    catch (Exception ex) {
      throw new RuntimeException("Failed to extract Blender runtime", ex);
    }
  }

  /* Called from native (GHOST_android_open_url).
   *
   * Every link in Blender -- the About box, "Online Manual" on a tool's context menu,
   * the manual buttons in Preferences -- ends at wm.url_open, which calls Python's
   * webbrowser.open(). That module looks for xdg-open, gio and x-www-browser with
   * shutil.which(), finds none of them on Android, and returns False without a word:
   * no browser, no error, no log line. Handing the URL to the platform is the only
   * way any of those links can work.
   *
   * FLAG_ACTIVITY_NEW_TASK because the browser belongs in its own task, and because
   * the call arrives on Blender's thread rather than through an Activity context.
   * Returns whether the intent was accepted, so the operator can report a failure
   * rather than repeating the silence this replaces. */
  public boolean openUrl(String url) {
    try {
      Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
      intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
      startActivity(intent);
      return true;
    }
    catch (Exception ex) {
      /* ActivityNotFoundException when the device has no browser at all, and a
       * SecurityException if one refuses the intent. */
      Log.w(TAG, "cannot open " + url, ex);
      return false;
    }
  }

  /* Called from native (popupOnScreenKeyboard). */
  public void showKeyboard() {
    runOnUiThread(() -> {
      inputView.setFocusableInTouchMode(true);
      inputView.requestFocus();
      InputMethodManager imm = (InputMethodManager)getSystemService(Context.INPUT_METHOD_SERVICE);
      imm.showSoftInput(inputView, InputMethodManager.SHOW_IMPLICIT);
    });
  }

  /* Called from native (hideOnScreenKeyboard). */
  public void hideKeyboard() {
    runOnUiThread(() -> {
      InputMethodManager imm = (InputMethodManager)getSystemService(Context.INPUT_METHOD_SERVICE);
      imm.hideSoftInputFromWindow(inputView.getWindowToken(), 0);
    });
  }

  /** Invisible view whose InputConnection captures IME text. */
  private class InputView extends View {
    InputView(Context context) {
      super(context);
      setFocusable(true);
      setFocusableInTouchMode(true);
    }

    @Override
    public boolean onCheckIsTextEditor() {
      return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
      outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
      outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;

      return new BaseInputConnection(this, false) {
        /* Text the IME is still composing -- the underlined word being typed.
         *
         * Soft keyboards do not send key events for ordinary characters. They
         * call setComposingText() once per keystroke with the whole word so
         * far, and only call commitText() when the word is finished: at a
         * space, at punctuation, or when a suggestion is tapped. Blender has no
         * concept of composing text, so the composition is mirrored into the
         * field as it grows and rewritten whenever it changes. Without this,
         * individual letters never arrive and only picking a suggestion types
         * anything -- while backspace still works, because that one *is*
         * delivered as a key event. */
        private String composing = "";

        /** Makes the field show `text` where it currently shows `composing`. */
        private void replaceComposing(String text) {
          int common = 0;
          final int max = Math.min(composing.length(), text.length());
          while (common < max && composing.charAt(common) == text.charAt(common)) {
            common++;
          }
          /* Never cut between the halves of a surrogate pair: sending one half
           * on its own would not be valid UTF-8 by the time it reaches GHOST. */
          if (common > 0 && common < text.length()
              && Character.isLowSurrogate(text.charAt(common)))
          {
            common--;
          }

          /* Backspace over the tail that no longer matches. Counted in code
           * points, since that is what one delete removes on Blender's side. */
          final int stale = composing.codePointCount(common, composing.length());
          for (int i = 0; i < stale; i++) {
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_DOWN, 0);
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_UP, 0);
          }
          if (common < text.length()) {
            nativeOnCommitText(text.substring(common));
          }
          composing = text;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
          replaceComposing(text.toString());
          return true;
        }

        @Override
        public boolean finishComposingText() {
          /* The composition became final as typed; it is already in the field. */
          composing = "";
          return true;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
          /* Usually the composition unchanged, but autocorrect and suggestions
           * commit something different -- diffing covers both. */
          replaceComposing(text.toString());
          composing = "";
          return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
          /* A key event outside the composition, so what was mirrored so far
           * stands on its own. */
          composing = "";
          nativeOnKey(event.getKeyCode(), event.getAction(), event.getMetaState());
          return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
          composing = "";
          for (int i = 0; i < beforeLength; i++) {
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_DOWN, 0);
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_UP, 0);
          }
          return true;
        }
      };
    }
  }
}
