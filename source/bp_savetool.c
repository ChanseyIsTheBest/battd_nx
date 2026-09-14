/* ---------------------------------------------------------------------------
 * bp_savetool.c -- edit Profile.Save at boot from save.txt.
 *
 * Retargeted from bloonspop_nx, which had the same container but a different
 * password and a much shallower field layout.
 *
 * FORMAT (all of it verified against real saves)
 *     0x00  int32   format version = 1
 *     0x04  int32   record length  = 36
 *     0x08  ...     FileFormatV1: int32 SaveCount, 16-byte Guid,
 *                   int64 DateCreated, int64 DateModified (DateTime.ToBinary)
 *     0x2c  uint64  password version = 3
 *     0x34  24      salt
 *     0x4c  ...     AES-128-CBC( zlib( UTF-8 JSON with BOM ) ), PKCS7
 *
 * Key material is Rfc2898DeriveBytes(password, salt, 10): first GetBytes(16)
 * is the IV, the second is the key -- read out of Constants.GetAes, not
 * assumed. The password is a LITERAL, recovered on hardware by
 * battd_save_probe.c; see password_for().
 *
 * HOW IT EDITS
 * Values are spliced textually into the decoded JSON at a DOTTED PATH, which is
 * the one real change from the Bloons Pop version: almost nothing worth editing
 * here is top-level. Descending by path also keeps the edit precise -- a
 * "money" nested inside some reward entry is never mistaken for
 * resources.money, because the walker only ever looks inside the object the
 * path named.
 *
 * A path the save does not contain is logged and skipped. Nothing is inserted,
 * ever: this can change values the game already writes, not invent fields it
 * would then fail to parse.
 *
 * SAFETY, in the order it happens
 *   1. decode, and bail if the password does not fit (old save, or a game
 *      update that changed the format)
 *   2. apply only the uncommented settings; if nothing actually differs, stop
 *      without writing
 *   3. re-encode, then DECODE THE RESULT AND COMPARE IT byte for byte with
 *      what we meant to write -- a save is not replaced on the strength of an
 *      encoder that was never checked
 *   4. keep the untouched original once as Profile.Save.orig
 *   5. write to a temp file and rename, then commit the SD card
 * MIT.
 * ------------------------------------------------------------------------- */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>
#include "config.h"
#include "bp_savetool.h"
#include "util.h"

const char *bp_game_root(void);

#define MAX_SAVE (4u << 20)

/* ---------------------------------------------------------------------------
 * The editable surface.
 *
 * Bloons Pop's version of this file could substitute at TOP-LEVEL keys only,
 * because that is where its values lived. Almost nothing interesting in BATTD
 * is top-level -- money is resources.money, a hero's level is
 * inventory.towers.<Name>.level -- so this one walks a dotted path instead.
 *
 * KIND: I = integer, B = boolean, S = quoted string.
 * A field is written only if save.txt has it uncommented; a path the save does
 * not contain is logged and skipped, never inserted. ------------------------ */
typedef enum { F_INT, F_BOOL, F_STR } FKind;
typedef struct { const char *key; const char *path; FKind kind; } Field;

static const Field FIELDS[] = {
  /* --- currencies ------------------------------------------------------- */
  { "money",                  "resources.money",                     F_INT  },
  { "gems",                   "resources.gems",                      F_INT  },
  { "xp",                     "resources.xp",                        F_INT  },
  { "crystals",               "resources.crystals",                  F_INT  },
  { "tower_xp",               "resources.genericTowerXpCurrency",    F_INT  },
  { "wish_orb_shards",        "resources.wishOrbs.numShards",        F_INT  },
  /* --- progress --------------------------------------------------------- */
  { "rank",                   "rank",                                F_INT  },
  { "seen_tips",              "progress.seenTips",                   F_INT  },
  { "daily_reward_index",     "progress.dailyRewardIndex",           F_INT  },
  { "tutorial_progress",      "progress.tutorialProgress",           F_STR  },
  { "unlocked_mars",          "progress.hasUnlockedMars",            F_BOOL },
  { "seen_mars_popup",        "progress.seenMarsFirstTimePopup",     F_BOOL },
  { "launched_after_tutorial","appLaunchedAfterTutorial",            F_BOOL },
  /* --- stats and settings ----------------------------------------------- */
  { "played_games",           "stats.playedGames",                   F_INT  },
  { "used_fast_forward",      "stats.usedFastForward",               F_BOOL },
  { "round_auto_play",        "gameSettings.roundAutoPlay",          F_BOOL },
};
#define N_FIELDS ((int)(sizeof FIELDS / sizeof *FIELDS))

