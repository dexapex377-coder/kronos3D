/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * NativeActivity entry point. Android owns the frame loop, so we invert
 * Blender's main loop: run init once, then WM_main_loop_body per frame.
 */

#include "GHOST_ISystem.hh"
#include "GHOST_SystemAndroid.hh"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <cstdio>
#include <cstdlib>
#include <jni.h>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

/* Route Blender's stdout/stderr to logcat (tag "blender") so init/errors are
 * visible; NativeActivity otherwise discards them. */
static int g_stdio_pipe[2];
static void *ghost_android_stdio_thread(void * /*arg*/)
{
  char line[1024];
  ssize_t count;
  while ((count = read(g_stdio_pipe[0], line, sizeof(line) - 1)) > 0) {
    if (line[count - 1] == '\n') {
      count--;
    }
    line[count] = '\0';
    __android_log_write(ANDROID_LOG_INFO, "blender", line);
  }
  return nullptr;
}
static void ghost_android_redirect_stdio()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  if (pipe(g_stdio_pipe) != 0) {
    return;
  }
  dup2(g_stdio_pipe[1], STDOUT_FILENO);
  dup2(g_stdio_pipe[1], STDERR_FILENO);
  pthread_t thread;
  if (pthread_create(&thread, nullptr, ghost_android_stdio_thread, nullptr) == 0) {
    pthread_detach(thread);
  }
}

namespace blender {
struct bContext;
/* Implemented in creator: runs Blender init + WM_main_entry, hands C back. */
int GHOST_android_launch(int argc, const char **argv);
void WM_main_loop_body(bContext *C);
}  // namespace blender

static blender::bContext *g_context = nullptr;
static bool g_blender_launched = false;

namespace blender {
/* Called by the creator once init finished, to hand the context to the loop. */
void GHOST_androidfinalize(bContext *C)
{
  g_context = C;
}
}  // namespace blender

static GHOST_SystemAndroid *android_system()
{
  return static_cast<GHOST_SystemAndroid *>(GHOST_ISystem::getSystem());
}

/* Read optional launch arguments from <internalDataPath>/blender_args.txt.
 * Returns an empty vector when the file is absent, which keeps the built-in defaults. */
static std::vector<std::string> ghost_android_read_launch_args(android_app *app)
{
  std::vector<std::string> args;
  if (app == nullptr || app->activity == nullptr || app->activity->internalDataPath == nullptr) {
    return args;
  }
  const std::string path = std::string(app->activity->internalDataPath) + "/blender_args.txt";
  FILE *file = fopen(path.c_str(), "r");
  if (file == nullptr) {
    return args;
  }
  char token[512];
  while (fscanf(file, "%511s", token) == 1) {
    args.push_back(token);
  }
  fclose(file);
  __android_log_print(ANDROID_LOG_INFO,
                      "blender",
                      "[BlenderAndroid] %zu launch argument(s) from %s",
                      args.size(),
                      path.c_str());
  return args;
}

