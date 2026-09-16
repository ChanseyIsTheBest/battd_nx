/* diag.h -- thread registry + stall watchdog for the frame-1 black-hang hunt.
 *
 * Purpose: a parked thread prints nothing, so we make the hang self-report.
 * Every thread that can block forever publishes a "wait beacon" (what kind of
 * wait, on which object, since when) into a small registry. An independent
 * libnx watchdog thread (NOT routed through the pthread shim we are debugging)
 * notices when frame progress stops and dumps every thread's state to the log.
 *
 * Overhead is near-zero on the happy path: a wait beacon is a few volatile
 * stores, and nothing is logged until a stall is actually detected.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */
#ifndef DIAG_H
#define DIAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* wait kinds, kept in sync with diag_wait_kind_name() in diag.c */
enum {
  DIAG_W_NONE = 0,
  DIAG_W_COND,        /* pthread_cond_wait / timedwait                       */
  DIAG_W_JOIN,        /* pthread_join                                        */
  DIAG_W_SEM,         /* sem_wait / semaphoreWait                            */
  DIAG_W_MUTEX,       /* contended pthread_mutex_lock (uncontended skips it) */
  DIAG_W_RWLOCK,      /* rwlock read/write lock                              */
  DIAG_W_FUTEX,       /* infinite FUTEX_WAIT (re-poll spin)                  */
};

/* Called from the thread that is about to run engine code. `entry` is the
 * guest entry fn (for labelling), is_main_engine marks the first engine thread
 * (android_main). Safe to call once per thread; sets this thread's TLS slot. */
void diag_thread_register(const void *entry, int is_main_engine);
void diag_thread_unregister(void);

/* Record a human name for the current thread (from pthread_setname_np/prctl).
 * If `target` is non-NULL it is a host pthread_t to match; NULL means "self". */
void diag_set_name(void *target_pthread, const char *name);

/* Wait beacons. enter() publishes (kind,obj,now); exit() clears. Cheap. */
void diag_wait_enter(int kind, const void *obj);
void diag_wait_exit(void);

/* Liveness counter for the futex re-poll path: a thread spinning here is alive
 * (value never satisfied), distinct from a hard-parked cond/sem/join. */
void diag_futex_spin(const void *obj);

/* Main render loop calls this once per frame so the watchdog can see progress.
 * `frame` is the current frame index (recorded for the dump header). */
void diag_frame(int frame);

/* Spawn the watchdog. Idempotent. Call once after boot, before the loop. */
void diag_watchdog_start(void);

/* How the GC bridge's "never freeze a thread mid-log" guard is actually doing.
 * gaveup   = times the 50 ms bound expired and a log-lock holder was suspended
 *            anyway. Any non-zero value confirms the bound is too short.
 * waited   = times it had to wait at all;  worst_us = longest wait that resolved.
 * Counters, not logs: wait_not_logging runs under g_pause_lock inside the
 * collector's stop-the-world, where logging is the hazard it guards against.
 * Read this from the frame loop. Any argument may be NULL. */
void diag_log_wait_stats(unsigned *gaveup, unsigned *waited, unsigned *worst_us);

#ifdef __cplusplus
}
#endif

int  diag_pause_pthread(void *target_pthread);    /* GC bridge (daggerfall_nx) */
int  diag_resume_pthread(void *target_pthread);
void diag_resume_all_gc_paused(void);

#include <switch.h>
int  diag_pause_pthread_ctx(void *target_pthread, ThreadContext *ctx);  /* 1 ctx, 2 no ctx, 0 not paused */

const char *diag_name_for_handle(uint32_t h);  /* registered thread name for a kernel handle */
const char *diag_state_for_handle(uint32_t h); /* what it is doing, from its wait beacon */

#endif /* DIAG_H */
