/* diag.c -- see diag.h. Frame-1 black-hang instrumentation.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */
#define _GNU_SOURCE
#include "editbox.h"
#include <switch.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "diag.h"
#include "util.h"   /* stallPrintf -- see below, NOT debugPrintf */

/* EVERY line in this file must use stallPrintf, never debugPrintf.
 * debugPrintf takes a global mutex; the thread this file exists to report
 * on is typically blocked inside the filesystem while holding it, so a
 * single debugPrintf here deadlocks the watchdog against its own subject.
 * That is exactly what happened: an earlier pass converted the lines
 * starting "[wd]" but missed two that start "\n[wd]", and the watchdog went
 * silent for four runs -- stall.log contained only the arm line. */
#include "so_util.h" /* so_find_module_by_addr for backtrace symbolication */

/* ------------------------------------------------------------------ tunables */
#define DIAG_MAX_THREADS   96
#define DIAG_POLL_NS       (1000ull * 1000ull * 1000ull)   /* watchdog tick: 1s */
/* 3s, not 6s. The engine's slowest legitimate frame measured 3.9s during the
 * initial scene load, so 6s was chosen to avoid false positives -- but the load
 * frames are now identifiable by their own [loop] SLOW markers, and waiting 6s
 * into a 100% CPU spin risks the console being power-cycled before the dump
 * lands. False positives during load are cheap; a missed dump is not. */
#define DIAG_STALL_NS      (8000ull * 1000ull * 1000ull)   /* declare stall: 8s. First launch copies files to the SD for >3s; a 3s threshold paused every thread for a false alarm. */
#define DIAG_REDUMP_NS     (6000ull * 1000ull * 1000ull)   /* re-dump cadence    */

typedef struct {
  volatile int          in_use;
  uint64_t              tid;            /* svcGetThreadId — matches crash reports */
  Handle                handle;         /* real thread handle for svcGetThreadContext3 */
  pthread_t             pth;            /* host handle, for setname target match  */
  char                  name[32];
  const void           *entry;
  int                   is_main_engine;
  /* live wait beacon */
  volatile int          wait_kind;
  volatile const void  *wait_obj;
  volatile uint64_t     wait_since;     /* tick (raw) when current wait began     */
  /* liveness counters */
  volatile uint64_t     waits_total;
  volatile uint64_t     wakes_total;
  volatile uint64_t     futex_spins;
  volatile uint64_t     last_active;    /* tick of last beacon activity           */
  volatile int          gc_paused;      /* held paused by the GC bridge (daggerfall_nx) */
} DiagThread;

static DiagThread       g_threads[DIAG_MAX_THREADS];
static Mutex            g_reg_lock;     /* zero-init libnx Mutex == unlocked       */
static __thread DiagThread *self;       /* this thread's slot (host TLS)           */

static volatile int      g_frame = -1;
static volatile uint64_t g_last_progress;   /* tick of last diag_frame()           */
static volatile int      g_wd_started;
static Thread            g_wd_thread;

/* ------------------------------------------------------------------ helpers */
static inline uint64_t now_tick(void) { return armGetSystemTick(); }
static inline uint64_t tick_to_ns(uint64_t t) { return armTicksToNs(t); }

static const char *wait_kind_name(int k) {
  switch (k) {
    case DIAG_W_COND:   return "cond_wait";
    case DIAG_W_JOIN:   return "join";
    case DIAG_W_SEM:    return "sem_wait";
    case DIAG_W_MUTEX:  return "mutex_lock";
    case DIAG_W_RWLOCK: return "rwlock";
    case DIAG_W_FUTEX:  return "futex_spin";
    default:            return "running";
  }
}

static DiagThread *slot_alloc(void) {
  mutexLock(&g_reg_lock);
  DiagThread *t = NULL;
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (!g_threads[i].in_use) { t = &g_threads[i]; break; }
  }
  if (t) {
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    t->pth = pthread_self();
    t->handle = threadGetCurHandle();   /* real handle, usable from the watchdog */
    uint64_t tid = 0;
    if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE))) t->tid = tid;
    t->last_active = now_tick();
  }
  mutexUnlock(&g_reg_lock);
  return t;
}

/* Return this thread's slot, lazily allocating one if it ran code we didn't
 * trampoline (e.g. the process main thread). Never returns NULL unless the
 * registry is full (then beacons silently no-op). */
