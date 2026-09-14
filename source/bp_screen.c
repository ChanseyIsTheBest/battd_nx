#include "bp_offsets.h"
#include "bp_managed.h"
/* ---------------------------------------------------------------------------
 * bp_screen.c -- make UnityEngine.Screen report the real render size, and the
 * shared guarded installer every managed hook in this port goes through.
 *
 * WHY Screen NEEDS HOOKING
 * ------------------------
 * Screen.width/height resolve through the Android player to the Java view
 * hierarchy, which does not exist here. Left alone they report whatever the
 * stubbed path produces -- typically 0 -- and anything laying out UI against
 * them divides by zero or places everything off-screen. cloverpit_nx records
 * catching UnityMain SPINNING because these returned wrong values, so this is
 * not merely cosmetic.
 *
 * safeArea is answered as the full rect. The Switch panel has no notch and no
 * rounded corners, so the safe area IS the screen; a game that insets its UI
 * from a wrongly-reported safe area leaves a visible band of dead space.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <string.h>

#include "bp_managed.h"
#include "config.h"
#include "diag.h"
#include "so_util.h"
#include "util.h"

extern int screen_width, screen_height;

typedef struct { float x, y, w, h; } URect;

/* UnityEngine.Resolution: int m_Width; int m_Height; RefreshRate m_RefreshRate.
 * RefreshRate is { uint numerator; uint denominator }, so 16 bytes total. */
typedef struct { int32_t w, h; uint32_t num, den; } UResolution;

/* --------------------------------------------------------------------------
 * The shared installer
 *
 * Verify THREE words, then hook. Two would not be enough: every accessor in
 * this binary starts with the same `stp x30, x19, [sp, #-0x10]!` + `adrp x19`
 * pair, and only the third instruction's displacement distinguishes
 * get_deltaTime from get_timeScale. A two-word guard would cheerfully accept a
 * hook installed on the wrong getter, and the result -- Time.timeScale
 * returning a delta -- is the kind of bug that looks like physics tuning.
 *
 * On mismatch: skip that hook and say which one. Never hook anyway. A missing
 * hook degrades one behaviour; a misplaced one corrupts an unrelated method.
 * ------------------------------------------------------------------------ */
int bp_managed_hook(so_module *il2cpp, const char *what, uint32_t rva,
                    const uint32_t guard3[3], void *fn) {
  if (!il2cpp || !il2cpp->load_virtbase) {
    debugPrintf("[hook] %s SKIPPED: libil2cpp not mapped\n", what);
    return 0;
  }

  uintptr_t addr = (uintptr_t)il2cpp->load_virtbase + rva;
  const uint32_t *live = (const uint32_t *)addr;

  for (int i = 0; i < 3; i++) {
    if (live[i] != guard3[i]) {
      debugPrintf("[hook] %s SKIPPED: guard word %d at il2cpp+0x%x differs "
                  "(expected 0x%08x, found 0x%08x)\n"
                  "       bp_managed.h is stale for this libil2cpp -- regenerate\n"
                  "       it with tools/scan_managed.py from a matching dump.cs\n",
                  what, i, (unsigned)(rva + i * 4), guard3[i], live[i]);
      return 0;
    }
  }

  hook_arm64(addr, (uintptr_t)fn);
  debugPrintf("[hook] %-38s il2cpp+0x%07x -> %p\n", what, (unsigned)rva, fn);
  return 1;
}

/* --------------------------------------------------------------------------
 * In-place word patch
 *
 * For managed methods too SHORT to hook. hook_arm64 writes four instructions;
 * a two-instruction accessor has room for one. Verifying the whole function and
 * rewriting a single word is both safer and smaller here.
 * ------------------------------------------------------------------------ */
int bp_managed_patch(so_module *il2cpp, const char *what, uint32_t rva,
                     const uint32_t *guard, int nguard, uint32_t word) {
  if (!il2cpp || !il2cpp->load_virtbase) {
    debugPrintf("[patch] %s SKIPPED: libil2cpp not mapped\n", what);
    return 0;
  }
  uintptr_t addr = (uintptr_t)il2cpp->load_virtbase + rva;
  const uint32_t *live = (const uint32_t *)addr;

  for (int i = 0; i < nguard; i++) {
    if (live[i] != guard[i]) {
      debugPrintf("[patch] %s SKIPPED: word %d at il2cpp+0x%x differs "
                  "(expected 0x%08x, found 0x%08x)\n",
                  what, i, (unsigned)(rva + i * 4), guard[i], live[i]);
      return 0;
    }
  }
  if (so_patch_code((uint32_t *)addr, &word, 4) != 0) {
    debugPrintf("[patch] %s FAILED to write at il2cpp+0x%x\n", what, (unsigned)rva);
    return 0;
  }
  debugPrintf("[patch] %-34s il2cpp+0x%07x <- 0x%08x\n", what, (unsigned)rva, word);
  return 1;
}

/* Multi-word variant. Verify ALL guard words, then write ALL replacement words.
 *
 * Needed because rewriting a function body is not a sequence of independent
 * one-word patches: a partial rewrite leaves a function that falls through into
 * a prologue expecting a frame it never built, which is far worse than not
 * patching at all. Verify-everything-then-write-everything is the only safe
 * order. */
