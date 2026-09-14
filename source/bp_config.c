/* bp_config.c -- config.txt: the player's settings, read at every boot. MIT.
 *
 * resolution -- the game's PORTRAIT resolution, by its "p" number (the width):
 * 720 = 720 x 1280 (default) up to 1080 = 1080 x 1920. One setting for handheld
 * and docked. Only the sharpness changes; the picture is presented exactly as
 * before. Values are snapped to the nearest multiple of 16, the sizes where
 * width * 16 / 9 is a whole number, so the image keeps the exact 9:16 shape
 * and every buffer divides evenly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "bp_config.h"
#include "unity_jni.h"
#include "util.h"

const char *bp_game_root(void);

int bp_res_w = 1280, bp_res_h = 720;
float bp_ui_dpi   = 320.0f;   /* config.txt ui_scale; drives the art-quality tier */
int   bp_memory_mb = 2048;
int   bp_allow_online = 0;   /* config.txt online; only consulted once cached */
int   bp_art_quality = 2;     /* config.txt art_quality; -1 = auto (no patch) */    /* config.txt memory_mb; the >=1536 gate for high art */
int bp_portrait_rot = 0;   /* landscape port: unused; config.h externs it */                 /* 1 = 90 CW (right Joy-Con up), 2 = 90 CCW (left Joy-Con up) */

/* Each setting's block in the template. A config.txt written by an older build
 * that lacks one gets that block appended, so new options show up without the
 * player deleting the file. */
static const struct { const char *key; const char *block; } SECTIONS[] = {
  { "resolution",
    "# --- resolution ------------------------------------------------------\n"
    "# The game's landscape resolution, by its \"p\" number (the width):\n"
    "#    1280 -> 1280 x  720   (default, the exact handheld panel)\n"
    "#    1600 -> 1600 x  900\n"
    "#   1920 -> 1920 x 1080\n"
    "# Any value from 1280 to 1920 is accepted and rounded to the nearest size that\n"
    "# keeps the exact 9:16 shape. One setting for both handheld and docked.\n"
    "# Higher is sharper (most visible on a TV) but costs performance.\n"
    "resolution = 1280\n" },
  { "ui_scale",
    "# --- ui_scale --------------------------------------------------------\n"
    "# Screen density reported to the game, as dots-per-inch.\n"
    "#\n"
    "# THIS SELECTS THE 2D ART QUALITY. The game picks its asset variant from\n"
    "# dpi / 160:  below 1.5 it loads the LOW-detail bundles, at or above 1.5 it\n"
    "# loads the high/ultra ones. The console panel's true density is about 237\n"
    "# dpi, which works out to 1.48 -- just under the cut, so the honest value\n"
    "# gets low-detail art. 320 (the standard phone \"xhdpi\" bucket this game's\n"
    "# UI was drawn for) clears it comfortably.\n"
    "#\n"
    "# It also scales any UI set to a constant physical size, so a much larger\n"
    "# number makes menus bigger. 240 is the smallest value that still gets the\n"
    "# better art; 320 is the default.\n"
    "ui_scale = 320\n" },
  { "art_quality",
    "# --- art_quality -----------------------------------------------------\n"
    "# Forces the 2D art tier the game loads, instead of letting it choose from\n"
    "# screen density and reported RAM.\n"
    "#   2    = best (default)\n"
    "#   1    = what the game picked on its own before this was forced; it\n"
    "#          loads the .low bundles\n"
    "#   0    = the small-screen tier\n"
    "#   auto = do not patch at all; the game decides, using ui_scale and\n"
    "#          memory_mb below\n"
    "art_quality = 2\n" },
  { "online",
    "# --- online ----------------------------------------------------------\n"
    "# What to do once ALL the downloadable content is cached:\n"
    "#   false = stay offline and play from the cache (default)\n"
    "#   true  = keep the internet on, so content updates are picked up\n"
    "#\n"
    "# This has NO EFFECT until the download has finished -- the first run has\n"
    "# to fetch about 244 MB, and the port stays online until it has. Once the\n"
    "# cache is complete this decides, and debug.log says which way it went.\n"
    "#\n"
    "# An empty file named force_online next to the .nro overrides this for one\n"
    "# launch, without editing the file.\n"
    "online = false\n" },
  { "memory_mb",
    "# --- memory_mb -------------------------------------------------------\n"
    "# Physical RAM reported to the game, in megabytes.\n"
    "#\n"
    "# The game asks for this only after it has decided on high-detail art, and\n"
    "# drops back to the LOW-detail bundles if the answer is under 1536. This\n"
    "# does NOT change how much memory the port actually uses, and it is not\n"
    "# what Unity sizes its own heaps from -- that still sees the conservative\n"
    "# value the shim reports through /proc/meminfo.\n"
    "memory_mb = 2048\n" },
};
#define N_SECTIONS ((int)(sizeof SECTIONS / sizeof *SECTIONS))