static DiagThread *diag_self(void) {
  if (self) return self;
  DiagThread *t = slot_alloc();
  if (t && t->name[0] == 0) {
    /* default label until a real name arrives */
    snprintf(t->name, sizeof(t->name), "T%llu", (unsigned long long)t->tid);
  }
  self = t;
  return t;
}

/* ------------------------------------------------------------------ public */
void diag_thread_register(const void *entry, int is_main_engine) {
  DiagThread *t = diag_self();
  if (!t) return;
  t->entry = entry;
  t->is_main_engine = is_main_engine;
  if (is_main_engine && t->name[0] == 'T')   /* keep until Unity renames it */
    snprintf(t->name, sizeof(t->name), "engine_main");
}

void diag_thread_unregister(void) {
  if (!self) return;
  mutexLock(&g_reg_lock);
  self->in_use = 0;
  mutexUnlock(&g_reg_lock);
  self = NULL;
}

void diag_set_name(void *target_pthread, const char *name) {
  if (!name) return;
  DiagThread *t = NULL;
  if (target_pthread) {
    pthread_t want = (pthread_t)target_pthread;
    mutexLock(&g_reg_lock);
    for (int i = 0; i < DIAG_MAX_THREADS; i++) {
      if (g_threads[i].in_use && pthread_equal(g_threads[i].pth, want)) { t = &g_threads[i]; break; }
    }
    mutexUnlock(&g_reg_lock);
  }
  if (!t) t = diag_self();   /* PR_SET_NAME / self-naming case */
  if (!t) return;
  strncpy(t->name, name, sizeof(t->name) - 1);
  t->name[sizeof(t->name) - 1] = 0;
}

void diag_wait_enter(int kind, const void *obj) {
  DiagThread *t = diag_self();
  if (!t) return;
  t->wait_kind  = kind;
  t->wait_obj   = obj;
  t->wait_since = now_tick();
  t->waits_total++;
  t->last_active = t->wait_since;
}

void diag_wait_exit(void) {
  DiagThread *t = self;          /* exit without a prior enter is harmless */
  if (!t) return;
  t->wait_kind = DIAG_W_NONE;
  t->wait_obj  = NULL;
  t->wakes_total++;
  t->last_active = now_tick();
}

void diag_futex_spin(const void *obj) {
  DiagThread *t = diag_self();
  if (!t) return;
  /* publish as a futex wait but keep counting spins so the watchdog can tell
   * "alive but never satisfied" from "hard-parked". Reset wait_since only on
   * the transition *into* a futex wait (or onto a different uaddr), so the
   * dumped "parked secs" measures the current spin episode. */
  int was_futex = (t->wait_kind == DIAG_W_FUTEX && t->wait_obj == obj);
  t->wait_kind  = DIAG_W_FUTEX;
  t->wait_obj   = obj;
  uint64_t now = now_tick();
  if (!was_futex) t->wait_since = now;
  t->futex_spins++;
  t->last_active = now;
}

void diag_frame(int frame) {
  g_frame = frame;
  g_last_progress = now_tick();
}

/* ------------------------------------------------------------------ watchdog */
static uint64_t prev_waits[DIAG_MAX_THREADS];
static uint64_t prev_wakes[DIAG_MAX_THREADS];
static uint64_t prev_spins[DIAG_MAX_THREADS];

/* ---- CPU-context snapshot: see *where in libunity/il2cpp* a thread is wedged.
 * The shim beacons only show which sync primitive a thread sits in; when the
 * hang is inside native engine code (our case: main thread parked inside
 * Unity_nativeRender), this backtrace is what actually pinpoints it. */

/* Our own NRO code region, resolved once via svcQueryMemory on a local fn. */
static uint64_t g_nro_base, g_nro_size;
/* Set per-thread by snapshot_thread: enable the stack scan only for the main /
 * loader threads so the dump stays readable. */
static int g_scan_stack;
static void nro_range_init(void) {
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&nro_range_init)) && mi.size) {
    g_nro_base = mi.addr; g_nro_size = mi.size;
  }
}
/* Write a symbolicated label for `addr` into buf: "libX+0xoff" / "NRO+0xoff"
 * / raw absolute. */
