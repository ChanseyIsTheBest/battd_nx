/* ---------------------------------------------------------------------------
 * bp_boot.c -- replaces UnityPlayer.java's lifecycle, for Unity 2020.3.15f2.
 *
 * The natives are file-local in libunity. Its JNI_OnLoad announces them with
 * RegisterNatives on com/unity3d/player/UnityPlayer and jni_fake.c captures the
 * table; every entry point below is resolved from that capture by name. This
 * build registers 28 of them (extracted from .rela.dyn):
 *
 *   initJni(Landroid/content/Context;)V     THREE args (env, thiz, ctx) --
 *                                           Unity 6 takes four
 *   nativeRecreateGfxState(ILandroid/view/Surface;)V   ATTACHES the window
 *   nativeSendSurfaceChangedEvent()V        notification, AFTER the attach
 *   nativeRender()Z  nativeResume()V  nativePause()Z  nativeDone()Z
 *   nativeFocusChanged(Z)V  nativeOrientationChanged(II)V ...
 *
 * There is NO nativeUnityPlayerSetRunning in 2020.3. The Bouncemasters boot
 * treated that Unity 6 gate as required and would have aborted here.
 *
 * Order mirrors this APK's own UnityPlayer (read from classes2.dex): initJni in
 * the constructor, then the UnityMain handler thread runs RecreateGfxState ->
 * SendSurfaceChangedEvent -> Resume -> FocusChanged and loops nativeRender.
 * MIT.
 * ------------------------------------------------------------------------- */
#include "editbox.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "android_native_unity.h"
#include "bp_jni.h"
#include "bp_managed.h"
#include "battd_il2cpp.h"
#include "bp_vsync.h"
#include "config.h"
#include "diag.h"
#include "jni_fake.h"
#include "so_util.h"
#include "util.h"

extern so_module unity_mod;
extern void *fake_unityplayer_thiz;
extern void *fake_context_obj;
extern void *fake_surface_obj;
void port_frame_tick(void);

typedef void (*fn_initJni)(void *env, void *thiz, void *ctx);
typedef void (*fn_recreate)(void *env, void *thiz, int flag, void *surface);
typedef void (*fn_v)(void *env, void *thiz);
typedef unsigned char (*fn_z)(void *env, void *thiz);
typedef void (*fn_vz)(void *env, void *thiz, int b);
typedef void (*fn_orient)(void *env, void *thiz, int orientation, int rotation);

static struct {
  fn_initJni  initJni;
  fn_recreate recreateGfxState;
  fn_v        sendSurfaceChanged;
  fn_z        render;
  fn_v        resume;
  fn_z        pause;
  fn_z        done;
  fn_vz       focusChanged;
  fn_orient   orientationChanged;
  fn_vz       setRunning;          /* Unity 6 only; absent here, harmless */
} U;

static int g_running;

static void *resolve_native(const char *name, int required) {
  void *p = jni_lookup_unity_native(name);
  if (!p) {
    debugPrintf(required ? "[boot] MISSING required native: %s\n"
                         : "[boot] optional native absent: %s\n", name);
    return NULL;
  }
  debugPrintf("[boot]   %-32s -> +0x%lx\n", name,
              (unsigned long)((uintptr_t)p - (uintptr_t)unity_mod.load_virtbase));
  return p;
}

static int resolve_all(void) {
  debugPrintf("[boot] resolving UnityPlayer natives from the RegisterNatives capture\n");
  U.initJni            = (fn_initJni)resolve_native("initJni", 1);
  U.recreateGfxState   = (fn_recreate)resolve_native("nativeRecreateGfxState", 1);
  U.sendSurfaceChanged = (fn_v)resolve_native("nativeSendSurfaceChangedEvent", 1);
  U.render             = (fn_z)resolve_native("nativeRender", 1);
  U.resume             = (fn_v)resolve_native("nativeResume", 0);
  U.pause              = (fn_z)resolve_native("nativePause", 0);
  U.done               = (fn_z)resolve_native("nativeDone", 0);
  U.focusChanged       = (fn_vz)resolve_native("nativeFocusChanged", 0);
  U.orientationChanged = (fn_orient)resolve_native("nativeOrientationChanged", 0);
  U.setRunning         = (fn_vz)jni_lookup_unity_native("nativeUnityPlayerSetRunning");
  if (!U.initJni || !U.recreateGfxState || !U.sendSurfaceChanged || !U.render) {
    debugPrintf("[boot] FATAL: a required UnityPlayer native did not register. Check the\n"
                "       '[jni] RegisterNatives' lines above for the class names seen.\n");
    return -1;
  }
  return 0;
}

