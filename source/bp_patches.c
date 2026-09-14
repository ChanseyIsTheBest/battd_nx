/* ---------------------------------------------------------------------------
 * bp_patches.c -- guarded code patches for Bloons Pop's libunity.so
 * (Unity 2020.3.15f2, BuildID e3e37b8a...). Every patch verifies the game's own
 * leading words (bp_offsets.h) before writing and SKIPS on mismatch: a skipped
 * patch is a named line in debug.log, a misplaced one is a crash with no cause.
 *
 * Applied once, after libunity is mapped and before ANY engine code runs --
 * the allocator table must be in place before MemoryManager's static init.
 *
 * Deliberately NOT here:
 *   vsync       pumped, not patched (bp_vsync.c)
 *   TimeManager the engine clock is a detour, not a patch (bp_tmclock.c),
 *               installed at the end of bp_apply_patches().
 * MIT.
 * ------------------------------------------------------------------------- */
#include <stdint.h>
#include <string.h>

#include "bp_managed.h"
#include "bp_net.h"
#include "bp_offsets.h"
#include "bp_patch_granularity.h"
#include "config.h"
#include "so_util.h"
#include "util.h"

extern so_module unity_mod;
static int g_applied, g_skipped;

static int patch_words(const char *what, uint32_t rva, const uint32_t guard[4],
                       const uint32_t *words, int n) {
  if (!bp_guard_ok(&unity_mod, rva, guard, 4, what)) { g_skipped++; return 0; }
  uint32_t *p = (uint32_t *)((uintptr_t)unity_mod.load_virtbase + rva);
  if (so_patch_code(p, words, (size_t)n * 4) != 0) {
    debugPrintf("[patch] %s FAILED: so_patch_code could not write +0x%x\n", what, (unsigned)rva);
    g_skipped++;
    return 0;
  }
  debugPrintf("[patch] %-40s +0x%07x (%d word%s)\n", what, (unsigned)rva, n, n == 1 ? "" : "s");
  g_applied++;
  return 1;
}

/* Single-word site: the guard is that one word only (neighbours may differ). */
static int patch_guard1(const char *what, uint32_t rva, const uint32_t guard[4], const uint32_t *word) {
  if (!bp_guard_ok(&unity_mod, rva, guard, 1, what)) { g_skipped++; return 0; }
  if (so_patch_code((uint32_t *)((uintptr_t)unity_mod.load_virtbase + rva), word, 4) != 0) {
    debugPrintf("[patch] %s FAILED to write +0x%x\n", what, (unsigned)rva);
    g_skipped++;
    return 0;
  }
  debugPrintf("[patch] %-40s +0x%07x (0x%08x -> 0x%08x)\n", what, (unsigned)rva, guard[0], word[0]);
  g_applied++;
  return 1;
}

static const uint32_t P_RET0[]  = { 0x52800000u, 0xd65f03c0u };   /* mov w0,#0  ; ret */
static const uint32_t P_RET2[]  = { 0x52800040u, 0xd65f03c0u };   /* mov w0,#2  ; ret */
static const uint32_t P_RET33[] = { 0x52800420u, 0xd65f03c0u };   /* mov w0,#33 ; ret */
static const uint32_t P_RET[]   = { 0xd65f03c0u };                /* ret */

/* 256 MB -> 64 MB region granularity. VERIFY ALL 21, THEN WRITE ALL 21: the
 * words implement one computation across ten functions, and a half-patched
 * allocator computes region indices two ways at once (free-list corruption,
 * not a clean failure). See bp_patch_granularity.h for the derivation. */
static int apply_granularity(void) {
  uintptr_t base = (uintptr_t)unity_mod.load_virtbase;
  if (!base) { debugPrintf("[gran] libunity not mapped\n"); return 0; }
  int bad = 0;
  for (int i = 0; i < BP_GRANULARITY_WORDS_N; i++) {
    const BpPatchWord *p = &BP_GRANULARITY_WORDS[i];
    uint32_t live = *(const uint32_t *)(base + p->rva);
    if (live != p->from) {
      debugPrintf("[gran] site %d MISMATCH at +0x%x: expected 0x%08x, found 0x%08x (%s)\n",
                  i, (unsigned)p->rva, p->from, live, p->what);
      bad++;
    }
  }
  if (bad) {
    debugPrintf("[gran] %d of %d sites differ -- NOTHING PATCHED. This is not the\n"
                "       libunity.so the table was derived from; rerun tools/offsets.\n",
                bad, BP_GRANULARITY_WORDS_N);
    g_skipped++;
    return 0;
  }
  for (int i = 0; i < BP_GRANULARITY_WORDS_N; i++) {
    const BpPatchWord *p = &BP_GRANULARITY_WORDS[i];
    if (so_patch_code((uint32_t *)(base + p->rva), &p->to, 4) != 0) {
      debugPrintf("[gran] FATAL: write failed at site %d AFTER %d writes; allocator is\n"
                  "       half-patched, do not trust this run\n", i, i);
      return -1;
    }
  }
  debugPrintf("[gran] %d words in 10 functions: block allocator 256 MB -> %u MB regions\n",
              BP_GRANULARITY_WORDS_N, BP_REGION_GRANULARITY_MB);
  g_applied++;
  return 1;
}

