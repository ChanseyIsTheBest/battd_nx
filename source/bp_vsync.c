/* ---------------------------------------------------------------------------
 * bp_vsync.c -- the frame clock Unity 2020.3's Android player expects.
 *
 * WaitVSync(target) locks a mutex and waits on a condvar until a 32-bit counter
 * reaches target (libunity+BP_RVA_WaitVSync; reference WaitVSync(int)). On a
 * phone, Choreographer frame callbacks (a Java proxy) bump that counter. There
 * is no Choreographer here, so without this file the first nativeRender parks
 * in WaitVSync forever. (If Swappy were active WaitVSync would return at once;
 * Swappy is off in this game's PlayerSettings and patched off in bp_patches.c.)
 *
 * PUMP THE COUNTER, DO NOT PATCH THE WAIT (bouncemasters_nx sections 6, 16).
 * A dedicated thread bumps it once per display period under the engine's own
 * mutex, with a broadcast on the engine's own condvar. It must be its own
 * thread: the counter is what nativeRender waits on, so bumping it after
 * nativeRender returns would be bumping it after something that never returns.
 *
 * The mutex/cond are bionic objects in libunity's .bss that the engine locks
 * through the pthread shims imports.c gave it, so this file locks them through
 * the SAME functions (bp_import_lookup). The addresses come from bp_offsets.h
 * and are guard-checked against WaitVSync's own adrp/add before first use.
 * MIT.
 * ------------------------------------------------------------------------- */
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "bp_managed.h"
#include "bp_offsets.h"
#include "bp_vsync.h"
#include "config.h"
#include "so_util.h"
#include "util.h"

extern so_module unity_mod;
uintptr_t bp_import_lookup(const char *name);   /* imports.c */

#define VSYNC_PERIOD_NS 16666667ull             /* 60 Hz panel */

typedef int (*pt_fn)(void *);
static pt_fn s_lock, s_unlock, s_bcast;
static void *s_mutex, *s_cond;
static volatile int32_t *s_counter;
static int s_ok;
static Thread s_thread;
static volatile int s_run;
static uint64_t s_ticks;
static uint8_t s_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));

static uint64_t vs_now_ns(void) { return armTicksToNs(armGetSystemTick()); }

int bp_vsync_init(void) {
#if !BP_PATCH_VSYNC
  debugPrintf("[vsync] disabled at compile time (BP_PATCH_VSYNC 0)\n");
  return -1;
#else
  if (!bp_guard_ok(&unity_mod, BP_RVA_WaitVSync, BP_GUARD_WaitVSync, 4, "WaitVSync") ||
      !bp_guard_ok(&unity_mod, BP_RVA_WaitVSync + 0x20, BP_GUARD_WaitVSync_state, 4,
                   "WaitVSync state references") ||
      !bp_guard_ok(&unity_mod, BP_RVA_GetVSyncCounter, BP_GUARD_GetVSyncCounter, 4,
                   "GetVSyncCounter")) {
    debugPrintf("[vsync] NOT ARMED: WaitVSync does not match bp_offsets.h, so the\n"
                "        first nativeRender will park in it forever. Regenerate the\n"
                "        offsets (tools/offsets/run_all.sh) for this libunity.\n");
    return -1;
  }
  s_lock   = (pt_fn)bp_import_lookup("pthread_mutex_lock");
  s_unlock = (pt_fn)bp_import_lookup("pthread_mutex_unlock");
  s_bcast  = (pt_fn)bp_import_lookup("pthread_cond_broadcast");
  if (!s_lock || !s_unlock || !s_bcast) {
    debugPrintf("[vsync] NOT ARMED: pthread shims not found in the import table\n");
    return -1;
  }
  uintptr_t b = (uintptr_t)unity_mod.load_virtbase;
  s_mutex   = (void *)(b + BP_VSYNC_MUTEX);
  s_cond    = (void *)(b + BP_VSYNC_COND);
  s_counter = (volatile int32_t *)(b + BP_VSYNC_COUNTER);
  s_ok = 1;
  debugPrintf("[vsync] armed: mutex=+0x%x cond=+0x%x counter=+0x%x (value %d)\n",
              BP_VSYNC_MUTEX, BP_VSYNC_COND, BP_VSYNC_COUNTER, (int)*s_counter);
  return 0;
#endif
}

void bp_vsync_tick(void) {
  if (!s_ok) return;
  s_lock(s_mutex);
  (*s_counter)++;
  s_bcast(s_cond);
  s_unlock(s_mutex);
  s_ticks++;
  if ((s_ticks % 600u) == 0)
    debugPrintf("[vsync] tick %llu  counter=%d\n", (unsigned long long)s_ticks, (int)*s_counter);
}

static void pump_main(void *arg) {
  (void)arg;
  install_bionic_tls(s_tls);   /* the shims read the stack guard through tpidr */
  uint64_t next = vs_now_ns() + VSYNC_PERIOD_NS;
  while (s_run) {
    uint64_t t = vs_now_ns();
    if (t < next) { svcSleepThread((s64)(next - t)); continue; }
    bp_vsync_tick();
    next += VSYNC_PERIOD_NS;
    /* Never catch up in a burst: that would hand the engine frames it never
     * rendered. Fall more than 8 periods behind and re-base instead. */
    if (vs_now_ns() > next + 8 * VSYNC_PERIOD_NS) next = vs_now_ns() + VSYNC_PERIOD_NS;
  }
}

int bp_vsync_start(void) {
  if (!s_ok) return -1;
  s_run = 1;
  Result rc = threadCreate(&s_thread, pump_main, NULL, NULL, 0x10000, 0x2B, -2);
  if (R_SUCCEEDED(rc)) rc = threadStart(&s_thread);
  if (R_FAILED(rc)) {
    s_run = 0;
    debugPrintf("[vsync] pump thread failed to start (0x%x)\n", rc);
    return -1;
  }
  debugPrintf("[vsync] pump started, period %llu ns\n", (unsigned long long)VSYNC_PERIOD_NS);
  return 0;
}

void bp_vsync_stop(void) {
  if (!s_run) return;
  s_run = 0;
  threadWaitForExit(&s_thread);
  threadClose(&s_thread);
  debugPrintf("[vsync] pump stopped after %llu ticks\n", (unsigned long long)s_ticks);
}
uint64_t bp_vsync_frames(void) { return 0; }  /* Unity 6 API, inert on 2020.3 */
