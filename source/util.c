/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
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
static unsigned g_log_buffered;        /* bytes in g_logbuf since the last flush */
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
         /* The eviction instrument. Both are bounded by construction -- a
          * blocked delete is capped at 20 per run and a real one is the event
          * being hunted -- so the per-line SD write is affordable, and losing
          * one costs a hardware run. [trace] is deliberately NOT here: opens,
          * reads, fstats and closes run to 400+ lines and flushing each would
          * be the per-line-flush stall all over again. */
         !strncmp(t, "[cache]", 7)  || !strncmp(t, "[evict]", 7) ||
         /* Meta: lines were lost. Rare, and if one of these is itself lost the
          * log silently claims completeness it does not have. */
         !strncmp(t, "[log]", 5)    ||
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
static int lock_bounded(Mutex *m, volatile int *wedged, const char *what, int ignore_wedged);
static volatile int g_log_wedged;
static volatile unsigned g_log_dropped;
static volatile unsigned g_log_wedge_events;   /* distinct wedges, not reports */
#endif
static int   g_stall_init;

/* LOCK-FREE, PRE-OPENED, COMMITTED WRITERS -- crash.log and wd.log.
 *
 * Three separate things had to be true for the crash dump to land, and each
 * one was learned by losing a run:
 *
 * 1. No lock. It used to go through stallPrintf, whose lock_bounded() gives up
 *    SILENTLY after 2 s -- so a watchdog thread mid-dump could erase the whole
 *    crash report by holding g_stall_lock.
 *
 * 2. Committed, not just flushed. The version before this kept one FILE open
 *    and fflush()ed per line. The result on hardware was a crash.log that
 *    EXISTED AND WAS ZERO BYTES, while stall.log -- same card, same process,
 *    same instant -- had content. stall.log fopen/fclose's per line; this did
 *    not. Written data on this system needs fsdevCommitDevice to survive the
 *    process being killed, which is exactly what nx_sd_flush() exists for.
 *
 * 3. No allocation. When a thread faults, Horizon stops the WHOLE process and
 *    runs this handler on the faulting thread. If that thread was inside
 *    malloc, it still holds newlib's heap lock -- and fopen/vfprintf both
 *    allocate. A handler that blocks there never returns and never breaks, so
 *    the process stays stopped: a frozen screen, every log dead, and HOME still
 *    able to kill it. That is the reported symptom, and a zero-byte file is
 *    what it leaves behind. So the descriptor is opened at boot, formatting
 *    goes into a static buffer, and the handler only calls write().
 *
 * The watchdog gets the same writer, for the same reason with a different
 * face: through six frozen boots it has never once delivered a thread dump,
 * and every one of its lines went through stallPrintf. If the process is
 * alive and the card is writable, this lands. If this does not land either,
 * that is itself the finding.
 *
 * No fallback in either: a fallback would reintroduce (3) on the path that
 * needs it most. A lost dump beats a handler that hangs. */
/* THE WRITER IS libnx fs, NOT newlib. The previous version called write(),
 * and on a guest thread that faults: newlib's _write_r -> __get_handle takes
 * __libc_lock_acquire on a lock word in bss, and the last two crash reports
 * show the exception handler recursing through exactly that -- a fault, the
 * handler, crashPrintf, write(), a fault in __get_handle, the handler again --
 * until exception delivery itself broke and Atmosphère wrote "Instruction
 * Abort at _start". Every guest-thread crash this port has had ended that way;
 * the one dump that ever landed was from the main thread. fsFileWrite is a
 * plain IPC on the calling thread's own TLS command buffer: no newlib, no
 * lock, no reent, valid on any thread. FsWriteOption_Flush plus fsFsCommit per
 * line is what makes it survive the process being killed. */
typedef struct { FsFile file; s64 off; int ok; char buf[1024]; } RawLog;
static RawLog g_crashlog, g_wdlog, g_iolog;
static FsFileSystem *g_rawfs;

static void rawlog_init(RawLog *l, const char *name) {
#if DEBUG_LOG
  if (l->ok) return;
  if (!g_rawfs) g_rawfs = fsdevGetDeviceFileSystem("sdmc");
  if (!g_rawfs) { debugPrintf("[log] no sdmc filesystem for %s\n", name); return; }
  /* fs paths have no "sdmc:" prefix */
  char path[600];
  const char *root = bp_game_root();
  if (!strncmp(root, "sdmc:", 5)) root += 5;
  snprintf(path, sizeof path, "%s/%s", root, name);
  fsFsCreateFile(g_rawfs, path, 0, 0);            /* exists -> harmless error */
  Result rc = fsFsOpenFile(g_rawfs, path, FsOpenMode_Write | FsOpenMode_Append, &l->file);
  if (R_FAILED(rc)) { debugPrintf("[log] fsFsOpenFile(%s) failed rc=0x%x -- that log will be empty\n", name, rc); return; }
  s64 sz = 0;
  if (R_SUCCEEDED(fsFileGetSize(&l->file, &sz))) l->off = sz;
  l->ok = 1;
#else
  (void)l; (void)name;
#endif
}