#define RES_MIN 1280
#define RES_MAX 1920

static void write_template(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { debugPrintf("[config] could not write %s\n", path); return; }
  fputs("# config.txt -- Bloons Adventure Time TD settings, read at every launch.\n", f);
  for (int i = 0; i < N_SECTIONS; i++) fprintf(f, "\n%s", SECTIONS[i].block);
  fclose(f);
  debugPrintf("[config] wrote %s (defaults)\n", path);
}

/* Does the text have a line "key =" or "#key =" (any spacing)? Prose that merely
 * contains the word does not count. */
static int mentions_key(const char *text, const char *key) {
  const size_t kl = strlen(key);
  for (const char *p = text; p && *p; ) {
    while (*p == ' ' || *p == '\t' || *p == '#') p++;
    if (!strncmp(p, key, kl)) {
      const char *q = p + kl;
      while (*q == ' ' || *q == '\t') q++;
      if (*q == '=') return 1;
    }
    p = strchr(p, '\n');
    if (p) p++;
  }
  return 0;
}
static void append_missing(const char *path) {
  char text[16384];
  FILE *f = fopen(path, "r");
  if (!f) return;
  const size_t n = fread(text, 1, sizeof text - 1, f);
  fclose(f);
  text[n] = 0;
  for (int i = 0; i < N_SECTIONS; i++) {
    if (mentions_key(text, SECTIONS[i].key)) continue;
    if (!(f = fopen(path, "a"))) return;
    fprintf(f, "%s\n%s", (n && text[n - 1] != '\n') ? "\n" : "", SECTIONS[i].block);
    fclose(f);
    debugPrintf("[config] added the new \"%s\" setting to %s\n", SECTIONS[i].key, path);
  }
}

/* true/false/on/off/yes/no/1/0, case-insensitive. -1 = not a flag.
 * Written out rather than using strcasecmp: that lives in <strings.h>, which
 * this file does not include, and the Makefile promotes an implicit declaration
 * to an error -- on AArch64 a wrongly-assumed int return is a real bug, not a
 * style point. ctype.h is already here. */
static int parse_flag(const char *v) {
  char b[8];
  size_t i = 0;
  for (; v[i] && i < sizeof b - 1; i++) b[i] = (char)tolower((unsigned char)v[i]);
  b[i] = 0;
  if (!strcmp(b, "true")  || !strcmp(b, "1") || !strcmp(b, "on")  || !strcmp(b, "yes")) return 1;
  if (!strcmp(b, "false") || !strcmp(b, "0") || !strcmp(b, "off") || !strcmp(b, "no"))  return 0;
  return -1;
}

