/* ---------------------------------------------------------------------------
 * bp_net_shim.c -- bionic-ABI BSD sockets over libnx (v2).
 *
 * The inherited shims refused every socket call (Bouncemasters was offline by
 * design). This layer follows acpc_nx, which downloads content through the
 * same libcurl/unitytls path Bloons Pop uses, and covers every network import
 * libunity (curl) and libil2cpp (System.Net) actually make:
 *
 *   sockaddr_in  bionic {u16 family; u16 port; u32 addr; u8 zero[8]} vs BSD+sin_len
 *   addrinfo     same order, ai_addr converted; IPv4 only (AF_INET6 is 10 vs 28)
 *   msghdr       il2cpp's scatter/gather Send/Receive (sendmsg/recvmsg)
 *   options      SOL_SOCKET 1 vs 0xffff, every SO_* number differs
 *   flags        MSG_*, O_NONBLOCK, FIONBIO/FIONREAD, POLLWRNORM differ
 *   errno        newlib vs Linux: EINPROGRESS 119/115, ETIMEDOUT 116/110, ...
 *
 * v2 fixes, found by auditing the imports against the inherited rows:
 *   poll        mixed socket + pipe sets fell to a stub that returned 0 forever;
 *               now sliced over native poll + fake-fd readiness, honouring timeout
 *   socketpair  always failed; curl_multi creates its wakeup pair with it. Now a
 *               loopback TCP pair (what curl itself does on Windows)
 *   ioctl       FIONBIO / FIONREAD returned -1 (il2cpp Socket.Available)
 *   send/recvmsg returned 0, i.e. EOF, for il2cpp's gather/scatter I/O
 *   close       never forgot a socket; a reused fd number was misclassified
 *   select, getnameinfo (numeric), inet_addr: were stubs
 *
 * connect(): acpc_nx's order -- BLOCKING connect, O_NONBLOCK applied after -- so
 * libcurl never sees an EINPROGRESS it would misread. BP_NET_CONNECT_TIMEOUT_MS
 * > 0 switches to a bounded non-blocking connect that still only ever reports
 * success or a final error.
 * MIT.
 * ------------------------------------------------------------------------- */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>        /* armGetSystemTick / armTicksToNs / svcSleepThread */

#include "bp_net.h"
#include "config.h"
#include "libc_shim.h"
#include "util.h"

#ifndef BP_NET_CONNECT_TIMEOUT_MS
#define BP_NET_CONNECT_TIMEOUT_MS 0
#endif

int close_fake(int fd);                                    /* libc_shim.c */
int fakefd_is_fake(int fd);                                /* fakefd.c    */
int fakefd_poll_state(int fd, int *readable, int *writable);
int  fakefd_pipe(int fds[2]);
long fakefd_write(int fd, const void *buf, unsigned long n);
long fakefd_read(int fd, void *buf, unsigned long n);

#ifndef BP_NET_LOG_FAILURES
#define BP_NET_LOG_FAILURES 1
#endif
/* Failures always log (first 32), successes once: enough to tell DNS from connect
 * from TLS in a single run without BP_NET_TRACE's firehose. */
static volatile int s_fail_logged;
#define NET_FAIL(...) do { if (BP_NET_LOG_FAILURES && s_fail_logged < 32) { s_fail_logged++; debugPrintf(__VA_ARGS__); } } while (0)

#define B_AF_INET        2
#define B_AF_INET6       10
#define B_SOCK_NONBLOCK  0x800
#define B_SOL_SOCKET     1
#define B_IPPROTO_TCP    6
#define B_O_NONBLOCK     0x800
#define B_F_GETFL        3
#define B_F_SETFL        4
#define B_MSG_OOB        0x1
#define B_MSG_PEEK       0x2
#define B_MSG_DONTWAIT   0x40
#define B_MSG_WAITALL    0x100
#define B_POLLRDNORM     0x40
#define B_POLLRDBAND     0x80
#define B_POLLWRNORM     0x100
#define B_POLLWRBAND     0x200
#define B_FIONREAD       0x541B
#define B_FIONBIO        0x5421
#define B_NI_NAMEREQD    8
#define LX_ENOTTY        25
#define LX_ENOPROTOOPT   92
#define LX_EAFNOSUPPORT  97

struct BSockaddrIn { uint16_t family; uint16_t port; uint32_t addr; uint8_t zero[8]; };
struct BAddrInfo {
  int flags, family, socktype, protocol;
  uint32_t addrlen;
  char *canonname;
  struct BSockaddrIn *addr;
  struct BAddrInfo *next;
};
struct BIovec  { void *base; size_t len; };
struct BMsghdr { void *name; uint32_t namelen; struct BIovec *iov; size_t iovlen;
                 void *control; size_t controllen; int flags; };