static int rawlog_write_raw(RawLog *l, const char *sbuf, size_t n) {
#if DEBUG_LOG
  if (!l->ok || !sbuf || !n) return 0;
  if (R_SUCCEEDED(fsFileWrite(&l->file, l->off, sbuf, n, FsWriteOption_Flush))) l->off += (s64)n;
  /* Commit per line ONLY for the crash log, where the handler can stop at any
   * point and a dump that reached the card up to there is the whole point.
   * wd.log and io.log commit when their caller asks (rawlog_commit) -- a
   * commit is a FAT metadata write, and doing one every two seconds plus one
   * per cache-entry close was a real load on the card during scene loads. */
  if (l == &g_crashlog && g_rawfs) fsFsCommit(g_rawfs);
#else
  (void)l; (void)sbuf; (void)n;
#endif
  return 0;
}

static int rawlog_vprintf(RawLog *l, const char *fmt, va_list list) {
#if DEBUG_LOG
  if (!l->ok) return 0;
  const int n = vsnprintf(l->buf, sizeof l->buf, fmt, list);
  if (n <= 0) return 0;
  const size_t len = (size_t)n < sizeof l->buf ? (size_t)n : sizeof l->buf - 1;
  return rawlog_write_raw(l, l->buf, len);
#else
  (void)l; (void)fmt; (void)list;
  return 0;
#endif
}

void crash_log_init(void) { rawlog_init(&g_crashlog, "crash.log"); }
void wd_log_init(void)    { rawlog_init(&g_wdlog,    "wd.log"); }
void io_log_init(void)    { rawlog_init(&g_iolog,    "io.log"); }

/* A pre-formatted block of any size, written whole and committed once. For
 * the per-entry syscall op log in libc_shim.c, which is dumped at close. */
int iolog_write(const char *sbuf, size_t n) { const int r = rawlog_write_raw(&g_iolog, sbuf, n); rawlog_commit(); return r; }

void rawlog_commit(void) { if (g_rawfs) fsFsCommit(g_rawfs); }

int crashPrintf(const char *fmt, ...) {
  va_list list; va_start(list, fmt);
  const int r = rawlog_vprintf(&g_crashlog, fmt, list);
  va_end(list); return r;
}
int wdPrintf(const char *fmt, ...) {
  va_list list; va_start(list, fmt);
  const int r = rawlog_vprintf(&g_wdlog, fmt, list);
  va_end(list); return r;
}

int stallPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;
  char path[600];
  static volatile int stall_wedged;
  if (!g_stall_init) { mutexInit(&g_stall_lock); g_stall_init = 1; }
  if (!lock_bounded(&g_stall_lock, &stall_wedged, "stall.log", 0)) return 0;
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

/* Returns 1 if the buffer reached the card, 0 if the lock could not be had and
 * up to 64 KB of log was therefore DISCARDED.
 *
 * That zero is why the last run's debug.log stops mid-trace with no ending. The
 * crash handler calls this first thing, but it went through lock_bounded, which
 * returns immediately when g_log_wedged is set -- and stall.log shows it was set
 * before the crash. So the buffered tail, which is exactly where the eviction
 * lines live, was thrown away. The caller can now say so. */