/* -1 = not set: leave the game's value alone. */
static long long g_val[N_FIELDS];
static char      g_str[N_FIELDS][64];

/* tower.<Name> -> inventory.towers.<Name>.level
 * adventure.<Name> -> adventureData.adventureProgress.<Name>.isUnlocked
 * Both are open-ended: the name is whatever the save already has, so new heroes
 * and adventures from a game update work without touching this file. */
#define N_DYN 64
typedef struct { char name[48]; long long v; } Dyn;
static Dyn g_tower[N_DYN];   static int g_ntower;
static Dyn g_adv[N_DYN];     static int g_nadv;
static int g_any;

/* ------------------------------------------------------------------ */
/* save.txt                                                            */
/* ------------------------------------------------------------------ */
static void write_template(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { debugPrintf("[save] could not write %s\n", path); return; }
  fputs(
"# save.txt -- Bloons Adventure Time TD save editing.\n"
"#\n"
"# Every line is commented out. Remove the '#' from one and give it a value; it\n"
"# is applied to Profile.Save at EVERY launch, for as long as the line stays\n"
"# uncommented. The original is kept once as Profile.Save.orig.\n"
"#\n"
"# A field the save does not contain is reported in debug.log and skipped --\n"
"# nothing is ever inserted, so this cannot invent data the game will choke on.\n"
"\n"
"# --- currencies ------------------------------------------------------\n"
"#money = 999999\n"
"#gems = 9999\n"
"#xp = 99999\n"
"#crystals = 9999\n"
"#tower_xp = 99999\n"
"#wish_orb_shards = 999\n"
"\n"
"# --- progress --------------------------------------------------------\n"
"#rank = 50\n"
"#seen_tips = 1\n"
"#daily_reward_index = 0\n"
"# tutorial_progress is an enum name. 'Complete' skips the tutorial;\n"
"# 'InitialMission' is where a new profile starts.\n"
"#tutorial_progress = Complete\n"
"#unlocked_mars = true\n"
"#seen_mars_popup = true\n"
"#launched_after_tutorial = true\n"
"\n"
"# --- stats and settings ----------------------------------------------\n"
"#played_games = 100\n"
"#used_fast_forward = true\n"
"#round_auto_play = true\n"
"\n"
"# --- heroes ----------------------------------------------------------\n"
"# tower.<Name> sets that hero's level. The name is whatever the save already\n"
"# holds, so heroes added by a game update work without changing the port.\n"
"# A new profile starts with Finn, Jake and Max.\n"
"#tower.Finn = 20\n"
"#tower.Jake = 20\n"
"#tower.Max = 20\n"
"\n"
"# --- adventures ------------------------------------------------------\n"
"# adventure.<Name> = true unlocks it. Names in a fresh save:\n"
"#   Tutorial, AppleThief, WinterIsComing, CandyCornered,\n"
"#   MarcelineTheVampireHunter, WizardBattle, LemonGrabbed, BurningRubber,\n"
"#   PirateInPeril, TroubleInLumpySpace\n"
"# This only flips the unlock flag; it does not fill in map completion.\n"
"#adventure.AppleThief = true\n"
"#adventure.WinterIsComing = true\n",
    f);
  fclose(f);
  debugPrintf("[save] wrote %s (every option commented out)\n", path);
}

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
/* Options added after a player already has a save.txt. Each block is appended
 * once, only if the file does not already mention the key -- so a new setting
 * shows up without anyone deleting their file, and editing a comment back in
 * does not duplicate it. `keys[1]` is an optional alias.
 *
 * Everything currently in the template shipped together, so this is empty. Add
 * a row here, not just to write_template(), when a new setting is introduced:
 * write_template only runs for a file that does not exist yet. */
