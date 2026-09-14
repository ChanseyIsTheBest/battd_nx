/* ---------------------------------------------------------------------------
 * bp_net.c -- internet for Bloons Pop, adopted from acpc_nx's approach.
 *
 * Three things have to be true for UnityWebRequest (libcurl + unitytls inside
 * libunity) to download game content:
 *   1. the BSD service is up with enough sessions for curl's connections;
 *   2. the socket calls speak bionic's ABI (bp_net_shim.c);
 *   3. TLS can verify servers. unitytls asks Android's KeyStore for root
 *      certificates over JNI and gets nothing here, so every HTTPS handshake
 *      would fail. acpc_nx's fix: append a real CA bundle (data/cacerts.pem,
 *      embedded by the Makefile) to unitytls' default CA list through its own
 *      API. The four functions are located for THIS build in bp_offsets.h.
 * Plus Application.internetReachability, which the game checks before it
 * tries: bp_patches.c redirects DVM::GetInternetReachability here.
 * MIT.
 * ------------------------------------------------------------------------- */
#include <stdio.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bp_managed.h"
#include "bp_net.h"
#include "bp_offsets.h"
#include "config.h"
#include "util.h"
#include "cacerts_pem.h"

static int s_sock_ok, s_nifm_ok;

/* ---- offline once the content is cached (maintainer's request) ------------------
 * When every AssetBundle in bp_cache_manifest.h is already in the Unity cache, the
 * game has nothing left to download: boot with the internet OFF, so it goes straight
 * to its cached content instead of re-checking and retrying. Keyed on content hash:
 * when Ninja Kiwi ships changed bundles the hashes no longer match, the check fails
 * and the game stays online to fetch them. <root>/force_online forces online. */
#include "bp_cache_manifest.h"
#include <sys/stat.h>
const char *bp_game_root(void);
static int s_offline;
int bp_net_is_offline(void) { return s_offline; }
static int offline_decision(void) {
#if BP_OFFLINE_WHEN_CACHED
  char p[700];
  struct stat st;
  const char *root = bp_game_root();
  snprintf(p, sizeof p, "%s/force_online", root);
  if (stat(p, &st) == 0) {
    debugPrintf("[net] %s present: staying online\n", p);
    return 0;
  }
  const int total = (int)(sizeof k_bp_cache_manifest / sizeof k_bp_cache_manifest[0]);
  int have = 0, first_missing = -1;
  for (int i = 0; i < total; i++) {
    snprintf(p, sizeof p, "%s/files/UnityCache/Shared/%s/%s/__data", root, k_bp_cache_manifest[i].id, k_bp_cache_manifest[i].hash);
    int ok = stat(p, &st) == 0 && st.st_size > 0;
    if (ok) {
      snprintf(p, sizeof p, "%s/files/UnityCache/Shared/%s/%s/__info", root, k_bp_cache_manifest[i].id, k_bp_cache_manifest[i].hash);
      ok = stat(p, &st) == 0;
    }
    if (ok) have++; else if (first_missing < 0) first_missing = i;
  }
  if (have != total) {
    /* Still downloading. config.txt "online" is deliberately ignored here: the
     * first run has to fetch the content, and a setting that could block that
     * would present as a game that never finishes loading. */
    debugPrintf("[net] cache: %d of %d bundles present (first missing %s/%s) -> online%s\n",
                have, total,
                k_bp_cache_manifest[first_missing].id, k_bp_cache_manifest[first_missing].hash,
                bp_allow_online ? "" : " (config.txt online=false ignored until the download finishes)");
    return 0;
  }
  if (bp_allow_online) {
    debugPrintf("[net] cache complete (%d bundles) but config.txt has online=true "
                "-> staying connected\n", total);
    return 0;
  }
  debugPrintf("[net] OFFLINE: all %d cached bundles present and config.txt has "
              "online=false -> internet disabled (set online=true, or create "
              "%s/force_online, to download updates)\n", total, root);
  return 1;
#endif
  return 0;
}