static void resolve_addr(char *buf, size_t n, uint64_t addr) {
  so_module *m = so_find_module_by_addr((const void *)(uintptr_t)addr);
  if (m) {
    /* m->name is the full sdmc path; the leading dirs are identical for every
     * loaded .so, so a fixed-width truncation makes libunity and libil2cpp
     * indistinguishable. Print the basename instead. */
    const char *base = m->name, *p;
    for (p = m->name; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    /* Bound the name explicitly. buf is 48 bytes and a basename can be 63, so
     * an unbounded %s lets the NAME consume the whole buffer and truncate the
     * OFFSET away -- which is the half that makes a backtrace line useful.
     * "libil2cpp.so" is 12 chars and the longest name here; 28 leaves room for
     * a 16-digit offset and never truncates in practice. */
    snprintf(buf, n, "%.28s+0x%llx", base,
             (unsigned long long)(addr - (uint64_t)(uintptr_t)m->load_virtbase));
  } else if (g_nro_size && addr >= g_nro_base && addr < g_nro_base + g_nro_size) {
    snprintf(buf, n, "NRO+0x%llx", (unsigned long long)(addr - g_nro_base));
  } else {
    snprintf(buf, n, "0x%llx", (unsigned long long)addr);
  }
}

/* Is v-4 safe to read?
 *
 * The BL/BLR check below dereferences the word BEFORE a candidate return
 * address. If that candidate happens to BE a module's first byte, v-4 is one
 * word before the mapping and the read faults -- which is exactly what crashed
 * this port on boot:
 *
 *     [gc] il2cpp base=0x72e6299000
 *     [crash] esr=92000007 far=00000072e6298ffc     <- base - 4
 *
 * Intermittent because it needs a stack slot holding precisely the base
 * address, which depends on ASLR and on what happened to be on the stack. The
 * value came from a plain stack scan, so it was never a real return address in
 * the first place; nothing is lost by refusing to probe it. */
static int prev_word_readable(const so_module *m, uint64_t v) {
  /* Alignment first. v comes from a RAW STACK SCAN, so it can be any 64-bit
   * value that happens to sit in the module's range -- a float, half a pointer,
   * packed struct fields. A real return address on AArch64 is always 4-byte
   * aligned, so anything else is junk by definition and there is no reason to
   * dereference near it. This also guarantees the uint32_t load below is
   * aligned rather than relying on the CPU tolerating an unaligned access. */
  if (v & 3u) return 0;

  /* Then the bound. so_find_module_by_addr matched v against
   * [load_virtbase, load_virtbase + load_size), and load_size is page-aligned
   * with the whole range mapped by a single svcMapProcessCodeMemory, so there
   * are no holes inside it: if v-4 is >= base it is mapped. */
  uint64_t base = (uint64_t)(uintptr_t)m->load_virtbase;
  return v >= base + 4;
}

/* A region the stack scans may read: mapped AND readable. svcQueryMemory answers
 * for any address, including unmapped or reserve-only ranges; a stale or bogus SP
 * then made the scan read an unreadable 2 MB region and fault inside the watchdog
 * (eighth hardware run: snapshot_thread <- dump_threads, far inside [slo, shi)). */
static int readable_region(uint64_t a, uint64_t *lo, uint64_t *hi) {
  MemoryInfo mi; u32 pi;
  if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || !mi.size) return 0;
  if (mi.type == MemType_Unmapped || mi.type == MemType_Io || !(mi.perm & Perm_R)) return 0;
  *lo = mi.addr; *hi = mi.addr + mi.size;
  return 1;
}

