/* ---------------------------------------------------------------------------
 * bp_assets.c -- see bp_assets.h for what this is and why.
 * MIT.
 * ------------------------------------------------------------------------- */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bp_assets.h"
#include "asset_pack.h"
#include "bp_root.h"
#include "config.h"
#include "diag.h"
#include "util.h"   /* debugPrintf */

/* --------------------------------------------------------------------------
 * Sizing
 *
 * This game stages ~60 files: 10 under bin/Data, 28 under aa/, and 19 loose
 * ones (EOS configs, the Firebase desktop config, loader splashes, guids).
 * The caps below are roughly 30x that, which covers a future content update
 * without making the index worth thinking about against a 108 MB code
 * footprint. Overflow is handled rather than asserted -- see add_entry().
 *
 * ARENA is sized from the longest real path: assets/aa/Android/131f26f9...
 * _monoscripts_4c63a9ec....bundle is 102 bytes on its own, and 22 of the 28
 * bundle names carry parentheses and language tags. 192 KB gives every entry
 * ~100 bytes with room to spare.
 * ------------------------------------------------------------------------ */
#define BPA_MAX_FILES   2048
#define BPA_ARENA       (192 * 1024)
#define BPA_SLOTS       4096            /* power of two, > 2x BPA_MAX_FILES */
#define BPA_MAX_DEPTH   12
#define BPA_PATH        512

typedef struct {
  uint32_t key_off;    /* normalised (lowercased, '/') key, in the arena */
  uint32_t real_off;   /* real relative path with original case, in the arena */
  int64_t  size;
} BmaEntry;

static BmaEntry  g_ent[BPA_MAX_FILES];
static int       g_n;
static int32_t   g_slot[BPA_SLOTS];      /* -1 empty, else index into g_ent */
static char      g_arena[BPA_ARENA];
static uint32_t  g_arena_used;

static char g_assets_dir[BPA_PATH];
static int  g_ready;
static int  g_overflowed;
static int  g_missing_required;

/* ------------------------------------------------------------------ helpers */