_Static_assert(sizeof(struct BSockaddrIn) == 16, "bionic sockaddr_in is 16 bytes");
_Static_assert(offsetof(struct BAddrInfo, canonname) == 24 && offsetof(struct BAddrInfo, next) == 40,
               "bionic arm64 addrinfo layout");
_Static_assert(offsetof(struct BMsghdr, iov) == 16 && offsetof(struct BMsghdr, flags) == 48,
               "bionic arm64 msghdr layout");

#define MAXFD 1024
static uint8_t s_sock[MAXFD], s_want_nb[MAXFD], s_conn[MAXFD];
static int tracked(int fd) { return fd >= 0 && fd < MAXFD && s_sock[fd]; }
static void track(int fd, int want_nb, int conn) {
  if (fd >= 0 && fd < MAXFD) { s_sock[fd] = 1; s_want_nb[fd] = (uint8_t)want_nb; s_conn[fd] = (uint8_t)conn; }
}

static int lx_errno(int e) {
#ifdef EINPROGRESS
  if (e == EINPROGRESS) return 115;
#endif
#ifdef EALREADY
  if (e == EALREADY) return 114;
#endif
#ifdef ENOTSOCK
  if (e == ENOTSOCK) return 88;
#endif
#ifdef EDESTADDRREQ
  if (e == EDESTADDRREQ) return 89;
#endif
#ifdef EMSGSIZE
  if (e == EMSGSIZE) return 90;
#endif
#ifdef EPROTOTYPE
  if (e == EPROTOTYPE) return 91;
#endif
#ifdef ENOPROTOOPT
  if (e == ENOPROTOOPT) return 92;
#endif
#ifdef EPROTONOSUPPORT
  if (e == EPROTONOSUPPORT) return 93;
#endif
#ifdef EAFNOSUPPORT
  if (e == EAFNOSUPPORT) return 97;
#endif
#ifdef EADDRINUSE
  if (e == EADDRINUSE) return 98;
#endif
#ifdef EADDRNOTAVAIL
  if (e == EADDRNOTAVAIL) return 99;
#endif
#ifdef ENETDOWN
  if (e == ENETDOWN) return 100;
#endif
#ifdef ENETUNREACH
  if (e == ENETUNREACH) return 101;
#endif
#ifdef ECONNABORTED
  if (e == ECONNABORTED) return 103;
#endif
#ifdef EISCONN
  if (e == EISCONN) return 106;
#endif
#ifdef ENOTCONN
  if (e == ENOTCONN) return 107;
#endif
#ifdef ETIMEDOUT
  if (e == ETIMEDOUT) return 110;
#endif
#ifdef EHOSTUNREACH
  if (e == EHOSTUNREACH) return 113;
#endif
  return e;
}
static int  fail(void)  { errno = lx_errno(errno); return -1; }
static long failL(void) { errno = lx_errno(errno); return -1; }

static void set_sin_len(struct sockaddr_in *n) {
  if (offsetof(struct sockaddr_in, sin_family) == 1) ((uint8_t *)n)[0] = (uint8_t)sizeof *n;   /* BSD sin_len */
}
static int b2n(const void *a, unsigned l, struct sockaddr_in *n) {
  const struct BSockaddrIn *b = (const struct BSockaddrIn *)a;
  if (!a || l < sizeof *b) { errno = EINVAL; return 0; }
  if (b->family != B_AF_INET) { errno = LX_EAFNOSUPPORT; return 0; }
  memset(n, 0, sizeof *n);
  set_sin_len(n);
  n->sin_family = AF_INET;
  n->sin_port = b->port;
  n->sin_addr.s_addr = b->addr;
  return 1;
}
static void n2b(const struct sockaddr_in *in, void *out, void *lenp) {
  if (!out || !lenp) return;
  struct BSockaddrIn b;
  memset(&b, 0, sizeof b);
  b.family = B_AF_INET; b.port = in->sin_port; b.addr = in->sin_addr.s_addr;
  unsigned cap = *(unsigned *)lenp;
  memcpy(out, &b, cap < sizeof b ? cap : sizeof b);
  *(unsigned *)lenp = sizeof b;
}
static int msgf(int f) {
  int o = 0;
  if (f & B_MSG_OOB)  o |= MSG_OOB;
  if (f & B_MSG_PEEK) o |= MSG_PEEK;
#ifdef MSG_DONTWAIT
  if (f & B_MSG_DONTWAIT) o |= MSG_DONTWAIT;
#endif
#ifdef MSG_WAITALL
  if (f & B_MSG_WAITALL) o |= MSG_WAITALL;
#endif
  return o;                     /* MSG_NOSIGNAL dropped: no signals on Switch */
}
static void set_nb(int fd, int on) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) fl = 0;
  fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