static void dump_thread_context(const char *name, const ThreadContext *ctx) {
  char a[40], b[40];
  resolve_addr(a, sizeof a, ctx->pc.x);
  resolve_addr(b, sizeof b, ctx->lr);
  stallPrintf("[wd]   %s  PC=%s  LR=%s\n", name, a, b);
  stallPrintf("[wd]     SP=0x%llx FP=0x%llx X0=0x%llx X1=0x%llx X2=0x%llx\n",
              (unsigned long long)ctx->sp, (unsigned long long)ctx->fp,
              (unsigned long long)ctx->cpu_gprs[0].x,
              (unsigned long long)ctx->cpu_gprs[1].x,
              (unsigned long long)ctx->cpu_gprs[2].x);
  /* For a thread parked in svcArbitrateLock, X1 is typically the mutex address
   * and X0 the owner's thread-handle tag -> identifies who holds the lock. */
  /* Clean backtrace via the frame-pointer (x29) chain: [fp]=caller fp, [fp+8]=lr.
   * Bound every dereference to the thread's mapped stack so a wild fp can't fault
   * the watchdog itself. */
  uint64_t slo = 0, shi = 0;
  { if (!readable_region(ctx->sp, &slo, &shi)) { slo = shi = 0; } }
  uint64_t fp = ctx->fp;
  for (int depth = 0; depth < 32 && (fp & 7) == 0; depth++) {
    if (slo) { if (fp < slo || fp + 16 > shi) break; }     /* stay in mapped stack */
    else if (fp < 0x1000) break;                            /* query failed: loose guard */
    const uint64_t nextfp = ((const uint64_t *)(uintptr_t)fp)[0];
    const uint64_t lr     = ((const uint64_t *)(uintptr_t)fp)[1];
    if (!lr) break;
    char s[40]; resolve_addr(s, sizeof s, lr);
    stallPrintf("[wd]     bt[%d] %s\n", depth, s);
    if (nextfp <= fp) break;   /* fp must climb up the stack */
    fp = nextfp;
  }
  /* Unity's hand-written wait stubs clobber the FP chain, so the bt[] above
   * often dead-ends in our glue. Raw-scan the top of the stack for any slot that
   * points into libunity / libil2cpp code -- those are return addresses the FP
   * walk missed, and they reveal what the thread is actually wedged inside.
   * Caller gates this (main/loader threads only) to keep the log readable. */
  if (g_scan_stack && slo) {
    uint64_t sp = ctx->sp & ~7ull;
    if (sp < slo) sp = slo;
    uint64_t top = sp + 0x2000;            /* ~1024 slots is plenty for the active frames */
    if (top > shi) top = shi;
    int printed = 0;
    for (uint64_t addr = sp; addr + 8 <= top && printed < 24; addr += 8) {
      uint64_t v = ((const uint64_t *)(uintptr_t)addr)[0];
      so_module *m = so_find_module_by_addr((const void *)(uintptr_t)v);
      if (!m) continue;
      if (!strstr(m->name, "unity") && !strstr(m->name, "il2cpp")) continue;  /* skip glue/main */
      /* A real return address points to the instruction *after* a call, so the
       * 4 bytes at v-4 must be BL (0b100101 imm26) or BLR (0xD63F0000 mask).
       * Without this check the scan reports jump-table targets, vtable pointers
       * and stale frames -- all of which look like code addresses but are NOT on
       * the live call chain. This filter is what makes the backtrace trustworthy.
       *
       * Bounds-checked: v-4 must still be inside the module, or a stack slot
       * holding a module's first byte faults the watchdog. See
       * prev_word_readable. */
      if (!prev_word_readable(m, v)) continue;
      uint32_t prev = ((const uint32_t *)(uintptr_t)(v - 4))[0];
      int is_bl  = (prev & 0xFC000000u) == 0x94000000u;
      int is_blr = (prev & 0xFFFFFC1Fu) == 0xD63F0000u;
      if (!is_bl && !is_blr) continue;
      char s[48]; resolve_addr(s, sizeof s, v);
      stallPrintf("[wd]     ret@0x%-4llx %s%s\n", (unsigned long long)(addr - sp), s,
                  is_blr ? " (via blr)" : "");
      printed++;
    }
  }
}

/* Pause just long enough to snapshot, RESUME before printing (so the watchdog
 * can't deadlock on a stdio/heap lock the paused thread was holding). */
/* Watchdog pause/resume through the shared lock (see g_pause_lock). A thread the
 * GC bridge holds stopped is never touched: resuming it would let it run during
 * the collector's mark. snap_pause() returns with the lock HELD on success;
 * snap_resume() releases it. Nothing between them may log. */
static Mutex g_pause_lock;
/* Never pause a thread that holds a log lock: wait (bounded, 50 ms) for it to
 * let go. Called with g_pause_lock held; drops it while waiting. */
static void wait_not_logging(DiagThread *t) {
  for (int i = 0; i < 500; i++) {
    const uint32_t h = t->handle;
    if (util_log_lock_owner() != h && util_stall_lock_owner() != h) return;
    mutexUnlock(&g_pause_lock);
    svcSleepThread(100000ull);
    mutexLock(&g_pause_lock);
  }
}
const char *diag_name_for_handle(uint32_t h) {
  if (!h) return "none";
  for (int i = 0; i < DIAG_MAX_THREADS; i++)
    if (g_threads[i].in_use && g_threads[i].handle == h) return g_threads[i].name[0] ? g_threads[i].name : "?";
  return "unregistered";
}
static Result snap_pause(DiagThread *t) {
  mutexLock(&g_pause_lock);
  wait_not_logging(t);
  if (t->gc_paused) { mutexUnlock(&g_pause_lock); return MAKERESULT(Module_Libnx, LibnxError_BadInput); }
  Result r = svcSetThreadActivity(t->handle, ThreadActivity_Paused);
  if (R_FAILED(r)) mutexUnlock(&g_pause_lock);
  return r;
}
static void snap_resume(DiagThread *t) {
  svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  mutexUnlock(&g_pause_lock);
}