void bp_net_init(void) {
  SocketInitConfig cfg = *socketGetDefaultInitConfig();
  cfg.num_bsd_sessions = BP_NET_BSD_SESSIONS;
#ifdef BP_NET_SB_EFFICIENCY
  cfg.sb_efficiency = BP_NET_SB_EFFICIENCY;
#endif
  Result rc = socketInitialize(&cfg);
  const int custom = R_SUCCEEDED(rc);
  if (!custom) rc = socketInitializeDefault();
  s_sock_ok = R_SUCCEEDED(rc);
  if (s_sock_ok)
    debugPrintf("[net] socket pool: %s (sb_efficiency %u, tcp buffers %u/%u KB, max %u/%u KB)\n",
                custom ? "custom config" : "DEFAULT config (custom refused)",
                (unsigned)(custom ? cfg.sb_efficiency : socketGetDefaultInitConfig()->sb_efficiency),
                (unsigned)(cfg.tcp_tx_buf_size >> 10), (unsigned)(cfg.tcp_rx_buf_size >> 10),
                (unsigned)(cfg.tcp_tx_buf_max_size >> 10), (unsigned)(cfg.tcp_rx_buf_max_size >> 10));
  Result nr = nifmInitialize(NifmServiceType_User);
  s_nifm_ok = R_SUCCEEDED(nr);
  debugPrintf("[net] BSD sockets %s (0x%x), nifm %s (0x%x)\n",
              s_sock_ok ? "up" : "FAILED", rc, s_nifm_ok ? "up" : "unavailable", nr);
  s_offline = offline_decision();
  if (s_sock_ok)
    debugPrintf("[net] internet %s at boot\n", bp_net_online() ? "CONNECTED" : "not connected");
}

int bp_net_online(void) {
  if (!s_sock_ok) return 0;
  if (!s_nifm_ok) return 1;               /* cannot ask: let the request decide */
  static u64 s_last;
  static int s_state = -1;
  u64 now = armGetSystemTick();
  if (s_state >= 0 && armTicksToNs(now - s_last) < 1000000000ull) return s_state;
  NifmInternetConnectionType type;
  u32 strength = 0;
  NifmInternetConnectionStatus st;
  Result rc = nifmGetInternetConnectionStatus(&type, &strength, &st);
  s_state = R_SUCCEEDED(rc) && st == NifmInternetConnectionStatus_Connected;
  s_last = now;
  return s_state;
}

/* NetworkReachability: 0 NotReachable, 1 ViaCarrierDataNetwork, 2 ViaLocalAreaNetwork */
int bp_net_reachability(void) { return (!s_offline && bp_net_online()) ? 2 : 0; }

typedef struct { uint32_t magic; uint32_t code; uint64_t reserved; } BpTlsErr;
typedef BpTlsErr (*tls_err_create_fn)(void);
typedef void *(*tls_ca_enter_fn)(BpTlsErr *err);
typedef void (*tls_ca_exit_fn)(void *list, BpTlsErr *err);
typedef void (*tls_append_pem_fn)(void *list, const uint8_t *pem, size_t len, BpTlsErr *err);
typedef void (*tls_append_der_fn)(void *list, const uint8_t *der, size_t len, BpTlsErr *err);
typedef uint64_t (*tls_list_get_x509_fn)(uint64_t listref, size_t index, BpTlsErr *err);

static int b64_val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
static size_t b64_decode(const char *s, const char *e, uint8_t *out, size_t cap) {
  uint32_t acc = 0; int bits = 0; size_t n = 0;
  for (; s < e && *s != '='; s++) {
    int v = b64_val((unsigned char)*s);
    if (v < 0) continue;                                 /* line breaks, spaces */
    acc = (acc << 6) | (uint32_t)v; bits += 6;
    if (bits >= 8) { bits -= 8; if (n >= cap) return 0; out[n++] = (uint8_t)(acc >> bits); }
  }
  return n;
}
static const char *mem_find(const char *p, const char *e, const char *pat) {
  const size_t m = strlen(pat);
  for (; p + m <= e; p++) if (*p == *pat && !memcmp(p, pat, m)) return p;
  return NULL;
}


/* ---- verification diagnostics (bloonspop_nx) ----------------------------------
 * Replaces unitytls_x509verify_default_ca with the same two calls it ends in --
 * explicit_ca(chain, default_ca_get(err), ...) -- and, when a chain is refused,
 * logs the verdict by name and every presented certificate's subject/issuer, and
 * saves the chain as DER to <root>/tls/ so it can be inspected offline. Installed
 * only after our list is published, so the original's Java-loading branch (for an
 * unset global) is never needed. */
typedef uint64_t (*tls_ca_get_fn)(BpTlsErr *err);
typedef uint32_t (*tls_explicit_fn)(uint64_t chain, uint64_t ca, const char *cn, size_t cn_len,
                                    void *cb, void *ud, BpTlsErr *err);
