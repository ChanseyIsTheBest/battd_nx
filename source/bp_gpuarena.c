/* bp_gpuarena.c -- dedicated contiguous arena for GPU buffers.
 *
 * Adopted from cloverpit_gpuarena.c (MIT), itself ported from pvz_fusion_nx.
 * Nothing in here is game-specific: it is a fix for how libdrm_nouveau
 * allocates, so it applies unchanged to any GLES title in this lineage.
 * Renamed identifiers only.
 *
 * WHY (this is the actual cause of the fixed-frame-count freeze)
 * libdrm_nouveau backs EVERY GPU buffer with memalign(0x1000, size). Those are
 * large (up to ~22 MB for a 1080p render target), page-aligned, and churned
 * constantly as the engine creates and destroys textures and framebuffers.
 * Serving them from newlib's general heap shreds it: once fragmented, a 20 MB
 * CONTIGUOUS page-aligned run cannot be placed even with a gigabyte free in
 * aggregate. memalign then returns NULL -> nouveau_bo_new fails ->
 * GL_OUT_OF_MEMORY -> incomplete framebuffer -> the compositor is handed
 * something invalid and the console wedges.
 *
 * That explains what nothing else did: the freeze happening at a FIXED frame
 * count (~21-26), independent of threading mode, pacing, vsync and swap
 * interval. It is not a race and not a system-memory shortage -- the memory is
 * there, it is just too fragmented to place one more big buffer. Every earlier
 * theory predicted the freeze would move when those knobs changed. It did not.
 *
 * THE FIX
 * Reserve one big contiguous region ONCE, at first GPU allocation, before the
 * heap has been churned. Hand GPU buffers out of it with a page bitmap, so they
 * can never fragment newlib and newlib can never fragment them. Small or
 * non-page-aligned requests still go to newlib.
 *
 * Ported from pvz_fusion_nx, where this is what made the game render.
 *
 * DEADLOCK NOTE, inherited and important: never call debugPrintf while holding
 * gpua_lock. Logging writes to the SD card and can allocate internally, which
 * re-enters __wrap_memalign -> gpua_alloc -> mutexLock(gpua_lock). libnx mutexes
 * are not recursive, so that self-deadlocks the calling thread. Record the
 * outcome under the lock; report it after releasing.
 *
 * MIT.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <malloc.h>
#include <switch.h>

#include "util.h"
#include "config.h"

/* Provided by the linker's --wrap: the genuine newlib entry points. */
extern void *__real_memalign(size_t align, size_t size);
extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t n, size_t size);
extern void *__real_realloc(void *p, size_t size);
extern void  __real_free(void *p);

#define GPUA_PAGE  0x1000u
/* Starting request. Shrinks in 64 MB steps until it fits, so a smaller console
 * or a heavier payload degrades instead of failing. CloverPit renders 720p, so
 * its buffers are ~3.5 MB rather than PvZ's ~8 MB, and it needs far less than
 * PvZ's 1152 MB -- but the arena costs only address space it would otherwise
 * have fragmented, so start generous. */
#define GPUA_BYTES ((size_t)bp_gpu_arena_mb * 1024 * 1024)   /* config.txt gpu_arena_mb */
#define GPUA_FLOOR ((size_t)96 * 1024 * 1024)
/* Below this, newlib is fine: small allocations do not cause the large-run
 * fragmentation this arena exists to prevent. */
#define GPUA_MIN   (64u * 1024)
/* ABOVE this, it is not a GPU buffer. nouveau's largest is a full-screen render
 * target (~3.5 MB at 720p, ~8 MB at 1080p); 64 MB is generous headroom.
 *
 * Without an upper bound the FIRST caller to hit the wrap was this port's own
 * overcommit pool -- memalign(0x1000, 896 MB) -- which triggered gpua_init(),
 * reserved the whole 512 MB arena before Unity had even loaded, then did not
 * fit in it and fell back to newlib anyway. Net effect: half a gigabyte locked
 * away for nothing and the game regressed to freezing at frame 0. */
#define GPUA_MAX   ((size_t)64 * 1024 * 1024)

static uint8_t  *gpua_base;
static size_t    gpua_pages;
static uint8_t  *gpua_used;      /* 1 byte per page: in use            */
static uint32_t *gpua_runlen;    /* run length, recorded at first page */
static size_t    gpua_hint;
static Mutex     gpua_lock;
static int       gpua_state;     /* 0 = untried, 1 = ready, -1 = disabled */
/* The arena stays inert until bp_gpua_enable() is called, so it can never be
 * triggered by the port's own boot-time allocations -- only by the graphics
 * driver once the engine is actually running. Belt and braces alongside
 * GPUA_MAX: either one alone would have prevented the frame-0 regression. */
static int       gpua_enabled;
static size_t    gpua_live_pages, gpua_peak_pages;
/* For the [mem] breakdown: what the GPU arena has actually been asked for. */
void bp_gpua_stats(size_t *reserved, size_t *live, size_t *peak) {
  if (reserved) *reserved = gpua_used ? gpua_pages * 4096u : 0;
  if (live)     *live     = gpua_live_pages * 4096u;
  if (peak)     *peak     = gpua_peak_pages * 4096u;
}