static void trim(char *s) {
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

void bp_config_load(void) {
  char path[640], line[256];
  snprintf(path, sizeof path, "%s/config.txt", bp_game_root());
  FILE *f = fopen(path, "r");
  if (!f) write_template(path);
  else { fclose(f); append_missing(path); }
  f = fopen(path, "r");
  int res = RES_MIN;
  while (f && fgets(line, sizeof line, f)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *key = line, *val = eq + 1;
    trim(key); trim(val);
    if (!*key || !*val) continue;
    if (!strcmp(key, "resolution")) {
      char *end;
      long v = strtol(val, &end, 10);
      if (end != val && (!*end || !strcmp(end, "p") || !strcmp(end, "P"))) res = (int)v;
      else debugPrintf("[config] resolution \"%s\" is not a number -- using %d\n", val, res);
    } else if (!strcmp(key, "ui_scale")) {
      char *end; long v = strtol(val, &end, 10);
      if (end != val && v >= 96 && v <= 640) bp_ui_dpi = (float)v;
      else debugPrintf("[config] ui_scale \"%s\" must be 96..640 -- using %d\n", val, (int)bp_ui_dpi);
    } else if (!strcmp(key, "art_quality")) {
      if (!strcmp(val, "auto")) bp_art_quality = -1;
      else {
        char *end; long v = strtol(val, &end, 10);
        if (end != val && v >= 0 && v <= 2) bp_art_quality = (int)v;
        else debugPrintf("[config] art_quality \"%s\" must be 0, 1, 2 or auto -- using %d\n",
                         val, bp_art_quality);
      }
    } else if (!strcmp(key, "online")) {
      const int b = parse_flag(val);
      if (b >= 0) bp_allow_online = b;
      else debugPrintf("[config] online \"%s\" must be true or false -- using %s\n",
                       val, bp_allow_online ? "true" : "false");
    } else if (!strcmp(key, "memory_mb")) {
      char *end; long v = strtol(val, &end, 10);
      if (end != val && v >= 256 && v <= 8192) bp_memory_mb = (int)v;
      else debugPrintf("[config] memory_mb \"%s\" must be 256..8192 -- using %d\n", val, bp_memory_mb);
    } else {
      debugPrintf("[config] unknown setting \"%s\" -- ignored\n", key);
    }
  }
  if (f) fclose(f);
  int s = res < RES_MIN ? RES_MIN : res > RES_MAX ? RES_MAX : res;
  s = ((s + 9) / 16) * 16;                               /* nearest exact 9:16 width */
  if (s != res) debugPrintf("[config] resolution %d -> %d (nearest exact 9:16 size from %d to %d)\n",
                            res, s, RES_MIN, RES_MAX);
  bp_res_w = s;
  bp_res_h = s * 9 / 16;
  debugPrintf("[config] resolution %d wide: the game renders %d x %d (landscape)\n", s, bp_res_w, bp_res_h);
  debugPrintf("[config] ui_scale %d dpi -> density %.3f (art tier: %s)\n",
              (int)bp_ui_dpi, bp_ui_dpi / 160.0f,
              (bp_ui_dpi / 160.0f) >= 1.5f ? "high/ultra" : "LOW");
  debugPrintf("[config] memory_mb %d (>=1536 keeps the high-detail art)\n", bp_memory_mb);
  debugPrintf("[config] online %s (only applies once the cache is complete)\n",
              bp_allow_online ? "true: stay connected" : "false: go offline when cached");
  if (bp_art_quality >= 0)
    debugPrintf("[config] art_quality %d: forced, ui_scale and memory_mb do not decide it\n", bp_art_quality);
  else
    debugPrintf("[config] art_quality auto: the game chooses from ui_scale and memory_mb\n");
}

/* Unity saved its last screen size in PlayerPrefs (720 x 1280 in the first saves).
 * Keep it in step with config.txt so an old entry can never pull a new resolution
 * back. Only keys the game already wrote are touched. */
void bp_config_sync_prefs(void) {
  static const char *const K[2] = { "Screenmanager Resolution Width", "Screenmanager Resolution Height" };
  char v[2][16];
  snprintf(v[0], sizeof v[0], "%d", bp_res_w);
  snprintf(v[1], sizeof v[1], "%d", bp_res_h);
  int changed = 0;
  for (int i = 0; i < 2; i++) {
    const char *cur = bp_prefs_has(K[i]) ? bp_prefs_get(K[i]) : NULL;
    if (cur && strcmp(cur, v[i])) { bp_prefs_set('I', K[i], v[i]); changed = 1; }
  }
  if (changed) {
    bp_prefs_commit();
    debugPrintf("[config] Unity's saved screen size updated to %s x %s\n", v[0], v[1]);
  }
}