/* ---------------------------------------------------------------- lifecycle */
int bpn_socket(int d, int t, int p) {
  if (d != B_AF_INET) { errno = LX_EAFNOSUPPORT; return -1; }   /* curl falls back to IPv4 */
  int type = t & 0xf;
  int fd = socket(AF_INET, type, p);
  if (fd < 0) {
    const int e = errno;
    NET_FAIL("[net] socket(type %d) FAILED errno %d -- socket service out of sockets/buffers?\n", type, e);
    errno = e;
    return fail();
  }
  track(fd, (t & B_SOCK_NONBLOCK) ? 1 : 0, 0);
  if (type != SOCK_STREAM && tracked(fd) && s_want_nb[fd]) set_nb(fd, 1);
  return fd;
}

/* Forget any socket state for fd. close_fake() (libc_shim.c) calls this on
 * EVERY close path, so a closed socket's fd number can never be reused by a
 * file and still be treated as a socket by poll/fcntl/ioctl. */
void bpn_untrack(int fd) {
  if (fd >= 0 && fd < MAXFD) { s_sock[fd] = 0; s_want_nb[fd] = 0; s_conn[fd] = 0; }
}

int bpn_close(int fd) {
  return close_fake(fd);        /* untracks via bpn_untrack(); files, pipes, sockets */
}

int bpn_connect(int s, const void *a, unsigned l) {
  struct sockaddr_in n;
  if (!b2n(a, l, &n)) return -1;
  if (bp_net_is_offline() && (ntohl(n.sin_addr.s_addr) >> 24) != 127) {   /* loopback only */
    errno = ENETUNREACH;
    return fail();
  }
  int rc;
#if BP_NET_CONNECT_TIMEOUT_MS > 0
  if (tracked(s)) {
    set_nb(s, 1);
    rc = connect(s, (const struct sockaddr *)&n, sizeof n);
    if (rc != 0 && (errno == EINPROGRESS || errno == EALREADY || errno == EAGAIN)) {
      struct pollfd p = { s, POLLOUT, 0 };
      int pr = poll(&p, 1, BP_NET_CONNECT_TIMEOUT_MS);
      if (pr == 0) { errno = ETIMEDOUT; rc = -1; }
      else if (pr > 0) {
        int err = 0; socklen_t el = sizeof err;
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) < 0) rc = -1;
        else if (err) { errno = err; rc = -1; }
        else rc = 0;
      }
    }
    if (rc == 0) { s_conn[s] = 1; set_nb(s, s_want_nb[s]); }
    else { int e = errno; set_nb(s, 0); errno = e; }
  } else
#endif
  {
    if (tracked(s)) set_nb(s, 0);             /* blocking connect, then non-blocking */
    rc = connect(s, (const struct sockaddr *)&n, sizeof n);
    if (rc == 0 && tracked(s)) { s_conn[s] = 1; if (s_want_nb[s]) set_nb(s, 1); }
  }
#if BP_NET_TRACE
  debugPrintf("[net] connect fd=%d %s:%u -> %d\n", s, inet_ntoa(n.sin_addr), ntohs(n.sin_port), rc);
#endif
  if (rc != 0) {
    const int e = errno;
    NET_FAIL("[net] connect %s:%u FAILED errno %d\n", inet_ntoa(n.sin_addr), (unsigned)ntohs(n.sin_port), e);
    errno = e;
  } else {
    static int told;
    if (!told) { told = 1; debugPrintf("[net] first connect ok: %s:%u\n", inet_ntoa(n.sin_addr), (unsigned)ntohs(n.sin_port)); }
  }
  return rc == 0 ? 0 : fail();
}

int bpn_bind(int s, const void *a, unsigned l) {
  struct sockaddr_in n;
  if (!b2n(a, l, &n)) return -1;
  return bind(s, (const struct sockaddr *)&n, sizeof n) < 0 ? fail() : 0;
}
int bpn_listen(int s, int b) { return listen(s, b) < 0 ? fail() : 0; }
int bpn_accept(int s, void *a, void *l) {
  struct sockaddr_in n;
  socklen_t nl = sizeof n;
  int fd = accept(s, (struct sockaddr *)&n, &nl);
  if (fd < 0) return fail();
  track(fd, 0, 1);
  n2b(&n, a, l);
  return fd;
}
int bpn_shutdown(int s, int how) { return shutdown(s, how) < 0 ? fail() : 0; }

/* AF_UNIX pairs do not exist here; a connected loopback TCP pair behaves the
 * same for curl's wakeup channel (and is what curl itself uses on Windows). */
