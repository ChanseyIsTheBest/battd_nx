/* ============================================================================
 * nx_crash_handler.c  --  drop-in user-exception crash dumper for Switch
 *                         homebrew (so-loader / Unity IL2CPP ports, or anything)
 *
 * WHAT IT DOES
 *   On any CPU fault (segfault, illegal instruction, bad blr, alignment, etc.)
 *   libnx calls __libnx_exception_handler. This one dumps, to your log:
 *     - the faulting PC and LR, symbolized as  <module>+0xoffset  when possible
 *     - the fault ("far") address and ESR (exception syndrome)
 *     - all 29 general-purpose registers, plus FP/SP
 *     - a frame-pointer backtrace (module+offset per frame)
 *     - a hex dump of the stack around SP
 *   ...then re-raises via svcBreak so Atmosphere/creport still writes its report.
 *
 *   Every memory read is svcQueryMemory-guarded, so the handler itself never
 *   faults on a wild pointer.
 *
 * WHY IT'S USEFUL
 *   creport gives you raw addresses. This gives you  libunity.so+0x871354 ,
 *   which you can feed straight into a disassembler. For so-loader ports it
 *   resolves addresses inside the dynamically-loaded .so (libunity/libil2cpp),
 *   which creport can't name because they aren't real modules.
 *
 * HOW TO USE  (three steps)
 *   1. Add this file to your build (drop it in source/, it compiles as C).
 *   2. Point the output at your logger: either
 *        (a) define CRASH_LOG_PRINTF to your own printf-like function, e.g.
 *              -DCRASH_LOG_PRINTF=stallPrintf   (NOT debugPrintf: debugPrintf
 *              holds a global mutex across a blocking SD write, and the faulting
 *              thread is usually the one holding it -- a crash dump routed there
 *              deadlocks and never reaches the card)
 *            (must have signature  int f(const char *fmt, ...) ), or
 *        (b) leave it undefined -- it falls back to libnx's own
 *            svcOutputDebugString via a tiny vsnprintf buffer.
 *   3. (Optional, so-loader only) implement the weak hook
 *        crash_resolve_module()  (see the stub at the bottom) so addresses
 *        inside your loaded .so images get named. If you don't, addresses
 *        still print as raw hex and everything else works.
 *
 * That's it. No init call needed -- providing __libnx_exception_handler and the
 * __nx_exception_* symbols is what enables user-mode exception handling in libnx.
 *
 * TUNING (compile-time -D flags, all optional)
 *   CRASH_LOG_PRINTF=fn      your logger (default: svcOutputDebugString shim)
 *   CRASH_STACK_BYTES=0x200  how much stack to hex-dump around SP
 *   CRASH_BT_DEPTH=16        max backtrace frames
 *   CRASH_REBREAK=1          re-raise via svcBreak after dumping (default 1;
 *                            set 0 to hang in a sleep loop instead, e.g. if you
 *                            don't want creport to also fire)
 * ==========================================================================*/

#include "libc_shim.h"
#include "diag.h"
#include <switch.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdalign.h>
#include <string.h>
#include <stdio.h>

#ifndef CRASH_STACK_BYTES
#define CRASH_STACK_BYTES 0x200
#endif
#ifndef CRASH_BT_DEPTH
#define CRASH_BT_DEPTH 16
#endif
#ifndef CRASH_REBREAK
#define CRASH_REBREAK 1
#endif

/* ---- output shim ---------------------------------------------------------- */
#ifdef CRASH_LOG_PRINTF
extern int CRASH_LOG_PRINTF(const char *fmt, ...);
#define CLOG(...) CRASH_LOG_PRINTF(__VA_ARGS__)
#else
static void crash_out(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    svcOutputDebugString(buf, (size_t)n);
}
#define CLOG(...) crash_out(__VA_ARGS__)
#endif