unsigned bp_gpua_peak_mb(void);
unsigned bp_gpua_live_mb(void);

void bp_gpua_enable(void) {
  gpua_enabled = 1;
  debugPrintf("[gpua] arena enabled (routing %uKB..%uMB page-aligned allocations)\n",
              (unsigned)(GPUA_MIN >> 10), (unsigned)(GPUA_MAX >> 20));
}

static void gpua_init(void) {
  if (!gpua_enabled) return;
  if (gpua_state) return;
  size_t got_mb = 0;
  int report = 0;

  mutexLock(&gpua_lock);
  if (!gpua_state) {
    size_t want = GPUA_BYTES;
    uint8_t *b = NULL;
    while (want >= GPUA_FLOOR) {
      b = (uint8_t *)__real_memalign(GPUA_PAGE, want);
      if (b) break;
      want -= (size_t)64 * 1024 * 1024;
    }
    if (b) {
      gpua_pages  = want / GPUA_PAGE;
      gpua_used   = (uint8_t *)__real_calloc(gpua_pages, 1);
      gpua_runlen = (uint32_t *)__real_calloc(gpua_pages, sizeof(uint32_t));
      if (gpua_used && gpua_runlen) {
        gpua_base = b; gpua_hint = 0; gpua_state = 1;
        got_mb = want >> 20; report = 1;
      } else {
        __real_free(gpua_used); __real_free(gpua_runlen); __real_free(b);
        gpua_used = NULL; gpua_runlen = NULL;
        gpua_state = -1; report = 2;
      }
    } else {
      gpua_state = -1; report = 3;
    }
  }
  mutexUnlock(&gpua_lock);

  if (report == 1)
    debugPrintf("[gpua] GPU arena reserved: %u MB contiguous\n", (unsigned)got_mb);
  else if (report == 2)
    debugPrintf("[gpua] GPU arena DISABLED (bitmap alloc failed)\n");
  else if (report == 3)
    debugPrintf("[gpua] GPU arena DISABLED (no contiguous region) -- expect the "
                "fragmentation freeze\n");
}

static void *gpua_alloc(size_t sz) {
  gpua_init();
  if (gpua_state != 1) return NULL;
  size_t need = (sz + GPUA_PAGE - 1) / GPUA_PAGE;
  if (!need || need > gpua_pages) return NULL;

  void *out = NULL;
  mutexLock(&gpua_lock);
  for (int pass = 0; pass < 2 && !out; pass++) {     /* hint first, then wrap */
    size_t i   = pass ? 0 : gpua_hint;
    size_t end = pass ? gpua_hint : gpua_pages;
    while (i + need <= end) {
      size_t run = 0;
      while (run < need && !gpua_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) gpua_used[i + k] = 1;
        gpua_runlen[i] = (uint32_t)need;
        gpua_hint = i + need;
        if (gpua_hint >= gpua_pages) gpua_hint = 0;
        gpua_live_pages += need;
        if (gpua_live_pages > gpua_peak_pages) gpua_peak_pages = gpua_live_pages;
        out = gpua_base + i * GPUA_PAGE;
        break;
      }
      i += run + 1;                                   /* skip past the blocker */
    }
  }
  mutexUnlock(&gpua_lock);
  return out;
}

static int gpua_owns(const void *p) {
  return gpua_state == 1 && (const uint8_t *)p >= gpua_base &&
         (const uint8_t *)p < gpua_base + gpua_pages * GPUA_PAGE;
}

static void gpua_free(void *p) {
  size_t i = (size_t)(((uint8_t *)p - gpua_base) / GPUA_PAGE);
  mutexLock(&gpua_lock);
  uint32_t n = (i < gpua_pages) ? gpua_runlen[i] : 0;
  if (n) {
    for (uint32_t k = 0; k < n; k++) gpua_used[i + k] = 0;
    gpua_runlen[i] = 0;
    gpua_live_pages = (gpua_live_pages >= n) ? gpua_live_pages - n : 0;
    if (i < gpua_hint) gpua_hint = i;
  }
  mutexUnlock(&gpua_lock);
}

/* Peak usage, for the render loop to report once frames are flowing. */
unsigned bp_gpua_peak_mb(void) {
  return (unsigned)((gpua_peak_pages * GPUA_PAGE) >> 20);
}
unsigned bp_gpua_live_mb(void) {
  return (unsigned)((gpua_live_pages * GPUA_PAGE) >> 20);
}

/* ---- the wrapped entry points ------------------------------------------- */

/* Route only nouveau's buffer shape: alignment >= 0x1000 AND size >= GPUA_MIN.
 *
 * The comparison direction matters and was wrong in the first version of this
 * file (`align <= GPUA_PAGE`), which routed every ordinary large allocation
 * with 8- or 16-byte alignment into the GPU arena. That fills the arena with
 * non-GPU data and starves the buffers it exists to serve -- the exact opposite
 * of the intent, and it would have looked like the arena "not helping" rather
 * than like a bug. */