#ifndef BP_NET_SOCKETPAIR_LOOPBACK
#define BP_NET_SOCKETPAIR_LOOPBACK 0
#endif
int bpn_socketpair(int d, int t, int p, int sv[2]) {
  (void)d; (void)p;
  if (!sv) { errno = EINVAL; return -1; }
#if !BP_NET_SOCKETPAIR_LOOPBACK
  /* In-process pipe pair first: curl's two callers (resolver signal, multi wake-up)
   * are one-way, and a pipe uses no socket-service resources. */
  if (fakefd_pipe(sv) == 0) {
    static int told;
    if (!told) { told = 1; debugPrintf("[net] socketpair: in-process pipe pair (uses no socket-service resources)\n"); }
    return 0;
  }
  /* no pipe slot left: fall through to the loopback pair */
#endif
  int lst = -1, c = -1, a = -1, want_nb = (t & B_SOCK_NONBLOCK) ? 1 : 0;
  struct sockaddr_in in;
  socklen_t il = sizeof in;
  memset(&in, 0, sizeof in);
  set_sin_len(&in);
  in.sin_family = AF_INET;
  in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if ((lst = socket(AF_INET, SOCK_STREAM, 0)) < 0) goto bad;
  if (bind(lst, (struct sockaddr *)&in, sizeof in) < 0 || listen(lst, 1) < 0 ||
      getsockname(lst, (struct sockaddr *)&in, &il) < 0) goto bad;
  if ((c = socket(AF_INET, SOCK_STREAM, 0)) < 0) goto bad;
  if (connect(c, (struct sockaddr *)&in, sizeof in) < 0) goto bad;
  if ((a = accept(lst, NULL, NULL)) < 0) goto bad;
  close(lst);
  track(c, want_nb, 1); track(a, want_nb, 1);
  if (want_nb) { set_nb(c, 1); set_nb(a, 1); }
  sv[0] = c; sv[1] = a;
  { static int told; if (!told) { told = 1; debugPrintf("[net] socketpair: loopback TCP pair\n"); } }
  return 0;
bad: {
    int e = errno;
    if (lst >= 0) close(lst);
    if (c >= 0) close(c);
    if (a >= 0) close(a);
    /* Loopback TCP is not available here: fall back to an in-process pipe pair.
     * curl's threaded resolver REQUIRES a socketpair -- without one every lookup
     * fails as "Could not resolve host" (the fourth hardware run). It only ever
     * writes one end and polls/reads the other, which a pipe pair serves. */
    if (fakefd_pipe(sv) == 0) {
      static int told;
      if (!told) { told = 1; debugPrintf("[net] socketpair: loopback TCP unavailable (errno %d) -> in-process pipe pair\n", e); }
      return 0;
    }
    NET_FAIL("[net] socketpair FAILED: loopback errno %d, no pipe slots left\n", e);
    errno = e;
    return fail();
  }
}

/* ---------------------------------------------------------------- data */
long bpn_send(int s, const void *b, size_t l, int f) {
  if (fakefd_is_fake(s)) {                         /* socketpair fallback end */
    long r = fakefd_write(s, b, (unsigned long)l);
    if (r < 0) { errno = 32; return -1; }          /* EPIPE */
    return r;
  }
  long r = send(s, b, l, msgf(f)); return r < 0 ? failL() : r;
}
long bpn_recv(int s, void *b, size_t l, int f) {
  if (fakefd_is_fake(s)) {
    /* socketpair ends are non-blocking by contract here: curl drains its wakeup
     * pair until recv fails, and fakefd_read would otherwise block when empty. */
    int rd = 0, wr = 0;
    if (fakefd_poll_state(s, &rd, &wr) < 0) { errno = 9; return -1; }     /* EBADF */
    if (!rd) { errno = 11; return -1; }                                      /* EAGAIN */
    return fakefd_read(s, b, (unsigned long)l);
  }
  long r = recv(s, b, l, msgf(f)); return r < 0 ? failL() : r;
}
long bpn_sendto(int s, const void *b, size_t l, int f, const void *a, unsigned al) {
  if (!a) return bpn_send(s, b, l, f);
  struct sockaddr_in n;
  if (!b2n(a, al, &n)) return -1;
  long r = sendto(s, b, l, msgf(f), (const struct sockaddr *)&n, sizeof n);
  return r < 0 ? failL() : r;
}
long bpn_recvfrom(int s, void *b, size_t l, int f, void *a, void *al) {
  struct sockaddr_in n;
  socklen_t nl = sizeof n;
  long r = recvfrom(s, b, l, msgf(f), a ? (struct sockaddr *)&n : NULL, a ? &nl : NULL);
  if (r < 0) return failL();
  if (a) n2b(&n, a, al);
  return r;
}
long bpn_sendmsg(int s, const void *mv, int f) {
  const struct BMsghdr *m = (const struct BMsghdr *)mv;
  if (!m) { errno = EINVAL; return -1; }
  long total = 0;
  for (size_t i = 0; i < m->iovlen; i++) {
    const struct BIovec *v = &m->iov[i];
    if (!v->len) continue;
    long r = m->name ? bpn_sendto(s, v->base, v->len, f, m->name, m->namelen)
                     : bpn_send(s, v->base, v->len, f);
    if (r < 0) return total ? total : r;
    total += r;
    if ((size_t)r < v->len) break;
  }
  return total;
}
long bpn_recvmsg(int s, void *mv, int f) {
  struct BMsghdr *m = (struct BMsghdr *)mv;
  if (!m) { errno = EINVAL; return -1; }
  long total = 0;
  for (size_t i = 0; i < m->iovlen; i++) {
    struct BIovec *v = &m->iov[i];
    if (!v->len) continue;
    long r;
    if (m->name && total == 0) {
      unsigned nl = m->namelen;
      r = bpn_recvfrom(s, v->base, v->len, f, m->name, &nl);
      if (r >= 0) m->namelen = nl;
    } else {
      r = bpn_recv(s, v->base, v->len, total ? (f | B_MSG_DONTWAIT) : f);
    }
    if (r < 0) { if (total) break; return r; }
    total += r;
    if ((size_t)r < v->len) break;
  }
  m->controllen = 0;
  m->flags = 0;
  return total;
}