static void snapshot_thread(DiagThread *t) {
  if (!t->handle || t->handle == threadGetCurHandle()) return;
  ThreadContext ctx;
  Result pr = snap_pause(t);
  Result gr = R_SUCCEEDED(pr) ? svcGetThreadContext3(&ctx, t->handle) : pr;
  if (R_SUCCEEDED(pr)) snap_resume(t);
  if (R_FAILED(gr)) { stallPrintf("[wd]   %-16s (snapshot failed rc=0x%x)\n", t->name, gr); return; }
  /* Scan the stack only for the threads whose wait we actually need to diagnose:
   * the main render/UI thread and the async loaders. */
  g_scan_stack = (strstr(t->name, "Main") || strstr(t->name, "Preload") ||
                  strstr(t->name, "AsyncRead") || t->is_main_engine) ? 1 : 0;
  dump_thread_context(t->name[0] ? t->name : "?", &ctx);
  g_scan_stack = 0;
}

/* Seconds after watchdog start at which to take an unconditional thread dump.
 * Chosen to bracket a boot: one while loading should still be busy, one after
 * anything reasonable has finished, one well past that. */
static const int DIAG_HEARTBEAT_SEC[] = { 12, 25, 45 };
#define DIAG_HEARTBEAT_MAX ((int)(sizeof DIAG_HEARTBEAT_SEC / sizeof DIAG_HEARTBEAT_SEC[0]))

/* --------------------------------------------------------------------------
 * Managed-frame sampler
 *
 * Every remaining hang in this port has been MANAGED: the engine is healthy,
 * frames render, threads are idle-but-correct, and some C# await never
 * completes. Native backtraces cannot name those -- they bottom out in
 * ScriptingInvocation::Invoke and tell you nothing about which method.
 *
 * libil2cpp is stripped, so on-device symbolization is impossible. But the
 * OFFSETS are enough: dumped raw, they map straight back to method names
 * through the Il2CppInspector dump with tools/managed_trace.py.
 *
 * So: sample UnityMain's stack periodically and print only the frames that land
 * inside libil2cpp. Those are the managed call chain. Filtered to real return
 * addresses (the preceding instruction must be BL or BLR) so the output is the
 * live chain rather than every code-looking word on the stack.
 * ------------------------------------------------------------------------ */
/* Start of libil2cpp's `il2cpp` (managed code) section. Anything below this is
 * the native runtime and will not resolve against an Il2CppInspector dump.
 * Re-derive with: readelf -SW libil2cpp.so | grep il2cpp */
/* Was bouncemasters_nx's section start (0x1f82e44): in THIS libil2cpp every managed
 * method lies below it, so every frame was discarded as "native" and the sampler
 * printed a header and nothing else. Print every libil2cpp frame; the offline
 * symbolizer (script.json) tells managed from runtime. */
#define IL2CPP_MANAGED_BASE 0x0ull

