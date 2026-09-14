/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include <time.h>
#include "config.h"
#include "bp_root.h"

// File-only logger (DEBUG_LOG builds only): open once + flush per line so the tail survives a
// crash, mutex-serialised across engine threads. Drops the high-frequency dlsym/dlopen/JNI spam.
#if DEBUG_LOG
static Mutex g_log_lock;
static int log_is_noisy(const char *t) {
  return !strncmp(t, "dlsym", 5) || !strncmp(t, "dlopen", 6) ||
         !strncmp(t, "JNI ", 4)  || !strncmp(t, "JNI:", 4) || !strncmp(t, "[jni]", 5);
}
#endif

/* Log file, deliberately NOT flushed per line.
 *
 * WHY THIS MATTERS MORE THAN IT LOOKS
 * Unity's Debug.Log reaches us through __android_log_print -> debugPrintf, and
 * the engine logs heavily during startup (the Play Games plugin alone emits a
 * dozen lines). This used to fflush() on every call, and each flush is a
 * synchronous fs IPC to the SD card. The first on-hardware loading-screen hang
 * was exactly this: seven watchdog stalls over 48 seconds, and at EVERY one the
 * engine main thread was parked in
 *     svcSendSyncRequest <- fsFileWrite <- fsdev_write <- _write_r
 * i.e. it was not deadlocked, it was spending all of frame 0 writing our own log
 * one flush at a time.
 *
 * Now: a 64 KB buffer, flushed on a timer, on important lines, and explicitly by
 * the crash handler. A crash still gets a complete log because
 * debug_log_flush() is called from the exception path before anything else. */
static FILE   *g_logf = NULL;
static char    g_logbuf[64 * 1024];
static uint64_t g_log_last_flush_ns = 0;
static int      g_log_dirty = 0;

#define LOG_FLUSH_INTERVAL_NS  10000000000ull  /* 10s */

static uint64_t log_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Lines that must reach the card immediately.
 *
 * This list is a cost, not a preference: each match is a blocking SD write on
 * whichever thread happened to log, so a prefix that fires periodically forces
 * a flush at that rate no matter what LOG_FLUSH_INTERVAL_NS says.
 *
 * The list had grown to include prefixes that are now PER-FRAME. Counted in a
 * 700-line run:
 *
 *     [vsync]  154   almost all "tick N counter=N dt=N"      (once a second)
 *     [gfx]    123   almost all "swap N enter" / "swap N ok" (once a frame)
 *     [boot]   113   almost all "frame N rendered"           (once a frame)
 *
 * ~390 forced SD writes in one short session, and the 10-second timer never got
 * a chance to do its job. Each of those three earned its place when the port
 * was dying at frame 0 and a buffered marker was a marker that never arrived.
 * The game boots and runs now, so they are buffered like everything else.
 *
 * What stays immediate is what a post-mortem reader actually needs and cannot
 * reconstruct: crashes, the watchdog, the frame-loop marker, and any line
 * announcing a failure. Those are all rare by construction. The crash handler
 * also calls debug_log_flush() before anything else, so a crash still gets a
 * complete log regardless of what is buffered.
 *
 * Rule of thumb for adding to this list: if it can fire more than a few times
 * per session, it does not belong here. */
static int log_is_important(const char *t) {
  /* Tolerate a leading newline: several diagnostic lines start "\n[wd]" /
   * "\n[crash]", and a plain prefix test silently downgraded them to
   * "buffer it, flush later" -- i.e. the most urgent lines were the ones most
   * likely to be lost. */
  while (*t == '\n' || *t == '\r') t++;

  return !strncmp(t, "[crash]", 7) || !strncmp(t, "[wd]", 4) ||
         !strncmp(t, "[loop]", 6) ||
         /* One-shot subsystem traces: each fires a handful of times at startup
          * and then never again, so flushing them is effectively free and
          * losing one costs a test cycle. */
         !strncmp(t, "[gc]", 4)     || !strncmp(t, "[gpua]", 6) ||
         !strncmp(t, "[dlc]", 5)    || !strncmp(t, "[fmod]", 6) ||
         !strncmp(t, "[input]", 7)  || !strncmp(t, "[touch]", 7) ||
         !strncmp(t, "[screen]", 8) || !strncmp(t, "[region]", 8) ||
         !strncmp(t, "[video]", 7)  || !strncmp(t, "[lang]", 6) ||
         /* Any failure, whatever its prefix. */
         strstr(t, "FATAL") || strstr(t, "ABORT") || strstr(t, "failed") ||
         strstr(t, "REFUSED");
}

/* INDEPENDENT log for the watchdog.
 *
 * The watchdog must NOT use debugPrintf. debugPrintf holds g_log_lock across
 * fflush(), and fflush is a blocking SD write that the engine main thread has
 * been observed parked inside. A watchdog using the shared path therefore blocks
 * on a lock held by precisely the thread it exists to report on -- which is what
 * happened: seven stall dumps in run 3, then total silence once buffering was
 * added, in exactly the runs where a stall dump mattered most.
 *
 * Separate FILE*, separate lock, separate file. Slow (open/write/close per line)
 * but it only runs during a stall, and it cannot be starved by the main log. */
