/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int debugPrintf(char *text, ...);

/* Log a line that is ALREADY formatted. Not the same as debugPrintf("%s", line):
 * the noisy/important tests are applied to the line, not to "%s". Anything
 * drained from a queue must come through here or it can never force a flush. */
int debugPuts(const char *line);

/* Force the log buffer to the card. Called by the crash handler and by
 * nx_sd_flush; debugPrintf otherwise flushes on a 2s timer, because flushing
 * per line made the engine spend all of frame 0 inside fsFileWrite. */
void debug_log_flush(void);

/* Same, but waits out the 2 s even if the log lock is already marked wedged.
 * Returns 1 if the buffer reached the card, 0 if it was DISCARDED -- a zero
 * means the tail of debug.log for that run does not exist. Crash path only, and
 * only after diag_resume_all_gc_paused(). */
int debug_log_flush_force(void);

unsigned debug_log_wedge_events(void);  /* distinct log-lock wedges this run */
unsigned debug_log_dropped(void);       /* lines dropped because of them */

/* Watchdog-only log, written to <root>/stall.log through its own FILE* and lock.
 * MUST be used instead of debugPrintf by anything that reports on a stuck
 * thread: debugPrintf holds a mutex across a blocking SD write, so it deadlocks
 * against the very thread being diagnosed. */
int stallPrintf(char *text, ...);

/* Crash-dump output: <root>/crash.log, NO lock, one handle for the whole dump.
 * The dump must never be silenced by another thread holding a log mutex, which
 * is what happened while it went through stallPrintf. Never call this from
 * anywhere but the exception handler. */
int crashPrintf(const char *fmt, ...);

/* Open crash.log BEFORE anything can fault. Call once, early in boot. The
 * handler runs with the whole process stopped and possibly with newlib's heap
 * lock held by the faulting thread, so it cannot afford to allocate -- which is
 * what an fopen() in the handler does. */
void crash_log_init(void);

/* The watchdog's output: <root>/wd.log, same lock-free pre-opened committed
 * writer as crash.log. Through six frozen boots the watchdog never delivered a
 * thread dump, and every line of it went through stallPrintf. */
void wd_log_init(void);
int  wdPrintf(const char *fmt, ...);
void rawlog_commit(void);   /* commit wd.log/io.log to the card; call after a dump, not per line */

/* <root>/io.log: the per-entry syscall sequence on cache bundles, one block per
 * close. Diff a ram_cache=512 run against a ram_cache=0 run. */
void io_log_init(void);
int  iolog_write(const char *s, size_t n);

void cpu_boost(int on);

// libff4.so reads its stack-protector canary from tpidr_el0 + 0x28.

int ret0(void);
int retm1(void);

/* Point TPIDR_EL0 at a zeroed per-thread block so the engine's stack-protector
 * prologues (which read the canary from TPIDR_EL0+0x28) have a valid, stable
 * bionic TLS. `buf` must outlive the thread. libnx keeps its own thread state in
 * TPIDR_RO_EL0, so commandeering TPIDR_EL0 is safe. EACH THREAD NEEDS ITS OWN
 * BLOCK -- sharing one corrupts the guard slot across threads. */
#define BIONIC_TLS_SIZE 0x400
#define BIONIC_TLS_TP_OFFSET 0x200   /* tp points into the block: headroom for any negative bionic TLS slots */
void install_bionic_tls(void *buf);

/* The main thread's TLS block, owned by main.c. bloonspop_boot.c re-asserts it
 * immediately before entering the engine: install_bionic_tls() re-zeroes the
 * block, and the canary is expected to be 0 (see __stack_chk_guard_fake in
 * imports.c), so re-asserting is idempotent. */
void bp_set_main_tls(void *buf);
void bp_reassert_main_tls(void);

static inline void* armGetTlsRw(void) {
  void* ret;
  __asm__ ("mrs %x[data], s3_3_c13_c0_2" : [data] "=r" (ret));
  return ret;
}

static inline void armSetTlsRw(void *addr) {
  __asm__  ("msr s3_3_c13_c0_2, %0" : : "r"(addr));
}

static inline uint64_t umin(uint64_t a, uint64_t b) {
  return (a < b) ? a : b;
}

#include <stdint.h>
uint32_t util_log_lock_owner(void);    /* handle of the thread holding the debug.log lock, 0 if free */
uint32_t util_stall_lock_owner(void);  /* same, stall.log */

#endif