int bp_apply_patches(void) {
  g_applied = g_skipped = 0;
  apply_granularity();   /* FIRST: decides whether the engine can allocate at all */

  /* No APK exists; without this Unity zip-scans a non-zip on every open. */
  patch_words("fs/apk-canhandle-false", BP_RVA_APK_CanHandle, BP_GUARD_APK_CanHandle, P_RET0, 2);
  /* 2020.3 returns 1 (AudioTrack, a Java path) or 2 (OpenSL, ours). */
  patch_words("audio/select-opensl", BP_RVA_GetAndroidAudioOutputType,
              BP_GUARD_GetAndroidAudioOutputType, P_RET2, 2);
  /* Agree with jni_fake's SDK_INT so nothing sees two API levels. */
  patch_words("android/api-level-33", BP_RVA_ApiLevel, BP_GUARD_ApiLevel, P_RET33, 2);

#if BP_PATCH_SWAPPY
  /* Swappy must never start: its Java choreographer thread would wait for
   * callbacks that cannot arrive. IsEnabledAndActive() == 0 also routes
   * WaitVSync to the counter wait that bp_vsync.c services. */
  patch_words("swappy/is-enabled-and-active=0", BP_RVA_Swappy_IsEnabledAndActive,
              BP_GUARD_Swappy_IsEnabledAndActive, P_RET0, 2);
  patch_words("swappy/gl-init=noop", BP_RVA_SwappyGL_Init, BP_GUARD_SwappyGL_Init, P_RET, 1);
  patch_words("swappy/check-device-support=0", BP_RVA_Swappy_CheckDeviceSupport,
              BP_GUARD_Swappy_CheckDeviceSupport, P_RET0, 2);
  patch_words("swappy/update-swap-interval=noop", BP_RVA_Swappy_UpdateSwapInterval,
              BP_GUARD_Swappy_UpdateSwapInterval, P_RET, 1);
  patch_words("swappy/update-frame-interval=noop", BP_RVA_Swappy_UpdateFrameInterval,
              BP_GUARD_Swappy_UpdateFrameInterval, P_RET, 1);
#endif

#if BP_PATCH_FRAMETIMETRACKER
  /* THE frame-2 hang on 2020.3 (badpiggies_nx): the FrameTimeTracker ctor starts
   * an Android Looper over JNI and waits for a thread that is never spawned.
   * s_FrameTimeTracker stays NULL; EnableFrameTimeTracker and its Disable twin
   * are the only two references and both null-check it. */
  patch_words("frametimetracker/enable=ret", BP_RVA_EnableFrameTimeTracker,
              BP_GUARD_EnableFrameTimeTracker, P_RET, 1);
#endif

#if BP_PATCH_FMOD
  /* FMOD OpenSL output (bouncemasters_nx's three changes; 2020.3's FMOD is
   * instruction-identical). Without them FMOD either refuses the device
   * (error 60, the bound check) or initialises with 0 up-front buffers and is
   * silent forever. Independent single-word patches, each guarded. */
  {
    const uint32_t g_ldr[4] = { BP_GUARD_FMOD_PERIOD_LDR }, g_cbz[4] = { BP_GUARD_FMOD_PERIOD_CBZ };
    const uint32_t g_bls[4] = { BP_GUARD_FMOD_BUFGEOM_BRANCH }, g_div[4] = { BP_GUARD_FMOD_BUFCOUNT };
    const uint32_t w_mov[1] = { 0x52800000u | ((BP_AUDIO_PERIOD_FRAMES & 0xffffu) << 5) | 9u };  /* movz w9,#period */
    const uint32_t w_str[1] = { 0xb903fa69u };                                                  /* str w9,[x19,#0x3f8] */
    const uint32_t w_b[1]   = { 0x14000004u };                                                  /* b +0x10 (same target) */
    const uint32_t w_cnt[1] = { 0x52800000u | ((BP_AUDIO_UPFRONT_BUFFERS & 0xffffu) << 5) | 9u };/* movz w9,#buffers */
    patch_guard1("audio/fmod-period-const", BP_RVA_FMOD_PERIOD_LDR, g_ldr, w_mov);
    patch_guard1("audio/fmod-period-store", BP_RVA_FMOD_PERIOD_CBZ, g_cbz, w_str);
    patch_guard1("audio/fmod-bufgeom-branch", BP_RVA_FMOD_BUFGEOM_BRANCH, g_bls, w_b);
#if BP_AUDIO_UPFRONT_BUFFERS
    patch_guard1("audio/fmod-upfront-buffers", BP_RVA_FMOD_BUFCOUNT, g_div, w_cnt);
#endif
  }
#endif

  /* Application.internetReachability -> our nifm-backed answer (bp_net.c).
   * The binding itself is a 4-byte tail call, so the redirect goes on the
   * JNI function it lands in, which has 356 bytes of room. */
  if (bp_redirect(&unity_mod, BP_RVA_DVM_GetInternetReachability,
                  BP_GUARD_DVM_GetInternetReachability, 4,
                  (void *)&bp_net_reachability, "net/internet-reachability"))
    g_applied++;
  else
    g_skipped++;

  /* Engine clock last: a detour over TimeManager::Update's entry. */
  if (bp_tmclock_install(&unity_mod)) g_applied++; else g_skipped++;

  debugPrintf("[patch] %d applied, %d skipped\n", g_applied, g_skipped);
  return g_applied;
}