static void sample_managed_frames(void) {
  DiagThread *t = NULL;
  for (int i = 0; i < DIAG_MAX_THREADS; i++)
    if (g_threads[i].in_use && g_threads[i].is_main_engine &&
        strstr(g_threads[i].name, "Main")) { t = &g_threads[i]; break; }
  if (!t || !t->handle || t->handle == threadGetCurHandle()) return;

  ThreadContext ctx;
  Result pr = snap_pause(t);
  Result gr = R_SUCCEEDED(pr) ? svcGetThreadContext3(&ctx, t->handle) : pr;
  if (R_SUCCEEDED(pr)) snap_resume(t);
  if (R_FAILED(gr)) return;

  uint64_t slo = 0, shi = 0;
  { if (!readable_region(ctx.sp, &slo, &shi)) { slo = shi = 0; } }
  if (!slo) return;

  uint64_t sp = ctx.sp & ~7ull;
  if (sp < slo) sp = slo;
  uint64_t top = sp + 0x4000;
  if (top > shi) top = shi;

  /* Managed code lives in libil2cpp's `il2cpp` section, NOT in .text -- .text is
   * the native runtime (the GC, the metadata loader). The first samples mixed
   * the two and the offsets resolved to nothing, because the dump only
   * describes managed methods. Report the section so the two are never confused
   * again: anything below IL2CPP_MANAGED_BASE is native. */
  int printed = 0, native = 0;
  for (uint64_t a = sp; a + 8 <= top && printed < 24; a += 8) {
    uint64_t v = ((const uint64_t *)(uintptr_t)a)[0];
    so_module *m = so_find_module_by_addr((const void *)(uintptr_t)v);
    if (!m || !strstr(m->name, "il2cpp")) continue;
    if (!prev_word_readable(m, v)) continue;      /* v-4 would leave the module */
    uint32_t prev = ((const uint32_t *)(uintptr_t)(v - 4))[0];
    if ((prev & 0xFC000000u) != 0x94000000u &&
        (prev & 0xFFFFFC1Fu) != 0xD63F0000u) continue;
    if (!printed && !native) {
      /* BOTH logs, deliberately.
       *
       * [mt] is the only instrument that can name a stalled C# await, and it
       * spent a whole cycle invisible because it wrote to stall.log while
       * debug.log was the file being sent. A diagnostic nobody reads is worth
       * nothing; ~600 bounded lines in debug.log is a trivial price. */
      stallPrintf("[mt] --- managed frames on UnityMain ---\n");
      debugPrintf("[mt] --- managed frames on UnityMain ---\n");
    }
    {
      unsigned long long off =
          (unsigned long long)(v - (uint64_t)(uintptr_t)m->load_virtbase);
      if (off < IL2CPP_MANAGED_BASE) { native++; continue; }   /* native runtime */
      stallPrintf("[mt] il2cpp+0x%llx\n", off);
      debugPrintf("[mt] il2cpp+0x%llx\n", off);
      printed++;
    }
  }
}

static void dump_threads(int episode, uint64_t now) {
  /* Announce entry before touching anything. dump_threads walks other threads'
   * contexts and stacks; a bad pointer there kills this thread outright, and
   * that is what happened -- stall.log held one beacon and then nothing, with no
   * stall header, so the crash was inside the dump rather than the detector. */
  stallPrintf("[wd] dump_threads: entered\n");
  uint64_t stalled_ns = tick_to_ns(now - g_last_progress);
  stallPrintf("\n[wd] ===== STALL #%d : no frame progress for %llu.%llus (last frame=%d) =====\n",
              episode, (unsigned long long)(stalled_ns / 1000000000ull),
              (unsigned long long)((stalled_ns % 1000000000ull) / 100000000ull), g_frame);
  stallPrintf("[wd] %-16s %-10s %-11s %-18s %7s  d_wait d_wake d_spin\n",
              "name", "tid", "state", "wait_obj", "secs");
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use) continue;
    int kind = t->wait_kind;
    uint64_t since = t->wait_since;
    uint64_t parked_ns = (kind != DIAG_W_NONE && since) ? tick_to_ns(now - since) : 0;
    uint64_t dwait = t->waits_total - prev_waits[i];
    uint64_t dwake = t->wakes_total - prev_wakes[i];
    uint64_t dspin = t->futex_spins - prev_spins[i];
    /* t->wait_start is only meaningful while the thread is inside one of our
     * instrumented waits. For anything else it is stale or zero, and printing
     * now-minus-stale produced a constant 1537228672.7 that looked like a
     * 48-year wait and sent one investigation down the wrong path entirely.
     * Print it only when it can be trusted. */
    prev_waits[i] = t->waits_total;
    prev_wakes[i] = t->wakes_total;
    prev_spins[i] = t->futex_spins;
    stallPrintf("[wd] %-16s %-10llu %-11s 0x%-16llx %3llu.%llu  %6llu %6llu %6llu%s\n",
                t->name[0] ? t->name : "?",
                (unsigned long long)t->tid,
                wait_kind_name(kind),
                (unsigned long long)(uintptr_t)t->wait_obj,
                (unsigned long long)(parked_ns / 1000000000ull),
                (unsigned long long)((parked_ns % 1000000000ull) / 100000000ull),
                (unsigned long long)dwait, (unsigned long long)dwake,
                (unsigned long long)dspin,
                t->is_main_engine ? "  <engine_main>" : "");
  }
  stallPrintf("[wd] legend: d_* = delta since previous dump (0/0/0 == hard-parked; "
              "d_spin>0 == alive on futex; d_wait>d_wake == entered a wait it hasn't left)\n");
  /* native backtrace: where each thread is wedged inside libunity/il2cpp/NRO */
  stallPrintf("[wd] --- thread CPU contexts (frame-pointer backtrace) ---\n");
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (g_threads[i].in_use) snapshot_thread(&g_threads[i]);
  }
  stallPrintf("\n");
}

