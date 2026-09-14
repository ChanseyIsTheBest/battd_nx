/* ---------------------------------------------------------------------------
 * bp_tmclock.c -- the engine clock for Unity 2020.3 (adopted from badpiggies_nx).
 *
 * TimeManager::Update(double newTime) is the per-frame entry of Unity's clock.
 * On a phone, newTime is derived from Choreographer/display timestamps, which do
 * not exist here, so the engine clock stalls or jumps: deltaTime goes wrong for
 * EVERY consumer -- C# Time.*, Animator, particles, coroutine WaitForSeconds and
 * async scene loads. Hooking the managed Time getters (the Unity 6 approach)
 * fixes only what C# reads; this fixes the source.
 *
 * The detour replays the prologue's frameCount++ / aux++ / pause gate exactly
 * (fields 0xc8/0xd0/0xf8, pinned by the 15-word guard), then enters the body
 * through bp_tm_trampoline.s with a live monotonic newTime. Everything else --
 * timeScale, maximumDeltaTime clamping, fixed-step accumulation -- is still the
 * engine's own code, so native systems and C# agree and Time.timeScale = 0
 * pauses animation too.
 *
 * A stall thread keeps the clock moving while the main thread is parked >100 ms
 * in a synchronous scene load, so load-progress timers do not freeze. trylock
 * only: it never waits on the main thread's tick.
 * MIT.
 * ------------------------------------------------------------------------- */
#include <stdint.h>
#include <switch.h>

#include "bp_managed.h"
#include "bp_offsets.h"
#include "config.h"
#include "jni_fake.h"
#include "so_util.h"
#include "util.h"

uint64_t g_tm_body_target;                        /* read by bp_tm_trampoline.s */
extern void bp_tm_call_body(void *tm, double newTime);

#define TM_STALL_NS 100000000ull

static int g_active;
static uint64_t g_base_ns;
static void *volatile g_tm;
static Mutex g_lock;
static volatile uint64_t g_last_main_ns;
static uint64_t g_ticks_main, g_ticks_thread;
static Thread g_thr;
static volatile int g_thr_run;

static uint64_t tm_now_ns(void) { return armTicksToNs(armGetSystemTick()); }

static void tm_tick(void *tm) {
  uint64_t n = tm_now_ns();
  if (!g_base_ns) g_base_ns = n;
  bp_tm_call_body(tm, (double)(n - g_base_ns) / 1e9);
}

void bp_tm_update_hook(void *tm, double newTime_ignored) {
  (void)newTime_ignored;
  g_tm = tm;
  g_last_main_ns = tm_now_ns();
  *(volatile uint64_t *)((char *)tm + BP_TM_FIELD_FRAMECOUNT) += 1;
  *(volatile uint32_t *)((char *)tm + BP_TM_FIELD_AUX) += 1;
  if (*(volatile uint8_t *)((char *)tm + BP_TM_FIELD_PAUSE) != 0) return;   /* stock: paused -> ret */
  mutexLock(&g_lock);
  tm_tick(tm);
  mutexUnlock(&g_lock);
  if (++g_ticks_main == 1)
    debugPrintf("[tmclock] first TimeManager::Update through the hook (tm=%p)\n", tm);
}

int bp_tmclock_active(void) { return g_active; }

int bp_tmclock_install(so_module *unity) {
#if !BP_TM_CLOCK
  (void)unity;
  debugPrintf("[tmclock] disabled at compile time (BP_TM_CLOCK 0)\n");
  return 0;
#else
  static const uint32_t body_word[1] = { BP_TM_BODY_WORD };
  if (!bp_guard_ok(unity, BP_RVA_TimeManager_Update, BP_GUARD_TimeManager_Update, 15,
                   "TimeManager::Update prologue") ||
      !bp_guard_ok(unity, BP_RVA_TimeManager_Update + BP_TM_BODY_OFF, body_word, 1,
                   "TimeManager::Update body")) {
    debugPrintf("[tmclock] NOT installed -- the Time icall hooks will carry managed time\n");
    return 0;
  }
  mutexInit(&g_lock);
  uintptr_t b = (uintptr_t)unity->load_virtbase;
  g_tm_body_target = (uint64_t)(b + BP_RVA_TimeManager_Update + BP_TM_BODY_OFF);
  hook_arm64(b + BP_RVA_TimeManager_Update, (uintptr_t)&bp_tm_update_hook);
  g_active = 1;
  debugPrintf("[tmclock] TimeManager::Update hooked at +0x%x (body +0x%x, 0x20-frame trampoline)\n",
              BP_RVA_TimeManager_Update, BP_TM_BODY_OFF);
  return 1;
#endif
}

static void tm_thread(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);       /* engine code reads its stack guard through tpidr */
  while (g_thr_run && !jni_quit_requested) {
    svcSleepThread(4000000ull);
    void *tm = g_tm;
    if (tm && g_active && (tm_now_ns() - g_last_main_ns) > TM_STALL_NS && mutexTryLock(&g_lock)) {
      tm_tick(tm);
      mutexUnlock(&g_lock);
      if (++g_ticks_thread == 1)
        debugPrintf("[tmclock] main thread parked >100 ms: stall thread advancing the clock\n");
    }
  }
}

void bp_tmclock_start(void) {
#if BP_TM_CLOCK && BP_TM_CLOCK_THREAD
  if (!g_active) return;
  g_thr_run = 1;
  Result rc = threadCreate(&g_thr, tm_thread, NULL, NULL, 0x8000, 0x2C, -2);
  if (R_SUCCEEDED(rc)) rc = threadStart(&g_thr);
  if (R_FAILED(rc)) { g_thr_run = 0; debugPrintf("[tmclock] stall thread failed (0x%x)\n", rc); }
  else debugPrintf("[tmclock] stall thread started\n");
#endif
}