/* ---------------------------------------------------------------- options */
static int so_native(int n) {
  switch (n) {
    case 2:  return SO_REUSEADDR;
    case 3:  return SO_TYPE;
    case 4:  return SO_ERROR;
    case 6:  return SO_BROADCAST;
    case 7:  return SO_SNDBUF;
    case 8:  return SO_RCVBUF;
    case 9:  return SO_KEEPALIVE;
    case 13: return SO_LINGER;
    case 20: return SO_RCVTIMEO;
    case 21: return SO_SNDTIMEO;
    default: return -1;
  }
}
int bpn_setsockopt(int s, int lv, int n, const void *v, unsigned l) {
  if (lv == B_SOL_SOCKET) {
    int nn = so_native(n);
    if (nn >= 0) (void)setsockopt(s, SOL_SOCKET, nn, v, (socklen_t)l);
    return 0;                    /* curl treats these as hints; never fail it */
  }
#ifdef TCP_NODELAY
  if (lv == B_IPPROTO_TCP && n == 1) { (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, v, (socklen_t)l); return 0; }
#endif
  return 0;                      /* keepalive tuning, IP-level hints: not needed */
}
int bpn_getsockopt(int s, int lv, int n, void *v, void *l) {
  if (!v || !l) { errno = EINVAL; return -1; }
  socklen_t len = *(unsigned *)l;
  int rc;
  if (lv == B_SOL_SOCKET) {
    int nn = so_native(n);
    if (nn < 0) { errno = LX_ENOPROTOOPT; return -1; }
    rc = getsockopt(s, SOL_SOCKET, nn, v, &len);
    if (rc == 0 && n == 4 && len >= sizeof(int)) *(int *)v = lx_errno(*(int *)v);   /* SO_ERROR */
    if (rc == 0 && n == 4 && len >= sizeof(int) && *(int *)v != 0)   /* async connect result */
      NET_FAIL("[net] SO_ERROR on fd %d: errno %d (asynchronous connect/transfer failure)\n", s, *(int *)v);
  } else {
    rc = getsockopt(s, lv, n, v, &len);
  }
  if (rc < 0) return fail();
  *(unsigned *)l = len;
  return 0;
}
int bpn_getsockname(int s, void *a, void *l) {
  struct sockaddr_in n; socklen_t nl = sizeof n;
  if (getsockname(s, (struct sockaddr *)&n, &nl) < 0) return fail();
  n2b(&n, a, l);
  return 0;
}
int bpn_getpeername(int s, void *a, void *l) {
  struct sockaddr_in n; socklen_t nl = sizeof n;
  if (getpeername(s, (struct sockaddr *)&n, &nl) < 0) return fail();
  n2b(&n, a, l);
  return 0;
}

int bpn_fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  long arg = va_arg(ap, long);
  va_end(ap);
  if (!tracked(fd)) return 0;                /* files: same as the inherited stub */
  if (cmd == B_F_GETFL) return 2 /* O_RDWR */ | (s_want_nb[fd] ? B_O_NONBLOCK : 0);
  if (cmd == B_F_SETFL) {
    s_want_nb[fd] = (arg & B_O_NONBLOCK) ? 1 : 0;
    if (s_conn[fd] || !s_want_nb[fd]) set_nb(fd, s_want_nb[fd]);   /* else deferred to connect */
    return 0;
  }
  return 0;                                   /* F_GETFD/F_SETFD (CLOEXEC): meaningless here */
}

int bpn_ioctl(int fd, unsigned long req, ...) {
  va_list ap;
  va_start(ap, req);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  if (!tracked(fd)) { errno = LX_ENOTTY; return -1; }
  if (req == B_FIONBIO) {
    int on = arg ? *(int *)arg : 0;
    s_want_nb[fd] = on ? 1 : 0;
    if (s_conn[fd] || !on) set_nb(fd, on);
    return 0;
  }
  if (req == B_FIONREAD) {
    int n = 0;
#ifdef FIONREAD
    if (ioctl(fd, FIONREAD, &n) < 0) return fail();
#endif
    if (arg) *(int *)arg = n;
    return 0;
  }
  errno = LX_ENOTTY;
  return -1;
}