static void watchdog_main(void *unused) {
  (void)unused;
  /* Own bionic TLS block. Every other thread that can reach engine-adjacent code
   * has one (main, clock, audio, the pthread shim); this thread never did. A
   * stack-protector prologue anywhere it reaches would read TPIDR_EL0+0x28 with
   * TPIDR unset and fault -- killing the watchdog exactly when it fires, which
   * matches the observed behaviour: one beacon, then silence, no stall header. */
  static uint8_t wd_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(wd_tls);
  int episode = 0;
  uint64_t last_dump = 0;
  /* prime so we don't false-trigger before the first frame */
  if (g_last_progress == 0) g_last_progress = now_tick();
  for (;;) {
    svcSleepThread(DIAG_POLL_NS);
    uint64_t now = now_tick();
    uint64_t idle = now - g_last_progress;

    /* Liveness beacon. Every diagnostic in this port has failed silently at
     * least once; a periodic "I am alive and here is the frame counter" line
     * makes the NEXT failure distinguishable from a dead watchdog thread. */
    {
      static uint64_t last_beat;
      if (tick_to_ns(now - last_beat) >= 2000000000ull) {
        last_beat = now;
        stallPrintf("[wd] alive: frame=%d idle=%llums%s\n",
                    g_frame, (unsigned long long)(tick_to_ns(idle) / 1000000ull),
                    tick_to_ns(idle) >= DIAG_STALL_NS ? "  STALLED" : "");
      }
    }

    if (tick_to_ns(idle) >= DIAG_STALL_NS && !editbox_is_open()) {   /* the system keyboard parks the main thread by design */
      if (last_dump == 0 || tick_to_ns(now - last_dump) >= DIAG_REDUMP_NS) {
        dump_threads(++episode, now);
        last_dump = now;
      }
    } else {
      /* progress resumed: reset so a later stall dumps fresh */
      last_dump = 0;
    }

    /* HEARTBEAT DUMPS, regardless of frame progress.
     *
     * The stall test above only catches a frozen RENDER loop. It is blind to
     * the failure this port keeps hitting: frames advancing perfectly while the
     * game's own logic waits on something. Every such hang so far produced a
     * completely silent watchdog -- "alive: frame=4418" all the way down -- and
     * had to be diagnosed from the managed log alone.
     *
     * So dump the threads a few times early on even when nothing is stalled.
     * A dump costs a few hundred lines once; not having one has repeatedly cost
     * a whole test cycle. Bounded to DIAG_HEARTBEAT_MAX so a healthy run does
     * not fill the card.
     *
     * When the game is loading a scene, this is what shows Loading.PreloadManager
     * and Loading.AsyncRead and whether they are working or parked. */
    /* Managed sample every ~2 s for the first minute. Cheap (one paused
     * thread, one 16 KB stack scan) and it is the only thing that can name a
     * stalled C# await. */
    {
      static int mt_n;
      static uint64_t mt_last;
      /* 60 samples over two minutes rather than 30 over one: every stall so far
       * has been diagnosed well after the first minute, and the earlier samples
       * are the least interesting ones (the boot is still legitimately busy). */
      if (mt_n < 60 && (mt_last == 0 || tick_to_ns(now - mt_last) >= 2000000000ull)) {
        mt_last = now; mt_n++;
        sample_managed_frames();
      }
    }

    {
      static int hb_done;
      static uint64_t t0;
      if (t0 == 0) t0 = now;
      if (hb_done < DIAG_HEARTBEAT_MAX &&
          tick_to_ns(now - t0) >= (uint64_t)DIAG_HEARTBEAT_SEC[hb_done] * 1000000000ull) {
        stallPrintf("\n[wd] ===== HEARTBEAT #%d at ~%ds (frames ARE advancing; "
                    "this is not a stall) =====\n", hb_done + 1,
                    DIAG_HEARTBEAT_SEC[hb_done]);
        dump_threads(-(++hb_done), now);
      }
    }
  }
}

