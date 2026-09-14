/* ---------------------------------------------------------------------------
 * bp_assets.h -- in-memory index of the staged asset tree.
 *
 * WHAT THIS REPLACES
 * ------------------
 * libc_shim.c's asset_redirect() rewrote "/assets/foo" to
 * "<root>/assets/foo" and handed it straight to newlib. That is correct as far
 * as it goes, and it goes about half as far as this port needs:
 *
 *   - It never checks whether the file is there. A user who staged an
 *     incomplete tree finds out through whatever the engine does forty seconds
 *     later, which is usually a black screen and never a filename.
 *   - It only handles one path shape. Unity also builds
 *     "jar:file:///data/app/.../base.apk!/assets/...", and libunity.so's string
 *     table even carries the doubled "file://jar:file://" form. Those reach
 *     newlib verbatim, resolve against the SD card root, and fail.
 *   - Every miss costs real I/O. Unity's Resources and Addressables layers
 *     probe several candidate paths per asset and expect most to fail. On
 *     FAT32 over the Switch's SD interface each of those failures walks a
 *     directory.
 *
 * WHAT THIS DOES
 * --------------
 * Walks <root>/assets once at boot and keeps a hash index of every file it
 * finds: normalised key -> real on-disk path + size. After that, existence and
 * size are memory reads, misses cost nothing, and the four URL shapes above all
 * normalise to the same key.
 *
 * It also verifies a manifest of the files this specific game cannot boot
 * without, and says plainly which are missing. That is the part worth having:
 * "aa/catalog.bin missing" is a five-second fix, and the same situation without
 * this check is an evening.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 * --------------------------------
 * It does not cache file CONTENTS. The staged tree is ~59 MB against a game
 * that already maps 108 MB of code, and Unity does its own asset caching one
 * layer up. This is an index of names, not a page cache.
 *
 * It is also not authoritative for opens. If a path is not in the index, the
 * resolver still hands back a rewritten on-disk path and lets the filesystem
 * answer -- so a file created at runtime, or one added after boot, still works.
 * The index makes the common case fast; it does not get to veto reality.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#ifndef BP_ASSETS_H
#define BP_ASSETS_H

#include <stddef.h>
#include <stdint.h>

/* Scan <bp_game_root()>/assets and verify the manifest.
 *
 * Call once, AFTER bp_resolve_game_root() and before the engine starts.
 * Returns the number of files indexed, or a negative value if the asset
 * directory is missing entirely (which is fatal and worth saying so).
 *
 * Safe to call twice; the second call is a no-op. */
int bp_assets_init(void);

/* Rewrite an engine-supplied path to something openable on the SD card.
 *
 * Handles, in order:
 *   file://jar:file://<apk>!/assets/<rel>
 *   jar:file://<apk>!/assets/<rel>
 *   <anything>!/assets/<rel>
 *   /assets/<rel>   and   /assets
 *   file://<abs>
 *
 * Returns `out` when it rewrote, or `path` unchanged when the path is none of
 * the above. Never returns NULL for a non-NULL input, so it is safe to chain
 * directly into open()/fopen(). */
const char *bp_assets_resolve(const char *path, char *out, size_t outsz);

/* Index queries. `rel` is relative to the assets directory, e.g.
 * "aa/catalog.bin". Case-insensitive, and '\\' is accepted for '/'. */
int     bp_assets_exists(const char *rel);
int64_t bp_assets_size(const char *rel);

/* Number of files indexed; 0 before init. */
int bp_assets_count(void);

/* Re-log the manifest verdict. bp_assets_init() already does this once. */
void bp_assets_report(void);

#endif /* BP_ASSETS_H */