typedef size_t (*tls_export_der_fn)(uint64_t x509, uint8_t *buf, size_t len, BpTlsErr *err);
static tls_err_create_fn s_create; static tls_ca_get_fn s_ca_get; static tls_explicit_fn s_explicit;
static tls_export_der_fn s_export; static tls_list_get_x509_fn s_get_x509;
static volatile int s_diag_left = 4;
const char *bp_game_root(void);

static void der_cn(const uint8_t *d, size_t n, int which, char *out, size_t cap) {
  /* the N-th commonName (OID 2.5.4.3) in the DER: issuer comes before subject */
  out[0] = 0;
  int seen = 0;
  for (size_t i = 0; i + 7 < n; i++) {
    if (d[i] == 0x06 && d[i + 1] == 0x03 && d[i + 2] == 0x55 && d[i + 3] == 0x04 && d[i + 4] == 0x03) {
      size_t len = d[i + 6];
      if (len >= 0x80 || i + 7 + len > n) continue;
      if (seen++ != which) continue;
      size_t k = 0;
      for (size_t j = 0; j < len && k + 1 < cap; j++) { char c = (char)d[i + 7 + j]; out[k++] = (c >= 0x20 && c < 0x7f) ? c : '.'; }
      out[k] = 0;
      return;
    }
  }
}
static const char *verify_flags_str(uint32_t f, char *b, size_t cap) {
  b[0] = 0;
  static const struct { uint32_t bit; const char *name; } k[] = {
    { 0x1, "EXPIRED" }, { 0x2, "REVOKED" }, { 0x4, "CN_MISMATCH" }, { 0x8, "NOT_TRUSTED" },
    { 0x10000, "USER_ERROR1" }, { 0x08000000, "UNKNOWN_ERROR" }, { 0x80000000u, "FATAL_ERROR" } };
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
    if (f & k[i].bit) { strncat(b, b[0] ? "|" : "", cap - strlen(b) - 1); strncat(b, k[i].name, cap - strlen(b) - 1); }
  if (!b[0]) snprintf(b, cap, "0x%x", (unsigned)f);
  return b;
}
static uint32_t bp_tls_verify_default_ca(uint64_t chain, const char *cn, size_t cn_len, void *cb, void *ud, BpTlsErr *err) {
  const uint64_t ca = s_ca_get(err);
  uint32_t r = s_explicit(chain, ca, cn, cn_len, cb, ud, err);
  if (r != 0 && s_diag_left > 0) {
    s_diag_left--;
    char host[128], fl[96];
    size_t hl = cn ? (cn_len < sizeof host - 1 ? cn_len : sizeof host - 1) : 0;
    if (hl) memcpy(host, cn, hl);
    host[hl] = 0;
    debugPrintf("[tls] verify %s -> %s (default list ref %s)\n", host[0] ? host : "?", verify_flags_str(r, fl, sizeof fl),
                ca > 1 ? "ok" : "INVALID");
    char dir[600];
    snprintf(dir, sizeof dir, "%s/tls", bp_game_root());
    mkdir(dir, 0777);
    static uint8_t der[16384];
    for (size_t i = 0; i < 8; i++) {
      BpTlsErr e = s_create();
      const uint64_t x = s_get_x509(chain, i, &e);
      if (e.code != 0 || x == 0 || x == 1) break;
      BpTlsErr e2 = s_create();
      const size_t len = s_export(x, der, sizeof der, &e2);
      if (e2.code != 0 || !len || len > sizeof der) { debugPrintf("[tls]   chain[%zu] export failed\n", i); continue; }
      char iss[72], sub[72], path[700];
      der_cn(der, len, 0, iss, sizeof iss); der_cn(der, len, 1, sub, sizeof sub);
      snprintf(path, sizeof path, "%s/%s_%zu.der", dir, host[0] ? host : "chain", i);
      FILE *f = fopen(path, "wb");
      if (f) { fwrite(der, 1, len, f); fclose(f); }
      debugPrintf("[tls]   chain[%zu] subject CN=%s  issuer CN=%s  (%zu bytes -> %s)\n", i, sub, iss, len, f ? path : "not saved");
    }
  }
#if BP_TLS_INSECURE_FALLBACK
  if (r == 0x8) {                                    /* NOT_TRUSTED only: host and dates were fine */
    static volatile int accepts;
    if (accepts < 16) {
      accepts++;
      char h[128];
      size_t hl = cn ? (cn_len < sizeof h - 1 ? cn_len : sizeof h - 1) : 0;
      if (hl) memcpy(h, cn, hl);
      h[hl] = 0;
      debugPrintf("[tls] INSECURE FALLBACK: accepted an untrusted chain for %s "
                  "(hostname and dates were verified; BP_TLS_INSECURE_FALLBACK 1)\n", h[0] ? h : "?");
    }
    return 0;
  }
#endif
  return r;
}
static void install_verify_hook(so_module *unity) {
#if BP_TLS_VERIFY_DIAG || BP_TLS_INSECURE_FALLBACK
  const uintptr_t b = (uintptr_t)unity->load_virtbase;
  if (!bp_guard_ok(unity, BP_RVA_unitytls_x509verify_explicit_ca, BP_GUARD_unitytls_x509verify_explicit_ca, 4, "unitytls_x509verify_explicit_ca") ||
      !bp_guard_ok(unity, BP_RVA_unitytls_x509verify_default_ca_get, BP_GUARD_unitytls_x509verify_default_ca_get, 4, "unitytls_x509verify_default_ca_get") ||
      !bp_guard_ok(unity, BP_RVA_unitytls_x509_export_der, BP_GUARD_unitytls_x509_export_der, 4, "unitytls_x509_export_der") ||
      !bp_guard_ok(unity, BP_RVA_unitytls_x509list_get_x509, BP_GUARD_unitytls_x509list_get_x509, 4, "unitytls_x509list_get_x509")) {
    debugPrintf("[tls] verify diagnostics not installed (guard mismatch)\n");
    return;
  }
  s_create   = (tls_err_create_fn)(b + BP_RVA_unitytls_errorstate_create);
  s_ca_get   = (tls_ca_get_fn)(b + BP_RVA_unitytls_x509verify_default_ca_get);
  s_explicit = (tls_explicit_fn)(b + BP_RVA_unitytls_x509verify_explicit_ca);
  s_export   = (tls_export_der_fn)(b + BP_RVA_unitytls_x509_export_der);
  s_get_x509 = (tls_list_get_x509_fn)(b + BP_RVA_unitytls_x509list_get_x509);
  if (bp_redirect(unity, BP_RVA_unitytls_x509verify_default_ca, BP_GUARD_unitytls_x509verify_default_ca, 4,
                  (void *)&bp_tls_verify_default_ca, "tls/default-ca-verify"))
    debugPrintf("[tls] default-CA verification instrumented (refused chains are logged and saved to tls/)\n");
#else
  (void)unity;
#endif
}