void diag_watchdog_start(void) {
  if (g_wd_started) return;
  g_wd_started = 1;
  nro_range_init();
  if (g_last_progress == 0) g_last_progress = now_tick();
  /* libnx thread: deliberately NOT via the pthread shim under test.
   * 16 KiB stack, priority 0x2C (same band as main), default core. */
  /* Try to outrank the engine, but NEVER fail closed.
   *
   * The failure being chased is a 100% CPU spin, and at equal priority (0x2C)
   * the watchdog is simply never scheduled against it -- two beacons then
   * silence, every run. But a blind jump to 0x18 was refused outright
   * (rc=0xe001, invalid priority: hbloader only permits a narrow band around
   * its own 0x2C) and the watchdog did not start AT ALL, which is strictly
   * worse than being starved.
   *
   * So: walk from the most useful priority down to the known-good one and take
   * the first that works. A modest bump still wins the scheduler against a
   * same-priority spinner. */
  static const int PRIOS[] = { 0x28, 0x2A, 0x2B, 0x2C };
  Result rc = 0xe001;
  int used = 0;
  for (unsigned i = 0; i < sizeof(PRIOS) / sizeof(PRIOS[0]); i++) {
    /* core 2 explicitly, not -2 (= "default"). The engine's main thread and its
     * job workers saturate the default core; giving the watchdog its own means
     * a spinner cannot hide from it even at equal priority. */
    rc = threadCreate(&g_wd_thread, watchdog_main, NULL, NULL, 0x4000, PRIOS[i], 2);
    if (R_FAILED(rc))   /* core 2 unavailable in this configuration */
      rc = threadCreate(&g_wd_thread, watchdog_main, NULL, NULL, 0x4000, PRIOS[i], -2);
    if (R_SUCCEEDED(rc)) { used = PRIOS[i]; break; }
  }
  if (R_SUCCEEDED(rc) && R_SUCCEEDED(threadStart(&g_wd_thread)))
    stallPrintf("[wd] watchdog armed (stall=%llus, poll=1s, prio=0x%02x)\n",
                (unsigned long long)(DIAG_STALL_NS / 1000000000ull), used);
  else
    stallPrintf("[wd] watchdog FAILED to start rc=0x%x\n", rc);
}


/* ============ real thread suspension for the GC stop-the-world ============
 * Adopted from daggerfall_nx. The watchdog's snapshot_thread() already pauses
 * threads with svcSetThreadActivity(); these expose that by pthread_t, which is
 * what the collector hands libc_shim's pthread_kill_gc().
 * SAFETY: never pause the caller (the collector itself); never double-pause, so
 * resume is symmetric; report failure so the caller still acks -- a collector
 * waiting forever is worse than one thread it could not stop. */
/* One lock serialises EVERY pause/resume in this file: the watchdog's snapshots
 * and the GC bridge's stop-the-world. Without it the watchdog could resume a
 * thread the collector is holding stopped (it then runs during the mark), or the
 * collector could "pause" a thread the watchdog is about to resume. Neither side
 * logs or waits on the other while holding it. */
/* Pause a GC target and read its registers while it is stopped.
 * 1 = paused + ctx valid, 2 = paused but no ctx, 0 = not paused. */
int diag_pause_pthread_ctx(void *target_pthread, ThreadContext *ctx) {
  pthread_t want = (pthread_t)target_pthread;
  int rc = 0;
  mutexLock(&g_pause_lock);
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use || !pthread_equal(t->pth, want)) continue;
    if (!t->handle || t->handle == threadGetCurHandle()) break;       /* never self */
    if (!t->gc_paused) {
      wait_not_logging(t);                          /* never freeze a thread mid-log */
      if (R_FAILED(svcSetThreadActivity(t->handle, ThreadActivity_Paused))) break;
      t->gc_paused = 1;
    }
    rc = (ctx && R_SUCCEEDED(svcGetThreadContext3(ctx, t->handle))) ? 1 : 2;
    break;
  }
  mutexUnlock(&g_pause_lock);
  return rc;                                 /* unknown thread: caller still acks */
}

int diag_pause_pthread(void *target_pthread) {
  return diag_pause_pthread_ctx(target_pthread, NULL) != 0;
}

int diag_resume_pthread(void *target_pthread) {
  pthread_t want = (pthread_t)target_pthread;
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use || !pthread_equal(t->pth, want)) continue;
    if (!t->gc_paused) return 0;
    mutexLock(&g_pause_lock);
    t->gc_paused = 0;
    svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
    mutexUnlock(&g_pause_lock);
    return 1;
  }
  return 0;
}

/* Crash path: never leave a thread paused behind a dump that needs its locks. */
void diag_resume_all_gc_paused(void) {
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (t->in_use && t->gc_paused && t->handle) {
      t->gc_paused = 0;
      svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
    }
  }
}