static int log_flush_impl(int force) {
#if DEBUG_LOG
  if (!lock_bounded(&g_log_lock, &g_log_wedged, "debug.log", force)) return 0;
  /* Take the FILE* under the lock, flush OUTSIDE it: fflush is a blocking SD
   * write, and this file's whole design is that no such write happens with the
   * global log mutex held. debugPrintf has always done it this way; this one
   * did not. */
  FILE *f = (g_logf && g_log_dirty) ? g_logf : NULL;
  g_log_dirty = 0;
  g_log_last_flush_ns = log_now_ns();
  g_log_buffered = 0;
  mutexUnlock(&g_log_lock);
  if (f) fflush(f);
  return 1;
#else
  (void)force;
  return 1;
#endif
}
void debug_log_flush(void)       { (void)log_flush_impl(0); }
int  debug_log_flush_force(void) { return log_flush_impl(1); }
unsigned debug_log_wedge_events(void) {
#if DEBUG_LOG
  return g_log_wedge_events;
#else
  return 0;
#endif
}
unsigned debug_log_dropped(void) {
#if DEBUG_LOG
  return g_log_dropped;
#else
  return 0;
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
/* ignore_wedged: wait out the 2 s even if the lock is already marked wedged.
 * Only the crash path passes 1, and only after diag_resume_all_gc_paused() has
 * made the holder runnable again -- which is the one case where the flag is
 * likely stale rather than informative. Everything else keeps the old
 * never-wait-twice behaviour. */
static int lock_bounded(Mutex *m, volatile int *wedged, const char *what, int ignore_wedged) {
  if (mutexTryLock(m)) return 1;
  if (*wedged && !ignore_wedged) return 0;        /* known wedged: never wait twice */
  for (int i = 0; i < 2000; i++) {                /* 2 s, 1 ms steps */
    svcSleepThread(1000000ull);
    if (mutexTryLock(m)) return 1;
  }
  /* Two threads can be inside the loop above at once, and both then timed out,
   * set the flag and reported -- which is why stall.log carried the identical
   * "held >2s by ... (Background Job.Worker 12)" line twice, same holder handle,
   * for what was ONE wedge. Reading that as two events is wrong. Claim the
   * report atomically so the count means what it says. */
  if (!__atomic_exchange_n(wedged, 1, __ATOMIC_SEQ_CST) && m != &g_stall_lock) {
    extern const char *diag_name_for_handle(uint32_t h);
    extern const char *diag_state_for_handle(uint32_t h);
    uint32_t o = mutex_owner(m);
    g_log_wedge_events++;
    stallPrintf("[log] %s lock held >2s by thread handle 0x%x (%s), which is %s; "
                "dropping lines until it is released\n",
                what, (unsigned)o, diag_name_for_handle(o), diag_state_for_handle(o));
  }
  return 0;
}
#else
uint32_t util_log_lock_owner(void)   { return 0; }
uint32_t util_stall_lock_owner(void) { return 0; }
#endif

/* One body, two entry points.
 *
 * `cls` is what log_is_noisy/log_is_important are tested against, and it MUST be
 * the text that ends up in the file. For debugPrintf the format string is that
 * text closely enough. For a pre-formatted line it is NOT: bp_tr_flush drains
 * the trace ring with debugPrintf("%s", line), so every test above ran against
 * the literal "%s" and matched nothing.
 *
 * That is not cosmetic. It means NO line that arrives through the ring --
 * [cache], [trace]/[evict], [miss], [ram], [assets] miss -- has ever been able
 * to force a flush, whatever log_is_important() says. They reach the card only
 * when the 10 s timer expires or the buffer hits three quarters, and anything
 * still buffered when the log lock wedges is discarded. The eviction instrument
 * is in that set, which is a sufficient explanation for it never producing a
 * line, and it is not one that needed the line to be rare or the path to be
 * wrong. */
#if DEBUG_LOG
static void log_emit(const char *cls, const char *fmt, va_list *ap, const char *literal) {
  int want_flush = 0;
  if (log_is_noisy(cls)) return;
  if (!lock_bounded(&g_log_lock, &g_log_wedged, "debug.log", 0)) { g_log_dropped++; return; }
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
    int wrote;
    if (literal) wrote = (fputs(literal, g_logf) >= 0) ? (int)strlen(literal) : 0;
    else         wrote = vfprintf(g_logf, fmt, *ap);
    g_log_dirty = 1;
    /* Flush BEFORE the buffer fills, not just on the timer.
     *
     * The explicit flush below is deliberately outside the lock. But the FILE is
     * fully buffered over 64 KB, and a FULL buffer makes vfprintf() write to the
     * card by itself -- INSIDE the lock. That is the one case the note below does
     * not cover, and at a 10 second timer a busy boot fills 64 KB long before it
     * expires. Whichever thread happens to overflow the buffer then blocks in the
     * filesystem while holding a global mutex, which is exactly what the watchdog
     * reported twice:
     *
     *   [log] debug.log lock held >2s by ... (Background Job.Worker 1)
     *
     * Flushing at three quarters full keeps the implicit write from ever
     * happening, so nothing writes to the card with the lock held. This is not a
     * consequence of the trace volume -- it needs only enough logging to fill a
     * buffer, which any run does -- but more logging reaches it sooner. */
    g_log_buffered += (unsigned)(wrote > 0 ? wrote : 0);
    uint64_t now = log_now_ns();
    want_flush = (log_is_important(cls) ||
                  g_log_buffered >= (sizeof g_logbuf * 3u) / 4u ||
                  now - g_log_last_flush_ns >= LOG_FLUSH_INTERVAL_NS);
    if (want_flush) { g_log_dirty = 0; g_log_last_flush_ns = now; g_log_buffered = 0; }
  }
  mutexUnlock(&g_log_lock);

  /* Flush OUTSIDE the lock. Holding a global mutex across a blocking SD write
   * makes every other thread that logs wait on whichever thread is currently
   * stuck in the filesystem -- which silenced the watchdog in exactly the runs
   * where it was needed. newlib serialises the FILE* internally. */
  if (want_flush && g_logf) fflush(g_logf);
}
#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;
  va_start(list, text);
  log_emit(text, text, &list, NULL);
  va_end(list);
#else
  (void)text;
#endif
  return 0;
}

/* Pre-formatted line, already ending in '\n'. Use this for anything drained from
 * a queue: the line itself is what gets classified, so log_is_important() can
 * still see [cache] / [evict] and push it to the card. */
int debugPuts(const char *line) {
#if DEBUG_LOG
  if (line) log_emit(line, NULL, NULL, line);
#else
  (void)line;
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