/* ---------------------------------------------------------------- readiness */
static short fake_revents(int fd, short want) {
  int rd = 0, wr = 0;
  if (fakefd_poll_state(fd, &rd, &wr) < 0) return POLLNVAL;
  short out = 0;
  if (rd) out |= (short)(want & (POLLIN | B_POLLRDNORM));
  if (wr) out |= (short)(want & (POLLOUT | B_POLLWRNORM));
  if (rd && !(want & (POLLIN | B_POLLRDNORM)) && fakefd_poll_state(fd, &rd, &wr) == 0) { }
  return out;
}
static uint64_t net_now_ms(void) { return armTicksToNs(armGetSystemTick()) / 1000000ull; }

#define POLL_MAX 128
int bpn_poll(void *fdsv, unsigned long nfds, int timeout) {
  struct pollfd *fds = (struct pollfd *)fdsv;
  if (nfds > POLL_MAX) { errno = EINVAL; return -1; }
  struct pollfd nat[POLL_MAX];
  int map[POLL_MAX], nn = 0, nother = 0;
  for (unsigned long i = 0; i < nfds; i++) {
    fds[i].revents = 0;
    if (fds[i].fd < 0) continue;
    if (tracked(fds[i].fd)) {
      short want = fds[i].events;
      short e = (short)(want & ~(B_POLLWRNORM | B_POLLWRBAND));
      if (want & B_POLLWRNORM) e |= POLLOUT;
      nat[nn].fd = fds[i].fd; nat[nn].events = e; nat[nn].revents = 0; map[nn] = (int)i; nn++;
    } else {
      nother++;
    }
  }
  const uint64_t deadline = timeout < 0 ? UINT64_MAX : net_now_ms() + (uint64_t)timeout;
  int ready;
  for (;;) {
    ready = 0;
    /* non-socket fds: fake pipes report their ring state; anything else is a
     * regular file, which POSIX defines as always ready */
    for (unsigned long i = 0; nother && i < nfds; i++) {
      int fd = fds[i].fd;
      if (fd < 0 || tracked(fd)) continue;
      short want = fds[i].events;
      short out = fakefd_is_fake(fd) ? fake_revents(fd, want)
                                     : (short)(want & (POLLIN | POLLOUT | B_POLLRDNORM | B_POLLWRNORM));
      fds[i].revents = out;
      if (out) ready++;
    }
    int slice;
    if (!nother) slice = timeout;                                  /* sockets only: one exact call */
    else if (ready || timeout == 0) slice = 0;
    else {
      uint64_t now = net_now_ms();
      uint64_t left = deadline == UINT64_MAX ? 10 : (deadline > now ? deadline - now : 0);
      slice = (int)(left < 10 ? left : 10);
    }
    if (nn) {
      int r = poll(nat, (nfds_t)nn, slice);
      if (r < 0) return fail();
      for (int k = 0; k < nn; k++) {
        short re = nat[k].revents, want = fds[map[k]].events;
        short out = (short)(re & (POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP | POLLNVAL | B_POLLRDNORM | B_POLLRDBAND));
        if ((re & POLLOUT) && (want & B_POLLWRNORM)) out |= B_POLLWRNORM;
        fds[map[k]].revents = out;
        if (out) ready++;
      }
    } else if (!ready && slice > 0) {
      svcSleepThread((int64_t)slice * 1000000ll);
    }
    if (ready || !nother || timeout == 0 || net_now_ms() >= deadline) break;
  }
  return ready;
}

/* bionic fd_set is 1024 bits in 16 unsigned longs; answered through bpn_poll. */
int bpn_select(int n, void *rv, void *wv, void *ev, void *tvv) {
  unsigned long *r = (unsigned long *)rv, *w = (unsigned long *)wv, *e = (unsigned long *)ev;
  const long *tv = (const long *)tvv;
  if (n > 1024) n = 1024;
  struct pollfd pf[POLL_MAX];
  int k = 0;
  for (int fd = 0; fd < n; fd++) {
    const unsigned long bit = 1ul << (fd % 64);
    const int wr_ = r && (r[fd / 64] & bit), ww = w && (w[fd / 64] & bit), we = e && (e[fd / 64] & bit);
    if (!(wr_ || ww || we)) continue;
    if (k >= POLL_MAX) { errno = EINVAL; return -1; }
    pf[k].fd = fd;
    pf[k].events = (short)((wr_ ? POLLIN : 0) | (ww ? POLLOUT : 0) | (we ? POLLPRI : 0));
    pf[k].revents = 0;
    k++;
  }
  const int timeout = tv ? (int)(tv[0] * 1000 + tv[1] / 1000) : -1;
  int rc = bpn_poll(pf, (unsigned long)k, timeout);
  if (rc < 0) return -1;
  for (int fd = 0; fd < n; fd++) {
    const unsigned long bit = 1ul << (fd % 64);
    if (r) r[fd / 64] &= ~bit;
    if (w) w[fd / 64] &= ~bit;
    if (e) e[fd / 64] &= ~bit;
  }
  int count = 0;
  for (int i = 0; i < k; i++) {
    const int fd = pf[i].fd;
    const unsigned long bit = 1ul << (fd % 64);
    const short re = pf[i].revents;
    if (r && (pf[i].events & POLLIN)  && (re & (POLLIN | POLLHUP | POLLERR))) { r[fd / 64] |= bit; count++; }
    if (w && (pf[i].events & POLLOUT) && (re & (POLLOUT | POLLERR)))          { w[fd / 64] |= bit; count++; }
    if (e && (pf[i].events & POLLPRI) && (re & POLLPRI))                      { e[fd / 64] |= bit; count++; }
  }
  return count;
}