static const struct { const char *keys[2]; const char *block; } ADDED[] = {
  { { NULL, NULL }, NULL },   /* keep the array non-empty; skipped below */
};

static void append_missing(const char *path) {
  char text[16384];
  FILE *f = fopen(path, "r");
  if (!f) return;
  const size_t n = fread(text, 1, sizeof text - 1, f);
  fclose(f);
  text[n] = 0;
  for (size_t i = 0; i < sizeof ADDED / sizeof *ADDED; i++) {
    if (!ADDED[i].keys[0] || !ADDED[i].block) continue;      /* placeholder row */
    if (mentions_key(text, ADDED[i].keys[0]) || mentions_key(text, ADDED[i].keys[1])) continue;
    if (!(f = fopen(path, "a"))) return;
    fprintf(f, "%s\n%s", (n && text[n - 1] != '\n') ? "\n" : "", ADDED[i].block);
    fclose(f);
    debugPrintf("[save] added the new \"%s\" option to %s\n", ADDED[i].keys[0], path);
  }
}

static void trim(char *s) {
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}
static int parse_count(const char *key, const char *v, long long *out) {
  char *end;
  long long x = strtoll(v, &end, 10);
  if (end == v || *end) { debugPrintf("[save] %s: \"%s\" is not a number -- ignored\n", key, v); return 0; }
  if (x < 0) x = 0;
  if (x > 2147483647LL) x = 2147483647LL;               /* the fields are C# int */
  *out = x;
  return 1;
}
static int parse_bool(const char *key, const char *v, int *out) {
  if (!strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "1"))  { *out = 1; return 1; }
  if (!strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "0")) { *out = 0; return 1; }
  debugPrintf("[save] %s: \"%s\" is not on/off -- ignored\n", key, v);
  return 0;
}

static int dyn_add(Dyn *tab, int *n, const char *name, const char *val,
                   const char *what, int as_bool) {
  if (*n >= N_DYN) { debugPrintf("[save] too many %s entries -- \"%s\" ignored\n", what, name); return 0; }
  long long v = -1;
  if (as_bool) { int b = -1; if (!parse_bool(name, val, &b)) return 0; v = b; }
  else if (!parse_count(name, val, &v)) return 0;
  snprintf(tab[*n].name, sizeof tab[*n].name, "%s", name);
  tab[*n].v = v;
  (*n)++;
  return 1;
}

static int read_config(void) {
  char path[640], line[256];
  for (int i = 0; i < N_FIELDS; i++) { g_val[i] = -1; g_str[i][0] = 0; }
  g_ntower = g_nadv = 0;
  snprintf(path, sizeof path, "%s/save.txt", bp_game_root());
  FILE *f = fopen(path, "r");
  if (!f) { write_template(path); return 0; }
  fclose(f);
  append_missing(path);
  if (!(f = fopen(path, "r"))) return 0;
  while (fgets(line, sizeof line, f)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;                                  /* '#' starts a comment anywhere */
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *key = line, *val = eq + 1;
    trim(key); trim(val);
    if (!*key || !*val) continue;

    if (!strncmp(key, "tower.", 6)) {
      g_any |= dyn_add(g_tower, &g_ntower, key + 6, val, "tower", 0);
      continue;
    }
    if (!strncmp(key, "adventure.", 10)) {
      g_any |= dyn_add(g_adv, &g_nadv, key + 10, val, "adventure", 1);
      continue;
    }
    int i;
    for (i = 0; i < N_FIELDS; i++) if (!strcmp(key, FIELDS[i].key)) break;
    if (i == N_FIELDS) { debugPrintf("[save] unknown setting \"%s\" -- ignored\n", key); continue; }
    if (FIELDS[i].kind == F_STR) {
      snprintf(g_str[i], sizeof g_str[i], "%s", val);
      g_any = 1;
    } else if (FIELDS[i].kind == F_BOOL) {
      int b = -1;
      if (parse_bool(key, val, &b)) { g_val[i] = b; g_any = 1; }
    } else {
      g_any |= parse_count(key, val, &g_val[i]);
    }
  }
  fclose(f);
  return g_any;
}