static void on_app_cmd(android_app *app, int32_t cmd)
{
  switch (cmd) {
    case APP_CMD_GAINED_FOCUS:
      if (GHOST_ISystem::getSystem()) {
        android_system()->handleWindowFocus(true);
      }
      break;

    case APP_CMD_LOST_FOCUS:
      if (GHOST_ISystem::getSystem()) {
        android_system()->handleWindowFocus(false);
      }
      break;

    case APP_CMD_INIT_WINDOW:
      if (!g_blender_launched) {
        /* Launch arguments come from blender_args.txt, whitespace separated, when that file
         * exists. A driver quirk on one device is diagnosed by toggling flags such as
         * --debug-gpu-force-workarounds or --log while watching logcat, and having to rebuild and
         * reinstall a 170 MB APK for each attempt makes that loop useless.
         *
         * Nothing is passed by default beyond --disable-crash-handler. There used to be a
         * --debug-gpu-force-workarounds here, added while bringing the port up to get past a
         * driver that refused compute
         * pipelines. That turned out to be the SPIR-V version the shaders were emitted as, which
         * is fixed at the source now, and the flag was making the viewport pay for it: taking the
         * forced path skips feature detection entirely and leaves dynamic rendering local read
         * off -- the one extension Blender enables specifically for Qualcomm, because reading
         * attachments from tile memory is what a deferred renderer needs on a tiler. Measured on
         * an S24 Ultra with a 292k vertex scene, turning it back on is worth 5-18% of the frame
         * while orbiting.
         *
         * The flag is still available through blender_args.txt when a driver needs it. */
        std::vector<std::string> file_args = ghost_android_read_launch_args(app);
        std::vector<const char *> argv;
        argv.push_back("blender");
        for (const std::string &arg : file_args) {
          argv.push_back(arg.c_str());
        }
        argv.push_back("--disable-crash-handler");
        /* A .blend the system asked us to open, resolved to a path by BlenderActivity
         * and left in the environment before the native thread started. Passed as an
         * argument rather than opened afterwards, so it is the file that loads instead
         * of the startup file loading and being replaced -- the same route a
         * double-clicked file takes into argv[1] on macOS. Blender's own fall-through
         * argument handler (main_args_handle_load_file) takes it from here.
         *
         * Copied out rather than used in place: argv holds borrowed pointers and the
         * unsetenv below would invalidate what getenv returned. */
        std::string open_file;
        if (const char *env = getenv("BLENDER_ANDROID_OPEN_FILE")) {
          open_file = env;
        }
        if (!open_file.empty()) {
          argv.push_back(open_file.c_str());
        }
        for (const char *arg : argv) {
          __android_log_print(ANDROID_LOG_INFO, "blender", "[BlenderAndroid] argv: %s", arg);
        }
        blender::GHOST_android_launch(int(argv.size()), argv.data());
        /* One launch only. A later tap arrives through onNewIntent instead, and must
         * not find a stale path here. */
        unsetenv("BLENDER_ANDROID_OPEN_FILE");
        g_blender_launched = true;
      }
      else if (GHOST_ISystem::getSystem()) {
        android_system()->handleNativeWindowInit(app);
      }
      break;
    /* Rotation. The activity declares orientation and screenSize in
     * configChanges, so it is not recreated: the surface is resized under it
     * and these are the only notice we get. CONFIG_CHANGED arrives for the
     * orientation switch itself and WINDOW_RESIZED once the surface follows;
     * both funnel to the same place, which is idempotent. */
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
      if (GHOST_ISystem::getSystem()) {
        android_system()->handleNativeWindowResize();
      }
      break;
    case APP_CMD_TERM_WINDOW:
      if (GHOST_ISystem::getSystem()) {
        android_system()->handleNativeWindowTerm();
      }
      break;
    default:
      break;
  }
}

static int32_t on_input_event(android_app * /*app*/, AInputEvent *event)
{
  if (!GHOST_ISystem::getSystem()) {
    return 0;
  }
  return android_system()->handleInputEvent(event);
}

static GHOST_SystemAndroid *android_system_if_ready()
{
  return GHOST_ISystem::getSystem() ? android_system() : nullptr;
}

/* Soft-keyboard text/keys from the Java InputConnection (BlenderActivity). */
extern "C" JNIEXPORT void JNICALL Java_com_kronos3d_app_BlenderActivity_nativeOnCommitText(
    JNIEnv *env, jobject /*thiz*/, jstring text)
{
  GHOST_SystemAndroid *system = android_system_if_ready();
  if (!system || !text) {
    return;
  }
  const char *utf = env->GetStringUTFChars(text, nullptr);
  system->handleTextInput(utf);
  env->ReleaseStringUTFChars(text, utf);
}

extern "C" JNIEXPORT void JNICALL Java_com_kronos3d_app_BlenderActivity_nativeOnKey(
    JNIEnv * /*env*/, jobject /*thiz*/, jint keycode, jint action, jint meta_state)
{
  if (GHOST_SystemAndroid *system = android_system_if_ready()) {
    system->handleJavaKeyEvent(keycode, action, meta_state);
  }
}

/* A .blend tapped in a file manager while Blender is already running, forwarded from
 * BlenderActivity.onNewIntent on the Android UI thread. */
extern "C" JNIEXPORT void JNICALL Java_com_kronos3d_app_BlenderActivity_nativeOpenMainFile(
    JNIEnv *env, jobject /*thiz*/, jstring path)
{
  GHOST_SystemAndroid *system = android_system_if_ready();
  if (!system || !path) {
    return;
  }
  const char *utf = env->GetStringUTFChars(path, nullptr);
  system->handleOpenMainFile(utf);
  env->ReleaseStringUTFChars(path, utf);
}