/* ---------------------------------------------------------------- names */
#if BP_NET_BLOCK_ADS
static const char *const k_block[] = {
  "ironsrc.", "supersonicads.", "applovin.", "unityads.", "adjust.com", "adjust.io",
  "vungle.", "facebook.", "fbcdn.", "doubleclick.", "googleadservices.", "googlesyndication.",
  "app-measurement.", "firebaselogging", "crashlytics", "appsflyer.", "chartboost.", "tapjoy.", NULL
};
static int host_blocked(const char *h) {
  char low[256]; size_t i = 0;
  for (; h[i] && i < sizeof low - 1; i++) low[i] = (char)((h[i] >= 'A' && h[i] <= 'Z') ? h[i] + 32 : h[i]);
  low[i] = 0;
  for (int k = 0; k_block[k]; k++) if (strstr(low, k_block[k])) return 1;
  return 0;
}
#endif

static int map_eai(int rc) {
#ifdef EAI_AGAIN
  if (rc == EAI_AGAIN) return 2;
#endif
#ifdef EAI_BADFLAGS
  if (rc == EAI_BADFLAGS) return 3;
#endif
#ifdef EAI_FAMILY
  if (rc == EAI_FAMILY) return 5;
#endif
#ifdef EAI_MEMORY
  if (rc == EAI_MEMORY) return 6;
#endif
#ifdef EAI_NONAME
  if (rc == EAI_NONAME) return 8;
#endif
#ifdef EAI_SERVICE
  if (rc == EAI_SERVICE) return 9;
#endif
#ifdef EAI_SOCKTYPE
  if (rc == EAI_SOCKTYPE) return 10;
#endif
  return 4;                                     /* EAI_FAIL */
}


/* Does a refused lookup for this host mean the game is missing CONTENT?
 *
 * Services say nothing about the cache: Unity Gaming Services config, Ninja
 * Kiwi's account and server-time APIs, and anything already on the ads and
 * telemetry list are all asked for on every launch whether or not a single byte
 * is missing. Only the CDN that serves AssetBundles is evidence.
 *
 * The list is deliberately of things to IGNORE rather than things to trust: a
 * host this port has never seen should count, because missing that signal
 * leaves the game stuck, while an extra online boot costs one launch. */
static int lookup_means_missing_content(const char *host) {
  static const char *const ignore[] = {
    "unity3d.com", "unity.com",              /* UGS config, analytics, crash   */
    "api.ninjakiwi.com",                     /* account, server time           */
    "googleapis.com", "google.com", "gstatic.com",
    "facebook.com", "fbcdn.net",
    NULL
  };
#if BP_NET_BLOCK_ADS
  /* Guarded: host_blocked() itself lives inside this #if, so calling it
   * unconditionally would break the build with BP_NET_BLOCK_ADS 0. */
  if (host_blocked(host)) return 0;          /* ads/telemetry: never content   */
#endif
  for (int i = 0; ignore[i]; i++)
    if (strstr(host, ignore[i])) return 0;
  return 1;
}