void bp_boot_pause(void) {
  if (!g_running) return;
  if (U.pause) U.pause(fake_env, fake_unityplayer_thiz);
  if (U.setRunning) U.setRunning(fake_env, fake_unityplayer_thiz, 0);
  debugPrintf("[boot] paused\n");
}

void bp_boot_resume(void) {
  if (!g_running) return;
  if (U.setRunning) U.setRunning(fake_env, fake_unityplayer_thiz, 1);
  if (U.resume) U.resume(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] resumed\n");
}

void bp_boot_focus(int focused) {
  if (g_running && U.focusChanged) U.focusChanged(fake_env, fake_unityplayer_thiz, focused ? 1 : 0);
}

int bp_boot_and_run(void) {
  if (resolve_all() != 0) return -1;
  if (!fake_unityplayer_thiz || !fake_context_obj) {
    debugPrintf("[boot] FATAL: fake UnityPlayer/Context objects are NULL --\n"
                "       unity_environment_init() did not run or did not complete\n");
    return -2;
  }
  extern void bp_reassert_main_tls(void);
  bp_reassert_main_tls();

  debugPrintf("[boot] initJni(env, thiz, ctx)  [2020.3: three args]\n");
  U.initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  debugPrintf("[boot] initJni returned\n");

  /* Pump BEFORE the graphics calls: the engine can reach WaitVSync from inside
   * the surface setup, and nothing else ever raises the counter. */
  if (bp_vsync_start() != 0)
    debugPrintf("[boot] WARNING: no vsync pump -- the first nativeRender will park in\n"
                "        WaitVSync and never return\n");

  /* ATTACH FIRST (recreateGfxState carries the Surface), THEN notify. */
  debugPrintf("[boot] recreateGfxState(0, surface=%p)  [attaches the window]\n", fake_surface_obj);
  U.recreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  debugPrintf("[boot] sendSurfaceChanged()  [engine sees %dx%d portrait]\n", screen_width, screen_height);
  U.sendSurfaceChanged(fake_env, fake_unityplayer_thiz);

  /* 1 == Configuration.ORIENTATION_PORTRAIT. The game only allows portrait. */
  if (U.orientationChanged) U.orientationChanged(fake_env, fake_unityplayer_thiz, 1, 0);

  if (U.setRunning) U.setRunning(fake_env, fake_unityplayer_thiz, 1);
  g_running = 1;
  if (U.resume) U.resume(fake_env, fake_unityplayer_thiz);
  if (U.focusChanged) U.focusChanged(fake_env, fake_unityplayer_thiz, 1);

  debugPrintf("[boot] entering frame loop\n");
  uint64_t frames = 0;
  uint32_t last_stat = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    editbox_pump();                     /* a queued soft-input request: swkbd + result, between frames */
    android_native_feed_hid();          /* touch/stick/gyro/mouse -> bp_touch, BEFORE render */
    unsigned char alive = U.render(fake_env, fake_unityplayer_thiz);
    battd_il2cpp_pump();                /* splash video: end the clip on THIS thread.
                                         * nativeRender is where managed code runs, and
                                         * the exit calls into the Animator -- it cannot
                                         * go in the swap wrapper, which is Unity's GL
                                         * worker thread with MT rendering on. */
    if (!alive && frames > 0) {
      debugPrintf("[boot] nativeRender reported not-alive at frame %llu\n", (unsigned long long)frames);
      break;
    }
    bp_jni_rumble_tick();
    { extern void firebase_stub_pump(void); firebase_stub_pump(); }
    bp_time_tick();
    if ((uint32_t)frames - last_stat >= 60) {
      extern void bp_audio_stats(char *out, size_t cap);
      char st[128];
      last_stat = (uint32_t)frames;
      bp_audio_stats(st, sizeof st);
      debugPrintf("[audio] %s\n", st);
    }
    bp_prefs_tick();
    port_frame_tick();
    diag_frame((int)frames);
    frames++;
    if (frames <= 5 || (frames % 60) == 0)
      debugPrintf("[boot] frame %llu rendered (alive=%d)\n", (unsigned long long)frames, (int)alive);
  }
  debugPrintf("[boot] leaving frame loop after %llu frames (quit=%d)\n",
              (unsigned long long)frames, jni_quit_requested);
  bp_prefs_flush_now();
  bp_vsync_stop();
  g_running = 0;
  if (U.pause) U.pause(fake_env, fake_unityplayer_thiz);
  if (U.done)  U.done(fake_env, fake_unityplayer_thiz);
  return 0;
}