/* ------------------------------------------------------------------ */
/* crypto: PBKDF2-HMAC-SHA1, AES-128-CBC (libnx)                       */
/* ------------------------------------------------------------------ */
/* Recovered on hardware by source/battd_save_probe.c, which logs what
 * PasswordGenerator.GetPassword actually returns. It is a LITERAL, not derived
 * from the app ID -- which is why a static search over millions of candidates
 * never found it. Version 3 is what this build writes; the others are unknown
 * and refused rather than guessed, so an old save is left alone instead of
 * being mangled. */
static const char *password_for(unsigned long long version) {
  return version == 3 ? "$LXvKp90n$Cc0GTX2nY5" : NULL;
}
static void pbkdf2_sha1(const char *pw, const u8 *salt, size_t slen, int iters, u8 *out, size_t outlen) {
  u8 in[64], u[20], v[20], t[20];
  const size_t pl = strlen(pw);
  u32 block = 1;
  for (size_t done = 0; done < outlen; block++) {
    memcpy(in, salt, slen);
    in[slen] = (u8)(block >> 24); in[slen + 1] = (u8)(block >> 16);
    in[slen + 2] = (u8)(block >> 8); in[slen + 3] = (u8)block;
    hmacSha1CalculateMac(u, pw, pl, in, slen + 4);
    memcpy(t, u, 20);
    for (int j = 1; j < iters; j++) {
      hmacSha1CalculateMac(v, pw, pl, u, 20);
      memcpy(u, v, 20);
      for (int k = 0; k < 20; k++) t[k] ^= u[k];
    }
    const size_t n = outlen - done < 20 ? outlen - done : 20;
    memcpy(out + done, t, n);
    done += n;
  }
}
static void derive(unsigned long long version, const u8 salt[24], u8 key[16], u8 iv[16]) {
  u8 dk[32];
  pbkdf2_sha1(password_for(version), salt, 24, 10, dk, 32);
  memcpy(iv, dk, 16);                                     /* first GetBytes(16) -> IV  */
  memcpy(key, dk + 16, 16);                               /* second GetBytes(16) -> Key */
}

/* ------------------------------------------------------------------ */
/* container                                                           */
/* ------------------------------------------------------------------ */
/* Decode a Profile.Save. On success *hdr_len covers version+length+record,
 * *json is malloc'd (NUL-terminated, BOM kept) and 1 is returned. */
static int save_decode(const u8 *b, size_t n, size_t *hdr_len, unsigned long long *pwver,
                       char **json, size_t *jlen, const char **why) {
  int32_t ver, rlen;
  if (n < 8) { *why = "too short"; return 0; }
  memcpy(&ver, b, 4); memcpy(&rlen, b + 4, 4);
  if (ver != 1 || rlen < 0 || rlen > 256 || (size_t)(8 + rlen + 32) > n) { *why = "unexpected header"; return 0; }
  const size_t h = 8 + (size_t)rlen;
  unsigned long long pv;
  memcpy(&pv, b + h, 8);
  if (!password_for(pv)) { *why = "unknown password version"; return 0; }
  const u8 *salt = b + h + 8, *ct = b + h + 32;
  const size_t clen = n - h - 32;
  if (!clen || clen % 16) { *why = "ciphertext is not whole AES blocks"; return 0; }
  u8 key[16], iv[16];
  derive(pv, salt, key, iv);
  u8 *pt = malloc(clen);
  if (!pt) { *why = "out of memory"; return 0; }
  Aes128CbcContext ctx;
  aes128CbcContextCreate(&ctx, key, iv, false);
  aes128CbcDecrypt(&ctx, pt, ct, clen);
  const u8 pad = pt[clen - 1];
  int ok = pad >= 1 && pad <= 16;
  for (int i = 1; ok && i <= pad; i++) ok = pt[clen - i] == pad;
  if (!ok) { free(pt); *why = "bad padding (wrong key?)"; return 0; }
  z_stream zs; memset(&zs, 0, sizeof zs);
  if (inflateInit(&zs) != Z_OK) { free(pt); *why = "zlib init"; return 0; }
  size_t cap = 1u << 16, len = 0;
  char *out = malloc(cap + 1);
  zs.next_in = pt; zs.avail_in = (uInt)(clen - pad);
  int zr = Z_OK;
  while (out && zr == Z_OK) {
    if (len == cap) {
      if (cap >= MAX_SAVE * 8u) break;
      cap *= 2;
      char *nb = realloc(out, cap + 1);
      if (!nb) { free(out); out = NULL; break; }
      out = nb;
    }
    zs.next_out = (Bytef *)out + len; zs.avail_out = (uInt)(cap - len);
    zr = inflate(&zs, Z_NO_FLUSH);
    len = cap - zs.avail_out;
  }
  inflateEnd(&zs);
  free(pt);
  if (!out || zr != Z_STREAM_END) { free(out); *why = "zlib stream"; return 0; }
  out[len] = 0;
  *hdr_len = h; *pwver = pv; *json = out; *jlen = len;
  return 1;
}

