/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

package org.blender.blender;

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

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/**
 * NativeActivity subclass for Blender.
 *
 * <p>Extracts the bundled runtime (datafiles + scripts + Python) from {@code assets/extract/}
 * to {@code <filesDir>/blender/<version>} on first launch, then loads {@code libblender.so}
 * (which the NDK native_app_glue introspects via {@code android.app.lib_name}). Bridgess
 * soft-keyboard IME text to native and opens tapped .blend files.</p>
 */
public class BlenderActivity extends NativeActivity {

  /* NativeActivity dlopen()s the library from native code, which never registers it with the
   * class loader, so the JNI lookup for the native methods below fails with
   * UnsatisfiedLinkError. Load it here as well to register it. */
  static {
    System.loadLibrary("blender");
  }

  /* Must match GHOST_SystemPathsAndroid: <filesDir>/blender/<version>. */
  private static final String VERSION = "5.3";
  /* Where the staged runtime lives inside the APK assets (see platform_android_stage.cmake). */
  private static final String EXTRACT_PREFIX = "extract";
  private static final String TAG = "blender";
  /* Must match build_files/deps and the shipped CPython stdlib. */
  private static final String PYTHON_VERSION = "3.13";
  private static final String PYTHON_FULL_VERSION = "3.13.13";
  private static final String PYTHON_BIN_LIB = "libpython3_13_bin.so";

  private InputView inputView;