/* ---- libnx user-exception plumbing --------------------------------------- *
 * Providing these three symbols + the handler is what turns on user-mode
 * exception handling. The stack must be aligned and reasonably sized. */
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/* ---- optional module resolver (so-loader hook) --------------------------- *
 * Return 1 and fill name/base if `addr` lies inside a module you loaded, so it
 * can be printed as name+0xoffset. Weak: define your own in the loader to name
 * addresses inside libunity/libil2cpp. Default tries dladdr, else gives up. */
__attribute__((weak))
int crash_resolve_module(uintptr_t addr, char *name_out, size_t name_cap,
                         uintptr_t *base_out) {
    (void)addr; (void)name_out; (void)name_cap; (void)base_out;
    return 0;
}

/* ---- guarded memory probing ---------------------------------------------- */
static int mem_readable(uintptr_t addr, size_t len) {
    if (!addr || addr < 0x1000) return 0;
    uintptr_t a = addr, end = addr + len;
    if (end < a) return 0;                 /* overflow */
    while (a < end) {
        MemoryInfo mi; u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
        if (mi.type == MemType_Unmapped) return 0;
        if ((mi.perm & Perm_R) == 0) return 0;
        uintptr_t block_end = (uintptr_t)mi.addr + mi.size;
        if (block_end <= a) return 0;      /* no forward progress */
        a = block_end;
    }
    return 1;
}

static const char *sym(uintptr_t v, char *buf, size_t cap) {
    char mod[64]; uintptr_t base = 0;
    if (crash_resolve_module(v, mod, sizeof mod, &base)) {
        snprintf(buf, cap, "%s+0x%lx", mod, (unsigned long)(v - base));
        return buf;
    }
    /* try the standard dynamic-linker resolver if the app links one */
    snprintf(buf, cap, "%016lx", (unsigned long)v);
    return buf;
}

