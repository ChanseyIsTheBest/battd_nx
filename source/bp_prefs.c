/* ---------------------------------------------------------------------------
 * bp_prefs.c -- back UnityEngine.PlayerPrefs with our own store.
 *
 * WHY
 * ---
 * The game's saves and its in-run currency both go through PlayerPrefs. Traced
 * from the game's own storage layer down to the engine:
 *
 *     StorageUtility.Save
 *       -> AndroidPlayerPrefsStorage.Save          (chosen at build time; there
 *          is no runtime platform branch in StorageUtility..ctor)
 *         -> CustomPlayerPrefs.SetString
 *           -> UnityEngine.PlayerPrefs.SetString
 *           -> UnityEngine.PlayerPrefs.Save        (when isSaveImmediately)
 *
 * On Android the engine implements PlayerPrefs natively and persists it through
 * a JNI SharedPreferences object. unity_jni.c answers that surface, and across a
 * whole session with currency being collected it logged **zero** puts and zero
 * flushes. The engine side never arrived.
 *
 * Chasing why the native implementation stays silent is open-ended: it is inside
 * libunity, it depends on a Context and a real SharedPreferences instance, and
 * every step of it reports success. Replacing PlayerPrefs is smaller, entirely
 * visible, and removes the JNI round trip from the save path completely.
 *
 * These hooks store into the same KV that unity_jni.c already persists to
 * prefs.kv -- which now also fsdevCommitDevice()s (sec.51) -- so saves become
 * durable by construction rather than by hoping the engine's path works.
 *
 * WHICH OVERLOADS
 * ---------------
 * Taken from the dump, with the overloads distinguished:
 *
 *     SetInt(key,int)  GetInt(key,def)  SetFloat(key,float)  GetFloat(key,def)
 *     SetString(key,s) GetString(key,def) GetString(key)
 *     HasKey  DeleteKey  DeleteAll  Save
 *
 * `GetInt(key)` is NOT hooked and does not need to be: it is
 * `mov w1, wzr ; b GetInt(key,def)`, a thunk into the 2-arg form we do hook.
 * `GetString(key)` is a real body, so it is hooked separately.
 *
 * STRINGS
 * -------
 * Il2CppString is UTF-16. Conversion uses libil2cpp's own exports --
 * il2cpp_string_new / il2cpp_string_chars / il2cpp_string_length -- resolved
 * once at install. Building strings by hand would mean knowing the object
 * header layout and allocating GC memory ourselves; the runtime already
 * exports the correct way to do it.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_managed.h"
#include "config.h"
#include "so_util.h"
#include "unity_jni.h"
#include "util.h"

/* --- libil2cpp string helpers, resolved at install ----------------------- */
static void    *(*g_string_new)(const char *);
static uint16_t *(*g_string_chars)(void *);
static int32_t   (*g_string_length)(void *);

/* UTF-16 -> UTF-8. Keys and values here are ASCII in practice (key names and
 * serialised numbers/JSON), but a stray non-ASCII char must not corrupt the
 * store, so encode properly rather than truncating. */
static void s_to_utf8(void *str, char *out, size_t cap) {
  out[0] = 0;
  if (!str || !g_string_chars || !g_string_length) return;
  const uint16_t *c = g_string_chars(str);
  int32_t n = g_string_length(str);
  if (!c || n < 0) return;
  size_t w = 0;
  for (int32_t i = 0; i < n && w + 4 < cap; i++) {
    uint32_t u = c[i];
    /* Combine a surrogate pair so astral characters survive a round trip. */
    if (u >= 0xd800 && u <= 0xdbff && i + 1 < n &&
        c[i + 1] >= 0xdc00 && c[i + 1] <= 0xdfff) {
      u = 0x10000 + ((u - 0xd800) << 10) + (c[i + 1] - 0xdc00);
      i++;
    }
    if (u < 0x80) out[w++] = (char)u;
    else if (u < 0x800) { out[w++] = (char)(0xc0 | (u >> 6)); out[w++] = (char)(0x80 | (u & 0x3f)); }
    else if (u < 0x10000) {
      out[w++] = (char)(0xe0 | (u >> 12));
      out[w++] = (char)(0x80 | ((u >> 6) & 0x3f));
      out[w++] = (char)(0x80 | (u & 0x3f));
    } else {
      out[w++] = (char)(0xf0 | (u >> 18));
      out[w++] = (char)(0x80 | ((u >> 12) & 0x3f));
      out[w++] = (char)(0x80 | ((u >> 6) & 0x3f));
      out[w++] = (char)(0x80 | (u & 0x3f));
    }
  }
  out[w] = 0;
}

#define KEYMAX 256
#define VALMAX_INIT 65536     /* starting size; grown on demand, never truncates */

/* One heap buffer, grown to fit. These all run on the engine main thread.
 *
 * A FIXED ceiling here is a silent data-loss path: s_to_utf8 stops writing when
 * it runs out of room, so a save blob larger than the buffer would be stored
 * SHORT, with no error anywhere -- the same class of bug as the reader's old
 * char[2048], just on the other side of the file. Grow instead of truncating. */
static char  *g_val;
static size_t g_val_cap;