static int save_encode(const u8 *hdr, size_t h, unsigned long long pv, const char *json, size_t jlen,
                       u8 **blob, size_t *blen) {
  uLongf zcap = compressBound((uLong)jlen);
  u8 *z = malloc(zcap + 16);
  if (!z || compress2(z, &zcap, (const Bytef *)json, (uLong)jlen, Z_DEFAULT_COMPRESSION) != Z_OK) { free(z); return 0; }
  const size_t pad = 16 - (zcap % 16);
  memset(z + zcap, (int)pad, pad);
  const size_t clen = zcap + pad;
  const size_t n = h + 32 + clen;
  u8 *b = malloc(n);
  if (!b) { free(z); return 0; }
  memcpy(b, hdr, h);
  if (h == 44) {                                          /* FileFormatV1: what a real save does */
    int32_t count; memcpy(&count, b + 8, 4); count++; memcpy(b + 8, &count, 4);
    const unsigned long long ticks = (unsigned long long)time(NULL) * 10000000ULL + 621355968000000000ULL;
    const unsigned long long modified = ticks | (1ULL << 62);          /* DateTimeKind.Utc */
    memcpy(b + 36, &modified, 8);
  }
  memcpy(b + h, &pv, 8);
  u8 *salt = b + h + 8, key[16], iv[16];
  randomGet(salt, 24);
  derive(pv, salt, key, iv);
  Aes128CbcContext ctx;
  aes128CbcContextCreate(&ctx, key, iv, true);
  aes128CbcEncrypt(&ctx, b + h + 32, z, clen);
  free(z);
  *blob = b; *blen = n;
  return 1;
}

/* ------------------------------------------------------------------ */
/* JSON: top-level keys only                                           */
/* ------------------------------------------------------------------ */
static size_t skip_string(const char *j, size_t n, size_t p) {     /* p at the opening quote */
  for (p++; p < n; p++) {
    if (j[p] == '\\') { p++; continue; }
    if (j[p] == '"') return p + 1;
  }
  return n;
}
static size_t skip_value(const char *j, size_t n, size_t p) {
  if (p >= n) return n;
  if (j[p] == '"') return skip_string(j, n, p);
  if (j[p] == '{' || j[p] == '[') {
    int depth = 0;
    for (; p < n; p++) {
      if (j[p] == '"') { p = skip_string(j, n, p) - 1; continue; }
      if (j[p] == '{' || j[p] == '[') depth++;
      else if ((j[p] == '}' || j[p] == ']') && --depth == 0) return p + 1;
    }
    return n;
  }
  while (p < n && j[p] != ',' && j[p] != '}' && j[p] != ']' && !isspace((unsigned char)j[p])) p++;
  return p;
}
/* Value extent of `key` inside the object whose '{' is at j[os]. */
static int obj_value(const char *j, size_t n, size_t os, const char *key,
                     size_t *vs, size_t *ve) {
  if (os >= n || j[os] != '{') return 0;
  const size_t kl = strlen(key);
  size_t p = os + 1;
  for (;;) {
    while (p < n && (isspace((unsigned char)j[p]) || j[p] == ',')) p++;
    if (p >= n || j[p] == '}') return 0;
    if (j[p] != '"') return 0;
    const size_t ks = p + 1, ke = skip_string(j, n, p) - 1;
    p = ke + 1;
    while (p < n && isspace((unsigned char)j[p])) p++;
    if (p >= n || j[p] != ':') return 0;
    p++;
    while (p < n && isspace((unsigned char)j[p])) p++;
    const size_t v0 = p, v1 = skip_value(j, n, p);
    if (ke - ks == kl && !memcmp(j + ks, key, kl)) { *vs = v0; *ve = v1; return 1; }
    p = v1;
  }
}

