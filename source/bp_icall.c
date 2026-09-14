/* ---------------------------------------------------------------------------
 * bp_icall.c -- verify-first redirects into libunity.
 *
 * Unity 2020.3 registers each icall through a five-instruction block
 * (adrp x0,name; adrp x1,fn; add; add; b register). tools/offsets/ reads those
 * blocks out of THIS libunity, checks every function body against the named
 * 2020.3.15f2 reference, and records the game's own leading words as guards.
 * A binding smaller than the 16-byte stub is refused rather than overwritten
 * into its neighbour.
 * MIT.
 * ------------------------------------------------------------------------- */
#include <stdint.h>
#include <string.h>
#include "bp_managed.h"
#include "so_util.h"
#include "util.h"

int bp_guard_ok(so_module *m, uint32_t rva, const uint32_t *guard, int n, const char *what) {
  if (!m || !m->load_virtbase) {
    debugPrintf("[guard] %s SKIPPED: module not mapped\n", what);
    return 0;
  }
  const uint32_t *live = (const uint32_t *)((uintptr_t)m->load_virtbase + rva);
  for (int i = 0; i < n; i++) {
    if (live[i] != guard[i]) {
      debugPrintf("[guard] %s SKIPPED: word %d at +0x%x is 0x%08x, expected 0x%08x\n"
                  "        bp_offsets.h does not match this binary -- rerun\n"
                  "        tools/offsets/run_all.sh after a game update\n",
                  what, i, (unsigned)(rva + 4u * (unsigned)i), live[i], guard[i]);
      return 0;
    }
  }
  return 1;
}

int bp_redirect(so_module *m, uint32_t rva, const uint32_t *guard, int nguard,
                void *fn, const char *what) {
  if (!bp_guard_ok(m, rva, guard, nguard, what)) return 0;
  hook_arm64((uintptr_t)m->load_virtbase + rva, (uintptr_t)fn);
  debugPrintf("[hook] %-34s +0x%07x -> %p\n", what, (unsigned)rva, fn);
  return 1;
}

int bp_icall_hook(so_module *unity, const BpSite *tab, int n, const char *name, void *fn) {
  for (int i = 0; i < n; i++) {
    if (strcmp(tab[i].name, name) != 0) continue;
    if (tab[i].size < 16) {
      debugPrintf("[hook] %s: %u-byte binding cannot take a 16-byte stub; skipped\n",
                  name, (unsigned)tab[i].size);
      return 0;
    }
    int ng = (int)(tab[i].size / 4u);
    if (ng > 4) ng = 4;
    return bp_redirect(unity, tab[i].rva, tab[i].guard, ng, fn, name);
  }
  debugPrintf("[hook] %s: not registered by this build\n", name);
  return 0;
}
