/* ---------------------------------------------------------------------------
 * battd_save_probe.c -- log the real Profile.Save password at runtime.
 *
 * WHY MEASURE INSTEAD OF SEARCH
 * Everything about the container is derived and verified (tools/battd_save.py):
 * PBKDF2-HMAC-SHA1 with 10 iterations, a 24-byte salt, AES-128-CBC with the IV
 * taken before the key, zlib via DotNetZip's ZlibStream, BOM'd UTF-8 JSON. The
 * three candidate password formulas are identified from the binary too. What is
 * missing is which one the file was written with and what appID went into it,
 * and a static sweep over millions of candidates did not find it.
 *
 * PasswordGenerator.GetPassword(int version) returns exactly the string that is
 * about to be handed to Rfc2898DeriveBytes. Log it once and the format is
 * solved. This is a DIAGNOSTIC: it changes no behaviour, logs the first few
 * calls, then goes quiet. BP_SAVE_PROBE 0 removes it.
 *
 * HOW IT CALLS THROUGH -- see battd_gp_trampoline.s for the full reasoning. The
 * short version: the function is patched ONCE at install, and the original body
 * is reached through a static trampoline that reproduces its (position-
 * independent) prologue. The earlier un-patch / call / re-patch approach had a
 * race no mutex can close, did blocking SD I/O inside the hook, and churned
 * virtual-address reservations on every call.
 *
 * NOTE: this writes the save password to debug.log in clear text. That is the
 * entire point, but it is worth knowing before sharing a log.
 *
 * MIT.
 * ------------------------------------------------------------------------- */
#include "config.h"

#ifndef BP_SAVE_PROBE
#error "config.h not included -- the probe would compile out silently"
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "bp_offsets.h"
#include "so_util.h"
#include "util.h"
#include "battd_save_probe.h"

/* Read by battd_gp_trampoline.s: the address of GetPassword's body, i.e. the
 * function start plus the four prologue words the trampoline reproduces.
 *
 * DEFINED UNCONDITIONALLY. The Makefile assembles every .s in source/ via a
 * wildcard, so battd_gp_trampoline.s is built even with BP_SAVE_PROBE 0 -- and it
 * references this symbol. Leaving the definition inside the #if made turning the
 * probe off a link error, which is the worst way for a feature switch to fail.
 * Zero here means the trampoline is never entered, because nothing hooks. */
uint64_t g_gp_body_target;

#if BP_SAVE_PROBE

/* PasswordGenerator.GetPassword(PasswordGenerator *this, int version,
 *                               MethodInfo *method) -> Il2CppString*        */
extern void *battd_gp_call_body(void *self, int version, void *method);

#define GP_PROLOGUE_WORDS 4

static int s_calls;   /* only read/written from the hook; see note below */

/* Il2CppString: Il2CppObject (klass, monitor) = 16 bytes, then int32 length,
 * then UTF-16 chars. */
typedef struct { char _hdr[0x10]; int32_t len; uint16_t chars[1]; } Il2CppStr;

/* Cap the logged length. Every candidate password is under 20 characters, so
 * this is generous -- and it keeps the two buffers below under a kilobyte.
 * They live on the GAME's thread, inside a managed call chain, which is not a
 * stack to take liberties with. */
#define GP_LOG_MAX 128

