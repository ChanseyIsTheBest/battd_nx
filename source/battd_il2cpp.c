/* ---------------------------------------------------------------------------
 * battd_il2cpp.c -- patches and hooks inside the game's own libil2cpp.so.
 *
 * Two unrelated jobs live here because both need the same thing: a guarded
 * write into managed code once libil2cpp is mapped.
 *
 *   1. Force the 2D art tier (see config.h).
 *   2. Play the splash videos, which the engine cannot.
 *
 * MIT.
 * ------------------------------------------------------------------------- */
#include "config.h"

#ifndef BATTD_VIDEO
#error "config.h not included -- the hooks would silently compile out"
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "so_util.h"
#include "util.h"
#include "bp_root.h"
#include "battd_video.h"
#include "battd_il2cpp.h"

/* ======================================================================== */
/*  1. FORCED ART TIER                                                       */
/* ======================================================================== */

static const uint32_t k_tier_call_guard = 0x9446d23eu;   /* bl   <tier()>  */
static const uint32_t k_tier_skip_guard = 0x540000e1u;   /* b.ne +0x1c     */

static int patch_art_tier(so_module *m) {
  if (bp_art_quality < 0) {
    debugPrintf("[art] art_quality auto -- GetActiveVariants left alone\n");
    return 0;
  }
  uintptr_t call = (uintptr_t)m->load_virtbase + BATTD_RVA_VariantTier_Call;
  uintptr_t skip = (uintptr_t)m->load_virtbase + BATTD_RVA_VariantTier_Skip;
  const uint32_t have_call = *(volatile uint32_t *)call;
  const uint32_t have_skip = *(volatile uint32_t *)skip;
  if (have_call != k_tier_call_guard || have_skip != k_tier_skip_guard) {
    debugPrintf("[art] GUARD FAILED at GetActiveVariants "
                "(want %08x/%08x, found %08x/%08x) -- tier NOT forced; "
                "the game will choose from ui_scale and memory_mb\n",
                k_tier_call_guard, k_tier_skip_guard, have_call, have_skip);
    return 0;
  }
  /* movz w0, #N -- replaces the call that derived the tier from density. */
  const uint32_t movz = 0x52800000u | ((uint32_t)(bp_art_quality & 0xffff) << 5);
  /* b +0x1c -- the b.ne becomes unconditional, so tier 2 no longer falls into
   * the GetPhysicalMemoryMB() >= 1536 test that would knock it back to 1. */
  const uint32_t br = 0x14000007u;
  if (so_patch_code((void *)call, &movz, 4) != 0 ||
      so_patch_code((void *)skip, &br, 4) != 0) {
    debugPrintf("[art] FAILED to write the tier patch\n");
    return 0;
  }
  debugPrintf("[art] asset variant tier forced to %d "
              "(density and RAM no longer decide it)\n", bp_art_quality);
  return 1;
}

/* ======================================================================== */
/*  2. SPLASH VIDEO                                                          */
/* ======================================================================== */
#if BATTD_VIDEO

/* WHY THE DRIVER, AND NOT PlayVideoNow OR VideoPlayer::Play.
 *
 * Two wrong hooks preceded this one, and the hardware logs named both.
 *
 * Attempt 1 replaced VideoPlayer::set_url and ::Play. That silences the engine's
 * error, and SplashScreenVideo only leaves on EndReached (loopPointReached) or
 * OnVideoError -- so with both silenced the splash would have waited forever.
 *
 * Attempt 2 hooked SplashScreenVideo::PlayVideoNow. The log showed the hook
 * installed and never fired, because the real order is
 *
 *     Start() -> TryPlayNextVideoInWaterfall()
 *                  -> VideoPlayer.set_url(...)
 *                  -> VideoPlayer.Prepare()        <-- fails, every clip
 *                  -> (on prepareCompleted) PlayVideoNow() -> Play()
 *
 * PlayVideoNow is downstream of Prepare succeeding; all it does is subscribe
 * EndReached to loopPointReached and call Play. Prepare never succeeds here, so
 * nothing below it runs.
 *
 * TryPlayNextVideoInWaterfall is the driver: entered from Start and again from
 * OnVideoError, owning both the waterfall array and the index. Replacing it
 * takes the engine out of the picture entirely -- no set_url, no Prepare, no
 * error -- and we drive the exit ourselves with the game's own
 * TriggerAnimationExit, which clears isPlaying and fires the exit animation.
 *
 * Every failure path calls TriggerAnimationExit immediately: no clips left, a
 * missing file, a clip that will not decode. The worst case is a splash that
 * advances with no video. It cannot hang. */

typedef void (*fn_trigger_exit)(void *self, void *method);
static fn_trigger_exit s_trigger_exit;

static void *s_splash_self;        /* the SplashScreenVideo whose clip is up  */
static volatile int s_watching;    /* a clip of ours is on screen             */

/* Il2CppArray on arm64: klass, monitor, bounds, max_length, then the vector.
 * Reading a managed array from native earns this much care: a wrong offset is a
 * data abort in the middle of the splash. Every read below is bounds-checked. */
#define ARR_MAX_LENGTH_OFF 0x18u
#define ARR_VECTOR_OFF     0x20u

typedef struct { char _hdr[0x10]; int32_t len; uint16_t chars[1]; } Il2CppStr;

static int str_to_ascii(const void *managed, char *out, size_t outsz) {
  const Il2CppStr *s = (const Il2CppStr *)managed;
  if (!s || s->len <= 0 || s->len > 4096) return 0;
  size_t n = (size_t)s->len < outsz - 1 ? (size_t)s->len : outsz - 1;
  for (size_t i = 0; i < n; i++) {
    uint16_t c = s->chars[i];
    out[i] = (c && c < 0x80) ? (char)c : '?';
  }
  out[n] = 0;
  return 1;
}

