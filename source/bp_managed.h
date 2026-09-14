/* bp_managed.h -- hook helpers and installer prototypes. MIT. */
#ifndef BP_MANAGED_H
#define BP_MANAGED_H
#include <stdint.h>
#include "so_util.h"
#include "bp_offsets.h"

/* libunity icall-binding hooks, verify-first (bp_icall.c) */
int bp_guard_ok(so_module *m, uint32_t rva, const uint32_t *guard, int n, const char *what);
int bp_redirect(so_module *m, uint32_t rva, const uint32_t *guard, int nguard, void *fn, const char *what);
int bp_icall_hook(so_module *unity, const BpSite *tab, int n, const char *name, void *fn);

/* inherited libil2cpp managed-method helpers (bp_screen.c) */
int bp_managed_hook(so_module *il2cpp, const char *what, uint32_t rva,
                    const uint32_t guard3[3], void *fn);
int bp_managed_patch(so_module *il2cpp, const char *what, uint32_t rva,
                     const uint32_t *guard, int nguard, uint32_t word);
int bp_managed_patch_n(so_module *il2cpp, const char *what, uint32_t rva,
                       const uint32_t *guard, const uint32_t *words, int n);

int  bp_time_install(so_module *unity);
int  bp_input_install(so_module *unity);
int  bp_screen_install(so_module *unity);

/* Hook libunity's systeminfo::GetPhysicalMemoryMB() so it answers config.txt
 * memory_mb. This is the second gate on the game's 2D art tier (see config.h);
 * it is deliberately NOT the global figure the shim reports through
 * /proc/meminfo, which Unity sizes its own heaps from. */
int bp_physmem_install(so_module *unity);
int  bp_prefs_install(so_module *il2cpp);
void bp_time_tick(void);
void bp_prefs_tick(void);
void bp_prefs_flush_now(void);

int  bp_tmclock_install(so_module *unity);
void bp_tmclock_start(void);
int  bp_tmclock_active(void);

#endif