/* An SD-card bundle overrides the built-in one: <root>/cacert.pem. */
static uint8_t *load_sd_bundle(size_t *len) {
  char path[600];
  snprintf(path, sizeof path, "%s/cacert.pem", bp_game_root());
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *buf = (n > 0 && n < 8 * 1024 * 1024) ? (uint8_t *)malloc((size_t)n + 1) : NULL;
  if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) { buf[n] = 0; *len = (size_t)n; }
  else { free(buf); buf = NULL; }
  fclose(f);
  if (buf) debugPrintf("[net] CA bundle source: %s (%ld bytes) -- overrides the built-in bundle\n", path, n);
  return buf;
}

/* HTTPS trust, v2. unitytls keeps ONE global default CA list (mutex-guarded).
 * enter_sync returns a fresh list only while that global is unset; exit_sync
 * publishes whatever it is handed -- but only if the errorstate passed to it
 * is clean. The first HTTPS request would otherwise fill it from Android's
 * TrustManager over JNI (nothing here).
 *
 * v1 appended the whole bundle with append_pem and reported success, yet every
 * request failed "UNITYTLS_X509VERIFY_FLAG_NOT_TRUSTED" (fifth run). The embedded
 * bundle is not NUL-terminated, and mbedtls only parses a buffer as PEM when its
 * last byte is NUL -- so the list was almost certainly empty. v2 decodes each
 * certificate itself and appends it as DER, exactly as Unity appends the
 * certificates it gets from Android, with a fresh errorstate per certificate,
 * then COUNTS the list through unitytls' own accessor before publishing it. */
