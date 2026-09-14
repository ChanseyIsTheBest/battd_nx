/* battd_save_probe.h -- log the real Profile.Save password at runtime.
 * See the .c for why this measures rather than searches, and why it can call
 * through without a trampoline. Diagnostic only; changes no behaviour. */
#ifndef BATTD_SAVE_PROBE_H
#define BATTD_SAVE_PROBE_H

#include "so_util.h"

/* Install after libil2cpp is mapped. Returns 1 if armed, 0 if the guard failed. */
int battd_save_probe_install(so_module *il2cpp);

#endif /* BATTD_SAVE_PROBE_H */