static uint32_t fnv1a(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
  return h;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Separator cleanup, CASE PRESERVED: backslashes to slashes, repeated slashes
 * collapsed, leading "./" and leading/trailing '/' removed.
 *
 * Case has to survive here. The cleaned form is what gets appended to the asset
 * directory when a path is NOT in the index, and lowercasing a path on the way
 * to a case-sensitive filesystem would turn a working open into a failing one.
 * Lowercasing belongs only in the lookup key. */
static void clean_sep(const char *in, char *out, size_t outsz) {
  size_t o = 0;
  int prev_slash = 0;
  if (in[0] == '.' && (in[1] == '/' || in[1] == '\\')) in += 2;
  while (*in == '/' || *in == '\\') in++;          /* no leading separator */
  for (; *in && o + 1 < outsz; in++) {
    char c = (*in == '\\') ? '/' : *in;
    if (c == '/') {
      if (prev_slash) continue;
      prev_slash = 1;
    } else {
      prev_slash = 0;
    }
    out[o++] = c;
  }
  while (o > 0 && out[o - 1] == '/') o--;         /* no trailing separator */
  out[o] = 0;
}

/* The lookup key: cleaned, then lowercased. Android is case-sensitive and this
 * filesystem is not reliably either way, so matching on a folded key avoids a
 * whole category of "but the file is right there" bug. */
static void normalise(const char *in, char *out, size_t outsz) {
  clean_sep(in, out, outsz);
  for (char *p = out; *p; p++) *p = lower(*p);
}

static char *arena_put(const char *s) {
  size_t n = strlen(s) + 1;
  if (g_arena_used + n > sizeof g_arena) return NULL;
  char *p = g_arena + g_arena_used;
  memcpy(p, s, n);
  g_arena_used += (uint32_t)n;
  return p;
}

static void add_entry(const char *rel, int64_t size) {
  if (g_n >= BPA_MAX_FILES) {
    if (!g_overflowed) {
      g_overflowed = 1;
      debugPrintf("[assets] index full at %d files -- the rest are NOT indexed.\n"
                  "         Lookups fall back to the filesystem, so nothing\n"
                  "         breaks; it is just slower. Raise BPA_MAX_FILES.\n",
                  BPA_MAX_FILES);
    }
    return;
  }

  char key[BPA_PATH];
  normalise(rel, key, sizeof key);

  char *kp = arena_put(key);
  char *rp = kp ? arena_put(rel) : NULL;
  if (!kp || !rp) {
    if (!g_overflowed) {
      g_overflowed = 1;
      debugPrintf("[assets] arena full -- remaining files not indexed "
                  "(fallback still works). Raise BPA_ARENA.\n");
    }
    return;
  }

  int idx = g_n++;
  g_ent[idx].key_off  = (uint32_t)(kp - g_arena);
  g_ent[idx].real_off = (uint32_t)(rp - g_arena);
  g_ent[idx].size     = size;

  /* Open addressing, linear probe. The table is >2x the entry cap, so load
   * factor stays under 0.5 and probes stay short. */
  uint32_t h = fnv1a(key) & (BPA_SLOTS - 1);
  while (g_slot[h] >= 0) {
    if (!strcmp(g_arena + g_ent[g_slot[h]].key_off, key)) {
      /* Duplicate key. Cannot happen from one filesystem walk unless the
       * filesystem is case-folding two real names onto one key -- keep the
       * first and say so, because silently preferring one is how you get a
       * localisation bundle that loads the wrong language. */
      debugPrintf("[assets] duplicate key '%s' (keeping '%s', ignoring '%s')\n",
                  key, g_arena + g_ent[g_slot[h]].real_off, rel);
      g_n--;
      return;
    }
    h = (h + 1) & (BPA_SLOTS - 1);
  }
  g_slot[h] = idx;
}

static const BmaEntry *lookup(const char *rel) {
  if (!g_ready || !rel) return NULL;
  char key[BPA_PATH];
  normalise(rel, key, sizeof key);
  uint32_t h = fnv1a(key) & (BPA_SLOTS - 1);
  while (g_slot[h] >= 0) {
    const BmaEntry *e = &g_ent[g_slot[h]];
    if (!strcmp(g_arena + e->key_off, key)) return e;
    h = (h + 1) & (BPA_SLOTS - 1);
  }
  return NULL;
}

/* ---------------------------------------------------------------- the walk */

static void walk(const char *abs_dir, const char *rel_prefix, int depth) {
  if (depth > BPA_MAX_DEPTH) {
    debugPrintf("[assets] depth limit at '%s' -- not descending further\n",
                rel_prefix);
    return;
  }

  DIR *d = opendir(abs_dir);
  if (!d) return;

  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

    char abs[BPA_PATH], rel[BPA_PATH];
    if ((int)snprintf(abs, sizeof abs, "%s/%s", abs_dir, de->d_name) >= (int)sizeof abs)
      continue;   /* path too long to represent; skip rather than truncate */
    if (rel_prefix[0])
      snprintf(rel, sizeof rel, "%s/%s", rel_prefix, de->d_name);
    else
      snprintf(rel, sizeof rel, "%s", de->d_name);

    /* d_type is not filled in by every filesystem this could run on, so stat()
     * rather than trusting it. We need the size regardless. */
    struct stat st;
    if (stat(abs, &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      walk(abs, rel, depth + 1);
    } else {
      add_entry(rel, (int64_t)st.st_size);
    }
  }
  closedir(d);
}

/* -------------------------------------------------------------- manifest */

/* Files this game cannot boot without. Every one was confirmed present in the
 * shipped APK; if the index cannot find one, the staging is wrong and there is
 * no point letting the engine discover that forty seconds later.
 *
 * Note "unity default resources" -- Unity really does ship a 3.7 MB file with
 * spaces in its name. This list originally carried it as "unity", truncated by
 * a whitespace-splitting read of a zip listing, and the entry then reported a
 * correctly-staged tree as broken. The index itself is fine with spaces (it
 * walks with readdir/stat and never goes through a shell); it was only the
 * manifest that was wrong. */
static const char *const REQUIRED[] = {
  "bin/Data/data.unity3d",
  "bin/Data/boot.config",
  "bin/Data/Managed/Metadata/global-metadata.dat",
  "bin/Data/Resources/unity default resources",
  "aa/settings.json",
  "aa/catalog.bin",
};

/* Present in the APK and read at startup, but a miss is survivable -- these
 * feed SDK paths that bp_sdk_stubs.c already fails cleanly. Worth a warning so
 * an SDK that dies for want of a config file is not mistaken for a stub bug. */
static const char *const EXPECTED[] = {
  "aa/catalog.hash",
  "EOS/EpicOnlineServicesConfig.json",
  "EOS/eos_android_config.json",
  "EOS/eos_product_config.json",
  "google-services-desktop.json",
  "guids",
};

void bp_assets_report(void) {
  if (!g_ready) { debugPrintf("[assets] not initialised\n"); return; }

  debugPrintf("[assets] %d files indexed under %s\n", g_n, g_assets_dir);

  int missing = 0;
  for (size_t i = 0; i < sizeof REQUIRED / sizeof *REQUIRED; i++) {
    if (!bp_assets_exists(REQUIRED[i])) {
      debugPrintf("[assets] MISSING (required): assets/%s\n", REQUIRED[i]);
      missing++;
    }
  }
  for (size_t i = 0; i < sizeof EXPECTED / sizeof *EXPECTED; i++) {
    if (!bp_assets_exists(EXPECTED[i]))
      debugPrintf("[assets] missing (expected): assets/%s\n", EXPECTED[i]);
  }

  g_missing_required = missing;

  if (missing) {
    debugPrintf("[assets] %d required file(s) missing. The game will not boot.\n"
                "         Re-stage from your own APK:\n"
                "             python3 tools/stage_sd.py Bouncemasters.apk -o out\n"
                "         then copy out/ next to the .nro.\n", missing);
  } else {
    int64_t d = bp_assets_size("bin/Data/data.unity3d");
    int64_t m = bp_assets_size("bin/Data/Managed/Metadata/global-metadata.dat");
    debugPrintf("[assets] manifest OK (data.unity3d %lld KB, metadata %lld KB)\n",
                (long long)(d / 1024), (long long)(m / 1024));
  }
}

/* ------------------------------------------------------------------- init */

int bp_assets_init(void) {
  if (g_ready) return g_n;

  for (int i = 0; i < BPA_SLOTS; i++) g_slot[i] = -1;

  snprintf(g_assets_dir, sizeof g_assets_dir, "%s/%s",
           bp_game_root(), BP_ASSET_DIR);

  /* Once assets.nxpack exists the loose tree is deleted, so there is no
   * directory here to stat or walk -- the pack index IS the asset tree. This
   * layer's users (asset_redirect) only need path fixup, which the pack answers
   * directly in libc_shim's open/stat, so reporting and returning is correct. */
  if (asset_pack_active()) {
    g_ready = 1;
    g_n = (int)asset_pack_entry_count();
    debugPrintf("[assets] served from assets.nxpack: %d entries "
                "(loose tree absent, as expected)\n", g_n);
    return g_n;
  }

  struct stat st;
  if (stat(g_assets_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    debugPrintf("[assets] FATAL: no asset directory at %s\n"
                "         Expected the staged tree next to the .nro:\n"
                "             <folder>/libmain.so libunity.so libil2cpp.so\n"
                "             <folder>/assets/bin/Data/...\n"
                "             <folder>/assets/aa/...\n",
                g_assets_dir);
    return -1;
  }

  walk(g_assets_dir, "", 0);
  g_ready = 1;

  bp_assets_report();
  return g_n;
}

int bp_assets_count(void) { return g_n; }

int bp_assets_exists(const char *rel) { return lookup(rel) != NULL; }

int64_t bp_assets_size(const char *rel) {
  const BmaEntry *e = lookup(rel);
  return e ? e->size : -1;
}

/* --------------------------------------------------------------- resolve */

/* Find the asset-relative tail of an engine path, or NULL.
 *
 * The shapes Unity produces on Android, all of which reach us verbatim:
 *
 *   file://jar:file:///data/app/~~x/com.joybits.bloonspop-y/base.apk!/assets/aa/catalog.bin
 *   jar:file:///data/app/.../base.apk!/assets/bin/Data/data.unity3d
 *   /assets/bin/Data/boot.config
 *
 * "!/assets/" is the APK separator and is the reliable anchor -- it does not
 * matter what the fake package code path in front of it looks like, and it will
 * not match a legitimate SD path. Check it before the plain "/assets/" prefix,
 * because the jar forms contain that substring too. */
static int sep(char c) { return c == '/' || c == '\\'; }

static const char *asset_tail(const char *p) {
  const char *bang = strstr(p, "!/assets");
  if (bang) {
    const char *t = bang + 8;               /* past "!/assets" */
    if (sep(*t)) return t + 1;              /* "...!/assets/foo" -> "foo"   */
    if (*t == 0) return "";                 /* "...!/assets"     -> the dir */
    return NULL;                            /* "!/assetsfoo" is not ours    */
  }
  /* Plain absolute form, either separator. .NET path handling on the managed
   * side can hand back "\\assets\\..." even though the engine emits '/'. */
  if ((p[0] == '/' || p[0] == '\\') && !strncmp(p + 1, "assets", 6)) {
    const char *t = p + 7;
    if (sep(*t)) return t + 1;
    if (*t == 0) return "";
  }
  return NULL;
}

const char *bp_assets_resolve(const char *path, char *out, size_t outsz) {
  if (!path || !out || outsz == 0) return path;

  /* Strip a file:// prefix first; Unity sometimes emits "file://jar:file://",
   * and the jar handling below wants the inner form. */
  const char *p = path;
  if (!strncmp(p, "file://", 7)) p += 7;

  const char *tail = asset_tail(p);
  if (!tail) {
    /* Not an asset path. If we stripped a file:// prefix off an otherwise
     * ordinary absolute path, hand back the stripped version -- newlib has no
     * idea what a URL is. Otherwise leave it entirely alone. */
    if (p != path) { snprintf(out, outsz, "%s", p); return out; }
    return path;
  }

  if (tail[0] == 0) {
    snprintf(out, outsz, "%s", g_assets_dir[0] ? g_assets_dir : bp_game_root());
    return out;
  }

  /* Prefer the indexed spelling. The engine may ask with different case than
   * the filesystem stored, and on a case-sensitive host that is a hard miss;
   * substituting the real name makes the open succeed either way.
   *
   * On a miss, fall back to the separator-cleaned tail rather than the raw one
   * so that "/assets//aa///settings.json" does not reach the filesystem with
   * its duplicate slashes intact. Case is preserved on this path -- see
   * clean_sep(). */
  char cleaned[BPA_PATH];
  clean_sep(tail, cleaned, sizeof cleaned);
  if (cleaned[0] == 0) {
    snprintf(out, outsz, "%s", g_assets_dir[0] ? g_assets_dir : bp_game_root());
    return out;
  }

  const BmaEntry *e = lookup(cleaned);
  const char *rel = e ? (g_arena + e->real_off) : cleaned;

  snprintf(out, outsz, "%s/%s", g_assets_dir[0] ? g_assets_dir : bp_game_root(), rel);

#if DEBUG_LOG
  /* Log only misses, and only the first few. A hit is the normal case and
   * logging it once per asset would bury the boot log; a miss is a lead. */
  if (!e) {
    static int miss_logged;
    if (miss_logged < 32) {
      miss_logged++;
        /* Queued, not written. This fires from open() AND stat(), both of which
         * run on Unity's worker threads, and a blocking write from one of those
         * can stall the game. The ring in libc_shim.c drains it on the main
         * thread. */
        { extern void bp_tr_log_line(const char *line);
          char m_[420];
          snprintf(m_, sizeof m_,
                   "[assets] miss: '%s' -> %s (not in index; trying anyway)\n",
                   path, out);
          bp_tr_log_line(m_);
          if (miss_logged == 32)
            bp_tr_log_line("[assets] (further misses not logged)\n");
        }
    }
  }
#endif

  return out;
}