int bp_managed_patch_n(so_module *il2cpp, const char *what, uint32_t rva,
                       const uint32_t *guard, const uint32_t *words, int n) {
  if (!il2cpp || !il2cpp->load_virtbase) {
    debugPrintf("[patch] %s SKIPPED: libil2cpp not mapped\n", what);
    return 0;
  }
  uintptr_t addr = (uintptr_t)il2cpp->load_virtbase + rva;
  const uint32_t *live = (const uint32_t *)addr;

  for (int i = 0; i < n; i++) {
    if (live[i] != guard[i]) {
      debugPrintf("[patch] %s SKIPPED: word %d at il2cpp+0x%x differs "
                  "(expected 0x%08x, found 0x%08x)\n",
                  what, i, (unsigned)(rva + i * 4), guard[i], live[i]);
      return 0;
    }
  }
  for (int i = 0; i < n; i++) {
    if (so_patch_code((uint32_t *)(addr + i * 4), &words[i], 4) != 0) {
      debugPrintf("[patch] %s FAILED at word %d -- function is now PARTIALLY "
                  "rewritten and must not be trusted\n", what, i);
      return 0;
    }
  }
  debugPrintf("[patch] %-42s il2cpp+0x%07x (%d words)\n", what, (unsigned)rva, n);
  return 1;
}

/* ------------------------------------------------------------------ hooks */

static int32_t bps_width(void)  { return screen_width;  }
static int32_t bps_height(void) { return screen_height; }

/* Reported DPI. The handheld panel is 6.2" at 1280x720, about 237 dpi; docked
 * output has no meaningful physical size at all. Unity uses this only to scale
 * UI on canvases set to "Constant Physical Size", so a plausible constant is
 * both correct enough and stable across docking -- which a "real" value would
 * not be. */
static float bps_dpi(void) { return bp_ui_dpi; }   /* config.txt ui_scale */

/* ---------------------------------------------------------------------------
 * SystemInfo.GetPhysicalMemoryMB()
 *
 * The second half of the art-quality gate (see config.h). The shim reports
 * 512 MB through sysconf(_SC_PHYS_PAGES) and a synthetic /proc/meminfo, which
 * is what Unity sizes its own heaps from and should stay conservative -- so
 * this hooks the ONE engine function the game's tier check calls instead of
 * raising the global figure. libunity's systeminfo::GetPhysicalMemoryMB() is
 * 24 bytes, comfortably over the 16-byte detour stub.
 * ------------------------------------------------------------------------- */
static int bps_physical_memory_mb(void) { return bp_memory_mb; }

int bp_physmem_install(so_module *unity) {
#if BP_PATCH_PHYSMEM
  uintptr_t b = (uintptr_t)unity->load_virtbase;
  if (!bp_guard_ok(unity, BP_RVA_GetPhysicalMemoryMB, BP_GUARD_GetPhysicalMemoryMB, 4,
                   "systeminfo::GetPhysicalMemoryMB"))
    return 0;
  hook_arm64(b + BP_RVA_GetPhysicalMemoryMB, (uintptr_t)&bps_physical_memory_mb);
  debugPrintf("[screen] GetPhysicalMemoryMB -> %d MB (art tier needs >= 1536)\n",
              bp_memory_mb);
  return 1;
#else
  (void)unity; return 0;
#endif
}

static void bps_currentResolution_Injected(UResolution *ret) {
  if (!ret) return;
  ret->w = screen_width;
  ret->h = screen_height;
  ret->num = 60; ret->den = 1;     /* 60/1 Hz, docked and handheld */
}

static void bps_safeArea_Injected(URect *ret) {
  if (!ret) return;
  ret->x = 0.0f; ret->y = 0.0f;
  ret->w = (float)screen_width;
  ret->h = (float)screen_height;
}

static int32_t bpd_systemWidth(void)     { return screen_width;  }
static int32_t bpd_systemHeight(void)    { return screen_height; }
static int32_t bpd_renderingWidth(void)  { return screen_width;  }
static int32_t bpd_renderingHeight(void) { return screen_height; }

/* ---------------------------------------------------------------- install */

/* libunity icall bindings. Display.* are 4-byte tail thunks in this build and
 * cannot take a 16-byte stub; the engine-side window size already reports the
 * portrait render size (android_native_unity.c), which is what they return. */
int bp_screen_install(so_module *unity) {
  const int N = BP_NSITES(BP_SCREEN_SITES);
  int n = 0;
#define H(nm, fn) n += bp_icall_hook(unity, BP_SCREEN_SITES, N, nm, (void *)&fn)
  H("get_width",                      bps_width);
  H("get_height",                     bps_height);
  H("get_dpi",                        bps_dpi);
  H("get_currentResolution_Injected", bps_currentResolution_Injected);
  H("get_safeArea_Injected",          bps_safeArea_Injected);
#undef H
  debugPrintf("[screen] %d/5 Screen icall bindings hooked (%dx%d landscape, %d dpi)\n",
              n, screen_width, screen_height, (int)bp_ui_dpi);
  return n;
}