/* ---- the handler ---------------------------------------------------------- */
void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  /* A thread the GC bridge paused may hold the stdio/heap lock the dump needs. */
  diag_resume_all_gc_paused();
  gc_paused_live_reset();
  /* DUMP FIRST. NOTHING THAT CAN BLOCK GOES ABOVE THIS.
   *
   * The previous order put debug_log_flush_force() -- which waits up to 2 s for
   * the log lock -- and two stallPrintf lines ahead of the dump. The run after
   * that change got exactly one line out and then stopped: the "tail LOST"
   * diagnostic landed and the exception dump itself never did. That is worse
   * than what it replaced, and it was avoidable. The log flush is a nice-to-have;
   * the PC and the backtrace are the reason this handler exists, so they go
   * first and everything that can wait goes after.
   *
   * pc/lr/esr/far are on the banner line for the same reason: if the process
   * dies after one line, that line should still be worth reading. */
  char b1[96], b2[96];

  CLOG("\n[crash] ===== USER EXCEPTION ===== pc=%s lr=%s esr=%08x far=%016lx type=0x%x\n",
       sym(ctx->pc.x, b1, sizeof b1), sym(ctx->lr.x, b2, sizeof b2),
       ctx->esr, (unsigned long)ctx->far.x, ctx->error_desc);
  CLOG("[crash] sp=%016lx  fp=%016lx\n",
       (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);

    for (int i = 0; i < 28; i += 4)
        CLOG("[crash] x%-2d %016lx  x%-2d %016lx  x%-2d %016lx  x%-2d %016lx\n",
             i,   (unsigned long)ctx->cpu_gprs[i].x,
             i+1, (unsigned long)ctx->cpu_gprs[i+1].x,
             i+2, (unsigned long)ctx->cpu_gprs[i+2].x,
             i+3, (unsigned long)ctx->cpu_gprs[i+3].x);
    CLOG("[crash] x28 %016lx\n", (unsigned long)ctx->cpu_gprs[28].x);

    /* frame-pointer backtrace: [fp]=next fp, [fp+8]=return addr */
    CLOG("[crash] backtrace:\n");
    uintptr_t fp = (uintptr_t)ctx->fp.x;
    for (int d = 0; d < CRASH_BT_DEPTH && fp; d++) {
        if (!mem_readable(fp, 16)) break;
        uintptr_t next = ((uintptr_t *)fp)[0];
        uintptr_t ret  = ((uintptr_t *)fp)[1];
        if (!ret) break;
        CLOG("[crash]   #%02d %s\n", d, sym(ret, b1, sizeof b1));
        if (next <= fp) break;             /* must grow upward */
        fp = next;
    }

    /* stack hex dump around SP */
    uintptr_t sp = (uintptr_t)ctx->sp.x;
    if (mem_readable(sp, CRASH_STACK_BYTES)) {
        CLOG("[crash] stack @ %016lx:\n", (unsigned long)sp);
        for (size_t off = 0; off < CRASH_STACK_BYTES; off += 0x20) {
            const u64 *q = (const u64 *)(sp + off);
            CLOG("[crash]   +%03zx: %016lx %016lx %016lx %016lx\n",
                 off, (unsigned long)q[0], (unsigned long)q[1],
                 (unsigned long)q[2], (unsigned long)q[3]);
        }
    } else {
        CLOG("[crash] stack @ %016lx: UNREADABLE\n", (unsigned long)sp);
    }

    CLOG("[crash] ============== END EXCEPTION DUMP ==============\n");

    /* Everything that can block goes here, AFTER the dump is on the card.
     * debug_log_flush_force() waits up to 2 s for the log lock; the two
     * stallPrintf lines can be starved silently by the watchdog. None of that
     * can cost us the backtrace now. */
    { extern int debug_log_flush_force(void);
      extern unsigned debug_log_dropped(void);
      const int flushed = debug_log_flush_force();
      CLOG("[crash] debug.log tail %s (%u lines already dropped)\n",
           flushed ? "FLUSHED -- that log is complete"
                   : "LOST -- the log lock could not be taken; everything since "
                     "the last flush is missing",
           debug_log_dropped());
      { unsigned gaveup = 0, waited = 0, worst_us = 0;
        diag_log_wait_stats(&gaveup, &waited, &worst_us);
        CLOG("[crash] GC pause vs log lock: %u gave up after 50ms, %u waited (worst %u us)\n",
             gaveup, waited, worst_us); }
      /* One pointer line into stall.log so a reader of the watchdog log knows
       * where the dump went. Last, because it can be silently dropped. */
      { extern int stallPrintf(char *text, ...);
        stallPrintf("[log] crashed -- full dump in crash.log (debug.log tail %s)\n",
                    flushed ? "flushed" : "LOST"); } }

#if CRASH_REBREAK
    /* re-raise so the process aborts and Atmosphere writes creport too */
    svcBreak(BreakReason_Panic, 0, 0);
#endif
    for (;;) svcSleepThread(1000000000ULL);
}

/* ===========================================================================
 * OPTIONAL: so-loader module resolver
 * ---------------------------------------------------------------------------
 * If you use an so-loader (dynamically map libunity.so / libil2cpp.so yourself),
 * copy this into your loader and adapt it to your module list so crash frames
 * inside those .so images get named. Delete this block if you don't need it --
 * the weak default above will just print raw hex.
 *
 *   // in your loader, NOT weak, overrides the weak stub above:
 *   int crash_resolve_module(uintptr_t addr, char *name_out, size_t cap,
 *                            uintptr_t *base_out) {
 *       for (my_module *m = g_module_list; m; m = m->next) {
 *           uintptr_t b = (uintptr_t)m->load_virtbase;
 *           if (addr >= b && addr < b + m->load_size) {
 *               snprintf(name_out, cap, "%s", m->name);
 *               *base_out = b;
 *               return 1;
 *           }
 *       }
 *       return 0;
 *   }
 * ==========================================================================*/