int bp_net_install_ca(so_module *unity) {
  if (!bp_guard_ok(unity, BP_RVA_unitytls_errorstate_create, BP_GUARD_unitytls_errorstate_create, 4, "unitytls_errorstate_create") ||
      !bp_guard_ok(unity, BP_RVA_unitytls_default_ca_enter_sync, BP_GUARD_unitytls_default_ca_enter_sync, 4, "unitytls_x509verify_default_ca_enter_sync") ||
      !bp_guard_ok(unity, BP_RVA_unitytls_default_ca_exit_sync, BP_GUARD_unitytls_default_ca_exit_sync, 4, "unitytls_x509verify_default_ca_exit_sync")) {
    debugPrintf("[net] CA bundle NOT installed -- HTTPS will fail certificate checks\n");
    return 0;
  }
  const uintptr_t b = (uintptr_t)unity->load_virtbase;
  tls_err_create_fn create = (tls_err_create_fn)(b + BP_RVA_unitytls_errorstate_create);
  tls_ca_enter_fn   enter  = (tls_ca_enter_fn)(b + BP_RVA_unitytls_default_ca_enter_sync);
  tls_ca_exit_fn    leave  = (tls_ca_exit_fn)(b + BP_RVA_unitytls_default_ca_exit_sync);
  const int have_der = bp_guard_ok(unity, BP_RVA_unitytls_x509list_append_der, BP_GUARD_unitytls_x509list_append_der, 4, "unitytls_x509list_append_der");
  const int have_pem = bp_guard_ok(unity, BP_RVA_unitytls_x509list_append_pem, BP_GUARD_unitytls_x509list_append_pem, 4, "unitytls_x509list_append_pem");
  const int have_cnt = bp_guard_ok(unity, BP_RVA_unitytls_x509list_get_x509, BP_GUARD_unitytls_x509list_get_x509, 4, "unitytls_x509list_get_x509");

  BpTlsErr err = create();
  void *list = enter(&err);
  if (!list || err.code != 0) {
    if (list) { BpTlsErr le = create(); leave(list, &le); }
    debugPrintf("[net] unitytls default CA list already set or unavailable (code %u) -- not replaced\n", (unsigned)err.code);
    return 0;
  }
  int seen = 0, ok = 0, bad = 0;
  if (have_der) {
    tls_append_der_fn append_der = (tls_append_der_fn)(b + BP_RVA_unitytls_x509list_append_der);
    static uint8_t der[16384];
    size_t sd_len = 0; uint8_t *sd = load_sd_bundle(&sd_len);
    const char *p = sd ? (const char *)sd : (const char *)cacerts_pem;
    const char *e = p + (sd ? sd_len : (size_t)cacerts_pem_size);
    for (;;) {
      const char *s = mem_find(p, e, "-----BEGIN CERTIFICATE-----");
      if (!s) break;
      s += 27;
      const char *q = mem_find(s, e, "-----END CERTIFICATE-----");
      if (!q) break;
      seen++;
      const size_t len = b64_decode(s, q, der, sizeof der);
      BpTlsErr ce = create();                            /* one bad root must not fail the rest */
      if (len) append_der(list, der, len, &ce);
      if (len && ce.code == 0) ok++; else bad++;
      p = q + 25;
    }
    free(sd);
  } else if (have_pem) {
    tls_append_pem_fn append_pem = (tls_append_pem_fn)(b + BP_RVA_unitytls_x509list_append_pem);
    char *z = (char *)malloc((size_t)cacerts_pem_size + 1);
    if (z) {
      memcpy(z, cacerts_pem, cacerts_pem_size); z[cacerts_pem_size] = 0;   /* mbedtls needs the NUL */
      BpTlsErr ce = create();
      append_pem(list, (const uint8_t *)z, (size_t)cacerts_pem_size + 1, &ce);
      ok = ce.code == 0; seen = 1; bad = !ok;
      free(z);
    }
  }
  int count = -1;
  if (have_cnt) {
    tls_list_get_x509_fn get_x509 = (tls_list_get_x509_fn)(b + BP_RVA_unitytls_x509list_get_x509);
    count = 0;
    for (size_t i = 0; i < 4096; i++) {
      BpTlsErr ge = create();
      const uint64_t h = get_x509((uint64_t)(uintptr_t)list, i, &ge);
      if (ge.code != 0 || h == 0 || h == 1) break;         /* 1 == UNITYTLS_INVALID_HANDLE */
      count++;
    }
  }
  BpTlsErr le = create();                                 /* clean state, or exit_sync will not publish */
  leave(list, &le);
  debugPrintf("[net] CA bundle: %d of %d roots appended %s (%d rejected); unitytls default list holds %d certificates%s\n",
              ok, seen, have_der ? "as DER" : "as PEM", bad, count,
              (count == 0 || ok == 0) ? "  <-- EMPTY: HTTPS will fail" : "");
  if (ok > 0 && count != 0) install_verify_hook(unity);
  return ok > 0 && count != 0;
}