  private native void nativeOnCommitText(String text);
  private native void nativeOnKey(int keycode, int action, int metaState);
  private native void nativeOpenMainFile(String path);

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
   * launchMode="singleInstance" routes it here rather than building a second
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
   * in creator.cc).
   *
   * An environment variable because the native thread has not started yet. */
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

  /* Blender opens files by path -- it has no notion of a stream. Every rung here
   * tries to name the real file, and the copy is only what is left when nothing does. */
  private String resolveUriToPath(Uri uri) throws Exception {
    if ("file".equals(uri.getScheme())) {
      String path = usable(uri.getPath());
      if (path != null) {
        return path;
      }
    }

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
      }
    }

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

    return copyToCache(uri);
  }

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
    if (hasFocus) {
      enterImmersive();
    }
  }

  /* Makes `sys.executable` real (online extensions). The interpreter ships in the native
   * library directory because the runtime payload lands in app-private data, which is mounted
   * noexec from API 29 on. Blender looks for it under <python>/bin/ (BKE_appdir_program_python_search).
   * The symlink is rebuilt on every launch: nativeLibraryDir contains a hash that changes when
   * the app is updated. */
  private void setUpPythonInterpreter() {
    File root = new File(getFilesDir(), "blender/" + VERSION);
    File pythonHome = new File(root, "python");
    if (!pythonHome.isDirectory()) {
      /* Runtime without Python; nothing to wire up. */
      return;
    }
    try {
      File interpreter = new File(getApplicationInfo().nativeLibraryDir, PYTHON_BIN_LIB);
      if (interpreter.exists()) {
        File binDir = new File(pythonHome, "bin");
        binDir.mkdirs();
        File link = new File(binDir, "python" + PYTHON_VERSION);
        link.delete();
        Os.symlink(interpreter.getAbsolutePath(), link.getAbsolutePath());
      }
      else {
        Log.w(TAG, "no bundled interpreter; online extensions will not work");
      }
      Os.setenv("PYTHONHOME", pythonHome.getAbsolutePath(), true);
      /* pyvenv.cfg: a child process runs with -E (isolated) and ignores PYTHONHOME; CPython
       * reads this file even in isolated mode, which is how a virtualenv's symlinked
       * interpreter finds its base. Written here because the path is only known at runtime. */
      writeText(new File(pythonHome, "pyvenv.cfg"),
          "home = " + new File(pythonHome, "bin").getAbsolutePath() + "\n"
              + "include-system-site-packages = true\n"
              + "version = " + PYTHON_FULL_VERSION + "\n");

      Os.setenv("LD_LIBRARY_PATH", getApplicationInfo().nativeLibraryDir, true);
    }
    catch (Exception ex) {
      /* Not fatal: everything except online extensions works without it. */
      Log.w(TAG, "python interpreter setup failed", ex);
    }
  }

  /* Device/chip names for Blender's Python (Build fields are free; Linux side has them closed). */
  private void publishHardwareNames() {
    try {
      String device = joinNonEmpty(capitalize(Build.MANUFACTURER), Build.MODEL);
      if (!device.isEmpty()) {
        Os.setenv("BLENDER_ANDROID_DEVICE", device, true);
      }
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
      Log.w(TAG, "hardware names unavailable", ex);
    }
  }

  private static String capitalize(String text) {
    if (text == null || text.isEmpty()) {
      return text;
    }
    return Character.toUpperCase(text.charAt(0)) + text.substring(1);
  }

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

  /* Recursively copy an asset tree, stripping the leading "<prefix>/" from every path so
   * `assets/extract/5.3/datafiles/...` lands at `<filesDir>/blender/5.3/datafiles/...`. */
  private static void extractAssetDir(AssetManager assets, String path, String prefix, File base)
      throws java.io.IOException
  {
    String[] names = assets.list(path);
    if (names == null || names.length == 0) {
      try (InputStream in = assets.open(path)) {
        String rel = path.startsWith(prefix + "/") ? path.substring(prefix.length() + 1) : path;
        File out = new File(base, rel);
        File parent = out.getParentFile();
        if (parent != null) {
          parent.mkdirs();
        }
        Files.copy(in, out.toPath(), StandardCopyOption.REPLACE_EXISTING);
      }
      return;
    }
    for (String name : names) {
      extractAssetDir(assets, path + "/" + name, prefix, base);
    }
  }

  private static void deleteRecursive(File file) throws java.io.IOException {
    File[] children = file.listFiles();
    if (children != null) {
      for (File child : children) {
        deleteRecursive(child);
      }
    }
    if (file.exists() && !file.delete()) {
      throw new java.io.IOException("Failed to delete " + file);
    }
  }

  /* Extract the staged runtime to <filesDir>/blender/<version> if absent or outdated (stamp =
   * package lastUpdateTime, per the SDL-era scheme: changes when the APK is updated). */
  private void extractRuntimeIfNeeded() {
    File root = new File(getFilesDir(), "blender/" + VERSION);
    String stampToken = Long.toString(new File(getApplicationInfo().sourceDir).lastModified());
    File stampFile = new File(getFilesDir(), ".installed-" + VERSION + "-" + stampToken);

    if (stampFile.isFile() && root.isDirectory()) {
      return;
    }
    Log.i(TAG, "Portable version directory missing or outdated, clearing and extracting...");
    try {
      if (root.exists()) {
        deleteRecursive(root);
      }
      root.mkdirs();
      AssetManager assets = getAssets();
      String[] top = assets.list(EXTRACT_PREFIX);
      if (top == null) {
        throw new java.io.IOException("no assets/" + EXTRACT_PREFIX);
      }
      extractAssetDir(assets, EXTRACT_PREFIX, EXTRACT_PREFIX, new File(getFilesDir(), "blender"));
      /* Written last: an interrupted extraction leaves no stamp and re-runs on next launch. */
      Files.write(stampFile.toPath(), stampToken.getBytes(StandardCharsets.UTF_8));
      Log.i(TAG, "Portable version directory extracted to " + root);
    }
    catch (Exception ex) {
      throw new RuntimeException("Failed to extract Blender runtime", ex);
    }
  }

  /* Called from native (GHOST_android_open_url in GHOST_SystemAndroid.cc). */
  public boolean openUrl(String url) {
    try {
      Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
      intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
      startActivity(intent);
      return true;
    }
    catch (Exception ex) {
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
        private String composing = "";

        /** Makes the field show `text` where it currently shows `composing`. */
        private void replaceComposing(String text) {
          int common = 0;
          final int max = Math.min(composing.length(), text.length());
          while (common < max && composing.charAt(common) == text.charAt(common)) {
            common++;
          }
          if (common > 0 && common < text.length()
              && Character.isLowSurrogate(text.charAt(common)))
          {
            common--;
          }
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
          composing = "";
          return true;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
          replaceComposing(text.toString());
          composing = "";
          return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
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