void *__wrap_memalign(size_t align, size_t size) {
  void *p = NULL;
  if (gpua_enabled && align >= GPUA_PAGE &&
      size >= GPUA_MIN && size <= GPUA_MAX)
    p = gpua_alloc(size);
  if (!p)
    p = __real_memalign(align ? align : 8, size);
  /* A failed page-aligned allocation IS the bug this file exists to prevent, so
   * say so loudly (bounded) rather than letting it surface as a GL error.
   *
   * READ THIS BEFORE BLAMING THE ARENA. The message fires for ANY failed
   * page-aligned memalign, including ones the arena never touched. Requests
   * outside GPUA_MIN..GPUA_MAX go straight to __real_memalign; if that returns
   * NULL the report below still prints, with "arena live=0 MB peak=0 MB" as the
   * tell that the arena was not involved. Unity's Dynamic Heap asks for ~512 MB
   * here, eight times GPUA_MAX, and a failure means newlib is out of contiguous
   * space -- not that the arena is too small. Raising GPUA_MAX to "fix" it would
   * route the engine's whole heap into an arena sized for 8 MB render targets. */
  if (!p && align >= GPUA_PAGE) {
    /* Before reporting, give the RAM cache back and try once more. It is a
     * cache: holding 300-odd MB of files is worth nothing next to an allocation
     * the game actually needs, and this exact failure -- a 128 MB request on the
     * character screen -- is what it cost. One retry only, and only after a real
     * failure, so the fast path is untouched. */
    extern long bp_ram_cache_release_all(void);
    if (bp_ram_cache_release_all() > 0) {
      p = __real_memalign(align, size);
      if (p) return p;
    }
    /* Last resort: take it from the ARENA, even though it is outside the
     * GPUA_MIN..GPUA_MAX routing window.
     *
     * The window governs which allocations are *routed* here by preference --
     * deliberately narrow, so Unity's ~512 MB Dynamic Heap does not colonise an
     * arena meant for render targets. It should not govern what happens once
     * newlib has already said no. At that point the alternative is failing, and
     * the arena is a 512 MB contiguous reservation that was sitting 280 MB idle
     * when a 128 MB request was refused on the character screen: capacity we had
     * taken from newlib and then declined to lend back.
     *
     * Contiguity, not capacity, is what runs out here. The heap is ~2.9 GB and
     * barely touched; it is the big carve-outs -- this arena, and the cache
     * blobs released above -- that leave no single run large enough. */
    if (gpua_enabled && size > GPUA_MAX) {
      p = gpua_alloc(size);
      if (p) {
        static unsigned nb;
        if (nb < 4) {
          nb++;
          debugPrintf("[gpua] %u KB served from the arena reserve after newlib "
                      "refused it (outside the routing window, but the space was "
                      "free)\n", (unsigned)(size >> 10));
        }
        return p;
      }
    }
    static unsigned nf;
    if (nf < 3) {
      nf++;
      debugPrintf("[gpua] memalign FAILED size=%u KB (arena live=%u MB peak=%u MB)\n",
                  (unsigned)(size >> 10), bp_gpua_live_mb(), bp_gpua_peak_mb());
      debugPrintf("[gpua]   (arena %s this request: %u KB is %s the %u KB..%u MB window)\n",
                  (size >= GPUA_MIN && size <= GPUA_MAX) ? "HANDLED" : "did NOT handle",
                  (unsigned)(size >> 10),
                  size > GPUA_MAX ? "above" : "below",
                  (unsigned)(GPUA_MIN >> 10), (unsigned)(GPUA_MAX >> 20));
    }
  }
  return p;
}

void __wrap_free(void *p) {
  if (!p) return;
  if (gpua_owns(p)) { gpua_free(p); return; }
  __real_free(p);
}

/* realloc must respect arena ownership: __real_realloc would treat an arena
 * pointer as a newlib block and corrupt the heap. */
void *__wrap_realloc(void *p, size_t size) {
  if (p && gpua_owns(p)) {
    /* Copy must be bounded by the OLD block, not the new size: on a grow,
     * copying `size` bytes reads past the end of the arena slot. Recover the
     * old length from the run-length table, exactly as PvZ does. */
    size_t old_pages = 0;
    size_t i = (size_t)(((uint8_t *)p - gpua_base) / GPUA_PAGE);
    if (i < gpua_pages) old_pages = gpua_runlen[i];
    size_t oldsz = old_pages * GPUA_PAGE;

    void *q = __real_malloc(size);
    if (q) memcpy(q, p, size < oldsz ? size : oldsz);
    gpua_free(p);
    return q;
  }
  return __real_realloc(p, size);
}

void *__wrap_malloc(size_t size)          { return __real_malloc(size); }
void *__wrap_calloc(size_t n, size_t sz)  { return __real_calloc(n, sz); }
