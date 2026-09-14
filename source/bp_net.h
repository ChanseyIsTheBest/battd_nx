/* bp_net.h -- networking for Bloons Pop (adopted from acpc_nx). MIT. */
#ifndef BP_NET_H
#define BP_NET_H
#include <stddef.h>
#include <stdint.h>
#include "so_util.h"

void bp_net_init(void);                    /* BSD sockets + nifm; call early   */
int  bp_net_install_ca(so_module *unity);  /* cacerts.pem -> unitytls CA list  */
int  bp_net_online(void);                  /* nifm: internet connected?        */
int  bp_net_reachability(void);            /* NetworkReachability: 0 or 2      */
int  bp_net_is_offline(void);             /* cache complete: internet disabled */

/* bionic-ABI socket layer (bp_net_shim.c), wired in imports.c */
int  bpn_socket(int d, int t, int p);
int  bpn_connect(int s, const void *a, unsigned l);
int  bpn_bind(int s, const void *a, unsigned l);
int  bpn_listen(int s, int b);
int  bpn_accept(int s, void *a, void *l);
long bpn_send(int s, const void *b, size_t l, int f);
long bpn_recv(int s, void *b, size_t l, int f);
long bpn_sendto(int s, const void *b, size_t l, int f, const void *a, unsigned al);
long bpn_recvfrom(int s, void *b, size_t l, int f, void *a, void *al);
int  bpn_shutdown(int s, int how);
int  bpn_setsockopt(int s, int lv, int n, const void *v, unsigned l);
int  bpn_getsockopt(int s, int lv, int n, void *v, void *l);
int  bpn_getsockname(int s, void *a, void *l);
int  bpn_getpeername(int s, void *a, void *l);
int  bpn_getaddrinfo(const char *node, const char *svc, const void *hints, void **res);
void bpn_freeaddrinfo(void *res);
int  bpn_poll(void *fds, unsigned long nfds, int timeout);
int  bpn_fcntl(int fd, int cmd, ...);
int  bpn_close(int fd);
void bpn_untrack(int fd);             /* called from close_fake() */
int  bpn_socketpair(int d, int t, int p, int sv[2]);
long bpn_sendmsg(int s, const void *msg, int f);
long bpn_recvmsg(int s, void *msg, int f);
int  bpn_ioctl(int fd, unsigned long req, ...);
int  bpn_select(int n, void *r, void *w, void *e, void *tv);
int  bpn_getnameinfo(const void *sa, unsigned salen, char *host, unsigned hostlen,
                     char *serv, unsigned servlen, int flags);
uint32_t bpn_inet_addr(const char *cp);
int  bpn_inet_aton(const char *cp, void *inp);
int  bpn_inet_pton(int af, const char *src, void *dst);
const char *bpn_inet_ntop(int af, const void *src, char *dst, unsigned size);
#endif
