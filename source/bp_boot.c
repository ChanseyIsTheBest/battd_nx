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
#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "android_native_unity.h"
#include "bp_jni.h"
#include "bp_managed.h"
#include "bp_net.h"
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
    bp_net_cache_state_refresh();       /* rate-limited to once a minute inside */
    { extern void bp_ram_check_frozen(void);
      bp_ram_check_frozen(); }        /* also once a minute; see libc_shim.c */
    { extern void bp_ram_report(void);
      bp_ram_report(); }              /* is the cache buying anything? */
    { extern void bp_tr_flush(void);
      bp_tr_flush(); }                /* drain queued trace lines on THIS thread */
    if (!alive && frames > 0) {
      debugPrintf("[boot] nativeRender reported not-alive at frame %llu\n", (unsigned long long)frames);
      break;
    }
    bp_jni_rumble_tick();
    { extern void firebase_stub_pump(void); firebase_stub_pump(); }
    bp_time_tick();
#if DEBUG_LOG
    /* Per-frame instruments. Every one of them reports through debugPrintf, so
     * with DEBUG_LOG 0 they could only burn work in silence: a svcQueryMemory
     * per frame for the stub-page check, another for the hid watch, a mallinfo
     * and a walk of the arena bitmap every second. Compiled out of a release
     * build entirely; set DEBUG_LOG 1 to get them back. */
    if (bp_diag_io) { extern void bp_ram_blob_scan(unsigned frame); bp_ram_blob_scan((unsigned)frames); }
    /* THE FATAL THAT TOOK THE CONSOLE DOWN. Atmosphère's report was 2345-0008
     * = libnx LibnxError_NotInitialized, raised by hid.c when
     * hidGetSharedmemAddr() returns NULL inside a hidGetNpadStates* call --
     * the every-frame input poll. That pointer is a libnx global in bss; it
     * was valid for 555 frames and then read as zero. Watch it here, one frame
     * ahead of the poll, and when it goes the same way say which of two
     * things happened: the page is still mapped RW and its bytes were zeroed
     * (a store from somewhere), or the page's mapping changed (a remap). That
     * is the question every bss and blob corruption in this port comes down
     * to, and svcQueryMemory answers it without touching anything. */
    /* THE LIBNX BSS CANARY. Two fatals at the same game moment, two victims:
     * g_hidSharedmem (2345-0008) and the default NWindow's binder state
     * (2349-0004, thrown by mesa's swap). In the ELF they are 1,776 bytes
     * apart, in one block of libnx service globals that also holds the hid,
     * nifm, nv, psm, sm and ssl sessions. Below that block is ffmpeg's static
     * table area. Something writes across it around frame 600.
     *
     * The block's addresses are libnx-internal and per build, but three public
     * accessors return pointers INTO it: hidGetServiceSession() (g_hidSrv),
     * nvGetServiceSession() (g_nvSrv) and nwindowGetDefault(). The session
     * globals are constant once initialised, so the span from just before
     * g_hidSrv (which covers g_hidSharedmem) to just past g_nvSrv is snapshotted
     * after boot and compared every frame. The window itself changes every
     * swap and is excluded. On the first difference: the frame, the offset,
     * how many bytes, and old/new for the first sixteen. Reports once. */
    { static unsigned char snap[0x200]; static const unsigned char *lo; static size_t span; static int armed, reported;
      if (!armed && frames >= 30) {
        const unsigned char *h = (const unsigned char *)hidGetServiceSession();
        const unsigned char *n = (const unsigned char *)nvGetServiceSession();
        if (h && n && n > h && (size_t)(n - h) < sizeof snap - 0x70) {
          lo = h - 0x60; span = (size_t)(n - lo) + 0x10;
          memcpy(snap, lo, span); armed = 1;
          debugPrintf("[mem] libnx canary armed: %p +0x%zx (hidSrv=%p nvSrv=%p nwindow=%p)\n",
                      (const void *)lo, span, (const void *)h, (const void *)n, (void *)nwindowGetDefault());
        } else { armed = -1;
          debugPrintf("[mem] libnx canary NOT armed (hidSrv=%p nvSrv=%p)\n", (const void *)h, (const void *)n); }
      }
      if (armed == 1 && !reported && memcmp(snap, lo, span) != 0) {
        reported = 1;
        size_t first = span, diff = 0;
        for (size_t k = 0; k < span; k++) if (lo[k] != snap[k]) { if (first == span) first = k; diff++; }
        char oldh[40] = "", newh[40] = "";
        for (size_t k = 0; k < 16 && first + k < span; k++) {
          snprintf(oldh + 2 * k, sizeof oldh - 2 * k, "%02x", snap[first + k]);
          snprintf(newh + 2 * k, sizeof newh - 2 * k, "%02x", lo[first + k]); }
        debugPrintf("[log] LIBNX BSS CANARY HIT at frame %u: %zu of %zu bytes changed, first at +0x%zx (%p): "
                    "was %s now %s\n", (unsigned)frames, diff, span, first, (const void *)(lo + first), oldh, newh);
        debug_log_flush();
      } }
    /* IS THE SYSCALL-STUB PAGE STILL EXECUTABLE. Run 22 died as an instruction
     * abort at svcSleepThread+0x4 -- the `ret` after the svc -- on the first
     * thread to return from a syscall, with thirty others parked at +0x0 about
     * to do the same. Nothing in the port changes code permissions, so what
     * the kernel says about that page one frame earlier is the question. One
     * svcQueryMemory per frame; reports once, with type/perm/attr, and flushes. */
    { static int text_lost;
      if (!text_lost) {
        MemoryInfo mi; u32 pi;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&svcSleepThread)) && !(mi.perm & Perm_X)) {
          text_lost = 1;
          debugPrintf("[log] SYSCALL STUB PAGE LOST EXECUTE at frame %u: block %p +0x%llx type=0x%x perm=%c%c%c attr=0x%x\n",
                      (unsigned)frames, (void *)(uintptr_t)mi.addr, (unsigned long long)mi.size, (unsigned)mi.type,
                      (mi.perm & Perm_R) ? 'R' : '-', (mi.perm & Perm_W) ? 'W' : '-', (mi.perm & Perm_X) ? 'X' : '-',
                      (unsigned)mi.attr);
          debug_log_flush();
        } } }
    /* HOW CLOSE TO THE WALL. Every fatal lands at the map-scene load, the
     * point where the process holds the most: 243 MB image + 83 MB pack +
     * 512 MB GPU arena + ~1 GB mmap arena + Unity's 512 MB dynamic heap, out
     * of 2981 MB. Once a second, what newlib says is left. If the map load is
     * the moment a large allocation starts failing somewhere that does not
     * check, this line shows the cliff. */
    if (frames % 60 == 0) {
      struct mallinfo mi = mallinfo();
      static size_t last_free;
      const size_t free_now = (size_t)mi.fordblks;
      if (frames == 0 || free_now < last_free / 2 || free_now < (128u << 20)) {
        /* Break the 2.5 GB down. Most of "in use" is the port's own up-front
         * reservations, not Unity: the mmap arena (Unity's Dynamic Heap lives
         * inside it), the GPU arena, the resident cache image, the 83 MB pack.
         * Reporting arena RESERVED vs its PEAK use is the whole point -- if the
         * arena is 768 MB reserved but never peaks above, say, 560, the
         * difference is free memory the game is being denied. */
        size_t a_res=0,a_use=0,a_peak=0,fb=0,g_res=0,g_live=0,g_peak=0,img=0,imgu=0;
        extern void bp_mmap_stats(size_t*,size_t*,size_t*,size_t*);
        extern void bp_gpua_stats(size_t*,size_t*,size_t*);
        extern void bp_ram_stats(size_t*,size_t*);
        bp_mmap_stats(&a_res,&a_use,&a_peak,&fb); bp_gpua_stats(&g_res,&g_live,&g_peak); bp_ram_stats(&img,&imgu);
        size_t cm=0, dc=0; unsigned cn=0;
        { extern void bp_mmap_commit_stats(size_t*,size_t*,unsigned*); bp_mmap_commit_stats(&cm,&dc,&cn); }
        /* live = what Unity has mprotect(RW)'d and not handed back. The gap
         * between that and the arena's reservation is memory the port is
         * holding for address space Unity has never touched. */
        /* IS THE HEAP ACTUALLY 2981 MB? A 64 MB allocation returned NULL while
         * the pool held 2168 used + 10 free = 2178 MB, with ~800 MB of the
         * granted heap apparently untouched and a 0 MB top chunk. mallinfo's
         * "arena" is the space sbrk has actually handed the allocator, so it
         * settles whether the pool can still grow or whether 2981 is a figure
         * newlib never really got. If arena stops climbing well below 2981,
         * the ceiling is sbrk, not Unity's appetite. */
        debugPrintf("[mem] allocator pool %zu MB from sbrk (top chunk %zu MB, %zu free) "
                    "of the %d MB newlib was granted -- %s\n",
                    (size_t)mi.arena >> 20, (size_t)mi.keepcost >> 20, free_now >> 20,
                    2981,
                    ((size_t)mi.arena >> 20) + 64 < 2981 ? "room to grow"
                                                         : "AT THE CEILING");
        { size_t fb=0; unsigned ac=0, mc=0;
          extern void bp_munmap_stats(size_t*,unsigned*,unsigned*);
          bp_munmap_stats(&fb,&ac,&mc);
          debugPrintf("[mem] munmap: %u calls, %u of them inside the arena, %zu MB returned "
                      "-- arena peak %zu now %zu\n", mc, ac, fb >> 20, a_peak >> 20, a_use >> 20); }
        debugPrintf("[mem] Unity committed %zu MB of the %zu MB the arena holds (%u mprotects) "
                    "-- gap %zd MB is reserved-but-untouched\n",
                    (cm - dc) >> 20, a_use >> 20, cn, (ssize_t)(a_use - (cm - dc)) >> 20);
        debugPrintf("[mem] heap %zu MB used / %zu free. Of that: mmap arena %zu reserved (peak %zu, now %zu), "
                    "GPU arena %zu reserved (peak %zu), cache image %zu, fallback maps %zu (%zu held by "
                    "refused partial unmaps). Rest (~%zu) is Unity + newlib.\n",
                    (size_t)mi.uordblks >> 20, free_now >> 20,
                    a_res>>20, a_peak>>20, a_use>>20, g_res>>20, g_peak>>20, img>>20, fb>>20,
                    ({ extern size_t bp_mmap_leaked(void); bp_mmap_leaked() >> 20; }),
                    ((size_t)mi.uordblks - a_res - g_res - img - fb) >> 20);
        last_free = free_now ? free_now : 1;
      } else last_free = free_now;
    }
    { static int hid_gone; static void *last_shm;
      if (!hid_gone) {
        void *shm = hidGetSharedmemAddr();
        if (shm) last_shm = shm;
        else { hid_gone = 1;
          MemoryInfo mi; u32 pi;
          const int q = last_shm && R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)last_shm));
          /* MemType_SharedMem = 0x06. If the old mapping is still that, the
           * hid service is fine and libnx's descriptor in bss was overwritten.
           * If it is unmapped or another type, hid was really torn down. */
          debugPrintf("[log] HID SHARED MEMORY POINTER IS NULL at frame %u -- the next input poll aborts "
                      "with 2345-0008. Last mapping %p is now: %s (type=0x%x perm=%c%c%c block %p +0x%llx) => %s\n",
                      (unsigned)frames, last_shm, q ? "queried" : "unknown",
                      q ? (unsigned)mi.type : 0u,
                      q && (mi.perm & Perm_R) ? 'R' : '-', q && (mi.perm & Perm_W) ? 'W' : '-', q && (mi.perm & Perm_X) ? 'X' : '-',
                      q ? (void *)(uintptr_t)mi.addr : NULL, q ? (unsigned long long)mi.size : 0ull,
                      (q && (mi.type & 0xff) == MemType_SharedMem) ? "MAPPING INTACT: the bss descriptor was OVERWRITTEN"
                                                      : "mapping gone: hid was torn down");
          debug_log_flush();
        } } }
    /* A SYSTEM crash has no handler and no report: whatever was in the log
     * buffer since the last flush is simply gone, and that was up to ten
     * seconds -- the run that wedged the console lost everything after its
     * last [input] line. Flush once a second from here, on the main thread,
     * outside every lock (debug_log_flush takes and drops the log lock and
     * fflushes after). With the cache serving from RAM the card is quiet, so
     * this is cheap; it bounds the loss to a second. */
    if (frames % (bp_diag_io ? 60 : 600) == 0) debug_log_flush();   /* 1 Hz heavy, 10 s light */
#endif  /* DEBUG_LOG */
    if ((uint32_t)frames - last_stat >= 60) {
      extern void bp_audio_stats(char *out, size_t cap);
      char st[128];
      last_stat = (uint32_t)frames;
      bp_audio_stats(st, sizeof st);
      debugPrintf("[audio] %s\n", st);
      /* Is the GC bridge freezing threads mid-log? The 50 ms bound in
       * wait_not_logging() gives up and suspends anyway, and nothing recorded
       * when that happened, so the theory could not be tested against a run.
       * Only printed when a number moves, so a healthy run stays silent. */
      { static unsigned seen_gaveup, seen_waited, seen_wedge;
        unsigned gaveup, waited, worst_us;
        diag_log_wait_stats(&gaveup, &waited, &worst_us);
        const unsigned wedge = debug_log_wedge_events();
        if (gaveup != seen_gaveup || waited != seen_waited || wedge != seen_wedge) {
          seen_gaveup = gaveup; seen_waited = waited; seen_wedge = wedge;
          debugPrintf("[log] GC pause vs log lock: %u gave up after 50ms, %u waited "
                      "(worst %u us) | %u log-lock wedges, %u lines dropped\n",
                      gaveup, waited, worst_us, wedge, debug_log_dropped());
        } }
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