/* Worst case is 4 UTF-8 bytes per UTF-16 unit, plus a terminator. */
static int val_reserve(size_t utf16_units) {
  size_t need = utf16_units * 4u + 1u;
  if (need <= g_val_cap) return 1;
  size_t cap = g_val_cap ? g_val_cap : VALMAX_INIT;
  while (cap < need) cap *= 2;
  char *n = realloc(g_val, cap);
  if (!n) {
    debugPrintf("[prefs] realloc(%zu) for a value failed -- NOT storing it "
                "rather than storing a truncated one\n", cap);
    return 0;
  }
  g_val = n; g_val_cap = cap;
  return 1;
}

/* --- hooks --------------------------------------------------------------- */

static void pp_SetString(void *key, void *val) {
  char k[KEYMAX]; s_to_utf8(key, k, sizeof k);
  /* Size the buffer to THIS string before converting, so nothing is ever
   * silently shortened. Refusing to store is better than storing a truncated
   * save blob that then overwrites the good one on the next flush. */
  int units = (val && g_string_length) ? g_string_length(val) : 0;
  if (units < 0) units = 0;
  if (!val_reserve((size_t)units)) return;
  s_to_utf8(val, g_val, g_val_cap);
  bp_prefs_set('S', k, g_val);
  debugPrintf("[prefs] SetString '%s' = %u bytes%s\n", k, (unsigned)strlen(g_val),
              g_val[0] ? "" : " <EMPTY>");
}

static void *pp_GetString2(void *key, void *def) {
  char k[KEYMAX]; s_to_utf8(key, k, sizeof k);
  const char *v = bp_prefs_get(k);
  if (v) return g_string_new ? g_string_new(v) : def;
  return def;
}

static void *pp_GetString1(void *key) {
  char k[KEYMAX]; s_to_utf8(key, k, sizeof k);
  const char *v = bp_prefs_get(k);
  return g_string_new ? g_string_new(v ? v : "") : NULL;
}

static void pp_SetInt(void *key, int32_t v) {
  char k[KEYMAX], b[32]; s_to_utf8(key, k, sizeof k);
  snprintf(b, sizeof b, "%d", (int)v);
  bp_prefs_set('I', k, b);
}

static int32_t pp_GetInt(void *key, int32_t def) {
  char k[KEYMAX]; s_to_utf8(key, k, sizeof k);
  const char *v = bp_prefs_get(k);
  return v ? (int32_t)strtol(v, NULL, 10) : def;
}

static void pp_SetFloat(void *key, float v) {
  char k[KEYMAX], b[64]; s_to_utf8(key, k, sizeof k);
  /* %.9g round-trips a binary32 exactly, so a float survives save/load. */
  snprintf(b, sizeof b, "%.9g", (double)v);
  bp_prefs_set('F', k, b);
}

static float pp_GetFloat(void *key, float def) {
  char k[KEYMAX]; s_to_utf8(key, k, sizeof k);
  const char *v = bp_prefs_get(k);
  return v ? (float)strtod(v, NULL) : def;
}

static int32_t pp_HasKey(void *key) {
  char k[KEYMAX]; s_to_utf8(key, k, sizeof k);
  return bp_prefs_has(k);
}

static void pp_DeleteKey(void *key) {
  char k[KEYMAX]; s_to_utf8(key, k, sizeof k);
  bp_prefs_delete(k);
}

static void pp_DeleteAll(void) {
  /* Marks the wipe as deliberate, so prefs_flush's empty-store guard lets this
   * one through. Everything else that empties the store is a bug and is
   * refused. */
  debugPrintf("[prefs] DeleteAll requested by the game\n");
  bp_prefs_delete_all();
}

/* PlayerPrefs.Save() is HOT: CustomPlayerPrefs calls it on every set that passes
 * isSaveImmediately, which for a currency counter is several times a second.
 *
 * Writing the whole file and calling fsdevCommitDevice() that often would be
 * genuinely bad -- a commit is a filesystem-wide flush, not a cheap fsync, and
 * doing one per coin would stutter the game. So Save() only records intent, and
 * bp_prefs_tick() does the work at most once a second.
 *
 * Nothing is at risk from the delay: prefs_flush() writes the entire store, so a
 * later flush subsumes every earlier one, and bp_prefs_flush_now() runs on the
 * way out. */
static int g_save_pending;

static void pp_Save(void) { g_save_pending = 1; }

/* Call once per frame. Also flushes when the store is dirty but Save() was never
 * called -- some paths set without saving, and losing those would look exactly
 * like the bug this file exists to fix. */
void bp_prefs_tick(void) {
  static unsigned n;
  if (++n < 60) return;
  n = 0;
  if (!g_save_pending) { bp_prefs_commit(); return; }  /* no-op when clean */
  g_save_pending = 0;
  bp_prefs_commit();
}

/* Unconditional flush for shutdown/suspend. */
void bp_prefs_flush_now(void) { g_save_pending = 0; bp_prefs_commit(); }

/* Unity 2020.3 still stores PlayerPrefs through Android SharedPreferences over
 * JNI, which unity_jni.c already services (the Unity 6 managed-hook path
 * Bouncemasters needed does not apply). The flush/backup machinery below still
 * runs for whatever reaches this store. */
int bp_prefs_install(so_module *il2cpp) {
  (void)il2cpp;
  debugPrintf("[prefs] PlayerPrefs via JNI SharedPreferences (2020.3); no managed hooks\n");
  return 0;
}