/* Dotted path: "resources.money", "inventory.towers.Finn.level". Descends one
 * object per segment; returns 0 the moment a segment is absent, so a save from
 * a different game version is skipped rather than half-edited. */
static int path_value(const char *j, size_t n, const char *path,
                      size_t *vs, size_t *ve) {
  size_t os = 0;
  while (os < n && j[os] != '{') os++;                     /* skip the BOM */
  if (os >= n) return 0;
  char seg[64];
  const char *p = path;
  for (;;) {
    const char *dot = strchr(p, '.');
    const size_t l = dot ? (size_t)(dot - p) : strlen(p);
    if (l == 0 || l >= sizeof seg) return 0;
    memcpy(seg, p, l); seg[l] = 0;
    size_t s, e;
    if (!obj_value(j, n, os, seg, &s, &e)) return 0;
    if (!dot) { *vs = s; *ve = e; return 1; }
    os = s;                                                /* descend */
    p = dot + 1;
  }
}
static int splice(char **j, size_t *n, size_t s, size_t e, const char *rep) {
  const size_t rl = strlen(rep), nn = *n - (e - s) + rl;
  char *b = malloc(nn + 1);
  if (!b) return 0;
  memcpy(b, *j, s); memcpy(b + s, rep, rl); memcpy(b + s + rl, *j + e, *n - e);
  b[nn] = 0;
  free(*j); *j = b; *n = nn;
  return 1;
}

static char g_changes[1024];
static void note(const char *fmt, const char *field, const char *from, const char *to) {
  size_t l = strlen(g_changes);
  snprintf(g_changes + l, sizeof g_changes - l, fmt, l ? ", " : "", field, from, to);
}
static void set_int(char **j, size_t *n, const char *path, long long v) {
  size_t s, e;
  if (v < 0) return;
  if (!path_value(*j, *n, path, &s, &e)) {
    debugPrintf("[save] the save has no \"%s\" -- left alone\n", path); return;
  }
  char old[48], rep[32];
  snprintf(old, sizeof old, "%.*s", (int)(e - s), *j + s);
  snprintf(rep, sizeof rep, "%lld", v);
  if (strcmp(old, rep) && splice(j, n, s, e, rep)) note("%s%s %s->%s", path, old, rep);
}
static void set_bool(char **j, size_t *n, const char *path, int v) {
  size_t s, e;
  if (v < 0) return;
  if (!path_value(*j, *n, path, &s, &e)) {
    debugPrintf("[save] the save has no \"%s\" -- left alone\n", path); return;
  }
  char old[16];
  snprintf(old, sizeof old, "%.*s", (int)(e - s), *j + s);
  const char *rep = v ? "true" : "false";
  if (strcmp(old, rep) && splice(j, n, s, e, rep)) note("%s%s %s->%s", path, old, rep);
}
/* Strings are spliced WITH their quotes, so the replacement is a valid JSON
 * value and not a fragment. Anything needing an escape is refused rather than
 * written half-quoted -- the fields this reaches are enum names. */
static void set_str(char **j, size_t *n, const char *path, const char *v) {
  size_t s, e;
  if (!v || !*v) return;
  if (strpbrk(v, "\"\\")) {
    debugPrintf("[save] %s: value contains a quote or backslash -- refused\n", path);
    return;
  }
  if (!path_value(*j, *n, path, &s, &e)) {
    debugPrintf("[save] the save has no \"%s\" -- left alone\n", path); return;
  }
  char old[80], rep[80];
  snprintf(old, sizeof old, "%.*s", (int)(e - s), *j + s);
  snprintf(rep, sizeof rep, "\"%s\"", v);
  if (strcmp(old, rep) && splice(j, n, s, e, rep)) note("%s%s %s->%s", path, old, rep);
}