int bpn_getaddrinfo(const char *node, const char *svc, const void *hints, void **res) {
  if (!res) return 4;
  *res = NULL;
  if (bp_net_is_offline()) {                        /* cached content: no lookups */
    static int told, asked;
    if (!told) {
      told = 1;
      debugPrintf("[net] offline mode: DNS disabled (first lookup: %s)\n", node ? node : "-");
    }
    /* A refused lookup for CONTENT means the cache was less complete than the
     * manifest claimed, and the next launch should come up online to finish.
     *
     * But only for content. The first refused lookup of every offline boot is
     * config.uca.cloud.unity3d.com -- Unity Gaming Services config, which the
     * game asks for unconditionally and which no amount of downloading will
     * ever satisfy. Treating that as evidence wrote the marker on EVERY offline
     * boot, so offline never stuck and the port simply alternated online and
     * offline for ever. */
    if (!asked && node && lookup_means_missing_content(node)) {
      asked = 1;
      bp_net_request_online_next_boot();
    }
    return 8;                                       /* bionic EAI_NONAME */
  }
#if BP_NET_BLOCK_ADS
  if (node && host_blocked(node)) {
#if BP_NET_TRACE
    debugPrintf("[net] getaddrinfo(%s) blocked (ads/telemetry)\n", node);
#endif
    return 8;
  }
#endif
  struct addrinfo nh;
  memset(&nh, 0, sizeof nh);
  nh.ai_family = AF_INET;
  const struct BAddrInfo *bh = (const struct BAddrInfo *)hints;
  if (bh) {
    if (bh->family == B_AF_INET6) return 5;     /* EAI_FAMILY: IPv4 only */
    nh.ai_flags = bh->flags & 0xf;              /* PASSIVE/CANONNAME/NUMERICHOST/NUMERICSERV */
    nh.ai_socktype = bh->socktype & 0xf;
    nh.ai_protocol = bh->protocol;
  }
  struct addrinfo *nl = NULL;
  int rc = getaddrinfo(node, svc, &nh, &nl);
#if BP_NET_TRACE
  debugPrintf("[net] getaddrinfo(%s, %s) -> %d\n", node ? node : "-", svc ? svc : "-", rc);
#endif
  if (rc != 0) {
    NET_FAIL("[net] getaddrinfo(%s) FAILED rc=%d errno=%d\n", node ? node : "-", rc, errno);
    return map_eai(rc);
  }
  { static int told;
    if (!told && nl && nl->ai_family == AF_INET) { told = 1;
      debugPrintf("[net] first DNS answer: %s -> %s\n", node ? node : "-", inet_ntoa(((struct sockaddr_in *)nl->ai_addr)->sin_addr)); } }
  struct BAddrInfo *head = NULL, **tail = &head;
  for (struct addrinfo *a = nl; a; a = a->ai_next) {
    if (a->ai_family != AF_INET || !a->ai_addr) continue;
    size_t cn = a->ai_canonname ? strlen(a->ai_canonname) + 1 : 0;
    struct BAddrInfo *b = (struct BAddrInfo *)calloc(1, sizeof *b + sizeof(struct BSockaddrIn) + cn);
    if (!b) { freeaddrinfo(nl); bpn_freeaddrinfo(head); return 6; }
    struct BSockaddrIn *sa = (struct BSockaddrIn *)(b + 1);
    const struct sockaddr_in *in = (const struct sockaddr_in *)a->ai_addr;
    sa->family = B_AF_INET; sa->port = in->sin_port; sa->addr = in->sin_addr.s_addr;
    b->flags = a->ai_flags; b->family = B_AF_INET;
    b->socktype = a->ai_socktype ? a->ai_socktype : nh.ai_socktype;
    b->protocol = a->ai_protocol;
    b->addrlen = sizeof *sa; b->addr = sa;
    if (cn) { b->canonname = (char *)(sa + 1); memcpy(b->canonname, a->ai_canonname, cn); }
    *tail = b; tail = &b->next;
  }
  freeaddrinfo(nl);
  if (!head) return 8;
  *res = head;
  return 0;
}
void bpn_freeaddrinfo(void *res) {
  struct BAddrInfo *b = (struct BAddrInfo *)res;
  while (b) { struct BAddrInfo *n = b->next; free(b); b = n; }
}

/* Numeric only: there is no reverse DNS here. NI_NAMEREQD callers get NONAME. */
int bpn_getnameinfo(const void *sa, unsigned salen, char *host, unsigned hostlen,
                    char *serv, unsigned servlen, int flags) {
  const struct BSockaddrIn *b = (const struct BSockaddrIn *)sa;
  if (!b || salen < sizeof *b || b->family != B_AF_INET) return 5;   /* EAI_FAMILY */
  if (flags & B_NI_NAMEREQD) return 8;                               /* EAI_NONAME */
  if (host && hostlen && !inet_ntop(AF_INET, &b->addr, host, (socklen_t)hostlen)) return 4;
  if (serv && servlen) snprintf(serv, servlen, "%u", (unsigned)ntohs(b->port));
  return 0;
}

int bpn_inet_aton(const char *cp, void *inp) { return inet_pton(AF_INET, cp, inp) == 1; }
uint32_t bpn_inet_addr(const char *cp) {
  struct in_addr a;
  return (cp && inet_pton(AF_INET, cp, &a) == 1) ? a.s_addr : 0xffffffffu;   /* INADDR_NONE */
}
int bpn_inet_pton(int af, const char *src, void *dst) {
  if (af != B_AF_INET) { errno = LX_EAFNOSUPPORT; return -1; }
  return inet_pton(AF_INET, src, dst);
}
const char *bpn_inet_ntop(int af, const void *src, char *dst, unsigned size) {
  if (af != B_AF_INET) { errno = LX_EAFNOSUPPORT; return NULL; }
  return inet_ntop(AF_INET, src, dst, (socklen_t)size);
}
