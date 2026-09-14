/* bp_root.h -- runtime data-root resolution.
 *
 * The SD folder name is NOT compiled in. bp_resolve_game_root() must be called
 * first thing in main(), before any file access and before the first log write,
 * since the log path is derived from the resolved root.
 */
#ifndef BP_ROOT_H
#define BP_ROOT_H

/* Resolve and return the data root. Idempotent; safe to call more than once. */
const char *bp_resolve_game_root(int argc, char **argv);

/* Resolved data root, e.g. "sdmc:/switch/bloonspop". Falls back to the
 * compiled-in GAME_HOME if nothing validated. Never NULL. */
const char *bp_game_root(void);

/* "<root>/debug.log". Never NULL. */
const char *bp_log_path(void);

/* Log how the root was chosen. Call after resolution. */
void bp_root_report(int argc, char **argv);

#endif /* BP_ROOT_H */