static void log_managed_string(const char *what, const void *managed) {
  const Il2CppStr *s = (const Il2CppStr *)managed;
  if (!s) { debugPrintf("[save-probe] %s = (null)\n", what); return; }
  if (s->len < 0 || s->len > 4096) {
    debugPrintf("[save-probe] %s = (implausible length %d -- not logged)\n",
                what, (int)s->len);
    return;
  }
  const int n = s->len < GP_LOG_MAX ? s->len : GP_LOG_MAX;
  if (n < s->len)
    debugPrintf("[save-probe] %s is %d chars; logging the first %d\n",
                what, (int)s->len, n);

  /* Printable form first: this is what gets pasted into --password. */
  char buf[GP_LOG_MAX + 1];
  int printable = 1;
  for (int i = 0; i < n; i++) {
    const uint16_t c = s->chars[i];
    if (c < 0x20 || c >= 0x7f) printable = 0;
    buf[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
  }
  buf[n] = 0;
  debugPrintf("[save-probe] %s = \"%s\"  (%d chars%s)\n", what, buf, n,
              printable ? "" : ", NOT all ASCII -- use the hex below");

  /* And the raw UTF-16 code units, always.
   *
   * The printable form substitutes '.' for anything outside ASCII, which would
   * quietly destroy the very answer this probe exists to capture. Every
   * candidate formula produces ASCII, so this should never differ -- but
   * "should never" is the assumption that has already cost this port a hardware
   * run. The hex costs a few bytes of log and makes the result recoverable
   * whatever it turns out to be.
   *
   * p is advanced by the CLAMPED return of snprintf, not its raw one: snprintf
   * returns the length it WOULD have written, so on truncation `p += snprintf`
   * walks past the end and the terminator below becomes an out-of-bounds
   * write. */
  char hex[GP_LOG_MAX * 5 + 1];
  size_t p = 0;
  for (int i = 0; i < n; i++) {
    const int w = snprintf(hex + p, sizeof hex - p, "%04x ", s->chars[i]);
    if (w <= 0 || (size_t)w >= sizeof hex - p) break;   /* truncated: stop */
    p += (size_t)w;
  }
  hex[p] = 0;
  debugPrintf("[save-probe] %s utf16 = %s\n", what, hex);
}

/* No lock. The only shared state is s_calls, and the worst a race can do is
 * log one extra line or skip one -- there is nothing here to corrupt, and a
 * mutex would only add a way to deadlock against the debug-log lock. */
static void *hk_GetPassword(void *self, int version, void *method) {
  void *result = battd_gp_call_body(self, version, method);
  const int n = ++s_calls;
  if (n <= 8) {
    debugPrintf("[save-probe] GetPassword(version=%d) call #%d\n", version, n);
    log_managed_string("password", result);
    debugPrintf("[save-probe] ^ decode with: python3 tools/battd_save.py decode "
                "Profile.Save out.json --password '<the string above>'\n");
  }
  return result;
}

int battd_save_probe_install(so_module *il2cpp) {
  if (!il2cpp || !il2cpp->load_virtbase) return 0;
  const uintptr_t addr = (uintptr_t)il2cpp->load_virtbase + BP_RVA_GetPassword;

  /* 1. The game's prologue must be the one the offsets were taken from. */
  for (int i = 0; i < GP_PROLOGUE_WORDS; i++) {
    const uint32_t w = *(volatile uint32_t *)(addr + 4u * i);
    if (w != BP_GUARD_GetPassword[i]) {
      debugPrintf("[save-probe] GUARD FAILED at PasswordGenerator::GetPassword "
                  "+0x%x (want %08x, found %08x) -- probe NOT installed\n",
                  i * 4, BP_GUARD_GetPassword[i], w);
      return 0;
    }
  }

  /* 2. And our trampoline must reproduce it EXACTLY. This is the check that
   *    makes copying a prologue safe rather than hopeful: if a future build
   *    changes those four words, step 1 already refused -- and if someone edits
   *    the .s without re-deriving, this refuses. Comparing the assembler's
   *    output against the game costs nothing and cannot drift. */
  const uint32_t *tramp = (const uint32_t *)(uintptr_t)&battd_gp_call_body;
  for (int i = 0; i < GP_PROLOGUE_WORDS; i++) {
    if (tramp[i] != BP_GUARD_GetPassword[i]) {
      debugPrintf("[save-probe] trampoline word %d is %08x but the game has "
                  "%08x -- battd_gp_trampoline.s is stale; probe NOT installed\n",
                  i, tramp[i], BP_GUARD_GetPassword[i]);
      return 0;
    }
  }

  g_gp_body_target = (uint64_t)(addr + 4u * GP_PROLOGUE_WORDS);
  hook_arm64(addr, (uintptr_t)&hk_GetPassword);
  debugPrintf("[save-probe] PasswordGenerator::GetPassword probed @ "
              "il2cpp+0x%06x (body at +0x%x) -- the save password will be "
              "logged on first use\n",
              (unsigned)BP_RVA_GetPassword, (unsigned)(GP_PROLOGUE_WORDS * 4));
  return 1;
}

#else   /* BP_SAVE_PROBE 0 */
int battd_save_probe_install(so_module *il2cpp) { (void)il2cpp; return 0; }
#endif