static Mutex g_stall_lock;
#if DEBUG_LOG
static int lock_bounded(Mutex *m, volatile int *wedged, const char *what);
static volatile int g_log_wedged;
static volatile unsigned g_log_dropped;
#endif
static int   g_stall_init;

int stallPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;
  char path[600];
  static volatile int stall_wedged;
  if (!g_stall_init) { mutexInit(&g_stall_lock); g_stall_init = 1; }
  if (!lock_bounded(&g_stall_lock, &stall_wedged, "stall.log")) return 0;
  stall_wedged = 0;
  snprintf(path, sizeof path, "%s/stall.log", bp_game_root());
  FILE *f = fopen(path, "a");
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);                 /* close each time: never hold a handle open */
  }
  mutexUnlock(&g_stall_lock);
#else
  (void)text;
#endif
  return 0;
}

void debug_log_flush(void) {
#if DEBUG_LOG
  if (!lock_bounded(&g_log_lock, &g_log_wedged, "debug.log")) return;
  if (g_logf && g_log_dirty) { fflush(g_logf); g_log_dirty = 0; }
  g_log_last_flush_ns = log_now_ns();
  mutexUnlock(&g_log_lock);
#endif
}

/* ---- log locks that can never wedge the process ------------------------------
 * The GC bridge really pauses threads. If one is paused while it holds a log
 * lock (debugPrintf holds g_log_lock across a blocking SD flush), every other
 * thread that logs would wait for as long as that thread stays paused -- the
 * fourth hardware run froze exactly there: stall.log got the watchdog's [mt] line,
 * debug.log never did. So: diag.c never pauses a thread that owns either lock
 * (util_*_lock_owner, exact: a libnx Mutex stores its owner's handle), and as
 * a second line of defence no log call waits more than 2 s. After that it drops
 * lines (counted, reported) and names the holder in stall.log. */
static uint32_t mutex_owner(const Mutex *m) { return (*(volatile const uint32_t *)m) & ~0x40000000u; }
#if DEBUG_LOG
uint32_t util_log_lock_owner(void)   { return mutex_owner(&g_log_lock); }
uint32_t util_stall_lock_owner(void) { return mutex_owner(&g_stall_lock); }
static int lock_bounded(Mutex *m, volatile int *wedged, const char *what) {
  if (mutexTryLock(m)) return 1;
  if (*wedged) return 0;                          /* known wedged: never wait twice */
  for (int i = 0; i < 2000; i++) {                /* 2 s, 1 ms steps */
    svcSleepThread(1000000ull);
    if (mutexTryLock(m)) return 1;
  }
  *wedged = 1;
  if (m != &g_stall_lock) {
    extern const char *diag_name_for_handle(uint32_t h);
    uint32_t o = mutex_owner(m);
    stallPrintf("[log] %s lock held >2s by thread handle 0x%x (%s); dropping lines until it is released\n",
                what, (unsigned)o, diag_name_for_handle(o));
  }
  return 0;
}
#else
uint32_t util_log_lock_owner(void)   { return 0; }
uint32_t util_stall_lock_owner(void) { return 0; }
#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;
  int want_flush = 0;
  if (log_is_noisy(text)) return 0;
  if (!lock_bounded(&g_log_lock, &g_log_wedged, "debug.log")) { g_log_dropped++; return 0; }
  if (g_log_wedged) {
    g_log_wedged = 0;
    if (g_logf) fprintf(g_logf, "[log] %u debug lines were dropped while the log lock was held\n", g_log_dropped);
  }
  if (!g_logf) {
    g_logf = fopen(bp_log_path(), "a");
    if (g_logf) setvbuf(g_logf, g_logbuf, _IOFBF, sizeof g_logbuf);
    g_log_last_flush_ns = log_now_ns();
  }
  if (g_logf) {
    va_start(list, text);
    vfprintf(g_logf, text, list);
    va_end(list);
    g_log_dirty = 1;
    uint64_t now = log_now_ns();
    want_flush = (log_is_important(text) ||
                  now - g_log_last_flush_ns >= LOG_FLUSH_INTERVAL_NS);
    if (want_flush) { g_log_dirty = 0; g_log_last_flush_ns = now; }
  }
  mutexUnlock(&g_log_lock);

  /* Flush OUTSIDE the lock. Holding a global mutex across a blocking SD write
   * makes every other thread that logs wait on whichever thread is currently
   * stuck in the filesystem -- which silenced the watchdog in exactly the runs
   * where it was needed. newlib serialises the FILE* internally. */
  if (want_flush && g_logf) fflush(g_logf);
#else
  (void)text;
#endif
  return 0;
}

// Per-thread bionic TLS. The engine reads its stack canary from tpidr_el0+0x28;
// every thread that runs engine code needs its OWN zeroed block here. A single
// shared block races: one thread's TLS writes (including the guard slot) corrupt
// another thread's in-flight canary, tripping a false __stack_chk_fail. `buf`
// must outlive the thread (TPIDR_EL0 points into it until the thread exits).
void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  armSetTlsRw((uint8_t *)buf + BIONIC_TLS_TP_OFFSET);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