static u8 *read_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  u8 *b = (sz > 0 && (unsigned long)sz <= MAX_SAVE) ? malloc((size_t)sz) : NULL;
  if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
  fclose(f);
  *n = b ? (size_t)sz : 0;
  return b;
}
static int write_file(const char *path, const u8 *b, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  const int ok = fwrite(b, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

static void patch_save(const char *path) {
  size_t n = 0, h = 0, jl = 0;
  unsigned long long pv = 0;
  char *json = NULL;
  const char *why = "";
  u8 *orig = read_file(path, &n);
  if (!orig) { debugPrintf("[save] cannot read %s\n", path); return; }
  if (!save_decode(orig, n, &h, &pv, &json, &jl, &why)) {
    debugPrintf("[save] %s: not decodable (%s) -- left alone\n", path, why);
    free(orig);
    return;
  }
  g_changes[0] = 0;
  for (int i = 0; i < N_FIELDS; i++) {
    switch (FIELDS[i].kind) {
      case F_INT:  set_int(&json, &jl, FIELDS[i].path, g_val[i]);        break;
      case F_BOOL: set_bool(&json, &jl, FIELDS[i].path, (int)g_val[i]);  break;
      case F_STR:  set_str(&json, &jl, FIELDS[i].path, g_str[i]);        break;
    }
  }
  for (int i = 0; i < g_ntower; i++) {
    char p[128];
    snprintf(p, sizeof p, "inventory.towers.%s.level", g_tower[i].name);
    set_int(&json, &jl, p, g_tower[i].v);
  }
  for (int i = 0; i < g_nadv; i++) {
    char p[160];
    snprintf(p, sizeof p, "adventureData.adventureProgress.%s.isUnlocked", g_adv[i].name);
    set_bool(&json, &jl, p, (int)g_adv[i].v);
  }
  if (!g_changes[0]) {
    debugPrintf("[save] %s already matches save.txt\n", path);
    free(json); free(orig);
    return;
  }
  u8 *blob = NULL; size_t bl = 0;
  if (!save_encode(orig, h, pv, json, jl, &blob, &bl)) {
    debugPrintf("[save] %s: re-encoding failed -- left alone\n", path);
    free(json); free(orig);
    return;
  }
  size_t h2, jl2; unsigned long long pv2; char *check = NULL;         /* verify before writing */
  if (!save_decode(blob, bl, &h2, &pv2, &check, &jl2, &why) || jl2 != jl || memcmp(check, json, jl)) {
    debugPrintf("[save] %s: verification of the new save FAILED (%s) -- left alone\n", path, why);
    free(check); free(blob); free(json); free(orig);
    return;
  }
  free(check);
  char aux[700];
  struct stat st;
  snprintf(aux, sizeof aux, "%s.orig", path);
  if (stat(aux, &st) != 0) {
    if (!write_file(aux, orig, n)) {
      debugPrintf("[save] could not write the backup %s -- not editing without it\n", aux);
      free(blob); free(json); free(orig);
      return;
    }
    debugPrintf("[save] original kept as %s\n", aux);
  }
  snprintf(aux, sizeof aux, "%s.tmp", path);
  int ok = write_file(aux, blob, bl);
  if (ok) {
    remove(path);
    ok = rename(aux, path) == 0 || write_file(path, blob, bl);
    remove(aux);
  }
  fsdevCommitDevice("sdmc");
  debugPrintf("[save] %s %s: %s\n", path, ok ? "updated" : "WRITE FAILED", g_changes);
  free(blob); free(json); free(orig);
}

static int find_saves(const char *dir, int depth) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  int found = 0;
  struct dirent *e;
  char p[700];
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (depth < 6 && strcmp(e->d_name, "UnityCache")) found += find_saves(p, depth + 1);
    } else if (!strcmp(e->d_name, "Profile.Save")) {
      patch_save(p);
      found++;
    }
  }
  closedir(d);
  return found;
}

void bp_savetool_run(void) {
  if (!read_config()) return;                             /* nothing uncommented */
  char files[640];
  snprintf(files, sizeof files, "%s/files", bp_game_root());
  if (!find_saves(files, 0))
    debugPrintf("[save] save.txt has settings, but there is no Profile.Save under %s yet "
                "(it appears after the first play session)\n", files);
}