/* Waterfall entries look like "jar:file://!/assets/<name>.mp4". Only the
 * basename is used, so a shape this port has not seen cannot walk out of the
 * asset directory. The path goes through fopen, so the asset pack serves it and
 * the clips need not be staged loose in videos/. */
static void entry_to_path(const char *entry, char *out, size_t outsz) {
  const char *slash = strrchr(entry, '/');
  const char *name = slash ? slash + 1 : entry;
  if (!*name) { out[0] = 0; return; }
  snprintf(out, outsz, "%s/assets/%s", bp_game_root(), name);
}

static void splash_exit(void *self) {
  s_splash_self = NULL;
  s_watching = 0;
  if (!self) return;
  *(volatile uint8_t *)((char *)self + BATTD_OFF_isPlaying) = 0;
  if (s_trigger_exit) s_trigger_exit(self, NULL);
}

static void hk_TryPlayNextVideoInWaterfall(void *self, void *source,
                                           void *errmsg, void *method) {
  (void)source; (void)errmsg; (void)method;
  if (!self) return;

  for (;;) {
    void *arr = *(void **)((char *)self + BATTD_OFF_videoClipPathWaterfall);
    const int32_t idx = *(int32_t *)((char *)self + BATTD_OFF_currentClipIdx);
    if (!arr || idx < 0) break;
    const uint64_t n = *(uint64_t *)((char *)arr + ARR_MAX_LENGTH_OFF);
    if (n >= 64u || (uint64_t)idx >= n) break;          /* out of clips */

    *(int32_t *)((char *)self + BATTD_OFF_currentClipIdx) = idx + 1;
    void *entry = *(void **)((char *)arr + ARR_VECTOR_OFF + 8u * (unsigned)idx);

    char raw[384], path[384];
    if (!str_to_ascii(entry, raw, sizeof raw)) continue;
    entry_to_path(raw, path, sizeof path);
    if (!path[0]) continue;

    debugPrintf("[video] splash clip %d/%llu: %s\n",
                (int)idx, (unsigned long long)n, raw);
    battd_video_play_path(path);
    if (battd_video_is_playing()) {
      *(volatile uint8_t *)((char *)self + BATTD_OFF_isPlaying) = 1;
      s_splash_self = self;
      s_watching = 1;
      return;                       /* the pump ends the splash when it does */
    }
    debugPrintf("[video] that clip would not decode -- trying the next\n");
  }

  debugPrintf("[video] no playable clip left -- advancing the splash\n");
  splash_exit(self);
}

/* Called once per frame from bp_boot.c's loop, straight after nativeRender().
 *
 * NOT from the eglSwapBuffers wrapper, which is where this used to live. This
 * game runs with MT rendering on, so the swap happens on Unity's GL worker
 * thread, and TriggerAnimationExit is managed code that reaches GetComponent and
 * the Animator. It has to run on the thread that runs managed code -- the one
 * that calls nativeRender. battd_video_draw stays in the swap wrapper, because
 * that IS the GL thread and that is where its GL belongs. */
void battd_il2cpp_pump(void) {
  if (!s_watching) return;
  if (battd_video_is_playing()) return;
  debugPrintf("[video] clip finished -- exiting the splash\n");
  splash_exit(s_splash_self);
}

static const uint32_t k_trynext_guard[4] = {
  0xa9bd57f6u, 0xa9014ff4u, 0xa9027bfdu, 0x910083fdu
};
static const uint32_t k_trigexit_guard[4] = {
  0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu, 0xb0010654u
};

static int guard4(uintptr_t addr, const uint32_t g[4]) {
  for (int i = 0; i < 4; i++)
    if (*(volatile uint32_t *)(addr + 4u * i) != g[i]) return 0;
  return 1;
}

static int install_video(so_module *m) {
  uintptr_t b = (uintptr_t)m->load_virtbase;
  uintptr_t tn = b + BATTD_RVA_TryPlayNextVideoInWaterfall;
  uintptr_t te = b + BATTD_RVA_TriggerAnimationExit;

  /* TriggerAnimationExit is CALLED, never patched -- the guard is only here to
   * refuse a wrong address rather than jump into the middle of something. */
  if (!guard4(te, k_trigexit_guard)) {
    debugPrintf("[video] TriggerAnimationExit GUARD FAILED -- splash video "
                "disabled (without it a played clip could not end the splash)\n");
    return 0;
  }
  s_trigger_exit = (fn_trigger_exit)te;

  if (!guard4(tn, k_trynext_guard)) {
    debugPrintf("[video] TryPlayNextVideoInWaterfall GUARD FAILED -- splash "
                "video disabled\n");
    s_trigger_exit = NULL;
    return 0;
  }
  hook_arm64(tn, (uintptr_t)&hk_TryPlayNextVideoInWaterfall);
  debugPrintf("[video] SplashScreenVideo::TryPlayNextVideoInWaterfall hooked @ "
              "il2cpp+0x%06x (TriggerAnimationExit at +0x%06x)\n",
              (unsigned)BATTD_RVA_TryPlayNextVideoInWaterfall,
              (unsigned)BATTD_RVA_TriggerAnimationExit);
  return 1;
}

#else   /* BATTD_VIDEO 0 */
void battd_il2cpp_pump(void) { }
static int install_video(so_module *m) { (void)m; return 0; }
#endif

int battd_il2cpp_install(so_module *il2cpp) {
  if (!il2cpp || !il2cpp->load_virtbase) {
    debugPrintf("[il2cpp] not mapped; managed hooks skipped\n");
    return 0;
  }
  int n = 0;
  n += patch_art_tier(il2cpp);
  n += install_video(il2cpp);
  return n;
}