extern "C" void android_main(struct android_app *app)
{
  ghost_android_redirect_stdio();
  GHOST_SystemAndroid::setAndroidApp(app);
  app->onAppCmd = on_app_cmd;
  app->onInputEvent = on_input_event;

  while (!app->destroyRequested) {
    int events;
    android_poll_source *source;
    /* Block until the window exists (and Blender is launched); afterwards never
     * block, so we fall through to render every frame. `GHOST_android_launch`
     * runs the whole init inside `source->process` and sets `g_context`
     * mid-drain, so the timeout must be re-evaluated after each event rather
     * than captured once — otherwise the loop blocks forever and never renders. */
    int timeout = g_context ? 0 : -1;
    while (ALooper_pollOnce(timeout, nullptr, &events, (void **)&source) >= 0) {
      if (source) {
        source->process(app, source);
      }
      if (app->destroyRequested) {
        return;
      }
      if (g_context) {
        timeout = 0;
      }
    }

    if (g_context) {
      blender::WM_main_loop_body(g_context);
    }
  }
}

extern "C" JNIEXPORT void JNICALL Java_com_kronos3d_app_BlenderActivity_nativeSendKeyCombo(
    JNIEnv * /*env*/, jobject /*thiz*/, jint modifier_mask, jint keycode, jint meta_state)
{
  GHOST_SystemAndroid *system = android_system_if_ready();
  if (!system) {
    return;
  }

  /* modifier_mask packs up to three Android keycodes, 10 bits each. Holding every one of
   * them down is what makes Ctrl+Shift+A work; the previous single-keycode form could
   * only ever press one modifier, so multi-modifier combinations silently lost all but
   * one. Order does not matter to the keymap, which reads the modifiers GHOST has seen. */
  constexpr int kModifierBits = 10;
  constexpr int kModifierMask = (1 << kModifierBits) - 1;
  constexpr int kMaxModifiers = 3;
  int modifiers[kMaxModifiers];
  int modifier_count = 0;
  for (int i = 0; i < kMaxModifiers; i++) {
    const int code = (modifier_mask >> (kModifierBits * i)) & kModifierMask;
    if (code != 0) {
      modifiers[modifier_count++] = code;
    }
  }

  for (int i = 0; i < modifier_count; i++) {
    system->handleJavaKeyEvent(modifiers[i], AKEY_EVENT_ACTION_DOWN, meta_state);
  }
  system->handleJavaKeyEvent(keycode, AKEY_EVENT_ACTION_DOWN, meta_state);
  system->handleJavaKeyEvent(keycode, AKEY_EVENT_ACTION_UP, meta_state);
  /* Release with meta 0: GHOST_SystemAndroid::meta_state_ persists between events, so
   * leaving the modifiers set would leak them into the next shortcut. */
  for (int i = 0; i < modifier_count; i++) {
    system->handleJavaKeyEvent(modifiers[i], AKEY_EVENT_ACTION_UP, 0);
  }
}

/* A .blend tapped in a file manager while Blender is already running, forwarded from
 * BlenderActivity.onNewIntent on the Android UI thread. */
/* Latch a modifier down or up without sending any key, and without releasing it.
 *
 * nativeSendKeyCombo() always ends by releasing its modifiers, because a shortcut is a
 * complete gesture. A toggle shortcut is not: it presses Shift and holds it so the next
 * tap in the 3D view is a genuine shift-click, the way holding Shift on a keyboard does.
 * Blender's keymap reads getModifierKeys(), which GHOST fills from modifier key events
 * it has seen, so a modifier that is never sent stays invisible no matter what meta bits
 * the following events carry.
 *
 * meta_state is passed through unchanged for the down and forced to 0 for the up, so a
 * latch release cannot leave a stale modifier behind for the next input. */
extern "C" JNIEXPORT void JNICALL Java_com_kronos3d_app_BlenderActivity_nativeSetModifierLatched(
    JNIEnv * /*env*/, jobject /*thiz*/, jint modifier_keycode, jboolean pressed, jint meta_state)
{
  GHOST_SystemAndroid *system = android_system_if_ready();
  if (!system || modifier_keycode == 0) {
    return;
  }
  system->handleJavaKeyEvent(modifier_keycode,
                            pressed ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP,
                            pressed ? meta_state : 0);
}

