/* libc_shim.c -- bionic-compatible libc wrappers for libcrx.so + libc++_shared
 *
 * The Android engine and its C++ runtime are linked against bionic. Where the
 * bionic and newlib ABIs differ (struct layouts, flag values, missing
 * functions) we provide converting wrappers here; everything that matches is
 * passed straight through from imports.c. Online/IPC functionality (sockets,
 * fork/exec, dlopen of system libs) is dead on Switch and stubbed to fail
 * cleanly so the engine falls back to offline behaviour.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>
#include <EGL/egl.h>     /* eglGetProcAddress: resolve the full GLES API for dlsym */

#include "config.h"
#include "bp_root.h"
#include "bp_assets.h"
#include "util.h"
#include "error.h"
#include "imports.h"   /* dynlib_find_export (dlsym shim lookup) */
#include "so_util.h"
#include "libc_shim.h"
#include "asset_pack.h"
#include "android_native_unity.h"
#include "diag.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memcpy(dst, src, n); }
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memmove(dst, src, n); }
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) { (void)dstlen; return memset(dst, c, n); }
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcat(dst, src); }
char *__strchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strchr(s, c); }
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcpy(dst, src); }
size_t __strlen_chk_fake(const char *s, size_t slen) { (void)slen; return strlen(s); }
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncat(dst, src, n); }
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncpy(dst, src, n); }
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) { (void)dstlen; (void)srclen; return strncpy(dst, src, n); }
char *__strrchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strrchr(s, c); }
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va); }
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsprintf(s, fmt, va); }

int __snprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va; va_start(va, fmt);
  int r = vsnprintf(s, maxlen, fmt, va);
  va_end(va);
  return r;
}
int __sprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va; va_start(va, fmt);
  int r = vsprintf(s, fmt, va);
  va_end(va);
  return r;
}

// fortified read helpers ignore the buffer-size guard
int   __open_2_fake(const char *path, int flags) { return open_fake(path, flags); }
/* MUST go through read_fake, not read().
 *
 * __read_chk is the _FORTIFY_SOURCE form of read(), and bionic-built libraries
 * use it pervasively -- libunity included. Calling the real read() here bypasses
 * the read-ahead layer, and that layer keeps a VIRTUAL file position: it answers
 * lseek from its own bookkeeping and never moves the real descriptor. So a
 * fortified read returned bytes from wherever the real fd happened to be left,
 * which is usually offset 0.
 *
 * This was harmless for as long as the read-ahead cache never attached to
 * anything -- which, because of the `used` flag bug, was its entire history. The
 * moment that was fixed, every fortified read on a cached file started returning
 * the wrong bytes: AssetBundles read as corrupt, Unity re-fetched them and
 * evicted the cached copies, and offline mode looked like it was at fault. It
 * was not. This line was. */
long  __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) {
  (void)buflen;
  return read_fake(fd, buf, count);
}
/* ======================================================= descriptor guard ===
 * A file must not be closed while another thread is still inside a call on it.
 *
 * newlib cannot survive that. Confirmed in the 1.1.8 binary: __get_handle()
 * takes __hndl_lock, loads handles[fd], RELEASES the lock and returns the raw
 * pointer -- no reference taken -- and close() -> __release_handle() takes the
 * same lock and frees the struct. A thread between those two points reads a
 * freed handle. On Android the same race is a harmless EBADF; here it is the
 * 1.1.8 crash: _lseek_r loaded devoptab_list[handle->device] from a freed
 * handle, got NULL, and faulted reading ->seek_r at +0x30.
 *
 * One atomic word per descriptor: the number of calls in flight, plus a CLOSING
 * bit. A call adds itself and tests the bit in ONE atomic step, so there is no
 * gap between "is it closing?" and "I am using it". close_fake() sets CLOSING,
 * waits for the count to drain, closes, clears CLOSING.
 *
 *  - close waits FIRST, holding no lock: close_fake() goes on to take the RAM
 *    cache's locks (ra_detach), and a read in flight needs those to finish.
 *  - A call that finds CLOSING set backs off and RETRIES rather than failing:
 *    once newlib has freed descriptor N another thread may reopen N before the
 *    bit is cleared, and failing that new file's first read would be a false
 *    error. If the bit stays set it gives up with EBADF -- the file is going.
 *  - Every wait is bounded. A call that never returns (a blocking socket read)
 *    cannot hang close forever; past the bound close proceeds exactly as it did
 *    before this existed, and says so once.
 *
 * FDG_MAX is newlib's own limit: __get_handle rejects fd > 0x3ff. The network
 * shim's socket pairs live at 0x40000000+ under their own lock, so a number
 * outside the table is simply not guarded. Guarded: read, write, lseek, fstat,
 * mmap, pread, pwrite, writev and __pread_chk (__read_chk goes through read). */
#define FDG_MAX            1024u
#define FDG_CLOSING        0x80000000u
#define FDG_ENTER_TRIES    500          /* x 100 us = 50 ms waiting for a close to finish */
#define FDG_CLOSE_TRIES    30000        /* x 100 us = 3 s waiting for calls to drain      */
static volatile uint32_t g_fdg[FDG_MAX];
static volatile uint32_t g_fdg_close_timeouts, g_fdg_enter_refused;

int fdg_enter(int fd) {
  if ((unsigned)fd >= FDG_MAX) return 1;
  for (int t = 0; ; t++) {
    const uint32_t s = __atomic_add_fetch(&g_fdg[fd], 1, __ATOMIC_SEQ_CST);
    if (!(s & FDG_CLOSING)) return 1;
    __atomic_sub_fetch(&g_fdg[fd], 1, __ATOMIC_SEQ_CST);
    if (t >= FDG_ENTER_TRIES) {
      __atomic_add_fetch(&g_fdg_enter_refused, 1, __ATOMIC_RELAXED);
      errno = EBADF;
      return 0;
    }
    svcSleepThread(100000ull);
  }
}
void fdg_leave(int fd) {
  if ((unsigned)fd < FDG_MAX) __atomic_sub_fetch(&g_fdg[fd], 1, __ATOMIC_SEQ_CST);
}
static void fdg_close_begin(int fd) {
  if ((unsigned)fd >= FDG_MAX) return;
  __atomic_or_fetch(&g_fdg[fd], FDG_CLOSING, __ATOMIC_SEQ_CST);
  for (int t = 0; (__atomic_load_n(&g_fdg[fd], __ATOMIC_SEQ_CST) & ~FDG_CLOSING) != 0; t++) {
    if (t >= FDG_CLOSE_TRIES) {
      if (__atomic_add_fetch(&g_fdg_close_timeouts, 1, __ATOMIC_RELAXED) == 1)
        debugPrintf("[io] close(fd=%d) waited 3 s for %u call(s) still inside it and "
                    "closed anyway -- the old unguarded behaviour, for this one close\n",
                    fd, __atomic_load_n(&g_fdg[fd], __ATOMIC_SEQ_CST) & ~FDG_CLOSING);
      return;
    }
    svcSleepThread(100000ull);
  }
}
static void fdg_close_end(int fd) {
  if ((unsigned)fd < FDG_MAX) __atomic_and_fetch(&g_fdg[fd], ~FDG_CLOSING, __ATOMIC_SEQ_CST);
}

long  __pread_chk_fake_unguarded(int fd, void *buf, size_t count, long off, size_t buflen) {
  (void)buflen;
  long cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
  long r = read(fd, buf, count);
  lseek(fd, cur, SEEK_SET);
  return r;
}
long  __pread_chk_fake(int fd, void *buf, size_t count, long off, size_t buflen) {
  if (!fdg_enter(fd)) return -1;
  long r = __pread_chk_fake_unguarded(fd, buf, count, off, buflen);
  fdg_leave(fd);
  return r;
}
void  __FD_SET_chk_fake(int fd, void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) ((unsigned long *)set)[fd / (8 * sizeof(long))] |= (1ul << (fd % (8 * sizeof(long)))); }
int   __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) return (((const unsigned long *)set)[fd / (8 * sizeof(long))] >> (fd % (8 * sizeof(long)))) & 1; return 0; }

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

// Native twin of the android.os.Build.* JNI fields. libunity calls this to
// detect API level / ABI / device; returning "" (the old stub) made it
// mis-detect the platform. Hand back Switch-sane values for the keys engines
// actually query; everything else stays empty (= property unset, the normal
// Android case). Return value is the value length, per bionic contract.
int __system_property_get_fake(const char *name, char *value) {
  if (!value) return 0;
  const char *v = "";
  if (name) {
    if      (!strcmp(name, "ro.build.version.sdk"))        v = "33";
    else if (!strcmp(name, "ro.build.version.release"))    v = "13";
    else if (!strcmp(name, "ro.build.version.codename"))   v = "REL";
    else if (!strcmp(name, "ro.product.cpu.abi"))          v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist"))      v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist64"))    v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abi2"))         v = "";
    else if (!strcmp(name, "ro.product.model"))            v = "Switch";
    else if (!strcmp(name, "ro.product.manufacturer"))     v = "Nintendo";
    else if (!strcmp(name, "ro.product.brand"))            v = "Nintendo";
    else if (!strcmp(name, "ro.product.name"))             v = "Switch";
    else if (!strcmp(name, "ro.product.device"))           v = "Switch";
    else if (!strcmp(name, "ro.product.board"))            v = "nx";
    else if (!strcmp(name, "ro.hardware"))                 v = "nx";
    else if (!strcmp(name, "ro.board.platform"))           v = "nx";
    else if (!strcmp(name, "ro.build.fingerprint"))        v = "Nintendo/Switch/Switch:13/REL/10007:user/release-keys";
    else if (!strcmp(name, "ro.build.characteristics"))    v = "default";
    else if (!strcmp(name, "ro.build.type"))               v = "user";
    else if (!strcmp(name, "ro.build.tags"))               v = "release-keys";
    else if (!strcmp(name, "ro.debuggable"))               v = "0";
    else if (!strcmp(name, "ro.secure"))                   v = "1";
    else if (!strcmp(name, "ro.kernel.qemu"))              v = "0";
    else if (!strcmp(name, "ro.opengles.version"))         v = "196610"; /* GLES 3.2 */
    else if (!strcmp(name, "dalvik.vm.heapsize"))          v = "512m";
    else if (!strcmp(name, "persist.sys.timezone"))        v = "UTC";
  }
  size_t n = strlen(v);
  if (n > 91) n = 91;            /* PROP_VALUE_MAX-1 */
  memcpy(value, v, n); value[n] = '\0';
  return (int)n;
}
unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int gettid_fake(void) {
  u64 tid = 1;
  if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE)) && tid)
    return (int)(tid & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID            178
#define ARM64_SYS_FUTEX             98
#define ARM64_SYS_SCHED_SETAFFINITY 122
#define ARM64_SYS_PROCESS_VM_READV  270
#define ARM64_SYS_PROCESS_VM_WRITEV 271

// futex(2) emulation over libnx mutex+condvar. The il2cpp runtime synchronizes
// its GC, thread pool and locks with raw futex; returning ENOSYS made every
// waiter spin forever (the syscall(98) -> ENOSYS flood) and threading never made
// progress. Wait queues are hashed by uaddr into a bucket array; FUTEX_WAKE wakes
// the whole bucket (waiters re-check *uaddr, so over-broad wakes are harmless).
// The bucket mutex serializes compare-and-sleep against wakers so no wake is lost.
// Infinite waits are capped at 16ms and return as if woken: under load a wake can
// be missed (the Unity Job System / GC otherwise deadlock), and a bounded re-poll
// recovers it safely since the waiter re-checks *uaddr before proceeding.
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CMD_MASK    0x7f  // strip FUTEX_PRIVATE_FLAG(128)/CLOCK_REALTIME(256)
#define FUTEX_BUCKETS     256

/* Longest single condvarWaitTimeout inside a timed futex wait. A libnx condvar
 * wait cannot be interrupted, so one bad deadline would otherwise park a thread
 * indefinitely; slicing caps that exposure while still honouring the caller's
 * real deadline across iterations. */
#define FUTEX_SLICE_NS    50000000ULL   /* 50 ms */

static Mutex   futex_lock[FUTEX_BUCKETS];   // libnx Mutex/CondVar are u32; 0 == ready
static CondVar futex_cond[FUTEX_BUCKETS];

static long futex_impl(volatile int32_t *uaddr, int op, int val, const struct timespec *to) {
  const int cmd = op & FUTEX_CMD_MASK;
  const unsigned h = (unsigned)(((uintptr_t)uaddr >> 4) & (FUTEX_BUCKETS - 1));
  if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
    long ret = 0;
    mutexLock(&futex_lock[h]);
    if (*uaddr != val) {
      errno = EAGAIN; ret = -1;
    } else if (to) {
      /* RELATIVE vs ABSOLUTE. This is the whole bug.
       *
       * Linux futex timeouts are not uniform:
       *   FUTEX_WAIT        -> `to` is a RELATIVE duration
       *   FUTEX_WAIT_BITSET -> `to` is an ABSOLUTE deadline, against
       *                        CLOCK_MONOTONIC, or CLOCK_REALTIME when
       *                        FUTEX_CLOCK_REALTIME (256) is set in `op`
       *
       * This code used to compute tv_sec*1e9 + tv_nsec and pass that straight
       * to condvarWaitTimeout, which takes a RELATIVE nanosecond count. For a
       * BITSET wait with a CLOCK_REALTIME deadline that turns ~1.5e9 seconds of
       * absolute time into a 1.5e18 ns relative wait -- about 48 years.
       *
       * It was doing exactly that: Loading.PreloadManager sat in
       * svcWaitProcessWideKeyAtomic <- condvarWaitTimeout across all three
       * heartbeat dumps, 33 seconds apart, with zero wait/wake deltas, and the
       * watchdog reported its wait as 1537228672.7 seconds. That number IS the
       * absolute deadline being used as a duration. No preload work was ever
       * serviced, so SceneManager.LoadSceneAsync never completed and the
       * bootstrap never fired ApplicationLoadedSignal.
       *
       * The FUTEX_CMD_MASK comment above already noted that bit 256 is the
       * CLOCK_REALTIME flag -- it was stripped from the command but never used
       * to interpret the timeout.
       *
       * Waiting is also sliced. A single long condvarWaitTimeout cannot be
       * interrupted, so one mis-derived deadline parks a thread past any hope
       * of recovery. Slicing bounds the damage to FUTEX_SLICE_NS and lets each
       * iteration re-check *uaddr -- which a correct futex waiter has to
       * tolerate anyway, since spurious wakeups are part of the contract. */
      s64 rel;
      if (cmd == FUTEX_WAIT_BITSET) {
        struct timespec now;
        clock_gettime((op & 256) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
        rel = ((s64)to->tv_sec - (s64)now.tv_sec) * 1000000000LL
            + ((s64)to->tv_nsec - (s64)now.tv_nsec);
      } else {
        rel = (s64)to->tv_sec * 1000000000LL + (s64)to->tv_nsec;
      }
      if (rel < 0) rel = 0;

      u64 remaining = (u64)rel;
      int timed_out = 1;
      while (remaining > 0) {
        u64 slice = remaining > FUTEX_SLICE_NS ? FUTEX_SLICE_NS : remaining;
        if (R_SUCCEEDED(condvarWaitTimeout(&futex_cond[h], &futex_lock[h], slice))) {
          timed_out = 0; break;              /* genuinely woken */
        }
        if (*uaddr != val) { timed_out = 0; break; }  /* value moved under us */
        remaining -= slice;
      }
      if (timed_out) { errno = ETIMEDOUT; ret = -1; }
    } else {
      /* Cast away volatile deliberately: diag_futex_spin only uses the address
       * as an identity for the log, and never dereferences it. */
      diag_futex_spin((const void *)(uintptr_t)uaddr); // beacon: alive-but-spinning
      condvarWaitTimeout(&futex_cond[h], &futex_lock[h], 16000000ULL); // capped infinite wait
    }
    mutexUnlock(&futex_lock[h]);
    return ret;
  }
  if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
    mutexLock(&futex_lock[h]);
    condvarWakeAll(&futex_cond[h]);
    mutexUnlock(&futex_lock[h]);
    return val > 0 ? val : 0; // approximate count woken
  }
  errno = ENOSYS;
  return -1;
}

/* struct nx_iovec lives in libc_shim.h -- see the note there on why newlib
 * forces us to declare the layout ourselves. */

/* Validate that [addr, addr+len) is mapped and readable via svcQueryMemory, so a
 * self process_vm_readv can copy safely instead of risking a fault. */
static int nx_addr_readable(uintptr_t addr, size_t len) {
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == 0) return 0;                 /* MemType_Unmapped */
    if ((mi.perm & Perm_R) == 0) return 0;      /* not readable */
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if (be <= a) return 0;
    a = be;
  }
  return 1;
}

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID: return gettid_fake();
    case ARM64_SYS_FUTEX: {
      va_list va; va_start(va, number);
      volatile int32_t *uaddr = va_arg(va, volatile int32_t *);
      const int op  = va_arg(va, int);
      const int val = va_arg(va, int);
      const struct timespec *to = va_arg(va, const struct timespec *);
      va_end(va);
      return futex_impl(uaddr, op, val, to);
    }
    case ARM64_SYS_SCHED_SETAFFINITY:
      return 0; // affinity hints are advisory; pretend success
    case ARM64_SYS_PROCESS_VM_READV:
    case ARM64_SYS_PROCESS_VM_WRITEV: {
      /* Self memory copy used as a fault-safe read/write probe. Stubbing it to
       * ENOSYS made the caller spin once per frame (a process_vm_readv flood),
       * wedging the boot path. Implement it for the own-process case: validate
       * each remote range with svcQueryMemory, then copy the readable parts. */
      va_list va; va_start(va, number);
      long pid                   = va_arg(va, long); (void)pid;
      const struct nx_iovec *liov   = va_arg(va, const struct nx_iovec *);
      unsigned long lcnt         = va_arg(va, unsigned long);
      const struct nx_iovec *riov   = va_arg(va, const struct nx_iovec *);
      unsigned long rcnt         = va_arg(va, unsigned long);
      va_end(va);
      int writing = (number == ARM64_SYS_PROCESS_VM_WRITEV);
      static int dbg = 0;
      if (dbg < 5) {
        dbg++;
        debugPrintf("[sys%ld] %s lcnt=%lu rcnt=%lu remote0=%p rlen0=%zu caller=%p\n",
                    number, writing ? "vm_writev" : "vm_readv", lcnt, rcnt,
                    rcnt ? riov[0].iov_base : NULL, rcnt ? riov[0].iov_len : 0,
                    __builtin_return_address(0));
      }
      ssize_t total = 0;
      unsigned long li = 0, ri = 0; size_t lo = 0, ro = 0;
      while (li < lcnt && ri < rcnt) {
        char *lp = (char *)liov[li].iov_base + lo;
        char *rp = (char *)riov[ri].iov_base + ro;
        size_t lrem = liov[li].iov_len - lo, rrem = riov[ri].iov_len - ro;
        size_t n = lrem < rrem ? lrem : rrem;
        char *probe = writing ? lp : rp;   /* the side being read-from must be readable */
        if (!nx_addr_readable((uintptr_t)probe, n)) {
          if (total == 0) { errno = EFAULT; return -1; }
          return total;
        }
        if (writing) memcpy(rp, lp, n); else memcpy(lp, rp, n);
        total += (ssize_t)n; lo += n; ro += n;
        if (lo == liov[li].iov_len) { li++; lo = 0; }
        if (ro == riov[ri].iov_len) { ri++; ro = 0; }
      }
      return total;
    }
  }
  debugPrintf("libc: syscall(%ld) -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
int sched_get_priority_max_fake(int policy) { (void)policy; return 0; }
int sched_get_priority_min_fake(int policy) { (void)policy; return 0; }
void android_set_abort_message_fake(const char *msg) { debugPrintf("abort message: %s\n", msg ? msg : "(null)"); }
size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
int __register_atfork_fake(void) { return 0; }
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    // Report 512 MB (matches synthetic /proc/meminfo MemTotal) to make Unity's
    // DynamicHeap reserve fewer 256MB regions; real backing (arena/OC) holds more.
    case BIONIC_SC_PHYS_PAGES: return (512ll * 1024 * 1024) / 0x1000;
    default: return -1;
  }
}
long pathconf_fake(const char *path, int name) { (void)path; (void)name; return -1; }

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

// The engine addresses asset packs as "<packdir>/<file>.mvgl" but we ship the
// data flat in the game dir. If a read path with a subdirectory is missing,
// fall back to just its basename in the cwd (the game dir). Reads only -- never
// redirect a write -- and only when the basename actually exists.
static int basename_fallback(const char *path, char *out, size_t outsz) {
  const char *slash = strrchr(path, '/');
  if (!slash || !slash[1]) return 0;   // no subdir component to strip
  struct stat st;
  snprintf(out, outsz, "%s", slash + 1); // basename, resolved against the cwd
  return stat(out, &st) == 0;
}

// Create one directory, skipping paths newlib's mkdir() can't handle safely.
// A bare "device:" path (e.g. "sdmc:") makes newlib resolve to a device root
// with an empty in-device path and dereference a NULL devoptab -- a Data Abort
// reading devoptab->mkdir_r at +0x68. Refuse those (and null/empty).
static int safe_mkdir(const char *p) {
  if (!p || !*p) { errno = EINVAL; return -1; }
  const char *colon = strchr(p, ':');
  if (colon) {                       // has a "device:" prefix
    const char *in = colon + 1;      // the path inside the device
    while (*in == '/') in++;
    if (!*in) { errno = EEXIST; return 0; }  // "sdmc:" / "sdmc:/" -> root, skip
    // A single top-level component ("sdmc:/switch") also null-derefs newlib's
    // devoptab. Such dirs (the homebrew mount point) always pre-exist already.
    if (!strchr(in, '/')) { errno = EEXIST; return 0; }
  }
  return mkdir(p, 0777);
}

// mkdir -p: create `dir` and every missing parent. Save data lives in subdirs
// the engine only mkdir()s one level at a time, so a deeper missing parent left
// the whole chain (and the save write) failing.
//
// We must NOT try to create the game root or any ancestor of it ("sdmc:",
// "sdmc:/switch", "sdmc:/switch/zookeeper"): they already exist, they aren't
// ours, and newlib's mkdir() of a *top-level* path (one component under the
// device, e.g. "sdmc:/switch") null-derefs its devoptab -> Data Abort at the
// mkdir_r slot (+0x68). So begin the parent walk *after* GAME_HOME.
static void mkdir_p_dir(const char *dir) {
  if (!dir || !*dir) return;
  char tmp[512];
  if (snprintf(tmp, sizeof(tmp), "%s", dir) <= 0) return;
  size_t skip;
  const char *root = bp_game_root();
  const size_t glen = strlen(root);
  if (strncmp(tmp, root, glen) == 0 && (tmp[glen] == '/' || tmp[glen] == '\0')) {
    skip = glen;                                  // only create *under* the game root
  } else {
    const char *colon = strchr(tmp, ':');         // unknown base: at least skip "device:"
    skip = colon ? (size_t)(colon + 1 - tmp) : 0;
  }
  for (char *p = tmp + skip + 1; *p; p++)
    if (*p == '/') { *p = '\0'; safe_mkdir(tmp); *p = '/'; }
  if (tmp[skip]) safe_mkdir(tmp);
}
// create the parent directory chain of a file path
static void mkdir_parents(const char *filepath) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", filepath);
  char *last = strrchr(tmp, '/');
  if (!last || last == tmp) return;
  *last = '\0';
  mkdir_p_dir(tmp);
}

// mkdir wrapper: create the full chain and treat "already exists" as success
int mkdir_fake(const char *path, unsigned mode) {
  (void)mode;
  if (!path || !*path) { errno = EINVAL; return -1; }
  mkdir_p_dir(path);
  int r = safe_mkdir(path);
  if (r != 0 && errno == EEXIST) r = 0;
  return r;
}

int g_watch_fd = -1;   /* data.unity3d fd: trace its reads/seeks to debug header load */
void watch_dump(const char *tag, int fd, long a, long b, const void *buf, long got) {
  if (fd != g_watch_fd) return;
  char h[64]; int n = (got > 16 ? 16 : (got < 0 ? 0 : (int)got));
  int p = 0; for (int i = 0; i < n; i++) p += snprintf(h + p, sizeof(h) - p, "%02x ", ((const unsigned char *)buf)[i]);
  h[p] = 0;
  if (TRACE_IO) debugPrintf("[io] %s fd=%d a=%ld b=%ld -> %ld  [%s]\n", tag, fd, a, b, got, h);
}

/* ---- read-ahead cache for big archive files (data.unity3d, sharedassets) ----
 * Unity deserializes archives with THOUSANDS of tiny read()s (2-8 bytes each,
 * field by field). On Android the archive sits in the OS page cache so these are
 * memory-fast; on Switch there is no page cache, so every tiny read is a direct
 * SD access (~ms) and boot crawls (8000+ reads just for the header). We buffer
 * each big read-only fd through a 1MB window filled by one large read, and
 * virtualize the logical file position -- turning ~250k tiny SD reads per MB into
 * a single one. Keyed by fd; the real fd position is used only as our scratch. */
/* 48, not 8: a resident file occupies a slot for as long as it is open, and the
 * game keeps ~30 AssetBundles open at once. With 8 slots most of them would fall
 * back to window mode and the residency would quietly do nothing. */
#define RA_SLOTS 48
#define RA_WIN   (1u << 20)     /* 1 MB read-ahead window */
static struct RaCache {
  int  fd;           /* -1 == free */
  long pos;          /* virtual file position (what read/lseek observe) */
  long size;         /* file size (for SEEK_END) */
  long base;         /* file offset of buf[0] */
  long len;          /* valid bytes currently in buf */
  unsigned char *buf;
  int  resident;     /* buf is a borrowed whole-file blob, not our window */
  int  used;         /* slot occupied. See below -- this is not cosmetic. */
  const char *blobpath;  /* borrowed from the blob store; for diagnostics only */
} g_ra[RA_SLOTS];
/* `used` replaces the old "fd < 0 means free" test, which never worked.
 * g_ra has static storage, so every slot starts with fd == 0 -- never < 0 --
 * and ra_attach's search for a free slot found none, every time. The read-ahead
 * cache has therefore been inert since it was written, here and in the parent
 * port. Worse, ra_find(0) matched slot 0 on fd equality, so if the loader ever
 * came up with descriptor 0 free, the first read through it would dereference a
 * NULL window buffer. Zero-initialised `used` means "free", which is what static
 * storage actually gives us. */

/* ---------------------------------------------------------------------------
 * RESIDENT FILES
 *
 * The read-ahead window above already turns Unity's tiny field reads into one
 * SD read per megabyte. Residency goes further: the whole file is held in RAM
 * and the window never refills, so a bundle that is loaded, closed and loaded
 * again is read from the card exactly once per boot.
 *
 * Keyed by PATH, not by fd, and never freed. That is the point -- Unity opens
 * and closes AssetBundles repeatedly, and an fd-keyed cache would re-read the
 * file every time. "Never freed" is affordable because the budget below is
 * fixed and small next to the 2.9 GB newlib heap this port is granted.
 *
 * ra_read() needs no changes for this: a resident slot is simply one whose
 * window already covers [0, size), so its refill branch never runs.
 * ------------------------------------------------------------------------ */
/* 30 cached bundles today, plus whatever else clears the rule. Entries are
 * 200-odd bytes each, so headroom here is free; the real limit is the byte
 * budget, not the slot count. */
#define BLOB_MAX 96
#define BLOB_HEAD 4096
static Mutex g_ram_lock;             /* defined below with the budget; the scan needs it early */
static struct FileBlob {
  char path[192];
  unsigned char *data;
  long size;
  unsigned char head[BLOB_HEAD];   /* reference copy of the first page, at load */
  uint32_t crc;                    /* whole-blob checksum, at load */
  int bad;                         /* already reported corrupt */
} g_blob[BLOB_MAX];
static int   g_blob_count;

/* ONE IMAGE, NEVER FREED.
 *
 * The blob store used to be a malloc per file, with the pointer lent to every
 * descriptor that opened the file and three separate paths that could free it
 * (fb_invalidate, the pressure release, the budget trim) while a slot still
 * pointed at it. fb_invalidate's own comment records one use-after-free of
 * exactly that shape. The asset pack -- 1,224 files, zero incidents across
 * every run -- and the PvZ port's block cache both do the opposite: one arena
 * allocated once, never freed, descriptors hold offsets into it.
 *
 * So: the freeze walk sizes the resident set, one allocation is made for the
 * lot, and fb_load_now carves from it in order, committing the carve only
 * after the load and the verify succeed. Dropping an entry marks it dead and
 * leaves the bytes where they are. Nothing hands out a pointer that anything
 * can free. If the "zeros on re-open" corruption survives this, it was never
 * the lifecycle and the canary and blob scan are what will find it. */
static unsigned char *g_image;
static size_t         g_image_cap, g_image_used;
static Mutex          g_load_lock;   /* one load at a time: the carve is tentative until commit */

/* Plain CRC-32 (IEEE), table built once. Boot cost: 243 MB once. */
static uint32_t blob_crc(const unsigned char *p, size_t n) {
  static uint32_t T[256]; static int init;
  if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; } init = 1; }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = T[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/* THE INTEGRITY SCAN. No privileged calls, no page tricks: compare each
 * blob's first page against the copy taken at load, every frame, from the
 * main thread. 30 x 4 KB is nothing. The first mismatch is reported with the
 * frame, the entry, the offset of the first differing byte, what is there now
 * versus what was loaded, how many bytes of the page differ, whether the
 * damage starts on a page boundary (a mapping event) or not (a store), and
 * whether the rest of the blob still checksums -- so "zeros" becomes a shape
 * with a frame number, and the log around that frame names the event. A full
 * checksum is also run at every close of a resident entry, which is rare, to
 * catch damage past the first page. */
static void blob_report(int i, const char *when, unsigned frame) {
  struct FileBlob *b = &g_blob[i];
  if (b->bad) return;
  const size_t n = b->size < (long)BLOB_HEAD ? (size_t)b->size : BLOB_HEAD;
  size_t first = n, diff = 0;
  for (size_t k = 0; k < n; k++)
    if (b->data[k] != b->head[k]) { if (first == n) first = k; diff++; }
  const uint32_t now = blob_crc(b->data, (size_t)b->size);
  if (first == n && now == b->crc) return;               /* intact */
  b->bad = 1;
  const uintptr_t at = (uintptr_t)b->data + first;
  debugPrintf("[log] BLOB CORRUPTED %s at frame %u (%s): first page %s (first diff at +%zu = %p, %s page boundary; "
              "now %02x, loaded %02x; %zu of %zu bytes differ), whole blob %s (crc now %08x, loaded %08x)\n",
              b->path, frame, when,
              first == n ? "intact" : "DAMAGED", first, (void *)at,
              (at & 0xfff) == 0 ? "ON a" : "not on a",
              first < n ? b->data[first] : 0, first < n ? b->head[first] : 0, diff, n,
              now == b->crc ? "intact" : "DAMAGED", now, b->crc);
}
void bp_ram_blob_scan(unsigned frame) {          /* main thread, once per frame */
  /* Find under the lock, report outside it: blob_report logs, and logging
   * under g_ram_lock would park every open of a resident file behind the log
   * lock. The entry is marked bad before the lock drops so it is reported once. */
  int hit = -1;
  mutexLock(&g_ram_lock);
  for (int i = 0; i < g_blob_count && hit < 0; i++) {
    struct FileBlob *b = &g_blob[i];
    if (b->bad || !b->data) continue;
    const size_t n = b->size < (long)BLOB_HEAD ? (size_t)b->size : BLOB_HEAD;
    if (memcmp(b->data, b->head, n) != 0) hit = i;
  }
  mutexUnlock(&g_ram_lock);
  if (hit >= 0) blob_report(hit, "frame scan", frame);
}
static long  g_ram_budget = -1;          /* bytes left; -1 = not yet initialised */

/* Shared with asset_pack.c, which holds the .nxpack the same way. */
long bp_ram_cache_take(long bytes) {
  if (bytes <= 0) return 0;
  mutexLock(&g_ram_lock);
  if (g_ram_budget < 0) g_ram_budget = (long)bp_ram_cache_mb << 20;
  long got = (bytes <= g_ram_budget) ? bytes : 0;
  g_ram_budget -= got;
  mutexUnlock(&g_ram_lock);
  return got;
}
void bp_ram_cache_give(long bytes) {
  if (bytes <= 0) return;
  mutexLock(&g_ram_lock);
  g_ram_budget += bytes;
  mutexUnlock(&g_ram_lock);
}

/* ---------------------------------------------------------------------------
 * WHERE DO "DOWNLOADED" BYTES ACTUALLY COME FROM?
 *
 * A re-fetch of three bundles (41 MB) reportedly completes faster than the
 * connection could deliver it. That is a real clue and it fits none of the
 * explanations offered so far, so measure it rather than argue about it: watch
 * every descriptor opened for writing under UnityCache, count the bytes and
 * time them, and report on close.
 *
 * 41 MB in eight seconds is a CDN. 41 MB in a fifth of a second is not a
 * network at all, and then the question becomes which local source is feeding
 * it -- which the path and size will narrow down immediately.
 * ------------------------------------------------------------------------ */
/* Queued, non-blocking log line; the ring and its drain are defined further
 * down. Declared this early because everything below it that runs on one of
 * Unity's worker threads must use it rather than debugPrintf -- writing to the
 * card from those threads is what stalled the log lock past the watchdog's
 * limit twice. */
static void tr_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define DLW_MAX 16
static struct { int fd; char path[160]; long bytes; uint64_t t0; } g_dlw[DLW_MAX];
static Mutex g_dlw_lock;

static void dlw_open(int fd, const char *path) {
  if (!path || !strstr(path, "/UnityCache/")) return;
  mutexLock(&g_dlw_lock);
  for (int i = 0; i < DLW_MAX; i++)
    if (!g_dlw[i].fd) {
      g_dlw[i].fd = fd + 1;                 /* +1 so 0 stays "free" */
      snprintf(g_dlw[i].path, sizeof g_dlw[i].path, "%s", path);
      g_dlw[i].bytes = 0;
      g_dlw[i].t0 = armTicksToNs(armGetSystemTick());
      break;
    }
  mutexUnlock(&g_dlw_lock);
}
static void dlw_wrote(int fd, long n) {
  if (n <= 0) return;
  mutexLock(&g_dlw_lock);
  for (int i = 0; i < DLW_MAX; i++)
    if (g_dlw[i].fd == fd + 1) { g_dlw[i].bytes += n; break; }
  mutexUnlock(&g_dlw_lock);
}
static void dlw_close(int fd) {
  long b = 0; uint64_t ms = 0; char p[160]; p[0] = 0;
  mutexLock(&g_dlw_lock);
  for (int i = 0; i < DLW_MAX; i++)
    if (g_dlw[i].fd == fd + 1) {
      b = g_dlw[i].bytes;
      ms = (armTicksToNs(armGetSystemTick()) - g_dlw[i].t0) / 1000000ull;
      snprintf(p, sizeof p, "%s", g_dlw[i].path);
      g_dlw[i].fd = 0;
      break;
    }
  mutexUnlock(&g_dlw_lock);
  if (b > 64 * 1024)
    tr_log("[dl] wrote %ld KB to %s in %llu ms (%llu KB/s)\n",
                b >> 10, p, (unsigned long long)ms,
                (unsigned long long)(ms ? (unsigned long long)(b >> 10) * 1000ull / ms : 0));
}

/* The path a blob came from, for diagnostics. Borrowed; valid while held. */
static const char *fb_path_of(const unsigned char *blob) {
  for (int i = 0; i < g_blob_count; i++)
    if (g_blob[i].data == blob) return g_blob[i].path;
  return NULL;
}

/* ---------------------------------------------------------------------------
 * LOADING HAPPENS ON A BACKGROUND THREAD, NEVER INSIDE open().
 *
 * The first version read the whole file synchronously the first time it was
 * opened. That put an SD read of the file's entire length inside a single
 * open() call -- about a second for sfx-ingame (22 MB) or ui-variants (17 MB) --
 * on whatever thread Unity happened to be loading on. The hardware log shows
 * sixteen entries and 161 MB going resident between frames 240 and 300, the
 * watchdog sampling a stalled main thread throughout, and the run dying in the
 * middle of it.
 *
 * A cache that makes the first access slower than not having it is not a cache.
 * So open() never blocks now: an eligible file is QUEUED, open() returns
 * immediately and that read goes to the card as it always did, and a later open
 * of the same file finds the copy ready. For UnityCache bundles -- opened and
 * closed repeatedly, which is the whole reason they are worth holding -- that
 * costs one ordinary open and wins every one after it.
 * ------------------------------------------------------------------------ */
#define PFQ_MAX 64
static struct { char path[192]; long size; } g_pfq[PFQ_MAX];
static int   g_pfq_head, g_pfq_tail;
static Mutex g_pfq_lock;
static CondVar g_pfq_cv;
static Thread  g_pfq_thread;
static int     g_pfq_running;

static unsigned char *fb_lookup(const char *path, long size);
static void fb_load_now(const char *path, long size);

static void pfq_push(const char *path, long size) {
  mutexLock(&g_pfq_lock);
  const int next = (g_pfq_tail + 1) % PFQ_MAX;
  if (next != g_pfq_head) {                       /* silently drop when full */
    int dup = 0;
    for (int i = g_pfq_head; i != g_pfq_tail; i = (i + 1) % PFQ_MAX)
      if (!strcmp(g_pfq[i].path, path)) { dup = 1; break; }
    if (!dup) {
      snprintf(g_pfq[g_pfq_tail].path, sizeof g_pfq[0].path, "%s", path);
      g_pfq[g_pfq_tail].size = size;
      g_pfq_tail = next;
      condvarWakeOne(&g_pfq_cv);
    }
  }
  mutexUnlock(&g_pfq_lock);
}

/* When the game last read through this shim. The prefetch thread waits for a
 * gap before touching the card at all.
 *
 * Not blocking open() was necessary but not sufficient: a background thread
 * reading 100+ MB still saturates the SD device, and every read the game makes
 * queues behind it. The hardware log shows the watchdog sampling a stalled main
 * thread all through residency loading. Reads are chunked and only issued when
 * the game has been quiet, so filling the cache can never make the game wait. */
static volatile uint64_t g_last_game_read;

/* Is the cache actually doing anything?
 *
 * Residency loads a file on the SECOND open, so it only pays off if the game
 * opens the same file again. If it opens each bundle once per session, every
 * megabyte held is a megabyte that bought nothing -- which would explain
 * "loading doesn't really seem improved" exactly, and is not something to guess
 * at when it can be counted. */
static volatile uint64_t g_bytes_from_ram, g_bytes_from_card;

static void pfq_wait_for_quiet(void) {
  for (;;) {
    const uint64_t now = armTicksToNs(armGetSystemTick());
    const uint64_t last = g_last_game_read;
    if (now - last > 250000000ull) return;        /* 250 ms with no game read */
    svcSleepThread(50000000ull);                  /* 50 ms, then look again   */
  }
}

static void pfq_main(void *arg) {
  (void)arg;
  for (;;) {
    char path[192]; long size;
    mutexLock(&g_pfq_lock);
    while (g_pfq_running && g_pfq_head == g_pfq_tail)
      condvarWaitTimeout(&g_pfq_cv, &g_pfq_lock, 200000000ull);
    if (!g_pfq_running) { mutexUnlock(&g_pfq_lock); return; }
    snprintf(path, sizeof path, "%s", g_pfq[g_pfq_head].path);
    size = g_pfq[g_pfq_head].size;
    g_pfq_head = (g_pfq_head + 1) % PFQ_MAX;
    mutexUnlock(&g_pfq_lock);
    fb_load_now(path, size);                      /* the slow part, off the
                                                   * game's threads entirely */
  }
}

/* Once a minute: what the cache is actually buying. */
void bp_ram_report(void) {
  static uint64_t next_ns;
  const uint64_t now = armTicksToNs(armGetSystemTick());
  if (now < next_ns) return;
  /* 10 seconds, not 60, and 1 MB rather than 4. Runs that end in five or nine
   * seconds are the norm while something is being chased, and a report that
   * needs a minute of uptime never appears in any of them -- this accounting
   * has been in the build for several rounds and has printed in exactly one. */
  next_ns = now + 10ull * 1000000000ull;
  const uint64_t ram = g_bytes_from_ram, card = g_bytes_from_card;
  const uint64_t tot = ram + card;
  if (tot < (1u << 20)) return;
  debugPrintf("[ram] served %llu MB from RAM, %llu MB from the card (%llu%% cached)\n",
              (unsigned long long)(ram >> 20), (unsigned long long)(card >> 20),
              (unsigned long long)(tot ? ram * 100 / tot : 0));
}

void bp_ram_prefetch_start(void) {
  if (g_pfq_running || bp_ram_cache_mb <= 0) return;
  g_pfq_running = 1;
  /* 0x3B is below every game thread: this must never take CPU from rendering. */
  if (R_FAILED(threadCreate(&g_pfq_thread, pfq_main, NULL, NULL, 0x4000, 0x3B, -2)) ||
      R_FAILED(threadStart(&g_pfq_thread))) {
    g_pfq_running = 0;
    debugPrintf("[ram] could not start the prefetch thread -- residency disabled\n");
  }
}

/* Returns a borrowed pointer to the whole file, or NULL to use window mode.
 * Caller must already know the file is read-only and worth caching. */
static unsigned char *fb_resident(const char *path, long size) {
  if (size <= 0 || size > ((long)BP_RAM_RESIDENT_MAX_MB << 20)) return NULL;
  unsigned char *hit = fb_lookup(path, size);
  if (hit) return hit;
  /* Not held yet. Queue it and get out of the way -- this open must not wait
   * on an SD read of the whole file. */
  pfq_push(path, size);
  return NULL;
}

/* ---------------------------------------------------------------------------
 * PROTECT COMMITTED CACHE ENTRIES
 *
 * Entries under files/UnityCache/Shared keep disappearing, and the routes have
 * been chased one at a time -- unlink, remove, unlinkat, rename, rmdir -- with a
 * new one turning up each round. Rather than keep guessing which call does it,
 * refuse them all.
 *
 * NOT the whole directory, though. Unity stages a download at <hash>_tmp/
 * __data_tmp and cleans it up afterwards; blocking that would leave litter and
 * break the one path that legitimately writes here. Only COMMITTED entries are
 * protected -- anything under Shared/ whose path does not contain "_tmp".
 *
 * And not always. Offline, a download cannot succeed, so every write here is a
 * commit that will fail and a delete that costs a file the game already had.
 * Online, a real update must be able to land. So the default is "locked while
 * offline", with config.txt able to force it either way.
 * ------------------------------------------------------------------------ */
int bp_cache_lock_mode = 1;                 /* 0 = never, 1 = when offline, 2 = always */

static int cache_locked_now(const char *path) {
  if (!path || bp_cache_lock_mode == 0) return 0;
  if (!strstr(path, "/UnityCache/Shared/")) return 0;
  if (strstr(path, "_tmp")) return 0;       /* Unity's own staging: leave alone */
  if (bp_cache_lock_mode == 2) return 1;
  { extern int bp_net_is_offline(void); return bp_net_is_offline(); }
}

/* __data and __info are not the same thing and must not be treated the same.
 *
 * __data is content, addressed by the hash in its own path. It never changes,
 * so refusing writes to it costs nothing.
 *
 * __info is BOOKKEEPING. Unity opens it for writing every time it USES an
 * entry, to stamp the last-used time. The first version of this lock refused
 * that -- and the log shows it refusing all thirty on one boot:
 *
 *   [cache] BLOCKED write-open of .../Shared/ui-variants/<hash>/__info
 *
 * Refusing an engine's own bookkeeping is a fine way to make it decide the
 * entry is unusable and fetch a fresh copy. Writes to __info are allowed;
 * only its DELETION is refused, because an entry without it counts as
 * incomplete and is as lost as one without content. */
static int is_info_file(const char *path) {
  const char *b = strrchr(path, '/');
  return b && !strcmp(b, "/__info");
}
static int cache_protected_write(const char *path) {
  return cache_locked_now(path) && !is_info_file(path);
}
static int cache_protected_delete(const char *path) {
  return cache_locked_now(path);
}

/* Deletes are answered with success rather than an error: the caller is trying
 * to tidy up something it believes is stale, and telling it the delete failed
 * invites a retry or an error path. The file simply stays. */
void bp_tr_dump(const char *path, const char *why);   /* defined below */

int bp_cache_block_delete(const char *path, const char *how) {
  if (!cache_protected_delete(path)) return 0;
  static unsigned n;
  if (n < 20) { n++;
    tr_log("[cache] BLOCKED %s of %s -- committed entries are read-only "
                "while offline\n", how, path);
    bp_tr_dump(path, how); }
  return 1;
}

/* ---------------------------------------------------------------------------
 * PER-ENTRY TRACE FOR UnityCache CONTENT
 *
 * The verifiers have ruled out the data: stored copies match their files, and
 * served bytes match the card read for read. So whatever differs when residency
 * is on, it is not the contents -- which leaves what the descriptor looks like
 * afterwards, what fstat reports, and the order and shape of the calls.
 *
 * Those are observable, so observe them rather than reason about them. Every
 * operation on a Shared/<name>/<hash>/__data file is counted per entry, and
 * when something deletes that entry the counters are printed. "Unity opened it
 * twice, read 64 bytes, never read the rest, then deleted it" is a diagnosis;
 * "the game evicts entries" is not.
 *
 * Counters are cheap. The raw line trace is bounded, because a 30-entry cache
 * read end to end would otherwise fill the card.
 * ------------------------------------------------------------------------ */
/* TRACE LINES NEVER TOUCH THE CARD FROM A READ.
 *
 * The first version called debugPrintf() straight from read_fake(). That is the
 * hottest path in the program, Unity reads bundles on its Background Job
 * workers, and debugPrintf takes the log lock and blocks on an SD flush. 165
 * lines in, the watchdog reported the log lock held for over two seconds by
 * "Background Job.Worker 15" and the process broke.
 *
 * util.h has warned about this since before any of my work, PORTING.md records
 * the parent port hitting it, section 36 records me doing it in the RAM cache
 * and adding lockcheck.py for it -- and lockcheck passed this build, because the
 * call is not inside a mutex I take. The hazard was never the mutex. It is doing
 * blocking I/O on a thread the game needs.
 *
 * So lines go into a ring in memory -- a vsnprintf under a lock held for
 * microseconds, no I/O -- and the frame loop drains them on the main thread,
 * where a blocking write is already normal. */
/* SLOT WIDTH IS LOAD-BEARING, and 176 was too narrow.
 *
 * vsnprintf() truncates at TRQ_LINE-1 and the '\n' is the LAST byte of every
 * line here, so an over-long line loses exactly its newline and the next line
 * drained gets glued onto its tail. debug.log from the last run has nine such
 * splices and every one is at column 175, which is what makes this a measurement
 * rather than a suspicion.
 *
 * The line it costs most is the eviction dump itself: with a real entry key it
 * formats to 206 characters, so the "KB from card" and "last offset" fields --
 * the ones that would say whether Unity ever read the bundle before deleting it
 * -- were being cut off every time.
 *
 * 256 clears the longest line here (the dump, 206) with room to spare. Cost is
 * 512 * 80 extra bytes = 40 KB of static, against a 2982 MB heap. */
#define TRQ_MAX  512
#define TRQ_LINE 256
static char g_trq[TRQ_MAX][TRQ_LINE];
static unsigned g_trq_head, g_trq_tail, g_trq_dropped, g_trq_cut;
static Mutex g_trq_lock;

static void tr_log(const char *fmt, ...) {
  mutexLock(&g_trq_lock);
  const unsigned next = (g_trq_tail + 1u) % TRQ_MAX;
  if (next == g_trq_head) {
    g_trq_dropped++;                      /* full: drop, never block a reader */
  } else {
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(g_trq[g_trq_tail], TRQ_LINE, fmt, ap);
    va_end(ap);
    /* Widening the slot is not a guarantee, so make truncation self-reporting
     * instead of silent: put the newline back and mark the cut. A short line
     * ending in '~' is obvious; a line with no newline at all is invisible
     * until you go looking at columns. */
    if (n >= TRQ_LINE) {
      g_trq[g_trq_tail][TRQ_LINE - 3] = '~';
      g_trq[g_trq_tail][TRQ_LINE - 2] = '\n';
      g_trq[g_trq_tail][TRQ_LINE - 1] = '\0';
      g_trq_cut++;
    }
    g_trq_tail = next;
  }
  mutexUnlock(&g_trq_lock);
}

/* A pre-formatted line from another translation unit. */
void bp_tr_log_line(const char *line) { tr_log("%s", line); }

/* Called once per frame from bp_boot.c, on the main thread. Bounded per call so
 * draining a full ring cannot itself become the stall. */
void bp_tr_flush(void) {
  for (int i = 0; i < 24; i++) {
    char line[TRQ_LINE];
    mutexLock(&g_trq_lock);
    if (g_trq_head == g_trq_tail) { mutexUnlock(&g_trq_lock); break; }
    memcpy(line, g_trq[g_trq_head], sizeof line);
    g_trq_head = (g_trq_head + 1u) % TRQ_MAX;
    mutexUnlock(&g_trq_lock);
    /* debugPuts, NOT debugPrintf("%s", line): the format string is what the
     * importance test reads, so the old call classified every drained line as
     * the literal "%s" and nothing here could ever force a flush. */
    debugPuts(line);                       /* outside the lock, on the main thread */
  }
  /* Report losses whenever there are any. This used to sit inside the
   * head==tail branch, so a ring that never fully drained -- which is the state
   * it is in during exactly the bundle-load burst worth watching -- never
   * reported a single drop.
   *
   * Straight to debugPuts, NOT tr_log: a report that the ring overflowed, queued
   * into the ring that just overflowed, is the one line guaranteed to be lost.
   * This runs on the main thread, where a direct log call is already normal. */
  unsigned dropped, cut;
  mutexLock(&g_trq_lock);
  dropped = g_trq_dropped; g_trq_dropped = 0;
  cut     = g_trq_cut;     g_trq_cut     = 0;
  mutexUnlock(&g_trq_lock);
  if (dropped) { char m[96];
    snprintf(m, sizeof m, "[log] %u trace lines dropped: the ring filled\n", dropped);
    debugPuts(m); }
  if (cut) { char m[96];
    snprintf(m, sizeof m, "[log] %u trace lines were too long for a ring slot\n", cut);
    debugPuts(m); }
}

#define TR_MAX 64
/* One recorded syscall on a cache entry's descriptor. 24 bytes; the buffer is
 * malloc'd on first use so unopened entries cost nothing. */
struct TrOp { uint8_t op; uint8_t whence; uint8_t path; int32_t got; int64_t pos; int64_t want; };
#define TR_OPS 2048
static struct TrEntry {
  char key[176];
  unsigned opens, reads, lseeks, fstats, closes, short_reads;
  unsigned long long bytes_ram, bytes_card;
  long last_off, file_size;
  int resident;
  struct TrOp *ops; unsigned nops, ops_dropped;
  unsigned reads_this_open;      /* reset at bind; for the ram_delay_ms experiment */
} g_tr[TR_MAX];
static int   g_tr_n;
static Mutex g_tr_lock;
static unsigned g_tr_lines;

/* THE SYSCALL SEQUENCE, VERBATIM, FOR THE A/B.
 *
 * With ram_cache on, Unity evicts the three bundles whose blocks-info sits at
 * the END of the file; with it off, it does not. The bytes served are verified
 * identical to the card, and the one short read is a correct EOF-bounded read
 * the card would answer the same way. Every path a read/seek/pread can take on
 * these descriptors has been traced on paper and looks equivalent -- and the
 * paper has been wrong before. So stop reasoning about the sequence and record
 * it: every open, read, pread, lseek, fstat and close on the entry, with the
 * position before, the bytes asked for and the bytes returned. Written out as
 * ONE block when the descriptor closes, through the lock-free committed writer,
 * so recording costs nothing during the reads and the block lands whole. Two
 * runs, one diff, and whatever differs is the answer.
 *
 * op: o=open r=read(RAM) R=read(window) C=read(card loop) p=pread s=lseek
 *     f=fstat c=close   pos=position before   want=bytes asked   got=result */
static void tr_op(struct TrEntry *e, char op, int64_t pos, int64_t want, int64_t got, int whence, int path) {
  if (!e || !bp_diag_io) return;                /* heavy: see config.h diag_io */
  mutexLock(&g_tr_lock);
  if (!e->ops) e->ops = calloc(TR_OPS, sizeof *e->ops);
  if (e->ops && e->nops < TR_OPS) {
    struct TrOp *o = &e->ops[e->nops++];
    o->op = (uint8_t)op; o->whence = (uint8_t)whence; o->path = (uint8_t)path;
    o->got = (int32_t)(got > INT32_MAX ? INT32_MAX : got < INT32_MIN ? INT32_MIN : got);
    o->pos = pos; o->want = want;
  } else e->ops_dropped++;
  mutexUnlock(&g_tr_lock);
}
static void tr_ops_dump(struct TrEntry *e, int fd) {
  if (!e || !e->ops || !e->nops || !bp_diag_io) return;
  extern int iolog_write(const char *s, size_t n);
  /* HEAP, NOT BSS. The first version of this was a 131 KB static array, and
   * the first hardware run took a WRITE PERMISSION FAULT 83 KB into it, at a
   * page boundary, while snprintf was filling it: pc in _dtoa_r, FAR at
   * out+0x14430, DFSC 0x0f. A page in the middle of this process's bss is
   * mapped read-only, and nothing in the port asks for that. main.c now walks
   * the bss at boot and reports any such page; until that says why, no
   * diagnostic gets a large static buffer. The heap is proven writable. */
  /* PER CALL, not shared. The first io.log had its first block twice, the
   * first copy cut mid-line: this buffer was static, formatted under
   * g_tr_lock and then written OUTSIDE it, so a second close could refill it
   * while the first write() was still reading it. One buffer per dump. */
  enum { OUT_CAP = TR_OPS * 64 + 512 };
  char *out = malloc(OUT_CAP);
  if (!out) return;
  size_t n = 0;
  mutexLock(&g_tr_lock);
  n += (size_t)snprintf(out + n, OUT_CAP - n,
                        "=== %s fd=%d open#%u: %u ops%s (size %ld, %s) ===\n",
                        e->key, fd, e->opens, e->nops,
                        e->ops_dropped ? " (buffer full, some dropped)" : "",
                        e->file_size, e->resident ? "RAM" : "card");
  static const char *WH[] = { "SET", "CUR", "END" };
  for (unsigned i = 0; i < e->nops && n < OUT_CAP - 96; i++) {
    const struct TrOp *o = &e->ops[i];
    switch (o->op) {
      case 's': n += (size_t)snprintf(out + n, OUT_CAP - n, "%4u s pos=%lld off=%lld %s -> %d\n",
                                      i, (long long)o->pos, (long long)o->want,
                                      o->whence < 3 ? WH[o->whence] : "?", o->got); break;
      case 'p': n += (size_t)snprintf(out + n, OUT_CAP - n, "%4u p off=%lld want=%lld -> %d\n",
                                      i, (long long)o->pos, (long long)o->want, o->got); break;
      case 'f': n += (size_t)snprintf(out + n, OUT_CAP - n, "%4u f -> size %d\n", i, o->got); break;
      case 'o': n += (size_t)snprintf(out + n, OUT_CAP - n, "%4u o flags=0x%llx -> fd %d\n",
                                      i, (long long)o->want, o->got); break;
      case 'c': n += (size_t)snprintf(out + n, OUT_CAP - n, "%4u c -> %d\n", i, o->got); break;
      default:  n += (size_t)snprintf(out + n, OUT_CAP - n, "%4u %c pos=%lld want=%lld -> %d%s\n",
                                      i, o->op, (long long)o->pos, (long long)o->want, o->got,
                                      (o->got >= 0 && o->got < o->want) ? "  SHORT" : ""); break;
    }
  }
  e->nops = 0; e->ops_dropped = 0;
  mutexUnlock(&g_tr_lock);
  iolog_write(out, n);
  free(out);
}

static const char *tr_rel(const char *p) {
  if (!p) return NULL;
  const char *r = strstr(p, "/UnityCache/Shared/");
  if (!r) return NULL;
  const char *b = strrchr(p, '/');
  if (!b || strcmp(b, "/__data")) return NULL;     /* content only */
  return r + strlen("/UnityCache/Shared/");
}

static struct TrEntry *tr_get(const char *p) {
  const char *rel = tr_rel(p);
  if (!rel) return NULL;
  mutexLock(&g_tr_lock);
  struct TrEntry *e = NULL;
  for (int i = 0; i < g_tr_n; i++)
    if (!strcmp(g_tr[i].key, rel)) { e = &g_tr[i]; break; }
  if (!e && g_tr_n < TR_MAX) {
    e = &g_tr[g_tr_n++];
    memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", rel);
  }
  mutexUnlock(&g_tr_lock);
  return e;
}

/* fd -> entry, so reads and seeks can be attributed without a path. */
#define TRF_MAX 96
static struct { int fd; struct TrEntry *e; } g_trf[TRF_MAX];

static struct TrEntry *tr_by_fd(int fd) {
  for (int i = 0; i < TRF_MAX; i++)
    if (g_trf[i].fd == fd + 1) return g_trf[i].e;
  return NULL;
}
static void tr_bind(int fd, struct TrEntry *e) {
  if (e) e->reads_this_open = 0;
  if (!e) return;
  for (int i = 0; i < TRF_MAX; i++)
    if (!g_trf[i].fd) { g_trf[i].fd = fd + 1; g_trf[i].e = e; return; }
}
static void tr_unbind(int fd) {
  for (int i = 0; i < TRF_MAX; i++)
    if (g_trf[i].fd == fd + 1) { g_trf[i].fd = 0; g_trf[i].e = NULL; return; }
}

/* Everything the game did to one entry, printed when it matters.
 *
 * Prefix is [evict], not [trace]. Only called on a delete attempt, so it is rare
 * by construction and can afford a forced flush -- which the ordinary [trace]
 * open/read/close lines (400+ per run) cannot. Separating them is what lets this
 * one line reach the card without reinstating the per-line-flush stall. */
void bp_tr_dump(const char *path, const char *why) {   /* queues; see tr_log */
  const char *rel = tr_rel(path);
  if (!rel) {                                   /* maybe a dir or __info: match by prefix */
    const char *r = path ? strstr(path, "/UnityCache/Shared/") : NULL;
    if (!r) return;
    rel = r + strlen("/UnityCache/Shared/");
  }
  for (int i = 0; i < g_tr_n; i++) {
    if (strncmp(g_tr[i].key, rel, strlen(g_tr[i].key) - 7 /* minus "/__data" */) != 0 &&
        strncmp(rel, g_tr[i].key, strlen(rel)) != 0) continue;
    const struct TrEntry *e = &g_tr[i];
    tr_log("[evict] %s (%s): resident=%d size=%ld | opens=%u reads=%u "
                "short=%u lseeks=%u fstats=%u closes=%u | read %llu KB from RAM, "
                "%llu KB from card | last offset %ld\n",
                e->key, why, e->resident, e->file_size, e->opens, e->reads,
                e->short_reads, e->lseeks, e->fstats, e->closes,
                e->bytes_ram >> 10, e->bytes_card >> 10, e->last_off);
    return;
  }
  tr_log("[evict] %s (%s): the game never opened it this session\n", rel, why);
}

/* Key the blob store on a path with any device prefix removed.
 *
 * The loader opens files as "sdmc:/switch/battd_nx/..." and the engine opens the
 * SAME files as "/switch/battd_nx/...". The frozen set was fixed for this two
 * rounds ago by comparing on the cache-relative part; the BLOB STORE was not,
 * and it keys on the full path. So the boot loader stored 30 entries under
 * sdmc:-prefixed names and every engine lookup missed:
 *
 *   [ram] boot load: 30 of 30 entries held, 243 of 243 MB
 *   [ram] served 12 MB from RAM, 896 MB from the card (1% cached)
 *
 * 243 MB held and read past. Stripping everything up to the first ':' makes both
 * spellings identical, and costs one scan of a short string. */
static const char *fb_key(const char *path) {
  if (!path) return path;
  const char *colon = strchr(path, ':');
  return (colon && colon[1] == '/') ? colon + 1 : path;
}

static unsigned char *fb_lookup(const char *path, long size) {
  path = fb_key(path);
  mutexLock(&g_ram_lock);
  for (int i = 0; i < g_blob_count; i++)
    if (!strcmp(g_blob[i].path, path) && g_blob[i].size == size) {
      unsigned char *hit = g_blob[i].data;
      mutexUnlock(&g_ram_lock);
      return hit;
    }
  mutexUnlock(&g_ram_lock);
  return NULL;
}

/* Runs ONLY on the prefetch thread. */
/* Absolute-offset read that leaves the descriptor where it found it. */
static int read_at_fd(int fd, void *buf, size_t n, uint64_t off) {
  if (fd < 0) return 0;
  const long cur = lseek(fd, 0, SEEK_CUR);
  if (lseek(fd, (long)off, SEEK_SET) < 0) return 0;
  size_t got = 0;
  while (got < n) {
    const long k = read(fd, (char *)buf + got, n - got);
    if (k <= 0) break;
    got += (size_t)k;
  }
  if (cur >= 0) lseek(fd, cur, SEEK_SET);
  return got == n;
}

static void fb_load_now_locked(const char *path, long size);
static void fb_load_now(const char *path, long size) {
  mutexLock(&g_load_lock);
  fb_load_now_locked(path, size);
  mutexUnlock(&g_load_lock);
}
static void fb_load_now_locked(const char *path, long size) {
  if (fb_lookup(path, size)) return;
  const char *key = fb_key(path);          /* stored form; `path` still opens */
  mutexLock(&g_ram_lock);
  const int full = (g_blob_count >= BLOB_MAX || strlen(key) >= sizeof g_blob[0].path);
  mutexUnlock(&g_ram_lock);
  if (full) return;

  /* Carve from the image (see g_image). Tentative: g_image_used moves only
   * when the load and the verify below succeed, so a failed load leaves no
   * hole and the next file takes the same space. Serialised by g_ram_lock at
   * commit; only the prefetch thread loads, so the tentative carve is safe. */
  if (!g_image) return;                         /* no image: everything from the card */
  mutexLock(&g_ram_lock);
  const size_t carve_at = g_image_used;
  const int fits = (size_t)size <= g_image_cap - g_image_used;
  mutexUnlock(&g_ram_lock);
  if (!fits) { debugPrintf("[ram] %s: image full (%zu of %zu MB used) -- served from the card\n",
                           path, g_image_used >> 20, g_image_cap >> 20); return; }
  unsigned char *data = g_image + carve_at;

  /* Read on a private fd: the caller's is about to be handed to the game with
   * a position of 0, and seeking it here would be an invisible side effect. */
  int rfd = open(path, O_RDONLY);
  int rfd2 = rfd;                      /* same descriptor, used only by the check */
  long done = 0;
  if (rfd >= 0) {
    /* 256 KB at a time, and only while the game is not using the card. A single
     * 112 MB read here is minutes of device time the game cannot have. */
    while (done < size) {
      if (g_pfq_running) pfq_wait_for_quiet();   /* only once the game is live */
      const size_t want = (size - done) > (256 * 1024) ? (256 * 1024) : (size_t)(size - done);
      long k = read(rfd, data + done, want);
      if (k <= 0) break;
      done += k;
    }
  }
  if (done != size) {
    if (rfd2 >= 0) close(rfd2);          /* every exit closes it; see below */
    /* carve not committed: the space is reused by the next file */
    debugPrintf("[ram] %s: short read (%ld of %ld) -- not made resident\n", path, done, size);
    return;
  }

#if BP_RAM_VERIFY
  /* Compare the whole stored copy against the file before anything can use it.
   *
   * This separates two possibilities that the per-read check cannot: a blob
   * that was loaded wrong, and a blob that is right but served wrong. An A/B
   * shows the game evicting cache entries with residency on and not with it
   * off, at a 100% hit rate -- so either these bytes differ from the card or
   * they do not, and that is worth one extra read of each file to settle.
   *
   * A mismatch drops the copy and falls back to the card, so a failure here
   * costs speed and nothing else. */
  {
    const size_t CH = 1u << 20;
    unsigned char *tmp = malloc(CH);
    int bad = 0;
    if (tmp) {
      long off = 0;
      while (off < size) {
        const size_t n = (size_t)(size - off) < CH ? (size_t)(size - off) : CH;
        if (!read_at_fd(rfd2, tmp, n, (uint64_t)off)) { bad = 1; break; }
        if (memcmp(tmp, data + off, n) != 0) {
          size_t k = 0;
          while (k < n && tmp[k] == data[off + k]) k++;
          debugPrintf("[ram] VERIFY FAILED loading %s at +%ld: stored %02x, file %02x\n",
                      path, off + (long)k, data[off + k], tmp[k]);
          bad = 1;
          break;
        }
        off += (long)n;
      }
      free(tmp);
    }
    if (bad) {
      /* Moving close() to the end of the function last round left it unreachable
       * from the two early returns. Twenty-seven files per boot, and a descriptor
       * leaked on each failure -- which on a handle-limited filesystem is how a
       * later open starts failing for no visible reason. */
      if (rfd2 >= 0) close(rfd2);
      /* carve not committed */
      debugPrintf("[ram] %s REJECTED -- the stored copy does not match the file\n", path);
      return;
    }
  }
#endif

  /* THE SUCCESS PATH HAD NO close() AT ALL.
   *
   * The original closed this descriptor straight after the read loop. Adding
   * the load-time verification meant holding it a little longer, so the close
   * moved -- to a place that did not survive the edit. Every file that loaded
   * cleanly leaked its descriptor: twenty-seven per boot, and only with the
   * cache on. fsdev's handle table is not large, and a shim that quietly eats
   * handles makes some later open() fail for no reason visible where it fails.
   *
   * Found by re-reading my own patch, not by a test: no host harness models a
   * handle limit, and the checkers look at declarations, not resource
   * lifetimes. */
  if (rfd2 >= 0) close(rfd2);

  mutexLock(&g_ram_lock);
  int n = 0; long held = 0;
  if (g_blob_count < BLOB_MAX && data == g_image + g_image_used) {   /* still ours: commit */
    g_image_used += (size_t)size;
    snprintf(g_blob[g_blob_count].path, sizeof g_blob[0].path, "%s", key);
    g_blob[g_blob_count].data = data;
    g_blob[g_blob_count].size = size;
    /* Reference copy of the head, and a checksum of the whole thing, for the
     * per-frame integrity scan (see blob_scan). */
    memcpy(g_blob[g_blob_count].head, data, size < (long)BLOB_HEAD ? (size_t)size : BLOB_HEAD);
    g_blob[g_blob_count].crc = blob_crc(data, (size_t)size);
    g_blob[g_blob_count].bad = 0;
    g_blob_count++;
    n = g_blob_count;
    held = (((long)bp_ram_cache_mb << 20) - g_ram_budget) >> 20;
  }
  mutexUnlock(&g_ram_lock);
  /* LOG AFTER UNLOCKING. debugPrintf takes the log lock and can block on an SD
   * flush; holding a cache lock across that is what deadlocked the port. */
  /* One line per file was 27 blocking SD writes during the busiest part of the
   * boot. A count and a total say the same thing. */
  if (n && (n == 1 || n % 8 == 0))
    debugPrintf("[ram] %d files held, %ld MB\n", n, held);
}
static Mutex g_ra_lock;
static struct RaCache *ra_find(int fd) {
  if (fd < 0) return NULL;
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].used && g_ra[i].fd == fd) return &g_ra[i];
  return NULL;
}
/* Worth holding in RAM? Two separate cases, and they need different rules.
 *
 *  - Anything under files/UnityCache/: the downloaded AssetBundles. Unity opens
 *    and closes these constantly, and TWO THIRDS OF THEM ARE UNDER 4 MB -- 20 of
 *    the 30 this game caches. Judging them by size would have skipped most of
 *    the set to save 19 MB, which is the wrong trade when the budget is 512 MB.
 *    Size is irrelevant here; how often the file is reopened is the point.
 *
 *  - Any other read-only file of 4 MB or more. That is the original rule, kept
 *    for data.unity3d and the sharedassets resources.
 *
 * Everything else is left alone: a 1 MB read-ahead window around a 25 KB file
 * costs more than it saves. */
/* 0 = leave it alone, 1 = read-ahead window only, 2 = hold the whole file.
 *
 * NOTHING UNDER UnityCache IS HELD ANY MORE, and that is a retreat, not a
 * refinement. Three attempts at scoping it all failed on hardware:
 *
 *   1. "anything under /UnityCache/"  -- also caught __info, Unity's own
 *      bookkeeping, which it rewrites constantly. It read back its own writes
 *      as the copy from before them and stopped trusting committed entries.
 *   2. "...but only __data"           -- also caught UnityCache/Temp/, the
 *      download staging file. A snapshot taken mid-download failed every CRC
 *      check, so nothing could be re-fetched either.
 *   3. "...but only Shared/__data"    -- Unity writes committed entries in
 *      place as well. A bundle snapshotted while it was still being written
 *      loaded as "File may be corrupted".
 *
 * Each fix was narrower than the last and each still broke the game, because
 * the premise was wrong: residency assumes an immutable file, and NOTHING in
 * UnityCache is immutable from this port's side. There is no path test that
 * distinguishes "finished" from "being written", and between them these bugs
 * cost a user their entire 244 MB cache.
 *
 * UnityCache now gets the 1 MB read-ahead window instead, which is what it had
 * before any of this and which does not copy the file. The window is also a
 * genuine improvement now -- it never attached at all until the `used` flag was
 * fixed (see the RaCache comment above).
 *
 * The asset pack keeps full residency, in asset_pack.c: it is a file this port
 * builds and the game never writes, which is exactly the property UnityCache
 * lacks. */
/* ---------------------------------------------------------------------------
 * UnityCache residency, safely: freeze the eligible set BEFORE the engine runs.
 *
 * Three earlier attempts scoped this by path and all three broke the game,
 * because no path test separates "finished" from "being written". The property
 * that actually matters is not WHERE a file is but WHEN it was finished.
 *
 * Shared/<id>/<hash>/__data is content-addressed: change the content and the
 * hash changes, so it is a different path. A file that was already complete
 * before the engine started therefore cannot be rewritten in place -- Unity
 * creates a new hash directory instead. Freeze that set at boot and only those
 * paths are ever held, which closes all three failures by construction:
 *
 *   __info          never listed (we only record __data)
 *   Temp/           not under Shared/, never listed
 *   mid-write       created during the session, so not in the boot list
 *
 * The remaining routes by which a frozen path's bytes could change are all
 * intercepted, and each drops the copy: open-for-write, rename onto it, and
 * truncate. remove/unlink need no hook -- a replacement file has to be opened
 * for writing before it has any content. */
#define FROZEN_MAX 256
static char g_frozen[FROZEN_MAX][192];
static int  g_frozen_count;
static int  g_frozen_done;

/* Compare on the cache-relative part, not the whole path.
 *
 * The loader opens files as "sdmc:/switch/battd_nx/..." and the engine opens the
 * SAME files as "/switch/battd_nx/...". A full-path compare therefore never
 * matched: three consecutive boots froze 30 entries as eligible and held none of
 * them, and the only clue was a [ram] line for global-metadata.dat -- which
 * qualifies under the size rule, not the frozen set -- carrying the prefix-less
 * form while the freeze had logged the prefixed one. */
static const char *cache_rel(const char *p) {
  return p ? strstr(p, "/UnityCache/Shared/") : NULL;
}

/* Re-stat everything that was complete at boot, and report anything that has
 * since gone.
 *
 * The unlink/remove/unlinkat hooks only catch deletion through those calls. An
 * entry has vanished between launches with none of them firing, so either it
 * went by a route this shim does not wrap, or it went while an older build was
 * running. Watching the files themselves closes that: whatever removes them,
 * this notices, and the line lands next to whatever the game was doing.
 *
 * Only stats -- 30 stat() calls a minute, and only of paths already known. */
void bp_ram_check_frozen(void);
/* Set when something is deleted under UnityCache, so the next frame checks the
 * frozen set immediately instead of waiting for the next poll. The interesting
 * moment is exactly then. */
volatile int g_cache_recheck;

void bp_ram_check_frozen(void) {
  static uint64_t next_ns;
  const uint64_t now = armTicksToNs(armGetSystemTick());
  if (now < next_ns && !g_cache_recheck) return;
  g_cache_recheck = 0;
  /* Every 5 seconds, not every 60. The run that prompted this lasted NINE
   * seconds, so a once-a-minute poll never fired and the log said nothing at
   * all about files that were gone by the end of it. A poll that cannot
   * complete a cycle inside a typical session is not an instrument. */
  next_ns = now + 5ull * 1000000000ull;
  if (!g_frozen_done) return;
  for (int i = 0; i < g_frozen_count; i++) {
    if (!g_frozen[i][0]) continue;                /* already reported */
    char full[900];
    snprintf(full, sizeof full, "%s/files/UnityCache/Shared%s",
             bp_game_root(), g_frozen[i] + strlen("/UnityCache/Shared"));
    struct stat st;
    if (stat(full, &st) == 0 && st.st_size > 0) continue;
    debugPrintf("[cache] VANISHED: %s was present at boot and is now gone "
                "(nothing called unlink/remove for it)\n", g_frozen[i]);
    g_frozen[i][0] = 0;                           /* report once */
  }
}

/* Load EVERY eligible cache entry now, before the engine starts.
 *
 * The measurement that forced this: the game read 3,149 MB from the card over a
 * 243 MB cache -- every bundle about thirteen times -- and the cache served 2%
 * of it. Loading on second open, throttled to leave the card free, meant the
 * prefetch never ran: the game reads continuously, so the 250 ms idle gap it
 * waited for never arrived. It held 205 MB and bought almost nothing.
 *
 * Front-loading fixes both halves. The reads happen once, at boot, where a wait
 * is expected and can be shown on screen -- instead of competing with the game
 * for the card during play, which is what the stalls were. After that the 3 GB
 * of repeat reads come out of memory.
 *
 * Budget exhaustion is not an error: whatever fits is held, the rest is read
 * from the card as before, and the log says which. */
void bp_ram_load_all(void) {
  if (bp_ram_cache_mb <= 0 || !g_frozen_done || g_frozen_count == 0) return;
  extern void startup_status_begin(const char *msg);
  extern void startup_status_update(const char *msg);
  extern void startup_status_end(void);

  long total = 0, done_bytes = 0;
  struct stat st;
  char full[900];
  for (int i = 0; i < g_frozen_count; i++) {
    snprintf(full, sizeof full, "%s/files/UnityCache/Shared%s",
             bp_game_root(), g_frozen[i] + strlen("/UnityCache/Shared"));
    if (stat(full, &st) == 0 && st.st_size > 0) total += (long)st.st_size;
  }
  if (total <= 0) return;

  char msg[160];
  snprintf(msg, sizeof msg, "Loading game content into memory (%ld MB)", total >> 20);
  startup_status_begin(msg);

  int held = 0;
  for (int i = 0; i < g_frozen_count; i++) {
    snprintf(full, sizeof full, "%s/files/UnityCache/Shared%s",
             bp_game_root(), g_frozen[i] + strlen("/UnityCache/Shared"));
    if (stat(full, &st) != 0 || st.st_size <= 0) continue;
    fb_load_now(full, (long)st.st_size);
    if (fb_lookup(full, (long)st.st_size)) { held++; done_bytes += (long)st.st_size; }
    snprintf(msg, sizeof msg, "Loading game content into memory (%ld of %ld MB)",
             done_bytes >> 20, total >> 20);
    startup_status_update(msg);
  }
  startup_status_end();
  debugPrintf("[ram] boot load: %d of %d entries held, %ld of %ld MB "
              "(anything that did not fit is read from the card)\n",
              held, g_frozen_count, done_bytes >> 20, total >> 20);
}

static int frozen_eligible(const char *path) {
  if (!g_frozen_done) return 0;
  const char *rel = cache_rel(path);
  if (!rel) return 0;
  for (int i = 0; i < g_frozen_count; i++)
    if (!strcmp(g_frozen[i], rel)) return 1;
  return 0;
}

/* Call once at boot, before the engine touches anything. */
/* Same depth-agnostic walk as the census: an entry is any directory holding a
 * non-empty __data with an __info beside it, however deep. Unity names entries
 * after the URL, so a bundle fetched from ".../talkingheads/audio" lands three
 * levels down, and a two-level walk never saw it. */
/* LEAVE BLOCKS-INFO-AT-END BUNDLES ON THE CARD.
 *
 * Of the thirty cached bundles, twenty-seven carry their blocks-info at the
 * start of the file (UnityFS flags 0x243) and three carry it at the END
 * (flags 0xc0). Every run with the RAM cache on, Unity rejects exactly those
 * three -- reads the header, the directory from the tail, the first block,
 * closes, and asks to re-download -- and never any of the other twenty-seven.
 * With the cache off it loads all thirty. The syscall sequences on those
 * descriptors are identical between the two paths for 293 operations, the
 * bytes served are verified, and the divergence is Unity's decision after
 * block 0; what differs in the RAM path for THAT loader shape is still not
 * understood.
 *
 * So: don't argue with it. Read the header at freeze time and keep any bundle
 * whose blocks-info is at the end out of the resident set. It is served from
 * the card through the read-ahead window like the cache-off case, which is
 * known to work, and the other twenty-seven keep the 100% RAM hit rate. A
 * rule on the header flag rather than three hashes survives a game update. */
static int blocks_info_at_end(const char *data_path) {
  unsigned char h[64];
  const int fd = open(data_path, O_RDONLY);
  if (fd < 0) return 0;
  const long n = read(fd, h, sizeof h);
  close(fd);
  if (n < 24 || memcmp(h, "UnityFS", 7) != 0) return 0;
  /* sig\0, u32 version, then two NUL-terminated strings, then u64 size,
   * u32 compressedBlocksInfoSize, u32 uncompressedBlocksInfoSize, u32 flags */
  long i = 8 + 4;
  while (i < n && h[i]) i++;                         /* unityVersion ... */
  i++;                                               /* ... and its NUL */
  while (i < n && h[i]) i++;                         /* generatorVersion ... */
  i++;
  i += 8 + 4 + 4;                                    /* size, cbi, ubi */
  if (i + 4 > n) return 0;
  const uint32_t flags = ((uint32_t)h[i] << 24) | ((uint32_t)h[i+1] << 16) |
                         ((uint32_t)h[i+2] << 8) | (uint32_t)h[i+3];
  return (flags & 0x80) != 0;
}

static void freeze_walk(const char *dir, int depth, long *bytes) {
  if (depth > 6 || g_frozen_count >= FROZEN_MAX) return;
  char data[1400], info[1400];
  struct stat st, si;
  snprintf(data, sizeof data, "%s/__data", dir);
  snprintf(info, sizeof info, "%s/__info", dir);
  if (stat(data, &st) == 0 && st.st_size > 0 && stat(info, &si) == 0) {
    const char *rel = cache_rel(data);
    if (rel && strlen(rel) < sizeof g_frozen[0]) {
      if (bp_ram_max_file_mb > 0 && st.st_size > (off_t)bp_ram_max_file_mb * 1024 * 1024) {
        debugPrintf("[ram] %s stays on the card: %ld MB is over ram_max_file_mb=%d (loaded once, "
                    "behind a loading screen)\n", rel, (long)(st.st_size >> 20), bp_ram_max_file_mb);
      } else if (bp_ram_skip_tail && blocks_info_at_end(data)) {
        debugPrintf("[ram] %s stays on the card: blocks-info at end of file (the shape Unity "
                    "rejects when served from RAM)\n", rel);
      } else {
        snprintf(g_frozen[g_frozen_count], sizeof g_frozen[0], "%s", rel);
        g_frozen_count++;
        *bytes += st.st_size;
      }
    }
    return;                                  /* an entry is a leaf */
  }
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char sub[1200];
    snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
    struct stat sd;
    if (stat(sub, &sd) != 0 || !S_ISDIR(sd.st_mode)) continue;
    freeze_walk(sub, depth + 1, bytes);
  }
  closedir(d);
}

void bp_ram_freeze_cache_set(const char *root) {
  char shared[700];
  snprintf(shared, sizeof shared, "%s/files/UnityCache/Shared", root);
  g_frozen_done = 1;
  struct stat st;
  if (stat(shared, &st) != 0) {
    debugPrintf("[ram] no UnityCache yet: nothing frozen for residency\n");
    return;
  }
  long bytes = 0;
  freeze_walk(shared, 0, &bytes);
  debugPrintf("[ram] froze %d complete cache entries (%ld MB) as eligible for "
              "residency; anything written this session is not\n",
              g_frozen_count, bytes >> 20);
  /* The image: sized to the frozen set, taken from the budget once. If the
   * budget will not cover it, residency is off for the session and every
   * entry is served from the card -- "no faster", never "wrong". */
  if (bytes > 0 && !g_image) {
    if (bp_ram_cache_take(bytes) == bytes) {
      g_image = malloc((size_t)bytes);
      if (g_image) { g_image_cap = (size_t)bytes; g_image_used = 0;
        debugPrintf("[ram] resident image: %ld MB in one allocation at %p, never freed\n",
                    bytes >> 20, (void *)g_image); }
      else { bp_ram_cache_give(bytes);
        debugPrintf("[ram] resident image: malloc(%ld MB) FAILED -- everything served from the card\n", bytes >> 20); }
    } else debugPrintf("[ram] resident image: budget will not cover %ld MB -- everything served from the card\n", bytes >> 20);
  }
}

static int residency_mode(const char *path, long size) {
  if (size <= 0 || !path) return 0;

  /* ram_cache = 0 means OFF, and off has to mean the behaviour this port had
   * before any of this existed -- no residency AND no read-ahead window.
   *
   * That distinction matters. The window looks like it predates the cache, but
   * it never actually ran: ra_attach() searched for a free slot with `fd < 0`
   * while static storage zero-initialises fd to 0, so it attached to nothing
   * for the whole life of the project. Fixing that in the same change that
   * added residency switched the window on for the first time too. Leaving it
   * enabled at ram_cache = 0 would offer an "off" switch that still changes
   * how every file over 4 MB is read. */
  if (bp_ram_cache_mb <= 0) return 0;

  /* Only entries that were already complete at boot. Never the 1 MB window
   * either: it has the same staleness problem in 1 MB pieces, and it had never
   * actually attached to anything until the `used` flag was fixed, so letting it
   * cover files the game writes would be a new hazard, not an old one. */
  if (strstr(path, "/UnityCache/")) return frozen_eligible(path) ? 2 : 0;

  /* The loader's own modules are read once by so_util and then mapped; holding
   * them cost 68 MB that nothing read again. */
  const size_t n = strlen(path);
  if (n > 3 && !strcmp(path + n - 3, ".so")) return 0;

  /* global-metadata.dat for the same reason, and more carefully: il2cpp reads
   * it once at startup, dup()s the descriptor and hands it to
   * MemoryMappedFile::Map, which this port's mmap shim satisfies by reading the
   * file into RAM anyway. Holding it duplicates 7 MB that is never read again
   * AND puts the residency path -- with its dup handling and its position
   * bookkeeping -- on the single most critical file in the process, for no
   * gain. It was one of only two things the cache held during the boots being
   * investigated; the other, the pack, verifies itself. */
  if (strstr(path, "global-metadata.dat")) return 0;

  /* Big, read-only, staged by the user and never written by the game:
   * data.unity3d, the sharedassets resources, global-metadata.dat. */
  return size >= (4 << 20) ? 2 : 0;
}

/* Stop virtualising one descriptor, leaving the real one where the reader
 * believes it is.
 *
 * Needed because dup() hands out a SECOND descriptor that shares the SAME file
 * offset, while only the original carries a cache slot. Reads through the
 * original are answered from RAM and never move the real offset, so the
 * duplicate reads from wherever the file was opened -- offset 0. Unity dup()s
 * a bundle it is verifying, and got "CRC Mismatch ... calculated 0 from data".
 *
 * Trying to keep both descriptors cached is not an option: POSIX says they
 * share one offset, and two cache slots would track two independent positions.
 * So the cache loses this file for the rest of the session, which is the
 * correct trade -- a cache that changes behaviour is not a cache. */
/* Forget everything we hold about one descriptor's file, by fd.
 *
 * fb_invalidate() works on a path, which is what open-for-write and rename give
 * us. ftruncate only gives an fd, so this finds the slot, frees the blob it
 * borrows, and detaches. Without it a truncate would leave a resident copy of
 * the file as it was before -- and ftruncate here is a real truncate, not the
 * no-op its "_stub" name suggests. */
void bp_ra_forget_fd(int fd) {
  mutexLock(&g_ram_lock);
  mutexLock(&g_ra_lock);
  for (int i = 0; i < RA_SLOTS; i++) {
    if (!g_ra[i].used || g_ra[i].fd != fd) continue;
    if (g_ra[i].resident && g_ra[i].buf) {
      for (int b = 0; b < g_blob_count; b++)
        if (g_blob[b].data == g_ra[i].buf) {
          /* image: drop the entry, the bytes stay, the budget was taken once */
          g_blob[b] = g_blob[--g_blob_count];
          break;
        }
    }
    g_ra[i].used = 0; g_ra[i].buf = NULL; g_ra[i].resident = 0;
    break;
  }
  mutexUnlock(&g_ra_lock);
  mutexUnlock(&g_ram_lock);
}

void bp_ra_devirtualise(int fd) {
  int hit = 0;
  mutexLock(&g_ra_lock);
  struct RaCache *c = NULL;
  for (int i = 0; i < RA_SLOTS; i++)
    if (g_ra[i].used && g_ra[i].fd == fd) { c = &g_ra[i]; break; }
  if (c) {
    lseek(fd, c->pos, SEEK_SET);        /* put the real fd where the reader is */
    c->used = 0;
    if (c->resident) { c->buf = NULL; c->resident = 0; }
    hit = 1;
  }
  mutexUnlock(&g_ra_lock);
  if (hit) tr_log("[ram] fd %d is being duplicated -- serving it from the "
                       "card from here on, so both descriptors share one "
                       "position\n", fd);
}

/* Give every held file back to the allocator.
 *
 * A cache must yield to real demand, and this one did not. On the character
 * screen the game asked newlib for a single 128 MB block and was refused while
 * 377 MB sat here -- 294 MB of bundles plus the 83 MB pack -- much of it in
 * awkward sizes (one blob is 112 MB) that fragment the heap as well as consume
 * it. The allocation failed, and the port broke into the debugger.
 *
 * Resident RaCache slots are detached first, and each real fd is seeked to the
 * position its resident slot had reached, so reads continue correctly from the
 * file instead of resuming at whatever offset the descriptor was left at. That
 * matters: the resident path tracks position virtually and never moves the real
 * descriptor.
 *
 * Locks are taken ram-then-ra, matching ra_attach_path's order (it calls
 * fb_resident, which takes the ram lock, before taking the ra lock). Taking
 * them the other way round here would be a lock-order inversion. */
long bp_ram_cache_release_all(void) {
  mutexLock(&g_ram_lock);
  mutexLock(&g_ra_lock);
  for (int i = 0; i < RA_SLOTS; i++)
    if (g_ra[i].used && g_ra[i].resident) {
      lseek(g_ra[i].fd, g_ra[i].pos, SEEK_SET);   /* sync the descriptor */
      g_ra[i].used = 0;
      g_ra[i].buf = NULL;
      g_ra[i].resident = 0;
    }
  long freed = 0;
  /* image: every slot is detached above, so the one allocation can go back
   * whole. Future opens are served from the card. */
  if (g_image) { freed = (long)g_image_cap; free(g_image); g_image = NULL; g_image_cap = g_image_used = 0; }
  g_blob_count = 0;
  g_ram_budget += freed;
  mutexUnlock(&g_ra_lock);
  mutexUnlock(&g_ram_lock);
  if (freed)
    debugPrintf("[ram] released %ld MB back to the allocator under memory pressure; "
                "files are read from the card from here on\n", freed >> 20);
  return freed;
}

/* Any write to a path drops its resident copy, so the next reader sees the file
 * rather than a snapshot of it. residency_mode() already keeps written files
 * out; this is the backstop for a path this port has not thought of. */
void bp_ram_forget_path(const char *path);
static void fb_invalidate(const char *path) {
  if (!path) return;
  const char *key = fb_key(path);
  int dropped = 0;
  mutexLock(&g_ram_lock);
  for (int i = 0; i < g_blob_count; i++)
    if (!strcmp(g_blob[i].path, key)) {
      dropped = 1;
      const long freed = g_blob[i].size;
      /* Detach every slot still borrowing this blob BEFORE freeing it.
       *
       * Without this the free leaves any resident slot pointing at released
       * memory, and the next read through that descriptor is a use-after-free
       * that returns whatever the allocator has since put there -- plausible
       * bytes, no fault, and a bundle that Unity rejects seconds later. Unity
       * opens a cached bundle for reading and then opens the same path for
       * writing when it decides to replace it, which is exactly this sequence.
       * Each descriptor is seeked to where its reader believes it is, so it
       * carries on correctly straight from the file. */
      mutexLock(&g_ra_lock);
      for (int r = 0; r < RA_SLOTS; r++)
        if (g_ra[r].used && g_ra[r].resident && g_ra[r].buf == g_blob[i].data) {
          lseek(g_ra[r].fd, g_ra[r].pos, SEEK_SET);
          g_ra[r].used = 0; g_ra[r].buf = NULL;
          g_ra[r].resident = 0; g_ra[r].blobpath = NULL;
        }
      mutexUnlock(&g_ra_lock);
      /* image: the bytes stay where they are and the budget was taken once
       * for the whole image; a dropped file is served from the card from now
       * on (its space is not reused -- "no faster", never "wrong"). */
      g_blob[i] = g_blob[--g_blob_count];
      (void)freed;                /* was: g_ram_budget += freed -- the note below explained why it
                                   * mattered for per-file blobs; it does not apply to an image.
                                   * budget, so a file invalidated and re-read a
                                   * few times would exhaust it and silently stop
                                   * anything else becoming resident. Done inline
                                   * because the lock is already held. */
      break;
    }
  mutexUnlock(&g_ram_lock);
}

/* pread bypasses the cache and the read counters; imports.c reports it here so
 * the op log sees it. */
void bp_tr_note_pread(int fd, long off, size_t n, long got) {
  struct TrEntry *te = tr_by_fd(fd);
  if (te) { te->bytes_card += got > 0 ? (unsigned long long)got : 0; tr_op(te, 'p', off, (int64_t)n, got, 0, 2); }
}

/* Same thing under a name other translation units can reach. imports.c needs it
 * for unlink/remove, which change a path's contents with no open-for-write for
 * fb_invalidate's usual callers to see. */
void bp_ram_forget_path(const char *path) { fb_invalidate(path); }

void ra_attach_path(int fd, long size, const char *path) {
  unsigned char *blob = path ? fb_resident(path, size) : NULL;
  mutexLock(&g_ra_lock);
  for (int i = 0; i < RA_SLOTS; i++) if (!g_ra[i].used) {
    if (blob) {
      g_ra[i].fd = fd; g_ra[i].pos = 0; g_ra[i].size = size;
      g_ra[i].base = 0; g_ra[i].len = size;
      g_ra[i].buf = blob; g_ra[i].resident = 1; g_ra[i].used = 1;
      g_ra[i].blobpath = fb_path_of(blob);
    } else {
      if (size < (4 << 20)) break;   /* too small for a window to pay for itself */
      if (!g_ra[i].buf || g_ra[i].resident) { g_ra[i].buf = malloc(RA_WIN); g_ra[i].resident = 0; }
      if (g_ra[i].buf) {
        g_ra[i].fd = fd; g_ra[i].pos = 0; g_ra[i].size = size;
        g_ra[i].base = 0; g_ra[i].len = 0; g_ra[i].resident = 0; g_ra[i].used = 1;
        g_ra[i].blobpath = NULL;
      }
    }
    break;
  }
  mutexUnlock(&g_ra_lock);
}

/* Kept for callers that have no path to offer. Defined AFTER ra_attach_path
 * because C needs the declaration first, and this file has no header. */
void ra_attach(int fd, long size) { ra_attach_path(fd, size, NULL); }

void ra_detach(int fd) {
  mutexLock(&g_ra_lock);
  struct RaCache *c = ra_find(fd);
  if (c) {
    c->used = 0;             /* window buffers stay allocated for reuse */
    c->fd = -1;
    if (c->resident) { c->buf = NULL; c->resident = 0; }   /* blob is borrowed */
  }
  mutexUnlock(&g_ra_lock);
}
/* THE REAL DESCRIPTOR OWNS THE POSITION. The cache holds DATA, nothing else.
 *
 * This layer used to keep a virtual position and leave the real fd wherever it
 * was opened. That is correct only if every route to the file goes through
 * read_fake and z_lseek -- and three separate routes did not, each found only
 * after it corrupted something on hardware: the `used` flag (the cache never
 * attached at all), __read_chk (the fortified read bionic libraries actually
 * call), and dup (a second descriptor sharing one offset). fdopen and ftruncate
 * were two more waiting their turn.
 *
 * Enumerating doors was the wrong strategy: nothing makes the list complete, and
 * the compiler cannot point at the missing ones. So the position is no longer
 * virtual. Every cached read reads the real position first and writes it back
 * after, which costs two cheap lseeks and no data transfer, and makes every
 * bypass -- present or future, ours or newlib's -- correct by construction. */
/* THE RESIDENT COPY RUNS WITHOUT THE LOCK, AND THAT IS ONLY TRUE NOW.
 *
 * g_ra_lock used to be held across the memcpy. Unity reads bundles from
 * sixteen Background Job workers at once, so every read of every cached
 * bundle queued on one mutex and parallel bundle loading became serial -- on
 * four A57s that is three cores idle behind one memcpy.
 *
 * It was not safe to copy outside the lock under the old per-file blob store:
 * three paths could free the buffer while a slot still pointed at it. With the
 * single immutable image it is. Once fb_load_now commits an entry, its bytes
 * never change and are never freed for the life of the process, and the refill
 * branch below is skipped for resident slots so nothing writes into a blob.
 * So the lock is taken only to snapshot the extent and publish the position;
 * the copy itself touches memory nobody can mutate.
 *
 * The WINDOW path is unchanged and still copies under the lock -- c->buf there
 * is a per-slot scratch buffer the refill genuinely rewrites.
 *
 * out_pos (optional) receives the position the fd was at BEFORE this read, so
 * the caller does not have to ask for it with a second lseek. */
static long ra_read_at(struct RaCache *c, int fd, void *buf, size_t count, long *out_pos) {
  /* Snapshot the extent first, so the seek strategy below can depend on
   * whether this slot is resident without a second trip through the lock. */
  mutexLock(&g_ra_lock);
  const int resident = c->resident;
  const unsigned char *rbuf = (const unsigned char *)c->buf;
  const long rbase = c->base, rlen = c->len;
  mutexUnlock(&g_ra_lock);

#if !BP_RAM_VERIFY_READS
  if (resident && rbuf && count <= (size_t)LONG_MAX) {
    /* ONE SEEK FOR A FULL READ, instead of one to learn the position and one
     * to write it back.
     *
     * An lseek here is not just a call: devkitPro's fd operations go through
     * newlib's __get_handle(), which takes a PROCESS-WIDE handle lock -- the
     * same __hndl_lock that showed up in the exception-handler recursion. With
     * sixteen Unity workers reading bundles, two seeks per read is two global
     * lock acquisitions per read.
     *
     * So seek FORWARD by count first: that returns start+count, which gives us
     * the start position AND leaves the descriptor exactly where a full read
     * should leave it. Only a short read (EOF) needs a correcting seek, and
     * from io.log that is about one read in a hundred.
     *
     * Safe on this target: fsdev_seek() rejects only offsets before the start
     * of the file; past EOF it just stores the offset and returns it, with no
     * IPC and no size query. The descriptor still owns the position, so dup,
     * __read_chk and fdopen stay correct -- that reasoning is unchanged. */
    const long end = lseek(fd, (long)count, SEEK_CUR);
    if (end >= (long)count) {
      const long real = end - (long)count;
      if (out_pos) *out_pos = real;
      size_t done = 0;
      const long avail = (rbase + rlen) - real;
      if (avail > 0) {
        done = (count < (size_t)avail) ? count : (size_t)avail;
        memcpy(buf, rbuf + (real - rbase), done);   /* immutable: see below */
      }
      const long endpos = real + (long)done;
      mutexLock(&g_ra_lock);
      c->pos = endpos;
      g_bytes_from_ram += done;
      mutexUnlock(&g_ra_lock);
      if (endpos != end) lseek(fd, endpos, SEEK_SET);   /* short read only */
      return (long)done;
    }
    if (end >= 0) lseek(fd, end - (long)count, SEEK_SET);   /* undo before falling through */
  }
#endif

  /* THE RESIDENT COPY RUNS WITHOUT THE LOCK, AND THAT IS ONLY TRUE NOW.
   *
   * g_ra_lock used to be held across the memcpy. Unity reads bundles from
   * sixteen Background Job workers at once, so every read of every cached
   * bundle queued on one mutex and parallel bundle loading became serial.
   *
   * It was not safe to copy outside the lock under the old per-file blob
   * store: three paths could free the buffer while a slot still pointed at it.
   * With the single immutable image it is. Once fb_load_now commits an entry,
   * its bytes never change and are never freed for the life of the process,
   * and the refill branch below is skipped for resident slots.
   *
   * The WINDOW path below is unchanged and still copies under the lock -- its
   * c->buf is per-slot scratch the refill genuinely rewrites. */
  size_t done = 0;
  const long real = lseek(fd, 0, SEEK_CUR);
  if (out_pos) *out_pos = real;
  if (real < 0) return read(fd, buf, count);   /* cannot sync: do not serve */

  mutexLock(&g_ra_lock);
  c->pos = real;
  while (done < count) {
    if (c->len == 0 || c->pos < c->base || c->pos >= c->base + c->len) {
      /* A resident slot holds the whole file; reaching here means EOF. A refill
       * would read() into the shared blob, and there is nothing to fetch: stop. */
      if (c->resident) break;
      if (lseek(fd, c->pos, SEEK_SET) < 0) break;
      long r = 0;
      while (r < (long)RA_WIN) { long k = read(fd, c->buf + r, RA_WIN - r); if (k <= 0) break; r += k; }
      if (r <= 0) break;
      c->base = c->pos; c->len = r;
    }
    long avail = (c->base + c->len) - c->pos;
    if (avail <= 0) break;
    size_t n = (count - done < (size_t)avail) ? count - done : (size_t)avail;
    memcpy((char *)buf + done, c->buf + (c->pos - c->base), n);
    c->pos += n; done += n;
    if (c->resident) g_bytes_from_ram += n; else g_bytes_from_card += n;
  }
  const long endpos = c->pos;
  const int was_resident = c->resident;
  const char *who = c->blobpath;
  mutexUnlock(&g_ra_lock);

#if BP_RAM_VERIFY_READS
  /* Compare what we just handed back against the same range of the real file.
   * A wrong cached read does not fault -- it returns plausible bytes, and the
   * first sign is Unity rejecting a bundle seconds later. This turns that into
   * an exact report. Bounded in both size and count so an early systematic
   * failure does not fill the card. */
  if (done && was_resident) {
    static unsigned reported;
    static unsigned char probe[64 * 1024];
    const size_t n = done < sizeof probe ? done : sizeof probe;
    if (lseek(fd, real, SEEK_SET) == real) {
      size_t got = 0;
      while (got < n) {
        const long k = read(fd, probe + got, n - got);
        if (k <= 0) break;
        got += (size_t)k;
      }
      if (got != n || memcmp(probe, buf, n) != 0) {
        if (reported < 8) {
          reported++;
          size_t at = 0;
          while (at < got && at < n && probe[at] == ((const unsigned char *)buf)[at]) at++;
          tr_log("[ram] VERIFY FAILED fd=%d %s off=%ld len=%zu: file gave %zu bytes, "
                      "first difference at +%zu (cache %02x, file %02x)\n",
                      fd, who ? who : "(unknown)", real, done, got, at,
                      at < n ? ((const unsigned char *)buf)[at] : 0,
                      at < got ? probe[at] : 0);
        }
      }
    }
  }
#endif

  if (done) lseek(fd, endpos, SEEK_SET);       /* leave the real fd where a
                                                * bypassing reader expects it */
  return (long)done;
}

/* lseek for arm64: off_t is already 64-bit, so this also services lseek64.
 * lseek64 was previously stubbed to return 0 (no seek) -- that made libunity's
 * archive reader see data.unity3d as empty/mis-positioned ("Unable to read
 * header from archive file"), since it lseek64(SEEK_END)s to size the file. */
long z_lseek_unguarded(int fd, long off, int whence) {
  /* Pack fds live outside the real fd space and carry their own position,
   * so they must be answered before ra_find() looks them up. */
  if (asset_pack_fd_is(fd)) return asset_pack_lseek_fd(fd, off, whence);
  struct RaCache *c = ra_find(fd);
  if (c) {
    /* Seek the REAL descriptor and mirror the answer, rather than tracking a
     * position only this layer knows about. A dup'd or fdopen'd view of the
     * same file then sees the same offset, which is the whole point. */
    const long np = lseek(fd, off, whence);
    if (np >= 0) { mutexLock(&g_ra_lock); c->pos = np; mutexUnlock(&g_ra_lock); }
    /* The [evict] counters said lseeks=0 for every evicted entry. That was
     * this function not counting, not Unity not seeking -- the trace showed
     * pos going 64 -> 12 with no seek recorded. SEEK_END is how the engine
     * sizes a bundle before deciding whether it is whole, so that one gets a
     * line: if the answer ever disagrees with the entry's size, that is the
     * eviction. */
    { struct TrEntry *te = tr_by_fd(fd);
      if (te) {
        te->lseeks++;
        tr_op(te, 's', c->pos, off, np, whence, 1);
        if (whence == SEEK_END)
          tr_log("[evict] SEEK_END %s fd=%d -> %ld (entry size %ld)%s\n",
                 te->key, fd, np, (long)c->size,
                 np == (long)c->size ? "" : "  *** DISAGREES ***");
      } }
    return np;
  }
  { const long np = lseek(fd, off, whence);
    struct TrEntry *te = tr_by_fd(fd);
    if (te) { te->lseeks++; tr_op(te, 's', -1, off, np, whence, 2); }
    return np; }
}
long z_lseek(int fd, long off, int whence) {
  if (!fdg_enter(fd)) return -1;
  long r = z_lseek_unguarded(fd, off, whence);
  fdg_leave(fd);
  return r;
}

static const char *synthetic_proc(const char *path);  /* defined below */

// Serve /proc and /sys reads that arrive through raw open() (e.g.
// /proc/self/maps, which the engine opens to enumerate memory mappings).
// newlib's open() can't be memory-backed, so materialize the synthetic content
// into a small file under the game dir and hand back a real fd. Returns an fd,
// or -1 if `path` isn't a node we synthesize (caller proceeds normally).
static int synth_proc_open(const char *path) {
  if (!path) return -1;
  if (strncmp(path, "/proc/", 6) && strncmp(path, "/sys/", 5)) return -1;
  static char buf[16384];
  int len;
  if (!strcmp(path, "/proc/self/maps") || !strcmp(path, "/proc/self/smaps")) {
    len = so_dump_maps(buf, sizeof buf);
  } else {
    const char *s = synthetic_proc(path);
    if (!s) return -1;                                   // not /proc or /sys
    len = (int)strlen(s);
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    memcpy(buf, s, (size_t)len);
  }
  char safe[160]; size_t j = 0;
  for (const char *p = path; *p && j < sizeof safe - 1; p++) safe[j++] = (*p == '/') ? '_' : *p;
  safe[j] = '\0';
  char tf[256];
  snprintf(tf, sizeof tf, "%s/.synth%s", bp_game_root(), safe);
  int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (wfd >= 0) { if (write(wfd, buf, (size_t)len) < 0) { /* best effort */ } close(wfd); }
  return open(tf, O_RDONLY);
}

// ---------------------------------------------------------------------------
// Synthetic inode numbers. libnx's fsdev returns st_ino==0 for EVERY file, but
// il2cpp's System.IO share layer keys its open-file table on (st_dev,st_ino):
// with every file colliding on inode 0 it treats UNRELATED files as the same
// one, so Easy Save's concurrent SaveData.es3.tmp (write) + SaveData.es3 (read)
// looks like an incompatible double-open of one file and throws "IOException:
// Sharing violation on path SaveData.es3" every frame on the menu -- and each of
// those exception backtraces mmaps libunity/libil2cpp, leaking us to OOM/exit.
// Give each distinct path a stable, non-zero inode (only when the real one is 0,
// so a genuine inode from a future fsdev is preserved). fstat() has no path, so a
// small fd->inode map is filled at open() time and mirrors stat(path)'s value.
#define FD_INO_MAX 4096
static uint64_t g_fd_ino[FD_INO_MAX];
static uint64_t path_ino(const char *path) {
  uint64_t h = 1469598103934665603ULL;               // FNV-1a 64 offset basis
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
  return h ? h : 1;                                   // 0 means "no inode" -- avoid it
}
static void fd_ino_set(int fd, const char *path) { if (fd >= 0 && fd < FD_INO_MAX) g_fd_ino[fd] = path_ino(path); }
static void fd_ino_clear(int fd) { if (fd >= 0 && fd < FD_INO_MAX) g_fd_ino[fd] = 0; }

/* The lowest fd we hold open on `path` (same key as fd_ino_set), or -1. No side
 * effects -- unlike the close-all helper below, which rename_fake needs. */
static int fd_open_on_path(const char *path) {
  const uint64_t ino = path_ino(path);
  if (!ino) return -1;
  for (int fd = 0; fd < FD_INO_MAX; fd++) if (g_fd_ino[fd] == ino) return fd;
  return -1;
}

/* Close every fd we know to be open on `path`, returning the LOWEST one we closed (or -1).
 * Used by rename_fake: Horizon refuses to rename, delete, or even open-for-write a file that
 * has a live handle, and il2cpp's ReplaceFile holds one on the very file we must replace.
 * g_fd_ino is a 64-bit FNV hash of the path, maintained by open_fake/close_fake, so a false
 * match would need a 64-bit hash collision against a path opened in this same process. */
int close_fake(int fd);          /* fwd: clears g_fd_ino and commits the SD card */
static int fd_close_by_path(const char *path) {
  const uint64_t ino = path_ino(path);
  int first = -1;
  for (int fd = 0; fd < FD_INO_MAX; fd++) {
    if (g_fd_ino[fd] != ino) continue;
    if (TRACE_IO) debugPrintf("[io] rename: closing caller's open handle fd=%d on %s\n", fd, path);
    close_fake(fd);
    if (first < 0) first = fd;
  }
  return first;
}

// libnx buffers sdmc: writes -- they reach the physical card only on fsdevCommitDevice() or a
// clean unmount. Exiting via HOME kills the process (title override), so that commit never runs
// and saves written this session vanish on reboot. Track fds opened for writing and commit the
// card when they close, so a save persists the moment the game finishes writing it.
#define WFD_MAX 4096
static unsigned char g_write_fd[WFD_MAX];
int nx_sd_dirty = 0;   /* a file has been opened for writing since the last commit */
static void mark_write_fd(int fd)   { nx_sd_dirty = 1; if (fd >= 0 && fd < WFD_MAX) g_write_fd[fd] = 1; }
static void commit_write_fd(int fd) { if (fd >= 0 && fd < WFD_MAX && g_write_fd[fd]) {
                                        g_write_fd[fd] = 0; nx_sd_dirty = 0; fsdevCommitDevice("sdmc"); } }

/* Commit any pending writes to the physical card. Called periodically from the render loop and
 * after the game's save (nx_save_prefs), so the save survives a HOME-kill / reboot. */
void nx_sd_flush(void) {
  debug_log_flush();   /* push our own buffered log before committing the FS */
  if (!nx_sd_dirty) return;
  nx_sd_dirty = 0;
  fsdevCommitDevice("sdmc");
}

/* Unity probes filesystem case-sensitivity on every boot: it creates
 * CASESENSITIVETEST<guid> (O_CREAT|O_EXCL) in the data root, re-opens it under a
 * different case, and never cleans up -- a fresh GUID name each launch, so junk
 * accumulates on the SD card. Redirect every such name (any case, any guid) onto ONE
 * fixed hidden scratch file. Different-case probes then hit the same file, which is
 * exactly the case-INSENSITIVE answer FAT/exFAT gives anyway, and main.c sweeps the
 * scratch (plus any strays from older builds) at boot. (Copied from zookeeperdx_nx.) */
static const char *casetest_redirect(const char *path) {
  if (!path) return path;
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  if (strncasecmp(b, "CASESENSITIVETEST", 17) == 0) {
    /* Was a compile-time literal; now built from the runtime root, so it needs
     * storage that outlives the call. One probe path, one buffer. */
    static char casetest[560];
    if (!casetest[0])
      snprintf(casetest, sizeof casetest, "%s/.casetest", bp_game_root());
    return casetest;
  }
  return path;
}

/* Android APK asset root -> our asset dir on the SD card.
 *
 * The game builds ABSOLUTE "/assets/..." paths (on Android that is the APK's
 * asset root). On Switch a leading "/" makes newlib resolve against the DEFAULT
 * DEVICE ROOT -- sdmc:/assets/... , the root of the SD card -- which does not
 * exist. Our assets live under <game root>/assets/. Left unrewritten, every
 * absolute asset open returns -1 even though the file is present.
 *
 * Relative "assets/..." paths already work (they resolve against the cwd, which
 * main.c chdir()s to the game root), so only the absolute and URL forms need
 * rewriting. basename_fallback() cannot cover this: it drops the subdirectory,
 * so "/assets/aa/catalog.bin" would degrade to looking for "catalog.bin" in the
 * game root. The full subpath has to survive.
 *
 * The rewriting now lives in bp_assets.c, which additionally handles the
 * "jar:file://<apk>!/assets/..." and "file://jar:file://..." shapes Unity emits
 * on Android, and substitutes the indexed spelling of the file so a case
 * mismatch between what the engine asks for and what is on the SD card still
 * opens. See bp_assets.h.
 *
 * bp_assets_resolve() is safe to call before bp_assets_init(): with an empty
 * index it falls back to the plain <root>/assets/<rel> rewrite, which is
 * exactly what this function used to do on its own. */
static const char *asset_redirect(const char *path, char *buf, size_t bufsz) {
  return bp_assets_resolve(path, buf, bufsz);
}

int open_fake(const char *path, int flags, ...) {
  char _rbuf[512];
  path = casetest_redirect(path);
  path = asset_redirect(path, _rbuf, sizeof _rbuf);
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  const int cvt = convert_open_flags(flags);
  const int writing = (flags & 3) != 0 || (flags & LINUX_O_CREAT);
  if (writing && cache_protected_write(path)) {
    static unsigned nb;
    if (nb < 20) { nb++;
      tr_log("[cache] BLOCKED write-open of %s -- committed entries are "
                  "read-only while offline\n", path); }
    errno = EACCES;
    return -1;
  }
  if (!writing) {
    /* Packed assets win over the filesystem: once the pack is built the loose
     * tree is deleted, so this is the only place the data exists. */
    int packed_fd = asset_pack_open_path(path);
    if (packed_fd >= 0) {
      if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d [pack]\n", path, flags, packed_fd);
      return packed_fd;
    }
    // /dev/urandom + /dev/random: Switch has no /dev node, but Mono/.NET (RNG
    // seeds, Guid.NewGuid, hashtable randomization) and asset crypto open these.
    // A failing open (-1) leaves those paths without entropy and can stall the
    // scene/asset load. Materialize a buffer of real CSPRNG bytes (libnx
    // randomGet) into a file and hand back a real fd so read() just works.
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) {
      static char rbuf[65536];
      randomGet(rbuf, sizeof rbuf);
      char tf[256];
      snprintf(tf, sizeof tf, "%s/.synth_dev_random", bp_game_root());
      int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (wfd >= 0) { if (write(wfd, rbuf, sizeof rbuf) < 0) { /* best effort */ } close(wfd); }
      int rfd = open(tf, O_RDONLY);
      fd_ino_set(rfd, path);
      if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d [urandom]\n", path, flags, rfd);
      return rfd;
    }
    // synthetic /proc, /sys (incl. self/maps)
    int sfd = synth_proc_open(path);
    if (sfd >= 0) { fd_ino_set(sfd, path); if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d [synthetic]\n", path, flags, sfd); return sfd; }
  }
  int fd = open(path, cvt, mode);
  if (fd < 0 && writing) {
    // save files: the target subdir may not exist yet -- create it and retry
    mkdir_parents(path);
    fd = open(path, cvt, mode);
  }
  if (fd < 0 && (flags & 3) == 0 && !(flags & LINUX_O_CREAT)) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt)))
      fd = open(alt, cvt, mode);
  }
  if (fd >= 0) {
    fd_ino_set(fd, path);
    if (writing) mark_write_fd(fd);
    struct stat _st;
    if (fstat(fd, &_st) == 0) {
      if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d size=%lld\n", path, flags, fd, (long long)_st.st_size);
      /* Big read-only asset files (data.unity3d ~424MB, sharedassets*.resource)
       * get a read-ahead cache so Unity's tiny per-field reads hit RAM, not SD. */
      if (writing) { fb_invalidate(path); dlw_open(fd, path); }
      const int mode = writing ? 0 : residency_mode(path, (long)_st.st_size);
      if (mode)
        ra_attach_path(fd, (long)_st.st_size, mode == 2 ? path : NULL);
      { struct TrEntry *te = tr_get(path);
        if (te) {
          te->opens++;
          te->file_size = (long)_st.st_size;
          { struct RaCache *rc = ra_find(fd); te->resident = rc && rc->resident; }
          tr_bind(fd, te);
          tr_op(te, 'o', 0, writing, fd, 0, te->resident);
          if (g_tr_lines < 400) { g_tr_lines++;
            tr_log("[trace] open  %s fd=%d size=%ld %s\n", te->key, fd,
                        te->file_size, te->resident ? "(served from RAM)"
                                                    : "(served from card)"); }
        } }
    } else {
      if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d size=?\n", path, flags, fd);
    }
  } else {
    if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d\n", path, flags, fd);
  }
  /* A failed open of game CONTENT, reported whatever TRACE_IO is set to.
   *
   * "The game is not being given the files it needs" is a hypothesis worth a
   * direct answer rather than an inference from what it does next. If a read
   * path is wrong -- a pack entry that does not resolve, a redirect that lands
   * somewhere else, a cache entry the game looks for under a name this port
   * does not produce -- it shows up here as the exact path that came back -1.
   *
   * Bounded, and only for content: the engine probes for plenty of files that
   * are genuinely absent (locale variants, optional configs) and those are not
   * interesting. Reads are what matter, so writes and creates are excluded. */
  if (fd < 0 && !writing && path &&
      (strstr(path, "/assets/") || strstr(path, "/UnityCache/") ||
       strstr(path, "/files/"))) {
    /* Two of these are normal and were each mistaken for the fault once.
     *
     * UnityCache/Shared/<id>/__info at depth 2 is how Unity asks "do I hold a
     * cache entry with this id?". ENOENT is the correct answer unless the entry
     * exists -- and in a cache known to work, that file exists for NONE of the
     * thirty entries. Making it succeed would be worse than leaving it: Unity
     * would then look for a __data that is not there.
     *
     * Analytics/ArchivedEvents/.../p is Unity Analytics checking for queued
     * events it has not written.
     *
     * Both are labelled rather than hidden, so a real miss still stands out. */
    const char *why = "";
    const char *tail = strstr(path, "/UnityCache/Shared/");
    if (tail) {
      const char *rest = tail + strlen("/UnityCache/Shared/");
      const char *slash = strchr(rest, '/');
      if (slash && !strcmp(slash, "/__info"))
        why = "  [normal: Unity asking whether this entry is cached]";
    } else if (strstr(path, "/Analytics/ArchivedEvents/")) {
      why = "  [normal: analytics queue probe]";
    }
    static unsigned nmiss;
    if (nmiss < 40) {
      nmiss++;
      tr_log("[miss] the game asked for %s and did not get it (errno %d)%s\n",
                  path, errno, why);
    }
  }
  return fd;
}
int openat_fake(int dirfd, const char *path, int flags, ...) {
  (void)dirfd;
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  // Delegate to open_fake so /dev/urandom, synthetic /proc + /sys, save-dir
  // creation and basename fallback all apply (some libc paths route open->openat).
  return open_fake(path, flags, mode);
}
int unlinkat_fake(int dirfd, const char *path, int flags) {
  (void)dirfd; (void)flags;
  { extern int bp_cache_block_delete(const char *p, const char *how);
    if (bp_cache_block_delete(path, "unlinkat")) return 0; }
  if (path && strstr(path, "/UnityCache/"))
    debugPrintf("[cache] the GAME deleted %s (via unlinkat)\n", path);
  fb_invalidate(path);          /* same reason as remove_fake/unlink_fake */
  return unlink(path);
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
  uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t __pad1;
  int64_t st_size; int32_t st_blksize; int32_t __pad2; int64_t st_blocks;
  struct bionic_timespec st_atim; struct bionic_timespec st_mtim; struct bionic_timespec st_ctim;
  uint32_t __unused4; uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino; out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size; out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime; out->st_mtim.tv_sec = in->st_mtime; out->st_ctim.tv_sec = in->st_ctime;
}

/* POSIX rename() REPLACES an existing destination. Horizon's fsFsRenameFile does NOT -- it
 * fails if the destination exists, and libnx's fsdev passes that straight through. Worse,
 * Horizon will not rename OR delete a file that currently has an open handle.
 *
 * Bad Piggies saves via SettingsData.TransactionalFileWrite:
 *     write Progress.dat.tmp
 *     if (File.Exists(Progress.dat)) File.Replace(tmp, Progress.dat, Progress.dat.bak);
 *     else                           File.Move   (tmp, Progress.dat);
 * and il2cpp's native ReplaceFile does:
 *     fd = open(backup, O_RDONLY);          <-- keeps .bak OPEN across the next call
 *     rename(dest -> backup);
 *     rename(source -> dest);
 *     close(fd);
 *
 * So on Switch that first rename hits a destination which (a) already exists and (b) is held
 * open by the caller itself. It cannot be renamed away and it cannot be deleted -- which is
 * why every save silently jammed, leaving the real save stranded in Progress.dat.tmp while
 * the game kept loading a stale Progress.dat. (The game try/catches its save, hence no
 * managed exception in the log.) Settings.xml goes down the identical path.
 *
 * The fix therefore must never touch the destination's DIRECTORY ENTRY -- only its CONTENTS.
 * We try, in order:
 *   1. plain rename                    (destination absent: nothing to do)
 *   2. park the destination aside, rename, drop the parked copy, roll back on failure
 *      (correct and near-atomic; works whenever the destination is not held open)
 *   3. copy the source's BYTES over the destination and unlink the source
 *      (the only thing that works when the destination has an open handle -- writing to an
 *       open file is allowed, renaming/deleting it is not)
 * Step 2 is kept ahead of step 3 because it is atomic and never leaves a half-written file;
 * step 3 is the fallback that actually rescues this game's save. We deliberately read the
 * source fully BEFORE truncating the destination, so a failure to read cannot destroy it. */
int rename_fake(const char *oldp, const char *newp) {
  char rb1[512], rb2[512];
  oldp = asset_redirect(oldp, rb1, sizeof rb1);
  newp = asset_redirect(newp, rb2, sizeof rb2);
  /* NEVER TOUCH THE DESTINATION IF THE SOURCE IS NOT THERE.
   *
   * POSIX rename() with a missing source is ENOENT and leaves the destination
   * alone. This did not: it went straight to the displace-and-swap dance below,
   * which moves the destination aside first. So a rename whose source did not
   * exist would park a good file at <dest>.rnold, fail, and -- if the rollback
   * also failed -- strand it there for the next call's remove() to delete.
   *
   * That is not hypothetical. Unity commits a downloaded bundle by writing
   * <hash>_tmp/__data_tmp and renaming it over <hash>/__data. With no network
   * the download fails and the _tmp source is cleaned up -- the log shows Unity
   * unlinking exactly those files -- and the commit rename then ran against a
   * source that was already gone. Three entries, about 41 MB, disappeared from
   * the card every session this way, and kept disappearing after each recopy. */
  /* A rename that LANDS on __info is Unity rewriting its bookkeeping; allow it.
   * One that lands on __data would replace content, which is what must not
   * happen while the source cannot possibly be a good download.
   *
   * MOVING __info AWAY IS A DELETE WEARING A RENAME'S CLOTHES, and two
   * exemptions that are each correct on their own compose into a hole:
   *
   *   rename(<entry>/__info -> <entry>/__info_tmp)
   *        oldp: is_info_file  -> cache_protected_write() exempts it
   *        newp: contains _tmp -> cache_locked_now() exempts it
   *        => ALLOWED
   *   unlink(<entry>/__info_tmp)
   *        contains _tmp       -> exempt
   *        => ALLOWED
   *
   * Net effect: __info is laundered through a _tmp name and destroyed, while
   * the rename of the entry directory, the rename of __data, the unlink of
   * __data and the rmdir are all correctly refused. The entry keeps its
   * content and loses its bookkeeping, walk_cache() then counts it incomplete,
   * and the census drops by exactly that entry's __data size -- which is what
   * "two entries, 38 MB gone" was. The content never left the card.
   *
   * So: allow __info -> __info (a commit from staging), allow writes to
   * __info, refuse __info -> anything that is not an __info. */
  if (cache_protected_write(oldp) || cache_protected_write(newp) ||
      (cache_locked_now(oldp) && is_info_file(oldp) && !is_info_file(newp))) {
    static unsigned nr;
    if (nr < 20) { nr++;
      tr_log("[cache] BLOCKED rename %s -> %s -- committed entries are "
             "read-only while offline\n", oldp, newp); }
    errno = EACCES;
    return -1;
  }

  struct stat sst;
  if (stat(oldp, &sst) != 0) {
    /* Report only when there was something to lose. The engine renames plenty
     * of paths that are served from the pack and have no real file behind them
     * -- 32 of those in one boot -- and a refusal there changes nothing, since
     * the destination does not exist either. The case worth seeing is a missing
     * source with a REAL destination: that is the one that used to displace a
     * good file and strand it. */
    struct stat dst;
    if (stat(newp, &dst) == 0)
      tr_log("[io] rename(%s -> %s) refused: the SOURCE does not exist and the "
             "destination DOES -- left untouched\n", oldp, newp);
    errno = ENOENT;
    return -1;
  }

  /* A rename replaces the destination's contents with no write-open on it, so
   * this is the one mutation route a resident copy would not otherwise see.
   * Unity commits downloads by renaming Temp/ over Shared/. */
  fb_invalidate(newp);

  if (rename(oldp, newp) == 0) return 0;          /* 1. destination absent: plain rename */

  struct stat st;
  if (stat(newp, &st) != 0) {                     /* dest really is absent -> genuine error */
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, dest absent (errno %d)\n", oldp, newp, errno);
    return -1;
  }

  /* 2. park the destination aside */
  char aside[576];
  snprintf(aside, sizeof aside, "%s.rnold", newp);
  /* A leftover .rnold is not litter -- it is a destination that was parked and
   * never restored, i.e. the only surviving copy. Deleting it was how the loss
   * became permanent. If the real path is missing, put it back. */
  {
    struct stat ast, nst;
    if (stat(aside, &ast) == 0) {
      if (stat(newp, &nst) != 0 && rename(aside, newp) == 0)
        debugPrintf("[io] recovered %s from an interrupted rename\n", newp);
      else
        remove(aside);
    }
  }
  if (rename(newp, aside) == 0) {
    if (rename(oldp, newp) == 0) {
      remove(aside);
      if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) ok (displaced existing)\n", oldp, newp);
      return 0;
    }
    if (rename(aside, newp) != 0)
      debugPrintf("[io] rename(%s -> %s) failed AND the original could not be put "
                  "back -- it is parked at %s\n", oldp, newp, aside);
    else if (TRACE_IO)
      debugPrintf("[io] rename(%s -> %s) FAILED, original restored (errno %d)\n", oldp, newp, errno);
    return -1;
  }

  /* 2b. The destination is held OPEN by the caller, and Horizon will not rename it, delete it,
   *     or even open it for writing (EIO) while a handle is live. il2cpp's native ReplaceFile is
   *     literally holding the file we have to replace:
   *         fd = open(backup, O_RDONLY);   rename(dest -> backup);   ... ;   close(fd);
   *     Nothing can be done to that file until the handle is gone -- so close it ourselves.
   *
   *     This is safe: the handle is a pure existence probe. ReplaceFile never reads a byte from
   *     it; its only other use is the close() afterwards, which on a stale fd returns EBADF and
   *     is ignored. To stop that fd NUMBER being handed to another thread in the gap, we re-open
   *     the destination right after the swap -- fds are allocated lowest-free, so we normally get
   *     the same number straight back and the caller's close() then closes our handle, which is
   *     exactly what it meant to do. */
  int victim = fd_close_by_path(newp);
  if (victim >= 0) {
    int ok = 0;
    if (rename(newp, aside) == 0) {
      if (rename(oldp, newp) == 0) { remove(aside); ok = 1; }
      else if (rename(aside, newp) != 0)
        debugPrintf("[io] rename(%s -> %s) failed AND the original could not be put "
                    "back -- it is parked at %s\n", oldp, newp, aside);
    }
    int re = open(newp, O_RDONLY);              /* re-occupy the fd number for the caller */
    if (re >= 0) {
      if (re == victim) fd_ino_set(re, newp);   /* caller's close() will close this */
      else close(re);
    }
    if (ok) {
      if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) ok (closed caller's handle, then displaced)\n", oldp, newp);
      return 0;
    }
  }

  /* 3. still stuck -> overwrite the destination's contents instead of its directory entry.
   *    Read the source completely first; only then truncate the destination. */
  int fsrc = open(oldp, O_RDONLY);
  if (fsrc < 0) {
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, cannot read source (errno %d)\n", oldp, newp, errno);
    return -1;
  }
  struct stat ss;
  if (fstat(fsrc, &ss) != 0 || ss.st_size < 0) { close(fsrc); return -1; }
  size_t n = (size_t)ss.st_size;
  unsigned char *buf = (unsigned char *)malloc(n ? n : 1);
  if (!buf) { close(fsrc); return -1; }
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fsrc, buf + got, n - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  close(fsrc);
  if (got != n) {
    free(buf);
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, short read of source (%zu/%zu)\n", oldp, newp, got, n);
    return -1;                                    /* destination untouched */
  }

  int fdst = open(newp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fdst < 0) {
    free(buf);
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, cannot write dest (errno %d)\n", oldp, newp, errno);
    return -1;
  }
  size_t put = 0;
  while (put < n) {
    ssize_t w = write(fdst, buf + put, n - put);
    if (w <= 0) break;
    put += (size_t)w;
  }
  fsync(fdst);
  close(fdst);
  free(buf);
  if (put != n) {
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, short write of dest (%zu/%zu)\n", oldp, newp, put, n);
    return -1;
  }

  /* The destination now holds the source's bytes; rename() also means the source is gone. */
  if (remove(oldp) != 0)
    if (TRACE_IO) debugPrintf("[io] rename: warning, could not unlink source %s (errno %d)\n", oldp, errno);
  if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) ok (%zu bytes copied over open dest)\n", oldp, newp, n);
  return 0;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  char _rbuf[512];
  path = asset_redirect(path, _rbuf, sizeof _rbuf);
  {
    uint64_t psz, pino; int pdir;
    if (asset_pack_stat_path_info(path, &psz, &pino, &pdir)) {
      struct stat pk; memset(&pk, 0, sizeof pk);
      pk.st_mode = pdir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
      pk.st_size = (off_t)psz;
      pk.st_ino  = (ino_t)pino;
      pk.st_nlink = 1;
      convert_stat(&pk, st);
      st->st_ino = pino;
      return 0;
    }
  }
  struct stat real; int r = stat(path, &real);
  if (r != 0) {
    /* Horizon will not open a file that is already open for WRITE, and libnx's
     * stat() works by opening the file -- so stat() of a file we are writing
     * fails. Unity's ArchiveStorageCreator::Finalize sizes its cache archive with
     * stat(path) while the archive is still open, reads the failure as size 0, and
     * throws the bundle away ("Mismatching archive size ... got 0"): every
     * downloaded AssetBundle, 68 of 68, in the eighth hardware run. Answer from the
     * handle we already hold; fstat works on it. */
    const int ofd = fd_open_on_path(path);
    if (ofd >= 0 && fstat(ofd, &real) == 0) r = 0;
  }
  if (r != 0) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt))) r = stat(alt, &real);
  }
  if (r == 0) {
    convert_stat(&real, st);
    if (st->st_ino == 0) st->st_ino = path_ino(path);   // fsdev gives 0 -> synth
  }
  return r;
}
int fstat_fake_unguarded(int fd, struct bionic_stat *st) {
  {
    uint64_t psz, pino; int pdir;
    if (asset_pack_fstat_fd(fd, &psz, &pino, &pdir)) {
      struct stat pk; memset(&pk, 0, sizeof pk);
      pk.st_mode = pdir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
      pk.st_size = (off_t)psz;
      pk.st_ino  = (ino_t)pino;
      pk.st_nlink = 1;
      convert_stat(&pk, st);
      st->st_ino = pino;
      return 0;
    }
  }
  struct stat real; const int r = fstat(fd, &real);
  if (r == 0) {
    convert_stat(&real, st);
    if (st->st_ino == 0) {                               // mirror stat(path)'s inode
      uint64_t ino = (fd >= 0 && fd < FD_INO_MAX) ? g_fd_ino[fd] : 0;
      st->st_ino = ino ? ino : ((uint64_t)(fd + 1) * 2654435761ULL) | 1;
    }
    /* What the game is TOLD about a cached entry, and whether it matches the
     * size the residency was built from. A disagreement here would make Unity
     * think the bundle is the wrong length, which is one of the three things
     * left that could explain an eviction with correct data. */
    { struct TrEntry *te = tr_by_fd(fd);
      if (te) {
        te->fstats++;
        tr_op(te, 'f', 0, 0, (int64_t)real.st_size, 0, 0);
        if (te->file_size && (long)real.st_size != te->file_size)
          tr_log("[trace] FSTAT MISMATCH %s: open saw %ld, fstat now says %ld\n",
                      te->key, te->file_size, (long)real.st_size);
        else if (g_tr_lines < 400) { g_tr_lines++;
          tr_log("[trace] fstat %s fd=%d -> size %ld\n",
                      te->key, fd, (long)real.st_size); }
      } }
  }
  return r;
}
int fstat_fake(int fd, struct bionic_stat *st) {
  if (!fdg_enter(fd)) return -1;
  int r = fstat_fake_unguarded(fd, st);
  fdg_leave(fd);
  return r;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }

/* truncate(path, len), for real (it was a silent no-op). A file we hold open cannot be
 * reopened on Horizon, so truncate through our own fd when there is one. */
int truncate_fake(const char *path, long len) {
  char rb[512];
  path = asset_redirect(path, rb, sizeof rb);
  fb_invalidate(path);
  const int ofd = fd_open_on_path(path);
  if (ofd >= 0) return ftruncate(ofd, (off_t)len);
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  const int r = ftruncate(fd, (off_t)len);
  const int e = errno;
  close(fd);
  errno = e;
  return r;
}

/* access()/exists checks hit the same Horizon rule as stat(): true if we hold it open. */
int bp_path_is_open(const char *path) {
  char rb[512];
  path = asset_redirect(path, rb, sizeof rb);
  return fd_open_on_path(path) >= 0;
}

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256];
};

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // not thread-safe (matches bionic readdir)
  if (asset_pack_dir_is(dirp)) {
    memset(&out, 0, sizeof(out));
    const char *nm = asset_pack_readdir_path(dirp, &out.d_type, &out.d_ino);
    if (!nm) return NULL;
    out.d_reclen = sizeof(out);
    snprintf(out.d_name, sizeof(out.d_name), "%s", nm);
    return &out;
  }
  struct dirent *e = readdir((DIR *)dirp);
  if (!e) return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C-locale versions
// ---------------------------------------------------------------------------

void *newlocale_fake(int mask, const char *locale, void *base) { (void)mask; (void)locale; (void)base; return (void *)1; }
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha) WRAP_ISW_L(iswblank) WRAP_ISW_L(iswcntrl) WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower) WRAP_ISW_L(iswprint) WRAP_ISW_L(iswpunct) WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper) WRAP_ISW_L(iswxdigit) WRAP_ISW_L(towlower) WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) { (void)loc; return strftime(s, max, fmt, (const struct tm *)tm); }
long double strtold_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) { if (dst) dst[i] = (unsigned char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) { if (dst) dst[i] = (char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

// --- anonymous mmap arena (page-granular; supports sub-range munmap) ----------
//
// Switch has no mmap. Unity reserves big *256MB-aligned* pools by over-mmapping a
// larger region then munmapping the unaligned head/tail to keep an aligned middle.
// A plain malloc/free-per-mmap frees the WHOLE block when the head is trimmed (the
// trim's addr == the registered base) and the kept aligned middle is then reused
// out from under the engine -> the TLSF allocator's free block reads back zeroed
// (next_free == NULL) and faults. So we manage a dedicated arena (carved 256MB-
// aligned in __libnx_initheap) with a per-page used-bitmap: mmap = find a free run
// of pages and mark them; munmap = clear exactly the pages of the sub-range. Big
// requests are handed back 256MB-aligned so Unity only ever trims the tail.
// File-backed maps (Unity streams BGM/SE this way) are served from the same arena.
// ------------------------------------------------------------------------------
extern void  *g_mmap_arena_base;   // set by __libnx_initheap (main.c)
extern size_t g_mmap_arena_size;
extern int    g_overcommit;        // 1 = alias-region on-demand commit
extern u64    g_alias_base, g_alias_size;

#define BIONIC_MAP_ANONYMOUS 0x20
#define MMAP_PAGE       0x1000u
#define MMAP_BIG_ALIGN  MMAP_ARENA_ALIGN
#define MMAP_BIG_THRESH ((size_t)64 * 1024 * 1024)
#define BIONIC_PROT_NONE 0x0
#define BIONIC_PROT_WRITE 0x2
#define BIONIC_MADV_DONTNEED 4

static uint8_t *mmap_arena;    // 256MB-aligned usable base (published last)
static size_t   mmap_usable;   // usable bytes
static size_t   mmap_pages;    // usable / page
static uint8_t *mmap_used;     // 1 byte/page bitmap: reserved (address space)
static uint8_t *mmap_committed;// 1 byte/page bitmap: physically committed (overcommit only)
static size_t   g_committed_pages;   // running count of committed pages
static size_t   g_commit_peak;       // high-water mark (pages)
static Mutex    g_mmap_lock;   // zero-init == valid unlocked libnx mutex

// --- overcommit commit/decommit (caller holds g_mmap_lock) -------------------
// svcMapPhysicalMemory zero-fills and draws from the freed physical limit; it
// FAILS on already-mapped pages, so we only ever commit pages we track as
// uncommitted, in contiguous runs. Out-of-physical is logged, not fatal.
static void arena_commit_locked(size_t first, size_t cnt) {
  size_t i = 0;
  while (i < cnt) {
    if (mmap_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && !mmap_committed[first + i + run]) run++;
    u64 a = (u64)(uintptr_t)(mmap_arena + (first + i) * MMAP_PAGE);
    Result rc = svcMapPhysicalMemory((void *)a, (u64)run * MMAP_PAGE);
    if (R_SUCCEEDED(rc)) {
      for (size_t k = 0; k < run; k++) mmap_committed[first + i + k] = 1;
      g_committed_pages += run;
      if (g_committed_pages > g_commit_peak) {
        size_t prev = g_commit_peak;
        g_commit_peak = g_committed_pages;
        if ((g_commit_peak >> 16) != (prev >> 16))   // new 256MB high-water mark
          if (TRACE_MMAP) debugPrintf("[mmap] committed peak %u MB (live %u MB)\n",
                      (unsigned)((g_commit_peak * MMAP_PAGE) >> 20),
                      (unsigned)((g_committed_pages * MMAP_PAGE) >> 20));
      }
    } else {
      if (TRACE_MMAP) debugPrintf("[mmap] COMMIT FAIL %u KB @ 0x%lx rc=0x%x (committed %u MB peak %u MB)\n",
                  (unsigned)((run * MMAP_PAGE) >> 10), (unsigned long)a, rc,
                  (unsigned)((g_committed_pages * MMAP_PAGE) >> 20),
                  (unsigned)((g_commit_peak * MMAP_PAGE) >> 20));
    }
    i += run ? run : 1;
  }
}

static void arena_decommit_locked(size_t first, size_t cnt) {
  size_t i = 0;
  while (i < cnt) {
    if (!mmap_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && mmap_committed[first + i + run]) run++;
    u64 a = (u64)(uintptr_t)(mmap_arena + (first + i) * MMAP_PAGE);
    if (R_SUCCEEDED(svcUnmapPhysicalMemory((void *)a, (u64)run * MMAP_PAGE))) {
      for (size_t k = 0; k < run; k++) mmap_committed[first + i + k] = 0;
      g_committed_pages -= run;
    }
    i += run ? run : 1;
  }
}

// translate [addr,addr+len) to a clamped page range within the arena; returns 0 if
// outside the arena (e.g. a newlib-fallback pointer), else 1 with *first/*cnt set.
static int arena_page_range(void *addr, size_t len, size_t *first, size_t *cnt) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return 0;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return 0;
  size_t f = off / MMAP_PAGE;
  size_t c = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (f + c > mmap_pages) c = mmap_pages - f;
  *first = f; *cnt = c;
  return 1;
}

// commit [addr,len) on demand (mprotect RW / anon mmap). no-op if not overcommit.
static void arena_commit_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) arena_commit_locked(first, cnt);
  mutexUnlock(&g_mmap_lock);
}

// decommit [addr,len) (mprotect PROT_NONE / munmap). reclaims physical. Safe
// because re-use of a decommitted page goes through mprotect(RW) -> recommit.
static void arena_decommit_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) arena_decommit_locked(first, cnt);
  mutexUnlock(&g_mmap_lock);
}

// madvise(MADV_DONTNEED): zero the committed pages but KEEP them committed. The
// Switch has no fault handler, so decommitting here would crash if the engine
// re-touches without an intervening mprotect(RW) (allowed on Linux). Zeroing
// preserves the "reads back as zero after DONTNEED" contract safely.
static void arena_dontneed_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) {
    for (size_t i = 0; i < cnt; ) {
      if (!mmap_committed[first + i]) { i++; continue; }
      size_t run = 0;
      while (i + run < cnt && mmap_committed[first + i + run]) run++;
      memset(mmap_arena + (first + i) * MMAP_PAGE, 0, run * MMAP_PAGE);
      i += run;
    }
  }
  mutexUnlock(&g_mmap_lock);
}

// ===========================================================================
// Stack-region overcommit (OC) arena.
// Boot probe established: svcMapMemory can alias heap pages into the STACK
// region (the alias region is rejected, kernel err 0xdc01), and Unity reserves
// ~2.8GB of PROT_NONE blocks while committing only ~80MB via mprotect(RW) with
// ZERO decommits. So we satisfy the big PROT_NONE reservations from a cheap
// stack-region address window and alias a small bump-allocated heap commit-pool
// in on mprotect(RW). Tried BEFORE the heap-backed arena for big anon PROT_NONE
// maps; anything else (and overflow when the window fills) falls through to the
// heap-backed arena, so if OC setup fails the engine runs exactly as before.
// Because decommits are never observed, the pool is a no-reclaim bump allocator.
// ===========================================================================
static uint8_t *oc_base;        // stack-region window base (256MB-aligned)
static size_t   oc_pages;       // window size in pages (0 => OC disabled)
static uint8_t *oc_used;        // 1/page: address space reserved by an mmap
static uint8_t *oc_committed;   // 1/page: physically backed via svcMapMemory
static uint8_t *oc_pool;        // commit-pool base (heap, page-aligned)
static size_t   oc_pool_pages;  // pool capacity in pages
static size_t   oc_pool_bump;   // next free pool page (bump; no reclaim)
static size_t   oc_live_pages;  // committed pages (diagnostic)

// Called once from main() after the newlib heap exists. window = a reserved
// stack-region range; pool = a heap buffer. Returns 1 if OC is armed.
int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes) {
  if (!window || !pool || !window_bytes || !pool_bytes) return 0;
  size_t wp = window_bytes / MMAP_PAGE;
  uint8_t *u = (uint8_t *)calloc(wp, 1);
  uint8_t *c = (uint8_t *)calloc(wp, 1);
  if (!u || !c) { free(u); free(c); return 0; }
  mutexLock(&g_mmap_lock);
  oc_base = (uint8_t *)window; oc_pages = wp; oc_used = u; oc_committed = c;
  oc_pool = (uint8_t *)pool; oc_pool_pages = pool_bytes / MMAP_PAGE;
  oc_pool_bump = 0; oc_live_pages = 0;
  mutexUnlock(&g_mmap_lock);
  return 1;
}

static int oc_contains(void *addr) {
  return oc_pages && (uint8_t *)addr >= oc_base &&
         (uint8_t *)addr < oc_base + oc_pages * MMAP_PAGE;
}

// A thread stack (e.g. an audio worker's) can get mapped by libnx INSIDE the OC
// window after arm time -- the boot hole-scan can't see stacks created later, and
// the virtmem reservation on the window doesn't reliably deflect them. Reserving
// such a range and later svcMapMemory'ing over it fails (0xd401 InvalidCurrentMemory)
// and leaves Unity's allocation unbacked -> the engine faults. So before handing out
// a candidate, confirm every page is still Unmapped; if a mapped span is found, mark
// its OC pages used so the scan permanently routes around it. caller holds g_mmap_lock.
static int oc_range_occupied(size_t i, size_t need) {
  uint64_t a   = (uint64_t)(uintptr_t)(oc_base + i * MMAP_PAGE);
  uint64_t end = a + (uint64_t)need * MMAP_PAGE;
  int occ = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) { occ = 1; break; }
    uint64_t span_end = mi.addr + mi.size;
    if (span_end <= a) { occ = 1; break; }
    if (mi.type != MemType_Unmapped) {
      occ = 1;
      uint64_t s = mi.addr > (uint64_t)(uintptr_t)oc_base ? mi.addr
                                                          : (uint64_t)(uintptr_t)oc_base;
      size_t p0 = (size_t)((s - (uint64_t)(uintptr_t)oc_base) / MMAP_PAGE);
      size_t p1 = (size_t)((span_end - (uint64_t)(uintptr_t)oc_base + MMAP_PAGE - 1) / MMAP_PAGE);
      for (size_t k = p0; k < p1 && k < oc_pages; k++) oc_used[k] = 1;
      debugPrintf("[oc] window range 0x%llx has mapped span 0x%llx..0x%llx (type=0x%x) -> routing around\n",
                  (unsigned long long)s, (unsigned long long)mi.addr,
                  (unsigned long long)span_end, mi.type);
    }
    a = span_end;
  }
  return occ;
}

// Reserve address space in the OC window. Mirrors mmap_arena_alloc_locked's
// 256MB-aligned tail-overflow so Unity's 511MB over-map nets one 256MB slot.
// caller holds g_mmap_lock.
static void *oc_alloc_locked(size_t len, size_t *got) {
  *got = 0;
  if (!oc_pages) return NULL;
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE; if (!need) need = 1;
  const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;
  size_t kept = need > step ? need - step : need;
  for (size_t i = 0; i + need <= oc_pages; i += step) {        // pass 1: full over-map fits
    size_t run = 0; while (run < need && !oc_used[i + run]) run++;
    if (run == need) {
      if (oc_range_occupied(i, need)) continue;   // a thread stack landed here -> skip
      for (size_t k = 0; k < need; k++) oc_used[i + k] = 1;
      *got = need * MMAP_PAGE; return oc_base + i * MMAP_PAGE;
    }
  }
  for (size_t i = 0; i < oc_pages; i += step) {                // pass 2: tail slot
    if (i + need <= oc_pages) continue;
    size_t avail = oc_pages - i; if (avail < kept) continue;
    size_t run = 0; while (run < avail && !oc_used[i + run]) run++;
    if (run == avail) {
      if (oc_range_occupied(i, avail)) continue;  // a thread stack landed here -> skip
      for (size_t k = 0; k < avail; k++) oc_used[i + k] = 1;
      *got = avail * MMAP_PAGE; return oc_base + i * MMAP_PAGE;
    }
  }
  return NULL;
}

// Commit [addr,len): alias contiguous bump-pool runs into the reserved OC range
// via svcMapMemory (which remaps the pool source away -- we only access via the
// OC address). Already-committed pages are skipped. caller holds g_mmap_lock.
static void oc_commit_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  size_t i = 0;
  while (i < cnt) {
    if (oc_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && !oc_committed[first + i + run]) run++;
    if (oc_pool_bump + run > oc_pool_pages) {
      debugPrintf("[oc] commit-pool EXHAUSTED: need %zu pages, %zu left (live %zu MB)\n",
                  run, oc_pool_pages - oc_pool_bump, (oc_live_pages * MMAP_PAGE) >> 20);
      return;   // can't back; touching it would fault (shouldn't happen at pool size)
    }
    void *dst = oc_base + (first + i) * MMAP_PAGE;
    void *src = oc_pool + oc_pool_bump * MMAP_PAGE;
    Result rc = svcMapMemory(dst, src, (u64)run * MMAP_PAGE);
    if (R_FAILED(rc)) {
      debugPrintf("[oc] svcMapMemory FAIL dst=%p run=%zu rc=0x%x\n", dst, run, rc);
      return;
    }
    memset(dst, 0, run * MMAP_PAGE);   // freshly committed anon must read as zero
    for (size_t k = 0; k < run; k++) oc_committed[first + i + k] = 1;
    oc_pool_bump += run; oc_live_pages += run;
    if (((oc_live_pages * MMAP_PAGE) >> 24) != (((oc_live_pages - run) * MMAP_PAGE) >> 24))
      if (TRACE_MMAP) debugPrintf("[oc] committed %zu MB (pool %zu/%zu MB)\n",
                  (oc_live_pages * MMAP_PAGE) >> 20,
                  (oc_pool_bump * MMAP_PAGE) >> 20, (oc_pool_pages * MMAP_PAGE) >> 20);
    i += run;
  }
}

// munmap of an OC range: reclaim only UNCOMMITTED pages (the tail-overflow slack
// Unity trims after each over-map). Committed pages stay reserved+mapped (a small
// bounded leak) so a later reservation can't collide with a live alias.
static void oc_free_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  for (size_t i = 0; i < cnt; i++)
    if (!oc_committed[first + i]) oc_used[first + i] = 0;
}

// caller holds g_mmap_lock
static void mmap_arena_init_locked(void) {
  if (mmap_arena) return;
  uint8_t *base; size_t usable;
  if (g_mmap_arena_base) {
    base   = (uint8_t *)g_mmap_arena_base;   // dedicated, already 256MB-aligned
    usable = g_mmap_arena_size;
  } else {
    // fallback (small heap / applet): memalign a modest arena (< 2GB newlib limit)
    /* Ask the allocator for the big alignment directly: dlmalloc's memalign
     * returns the leading slack to the heap, where the old memalign(4 KB) +
     * manual align-up left up to MMAP_BIG_ALIGN (64 MB) dead inside the chunk.
     * The heap was at 2079 MB in use with 58 MB free before the game had
     * loaded a level; every megabyte here is one Unity gets back. Falls back
     * to the old path if the allocator cannot do the alignment. */
    usable = (size_t)bp_mmap_arena_mb * 1024 * 1024;
    uint8_t *raw = memalign(MMAP_BIG_ALIGN, usable);
    if (raw) base = raw;
    else {
      const size_t want = usable + MMAP_BIG_ALIGN;
      raw = memalign(MMAP_PAGE, want);
      if (!raw) fatal_error("mmap arena alloc (%u MB) failed", (unsigned)(want >> 20));
      base = (uint8_t *)(((uintptr_t)raw + (MMAP_BIG_ALIGN - 1)) & ~(MMAP_BIG_ALIGN - 1));
    }
  }
  size_t pages  = usable / MMAP_PAGE;
  uint8_t *used = (uint8_t *)calloc(pages, 1);
  if (!used) fatal_error("mmap bitmap alloc failed");
  if (g_overcommit) {
    mmap_committed = (uint8_t *)calloc(pages, 1);
    if (!mmap_committed) fatal_error("mmap commit-bitmap alloc failed");
  }
  mmap_usable = usable; mmap_pages = pages; mmap_used = used;
  mmap_arena  = base;   // publish last (alloc/free key off this)
  if (TRACE_MMAP) debugPrintf("[mmap] arena: %u MB %s at %p\n", (unsigned)(usable >> 20),
              g_overcommit ? "virtual (alias, on-demand commit)" : "256MB-aligned heap-backed",
              base);
}

// caller holds g_mmap_lock.
// Returns the mapped base and writes the number of bytes ACTUALLY reserved
// (in-arena) to *got. For big alignment over-maps (Unity reserves block+align,
// then munmaps the unaligned head/tail), the request is much larger than the
// ~256MB block Unity actually keeps. Normally we reserve the whole over-map and
// let the tail-munmap give it back. But for the LAST 256MB slot the full over-map
// runs past the arena end, so a plain "need contiguous pages" search fails even
// though the kept block fits. In that case we reserve only [slot, arena_end) -- the
// kept block lives there; Unity's tail-munmap targets addresses beyond our arena
// and is a harmless no-op. This removes the transient peak so each block costs
// exactly its 256MB slot (floor(arena/256MB) blocks fit, no 2x headroom needed).
static void *mmap_arena_alloc_locked(size_t len, size_t *got) {
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (!need) need = 1;
  if (len >= MMAP_BIG_THRESH) {
    const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;   // 256MB in pages
    size_t kept = need > step ? need - step : need;   // pages Unity actually keeps
    // pass 1: full over-map fits within the arena (normal case for all but the last slot)
    for (size_t i = 0; i + need <= mmap_pages; i += step) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
    }
    // pass 2: tail slot -- the over-map would spill past the arena end, but the kept
    // block fits in [slot, arena_end). Reserve only that; the spill is trimmed away.
    for (size_t i = 0; i < mmap_pages; i += step) {
      if (i + need <= mmap_pages) continue;          // handled by pass 1
      size_t avail = mmap_pages - i;
      if (avail < kept) continue;                    // kept block wouldn't fit
      size_t run = 0;
      while (run < avail && !mmap_used[i + run]) run++;
      if (run == avail) {
        for (size_t k = 0; k < avail; k++) mmap_used[i + k] = 1;
        *got = avail * MMAP_PAGE;                     // only the in-arena portion
        return mmap_arena + i * MMAP_PAGE;
      }
    }
  } else {
    for (size_t i = 0; i + need <= mmap_pages; ) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
      i += run + 1;
    }
  }
  *got = 0;
  return NULL;
}

/* Does Unity ever give arena space back? "now" has equalled "peak" in every
 * [mem] sample ever taken, which means the arena only ever grows. Either Unity
 * does not munmap these maps, or munmap is not reaching here. Counting both
 * separates those two, and they need completely different fixes. */
static size_t g_arena_freed_b; static unsigned g_arena_free_n, g_munmap_n;
void bp_munmap_stats(size_t *freed, unsigned *arena_calls, unsigned *all_calls) {
  if (freed)       *freed       = g_arena_freed_b;
  if (arena_calls) *arena_calls = g_arena_free_n;
  if (all_calls)   *all_calls   = g_munmap_n;
}
static void mmap_arena_free(void *addr, size_t len) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return;
  size_t first = off / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  mutexLock(&g_mmap_lock);
  g_arena_freed_b += len; g_arena_free_n++;       /* under the lock: were racy */
  for (size_t k = 0; k < cnt && first + k < mmap_pages; k++)
    mmap_used[first + k] = 0;
  mutexUnlock(&g_mmap_lock);
}

// Stopgap: when the 256MB-block arena is exhausted by Unity's 9 pools, small
// (sub-threshold) il2cpp/GC mmaps still need to land somewhere. Serve them from
// newlib's free heap via memalign and track each so munmap can free it. This is
// not real overcommit (it consumes physical newlib heap), but it unblocks the
// sub-1MB il2cpp allocations that were failing and surfaces il2cpp's true mmap
// appetite in the log to size the proper fix.
#define MMAP_FALLBACK_MAX 4096
/* Each spilled map remembers which byte ranges Unity has already unmapped
 * (offsets from ptr, sorted, merged). See mmap_fallback_free(). nrel < 0 means
 * the pattern got too fragmented to track, so the chunk is simply kept whole --
 * the old, safe behaviour. */
#define FB_IV 4
/* Return a released tail to the allocator at once, instead of when the whole
 * map has been given back. Saves ~64 MB per long-lived spill. The cost: once
 * returned, that range cannot be MAP_FIXED back, and the GC aborts if a fixed
 * remap is refused. Nothing that trims a tail remaps it -- Unity never does,
 * and the GC unmaps by remapping PROT_NONE and never calls munmap -- and a
 * refusal is logged ("MAP_FIXED ... is not memory this shim owns"). If that
 * line ever appears, set this to 0: interval tracking alone still fixes the
 * leak, it just holds the tail until the block is released. */
#define FB_SHRINK_TAILS 1
typedef struct { uint32_t lo, hi; } FbRange;
static struct { void *ptr; size_t len; int nrel; FbRange rel[FB_IV]; } g_fb[MMAP_FALLBACK_MAX];
static int   g_fb_n = 0;
static size_t g_fb_bytes = 0;
static size_t g_arena_peak_pages;   /* high-water mark of reserved arena pages */

/* UNITY'S COMMIT RATIO -- the one number nobody has measured.
 *
 * Every big map Unity asks for is PROT_NONE: address space it reserves and may
 * never touch. It commits pieces later with mprotect(RW). On Horizon there is
 * no reserve-without-commit, so the arena hands back real memory for all of it.
 * If Unity only ever writes to a fraction, the rest is dead weight -- and the
 * overcommit machinery in this file (oc_arena_init, arena_commit_locked,
 * svcMapPhysicalMemory) exists to reclaim exactly that, but has never been
 * armed because nobody knew whether the gap was 50 MB or 500.
 *
 * These count bytes mprotect()ed to RW versus back to PROT_NONE inside the
 * arena, so rw - none is Unity's live committed footprint. Reported on the
 * [mem] line next to the arena's reservation. */
static size_t rw_b = 0, none_b = 0;
static unsigned rw_n = 0, none_n = 0, oth_n = 0;
void bp_mmap_commit_stats(size_t *committed, size_t *decommitted, unsigned *n) {
  if (committed)   *committed   = rw_b;
  if (decommitted) *decommitted = none_b;
  if (n)           *n           = rw_n + none_n;
}
static size_t g_fb_leaked;          /* held by refused partial munmaps -- see mmap_fallback_free */
/* For the [mem] breakdown: what the mmap arena and the fallback path hold. */
void bp_mmap_stats(size_t *arena_reserved, size_t *arena_used, size_t *arena_peak, size_t *fallback) {
  size_t used = 0;
  if (mmap_used) for (size_t i = 0; i < mmap_pages; i++) used += mmap_used[i] ? 1 : 0;
  if (used > g_arena_peak_pages) g_arena_peak_pages = used;
  if (arena_reserved) *arena_reserved = mmap_pages * MMAP_PAGE;
  if (arena_used)     *arena_used     = used * MMAP_PAGE;
  if (arena_peak)     *arena_peak     = g_arena_peak_pages * MMAP_PAGE;
  if (fallback)       *fallback       = g_fb_bytes;
}
void bp_ram_stats(size_t *image, size_t *image_used) {
  if (image) *image = g_image_cap;
  if (image_used) *image_used = g_image_used;
}
static Mutex g_fb_lock;

static void *mmap_fallback(size_t length, int flags, int fd, long offset) {
  /* Big anonymous reservations are Unity Dynamic-Heap pools: its allocator masks
   * pointers down to a large-aligned pool base to derive block indices, so a
   * 4KB-aligned block makes that math walk off into unmapped memory (the
   * libunity+0xdce75c Data Abort). Hand big anon maps a MMAP_BIG_ALIGN-aligned
   * base so the index math lands correctly even out here in the newlib overflow. */
  size_t align = (length >= MMAP_BIG_THRESH && (flags & BIONIC_MAP_ANONYMOUS))
                   ? MMAP_BIG_ALIGN : MMAP_PAGE;
  void *q = memalign(align, length);
  if (!q) return NULL;
  long got = 0;
  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(q, 0, length);
  } else {
    if (fd >= 0) {
      long cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0)
        while ((size_t)got < length) { long r = read(fd, (char *)q + got, length - got); if (r <= 0) break; got += r; }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    if ((size_t)got < length) memset((char *)q + got, 0, length - got);
  }
  mutexLock(&g_fb_lock);
  int tracked = 0;
  if (g_fb_n < MMAP_FALLBACK_MAX) { g_fb[g_fb_n].ptr = q; g_fb[g_fb_n].len = length; g_fb[g_fb_n].nrel = 0; g_fb_n++; g_fb_bytes += length; tracked = 1; }
  const size_t total = g_fb_bytes;
  mutexUnlock(&g_fb_lock);
  /* An untracked map can never be freed: munmap finds it in neither the
   * registry nor the arena and does nothing. The old code handed it out anyway,
   * silently, and left it out of the [mem] fallback figure. Refuse instead --
   * an honest ENOMEM beats a leak nobody can see. */
  if (!tracked) {
    free(q);
    static int told;
    if (!told) { told = 1;
      debugPrintf("[mmap] fallback registry full (%d entries) -- refusing rather than "
                  "leaking an untracked %zu KB map\n", MMAP_FALLBACK_MAX, length >> 10); }
    return NULL;
  }
  if (TRACE_MMAP) debugPrintf("[mmap] fallback %u KB -> %p  anon=%d fd=%d off=0x%lx got=%ld (total %u MB)\n",
              (unsigned)(length >> 10), q, !!(flags & BIONIC_MAP_ANONYMOUS), fd, offset, got,
              (unsigned)(total >> 20));
  return q;
}

// returns 1 and frees if addr was a fallback allocation
/* Record [lo, hi) as given back. Returns 1 once the whole chunk is covered.
 * Touching ranges merge, so a head trim plus a tail trim plus the release of
 * the kept block in the middle collapse to [0, len). Called under g_fb_lock. */
static int fb_add_release(int i, size_t lo, size_t hi) {
  if (g_fb[i].nrel < 0) return 0;
  if (g_fb[i].len > UINT32_MAX || hi <= lo) { g_fb[i].nrel = -1; return 0; }
  uint64_t L = lo, H = hi;
  FbRange out[FB_IV + 1]; int m = 0, placed = 0;
  for (int k = 0; k < g_fb[i].nrel; k++) {
    const FbRange r = g_fb[i].rel[k];
    if (r.hi < L)            out[m++] = r;                 /* wholly before */
    else if (r.lo > H) {                                   /* wholly after  */
      if (!placed) { out[m].lo = (uint32_t)L; out[m].hi = (uint32_t)H; m++; placed = 1; }
      out[m++] = r;
    } else { if (r.lo < L) L = r.lo; if (r.hi > H) H = r.hi; }   /* overlap or touch: merge */
    if (m > FB_IV) break;
  }
  if (!placed && m <= FB_IV) { out[m].lo = (uint32_t)L; out[m].hi = (uint32_t)H; m++; }
  if (m > FB_IV) { g_fb[i].nrel = -1; return 0; }          /* too fragmented: keep whole */
  memcpy(g_fb[i].rel, out, (size_t)m * sizeof out[0]);
  g_fb[i].nrel = m;
  return m == 1 && g_fb[i].rel[0].lo == 0 && g_fb[i].rel[0].hi >= g_fb[i].len;
}
static void fb_drop(int i) {                               /* under g_fb_lock */
  free(g_fb[i].ptr);
  g_fb_bytes -= g_fb[i].len;
  g_fb[i] = g_fb[--g_fb_n];
}

/* UNMAPPING PART OF A SPILLED MAP.
 *
 * Unity reserves 128 MB - 4 KB, rounds up to the next 64 MB boundary, keeps
 * 64 MB from there and unmaps the rest; later it unmaps the 64 MB it kept. So
 * over its life every byte of a spilled map is given back -- in two or three
 * pieces, none of which is the whole map at its base.
 *
 * This used to accept only "base address, whole length". A tail trim at
 * base + 64 MB matched no entry and was ignored in silence; the release of the
 * kept block at the base was refused as "partial". Unity gave back all 128 MB
 * and the port freed none of it. On the 1.1.6 crash, Unity reserved and
 * released five in a burst at frame 1080 and they came back at 0x79c0..,
 * 0x79c8.., 0x79d0.., 0x79d8.., 0x79e0.. -- each 128 MB above the last,
 * because the previous one was never freed. That burst is what ran the heap
 * out, and the 64 MB NULL that crashed Unity was the next request.
 *
 * Refusing partial unmaps was not wrong: an earlier version freed the whole
 * chunk on one, while Unity still used the rest, and handed live memory to the
 * next malloc. So nothing here frees early. Released ranges are RECORDED, and
 * the chunk is freed only when every byte has been given back. The one
 * exception returns memory sooner without freeing anything in use: when the
 * TAIL is released and the front is not, the chunk is shrunk in place with
 * realloc. Both newlib allocators keep the pointer on a shrink (dlmalloc splits
 * the chunk, nano-malloc returns it as is), which is checked anyway, and GPU
 * arena chunks are left alone because their realloc path moves the block. */
static int mmap_fallback_free(void *addr, size_t length) {
  uint8_t *a = (uint8_t *)addr;
  /* A zero length is an error that unmaps NOTHING (POSIX: EINVAL). This used to
   * mean "to the end": munmap(base, 0) freed the whole map (inherited), and
   * munmap(inside, 0) would have recorded [inside, end) as released -- and the
   * tail shrink would then have handed memory Unity still holds back to the
   * allocator. Found in the audit of 1.1.7, before it ever ran. */
  if (!length) return 0;
  /* A range that runs off the top of the address space is also EINVAL and
   * unmaps nothing. Clamping it to the end of the map instead would release
   * bytes the caller never asked to release. */
  if (length > UINTPTR_MAX - (uintptr_t)addr) return 0;
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++) {
    uint8_t *p = (uint8_t *)g_fb[i].ptr;
    const size_t n = g_fb[i].len;
    if (a < p || a >= p + n) continue;
    if (a == p && length >= n) {                           /* the whole map at once */
      fb_drop(i);
      mutexUnlock(&g_fb_lock);
      return 1;
    }
    const size_t lo = (size_t)(a - p);
    const size_t hi = (length > n - lo) ? n : lo + length;   /* no wrap on a huge length */
    if (fb_add_release(i, lo, hi)) {                       /* last piece given back */
      fb_drop(i);
      mutexUnlock(&g_fb_lock);
      static unsigned told;
      if (told < 4) { told++;
        debugPrintf("[mmap] spilled map %p fully given back in pieces -- freed %zu MB\n",
                    (void *)p, n >> 20); }
      return 1;
    }
    /* Tail given back while the front is still in use: return it now. */
    size_t shrunk = 0;
    if (FB_SHRINK_TAILS && g_fb[i].nrel > 0) {
      const FbRange last = g_fb[i].rel[g_fb[i].nrel - 1];
      extern int bp_gpua_owns(const void *);
      if (last.hi >= n && last.lo > 0 && !bp_gpua_owns(p)) {
        void *q = realloc(p, last.lo);
        if (q == p) {
          shrunk = n - last.lo;
          g_fb[i].len = last.lo;
          g_fb[i].nrel--;
          g_fb_bytes -= shrunk;
        } else if (q) {
          /* Cannot happen with either newlib allocator. If it ever does, the
           * block Unity holds has moved; say so as loudly as possible. */
          debugPrintf("[mmap] *** realloc MOVED a spilled map (%p -> %p) -- Unity's "
                      "pointer is now stale ***\n", (void *)p, q);
        }
      }
    }
    mutexUnlock(&g_fb_lock);
    if (shrunk) {
      static unsigned told2;
      if (told2 < 4) { told2++;
        debugPrintf("[mmap] tail of spilled map %p given back -- shrunk in place, %zu MB "
                    "returned now\n", (void *)p, shrunk >> 20); }
    }
    return 1;
  }
  mutexUnlock(&g_fb_lock);
  return 0;
}

size_t bp_mmap_leaked(void) {          /* given back by Unity, not yet freed */
  size_t held = 0;
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++)
    for (int k = 0; k < g_fb[i].nrel; k++) held += g_fb[i].rel[k].hi - g_fb[i].rel[k].lo;
  mutexUnlock(&g_fb_lock);
  (void)g_fb_leaked;
  return held;
}

// ---- read-only file-map dedup cache ---------------------------------------
// il2cpp builds a stack trace for every thrown managed exception (even caught
// ones -- GPG login retries, etc.), and the symbolizer mmaps libil2cpp.so
// (~40MB) + libunity.so (~25MB) read-only EACH TIME, never munmapping. Over a
// play session that is dozens of fresh copies (~1.4GB) that exhaust newlib ->
// the "arena FULL / out of RAM" self-exit. Dedup: hand back one shared, pinned
// buffer per (file inode, offset, length). Safe -- these maps are read-only.
#define MAPC_N 24
static struct { uint64_t ino; long off; size_t len; void *ptr; } g_mapc[MAPC_N];
static int g_mapc_n = 0;
static void *mapcache_get(uint64_t ino, long off, size_t len) {
  void *r = NULL;
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_mapc_n; i++)
    if (g_mapc[i].ino == ino && g_mapc[i].off == off && g_mapc[i].len == len) { r = g_mapc[i].ptr; break; }
  mutexUnlock(&g_fb_lock);
  return r;
}
static void mapcache_put(uint64_t ino, long off, size_t len, void *ptr) {
  mutexLock(&g_fb_lock);
  /* Pin ONLY if it can actually be cached. This used to drop the pointer from
   * the fallback list first and check for room second -- so with the cache
   * full (MAPC_N entries) the map was neither freeable by munmap nor findable
   * by mapcache_get: leaked for the life of the process, on every read-only
   * file map past the 24th. Left in the fallback list it stays freeable. */
  if (g_mapc_n < MAPC_N) {
    for (int i = 0; i < g_fb_n; i++)
      if (g_fb[i].ptr == ptr) { g_fb_bytes -= g_fb[i].len; g_fb[i] = g_fb[--g_fb_n]; break; }
    g_mapc[g_mapc_n].ino = ino; g_mapc[g_mapc_n].off = off;
    g_mapc[g_mapc_n].len = len; g_mapc[g_mapc_n].ptr = ptr; g_mapc_n++;
  }
  mutexUnlock(&g_fb_lock);
}

#define BIONIC_MAP_FIXED 0x10
/* Anonymous MAP_FIXED over memory this shim handed out: replace the pages IN
 * PLACE and return the same address, as Linux does. Boehm's GC_unmap returns
 * idle heap to the OS with exactly
 *     mmap(start, len, PROT_NONE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0)
 * and ABORTS unless it gets `start` back ("mmap(PROT_NONE) failed"). This shim
 * used to ignore addr, return fresh memory elsewhere (leaking it) and the
 * collector aborted -- the fifth hardware run, frame il2cpp+0x7d3b3c. Boehm then
 * re-enables such pages with mprotect(RW), which mprotect_fake already commits.
 * Contents need not survive (Linux zero-fills), and the collector clears what it
 * hands out, so PROT_NONE releases physical and RW commits + zeroes. */
static void *mmap_fixed_anon(void *addr, size_t len, int prot) {
  uint8_t *a = (uint8_t *)addr;
  /* mmap_arena / mmap_usable, NOT g_mmap_arena_base. That symbol is set to NULL
   * in main.c and never assigned -- it is the hook for a dedicated arena that
   * __libnx_initheap does not provide -- so this test was ALWAYS FALSE and the
   * branch below was dead. Boehm's MAP_FIXED remaps inside the arena therefore
   * fell through to the ownership check and were refused, and the comment above
   * records what Boehm does when it does not get `start` back: it aborts. */
  const int in_arena = mmap_arena && a >= mmap_arena &&
                       a + len <= mmap_arena + mmap_usable;
  if (in_arena) {
    if (prot == BIONIC_PROT_NONE) { if (g_overcommit) arena_decommit_range(addr, len); }
    else { if (g_overcommit) arena_commit_range(addr, len); memset(addr, 0, len); }
    return addr;
  }
  if (oc_contains(addr)) {
    if (prot != BIONIC_PROT_NONE) {
      mutexLock(&g_mmap_lock); oc_commit_locked(addr, len); mutexUnlock(&g_mmap_lock);
      memset(addr, 0, len);
    }
    return addr;                                   /* PROT_NONE: left mapped (as mprotect_fake does) */
  }
  /* newlib-fallback allocations: ONLY ranges this shim actually handed out.
   *
   * This used to accept any address whose pages were mapped RW -- which is the
   * entire newlib heap, every RAM-cache blob included -- and memset it to zero.
   * The comment above it said "fallback allocations"; the check said "anything
   * writable". Consult the fallback list and the map cache instead, and refuse
   * the rest. A refusal is logged with the address so a caller that believed
   * it owned that memory shows up as exactly that. */
  { int owned = 0;
    mutexLock(&g_fb_lock);
    for (int i = 0; i < g_fb_n && !owned; i++)
      if (a >= (uint8_t *)g_fb[i].ptr && a + len <= (uint8_t *)g_fb[i].ptr + g_fb[i].len) {
        owned = 1;
        /* A fixed remap makes its range LIVE again. If part of it had been
         * unmapped earlier, the release record for this spill is now wrong, and
         * trusting it would free the chunk while this range is in use. Stop
         * tracking it piecewise: it goes back to the old rule, freed only by a
         * whole-map unmap -- a possible leak, never a use-after-free. */
        const size_t off = (size_t)(a - (uint8_t *)g_fb[i].ptr);
        for (int k = 0; k < g_fb[i].nrel; k++)
          if (g_fb[i].rel[k].lo < off + len && off < g_fb[i].rel[k].hi) { g_fb[i].nrel = -1; break; }
      }
    for (int i = 0; i < g_mapc_n && !owned; i++)
      if (a >= (uint8_t *)g_mapc[i].ptr && a + len <= (uint8_t *)g_mapc[i].ptr + g_mapc[i].len) owned = 1;
    mutexUnlock(&g_fb_lock);
    if (owned) { if (prot != BIONIC_PROT_NONE) memset(addr, 0, len); return addr; } }
  debugPrintf("[mmap] MAP_FIXED %p len=%zu prot=0x%x is not memory this shim owns -> ENOMEM\n", addr, len, prot);
  return NULL;
}

void *mmap_fake_unguarded(void *addr, size_t length, int prot, int flags, int fd, long offset) {
  if (length == 0) length = 1;
  if ((flags & BIONIC_MAP_FIXED) && addr && (flags & BIONIC_MAP_ANONYMOUS)) {
    static int told;
    void *r = mmap_fixed_anon(addr, length, prot);
    if (r && !told) { told = 1; debugPrintf("[mmap] MAP_FIXED in-place remap honoured (%p, %zu KB, prot=0x%x) -- GC heap unmap/remap works\n", addr, length >> 10, prot); }
    if (r) return r;
    errno = ENOMEM; return (void *)-1;
  }

  // Big anonymous PROT_NONE reservations -> stack-region OC arena: cheap address
  // space now, physical aliased in on the later mprotect(RW). On a full window we
  // fall through to the heap-backed arena below (no behaviour change there).
  if (oc_pages && length >= MMAP_BIG_THRESH &&
      (flags & BIONIC_MAP_ANONYMOUS) && prot == BIONIC_PROT_NONE) {
    size_t ocres = 0;
    mutexLock(&g_mmap_lock);
    void *op = oc_alloc_locked(length, &ocres);
    mutexUnlock(&g_mmap_lock);
    if (op) {
      if (TRACE_MMAP) debugPrintf("[mmap] %u MB (prot=0x0 anon=1) -> %p  [OC reserve %u MB]\n",
                  (unsigned)(length >> 20), op, (unsigned)(ocres >> 20));
      return op;   // reserved-only; committed lazily via mprotect_fake
    }
    if (TRACE_MMAP) debugPrintf("[mmap] OC window full for %u MB -> heap-backed arena\n",
                (unsigned)(length >> 20));
  }

  size_t reserved = 0;
  mutexLock(&g_mmap_lock);
  mmap_arena_init_locked();
  void *p = mmap_arena_alloc_locked(length, &reserved);
  mutexUnlock(&g_mmap_lock);
  /* NOT behind TRACE_MMAP any more. These are >=64 MB maps -- a handful per
   * run -- and the arena has been sized by guesswork for want of exactly this
   * line. p==NULL here means the arena could not place it and the caller is
   * about to fall back to the newlib heap, which is the fragmentation the
   * arena exists to prevent: say so loudly, because undersizing
   * mmap_arena_mb shows up here first and nowhere else. */
  if (length >= MMAP_BIG_THRESH)
    debugPrintf("[mmap] %u MB request (prot=0x%x anon=%d) -> %s%p [arena reserved %u MB]%s\n",
                (unsigned)(length >> 20), prot, !!(flags & BIONIC_MAP_ANONYMOUS),
                p ? "" : "ARENA FULL, falling back to the heap ", p,
                (unsigned)(reserved >> 20),
                p ? "" : "  *** raise mmap_arena_mb ***");
  /* A file-backed map must be contiguous and fully readable. When the arena can
   * only give a tail-overflow reservation (reserved < length) we'd read just
   * `fill` bytes and leave the tail unfilled -- silently truncating the file in
   * RAM. For global-metadata.dat that nulls out System.Object (Class::Init NULL).
   * Hand any short-reserved file map to newlib, which backs the whole length. */
  if (p && !(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0 && reserved < length) {
    /* NO outer lock: mmap_arena_free() takes g_mmap_lock itself, and libnx's
     * Mutex is not recursive. The old code locked it here first, so this branch
     * -- a file map the arena could only half-fit -- blocked forever holding the
     * lock, and every later mmap/munmap/mprotect on any thread blocked behind
     * it. A whole-game freeze with no crash report, on a rare branch: exactly
     * what "the game hangs sometimes" looks like. tools/relock.py finds these. */
    mmap_arena_free(p, length);
    if (TRACE_MMAP) debugPrintf("[mmap] file fd=%d len=%zu: arena tail-overflow (reserved=%zu) -> newlib\n",
                fd, length, reserved);
    p = NULL;
  }
  if (!p) {
    // Arena exhausted (Unity's pools fill it). Route the request to newlib's free
    // heap regardless of size -- il2cpp's resource-extraction maps can exceed 64MB,
    // and rejecting them is what NULL-derefs the engine. Only a genuinely huge map
    // (> newlib free) will fail, and we log that distinctly.
    // Read-only file maps get deduped (backtrace .so symbolication leak, see above).
    int ro_file = fd >= 0 && !(flags & BIONIC_MAP_ANONYMOUS) && !(prot & BIONIC_PROT_WRITE);
    uint64_t mino = (ro_file && fd < FD_INO_MAX) ? g_fd_ino[fd] : 0;
    if (mino) { void *hit = mapcache_get(mino, offset, length); if (hit) return hit; }
    void *q = mmap_fallback(length, flags, fd, offset);
    if (q) { if (mino) mapcache_put(mino, offset, length, q); return q; }
    if (TRACE_MMAP) debugPrintf("[mmap] arena FULL and newlib fallback FAILED for %u MB (out of RAM)\n",
                (unsigned)(length >> 20));
    errno = ENOMEM; return (void *)-1;
  }

  // Never touch beyond what we actually reserved in-arena (tail over-maps reserve
  // less than the requested length; the spill lives past the arena and is trimmed).
  size_t fill = length < reserved ? length : reserved;

  if (g_overcommit) {
    // PROT_NONE reservation: address space only, no physical -- the whole point.
    // The engine commits the sub-ranges it uses later via mprotect(RW).
    if (prot == BIONIC_PROT_NONE) return p;
    // Otherwise commit now (anon RW, file maps): svcMapPhysicalMemory zero-fills,
    // so anon needs no memset; file maps read their contents over the zeros.
    arena_commit_range(p, fill);
    if (!(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0) {
      long got = 0, cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0)
        while ((size_t)got < fill) { long r = read(fd, (char *)p + got, fill - got); if (r <= 0) break; got += r; }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    return p;
  }

  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(p, 0, fill);   // anonymous memory must read back as zero
  } else {
    // File-backed mapping: pull [offset, offset+fill) into RAM (no real mmap).
    long got = 0;
    if (fd >= 0) {
      long cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0) {
        while ((size_t)got < fill) {
          long r = read(fd, (char *)p + got, fill - (size_t)got);
          if (r <= 0) break;
          got += r;
        }
      }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    if ((size_t)got < fill) memset((char *)p + got, 0, fill - (size_t)got);
    if (fd >= 0)
      if (TRACE_MMAP) debugPrintf("[mmap] file map fd=%d len=%zu reserved=%zu fill=%zu got=%ld%s\n",
                  fd, length, reserved, fill, got,
                  ((size_t)got < length) ? "  *** TRUNCATED ***" : "");
  }
  return p;
}
void * mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset) {
  if (!fdg_enter(fd)) return (void *)-1;
  void * r = mmap_fake_unguarded(addr, length, prot, flags, fd, offset);
  fdg_leave(fd);
  return r;
}

int munmap_fake(void *addr, size_t length) {
  g_munmap_n++;
  if (mmap_fallback_free(addr, length)) return 0;   // newlib fallback allocation
  if (oc_contains(addr)) {                   // stack-region OC reservation
    mutexLock(&g_mmap_lock);
    oc_free_locked(addr, length);
    mutexUnlock(&g_mmap_lock);
    return 0;
  }
  arena_decommit_range(addr, length);       // reclaim physical (overcommit only)
  mmap_arena_free(addr, length);            // unreserve address space
  return 0;
}

// In overcommit mode mprotect drives commit/decommit: RW/R commits physical at
// the alias address, PROT_NONE decommits it (safe -- reuse re-mprotects to RW).
// In heap-backed mode the arena is always RW so this is a no-op.
int mprotect_fake(void *addr, size_t len, int prot) {
  /* Diagnostic (fires even heap-backed): measure Unity's commit pattern so we can
   * confirm it commits PROT_NONE reservations via mprotect(RW) and size the
   * overcommit commit-pool. Tracks cumulative RW-commit vs PROT_NONE-decommit
   * bytes that fall inside the mmap arena (= Unity's live committed footprint). */
  {
    int in_arena = mmap_arena && (uint8_t *)addr >= mmap_arena &&
                   (uint8_t *)addr <  mmap_arena + mmap_usable;
    if (prot == BIONIC_PROT_NONE)       { none_n++; if (in_arena) none_b += len; }
    else if (prot & BIONIC_PROT_WRITE)  { rw_n++;   if (in_arena) rw_b   += len; }
    else                                  oth_n++;
    if (len >= 4u * 1024 * 1024 || ((rw_n + none_n) & 0x7F) == 0)
      if (TRACE_MMAP) debugPrintf("[mprot] addr=%p len=%zuKB prot=0x%x arena=%d | RW %u/%zuMB NONE %u/%zuMB oth %u  net=%zdMB\n",
                  addr, len >> 10, prot, in_arena, rw_n, rw_b >> 20, none_n, none_b >> 20, oth_n,
                  (ssize_t)(rw_b - none_b) >> 20);
  }
  if (oc_contains(addr)) {
    // OC reservation being committed/decommitted. Unity only ever commits (RW);
    // decommits aren't observed, so PROT_NONE here is left mapped (cheap + safe).
    if (prot != BIONIC_PROT_NONE) {
      mutexLock(&g_mmap_lock);
      oc_commit_locked(addr, len);
      mutexUnlock(&g_mmap_lock);
    }
    return 0;
  }
  if (!g_overcommit) return 0;
  if (prot == BIONIC_PROT_NONE) arena_decommit_range(addr, len);
  else                          arena_commit_range(addr, len);
  return 0;
}
// madvise(MADV_DONTNEED): overcommit zeroes-but-keeps (see arena_dontneed_range);
// heap-backed leaves pages as-is (always RW-backed).
int madvise_fake(void *addr, size_t len, int advice) {
  if (g_overcommit && advice == BIONIC_MADV_DONTNEED) arena_dontneed_range(addr, len);
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

char *realpath_fake(const char *path, char *resolved) {
  if (!path) return NULL;          /* POSIX: realpath(NULL,..) is an error, not a crash */
  if (!resolved) resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}
int strerror_r_fake(int err, char *buf, size_t len) { snprintf(buf, len, "%s", strerror(err)); return 0; }
int statvfs_fake(const char *path, void *buf) { (void)path; memset(buf, 0, 0x70); return 0; }
int statfs_fake(const char *path, void *buf) { (void)path; memset(buf, 0, 0x78); return 0; }

// Synthetic /proc and /sys files. Unity reads /proc/meminfo (MemTotal) to size
// its allocator reservations and /proc/cpuinfo + /sys cpu range to count cores
// for the job system. We report ~1 GB (NOT the real ~3 GB) so the engine's big
// 256MB-block dynamic-heap reservations stay within our mmap arena -- the arena
// is the real backing and has headroom, but Unity must not try to reserve 3 GB
// of address space up front. 3 cores (homebrew gets 0-2).
static const char *synthetic_proc(const char *path) {
  if (!path) return NULL;
  if (!strcmp(path, "/proc/meminfo"))
    return "MemTotal:        524288 kB\n"
           "MemFree:         393216 kB\n"
           "MemAvailable:    393216 kB\n"
           "Buffers:              0 kB\n"
           "Cached:               0 kB\n"
           "SwapTotal:            0 kB\n"
           "SwapFree:             0 kB\n";
  if (!strcmp(path, "/proc/cpuinfo"))
    return "processor\t: 0\nprocessor\t: 1\nprocessor\t: 2\n"
           "Features\t: fp asimd aes pmull sha1 sha2 crc32\n"
           "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\n"
           "CPU part\t: 0xd07\nCPU revision\t: 1\n";
  if (strstr(path, "cpu_capacity")) return "1024\n";
  if (strstr(path, "cpuinfo_max_freq") || strstr(path, "scaling_max_freq")) return "1785000\n";
  if (strstr(path, "cpuinfo_min_freq") || strstr(path, "scaling_min_freq")) return "1020000\n";
  if (strstr(path, "/cpu/possible") || strstr(path, "/cpu/present") || strstr(path, "/cpu/online"))
    return "0-2\n";
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5)) return ""; // empty for the rest
  return NULL;
}

// a buffered fopen for the big .mvgl archives: the engine issues many small
// reads/seeks and the fsdev round-trips dominate without a large buffer.
FILE *fopen_fake(const char *path, const char *mode) {
  char _rbuf[512];
  path = asset_redirect(path, _rbuf, sizeof _rbuf);
  const char *synth = synthetic_proc(path);
  if (synth) {
    size_t n = strlen(synth);
    return fmemopen((void *)strdup(synth), n ? n : 1, "r");
  }
  const int writing = strpbrk(mode, "wa+") != NULL;
  if (!writing) {
    void *data = NULL; size_t size = 0;
    if (asset_pack_read_all_path(path, &data, &size)) {
      FILE *pf = fmemopen(data, size ? size : 1, "r");
      if (pf) {
        if (TRACE_IO) debugPrintf("[io] fopen(%s,%s) -> %p [pack]\n", path, mode, (void *)pf);
        return pf;
      }
      free(data);
    }
  }
  FILE *f = fopen(path, mode);
  if (!f && writing) {            // save file: create the subdir and retry
    mkdir_parents(path);
    f = fopen(path, mode);
  }
  if (!f && !writing && strchr(mode, 'r')) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt)))
      f = fopen(alt, mode);
  }
  if (!f)
    return NULL;
  if (writing) mark_write_fd(fileno(f));
  if (TRACE_IO) debugPrintf("[io] fopen(%s,%s) -> %p\n", path, mode, (void *)f);
  if (strchr(mode, 'r')) {
    /* Large-sequential-read buffering.
     *
     * CHANGED FOR BOUNCEMASTERS: was keyed on ".mvgl", which is a Chaos Rings 3
     * archive extension. Bouncemasters has no .mvgl files at all, so this branch
     * never fired and every big asset read went unbuffered off the SD card.
     *
     * Bouncemasters's large sequential reads are:
     *     assets/bin/Data/data.unity3d                 67.8 MB
     *     assets/bin/Data/<name>.resource               streamed audio/texture
     *     assets/bin/Data/Managed/Metadata/global-metadata.dat   15.7 MB
     *
     * global-metadata.dat is read once at il2cpp init but it is read hard, and
     * data.unity3d is read throughout the session. */
    const char *ext = strrchr(path, '.');
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if ((ext && (strcasecmp(ext, ".unity3d")  == 0 ||
                 strcasecmp(ext, ".resource") == 0 ||
                 strcasecmp(ext, ".resS")     == 0)) ||
        strcasecmp(base, "global-metadata.dat") == 0)
      /* 1 MB, not 256 KB. data.unity3d is 67 MB and global-metadata.dat 15.7 MB,
       * read sequentially off an SD card during the load frames that measured
       * 1188 ms and 2987 ms on hardware. Larger buffers mean fewer fs IPC round
       * trips, which is the dominant cost on this path. */
      setvbuf(f, NULL, _IOFBF, 1024 * 1024);
  }
  return f;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr). libc++_shared wires
// std::cout/cerr/cin to &__sF[1]/[2]/[0]; these wrappers absorb writes to those
// fake FILEs and forward everything else to newlib.
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c (__sF / std{in,out,err})

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    const size_t total = size * n < sizeof(buf) - 1 ? size * n : sizeof(buf) - 1;
    memcpy(buf, ptr, total); buf[total] = '\0';
    debugPrintf("stdio: %s", buf);
#endif
    return n;
  }
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}
int fputc_fake(int c, FILE *f) { if (is_fake_file(f)) return c; return fputc(c, f); }
int fputs_fake(const char *s, FILE *f) { if (is_fake_file(f)) { debugPrintf("stdio: %s", s); return 0; } return fputs(s, f); }
int fflush_fake(FILE *f) { if (is_fake_file(f) || f == NULL) return 0; return fflush(f); }
int fclose_fake(FILE *f) { if (is_fake_file(f)) return 0;
  int fd = fileno(f); fd_ino_clear(fd);   /* keep g_fd_ino free of stale entries: fd_close_by_path
                                             (rename_fake) closes fds by that table */
  int r = fclose(f); commit_write_fd(fd); return r; }
int ferror_fake(FILE *f) { if (is_fake_file(f)) return 0; return ferror(f); }
int feof_fake(FILE *f) { if (is_fake_file(f)) return 1; return feof(f); }
int fileno_fake(FILE *f) { if (is_fake_file(f)) return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100; return fileno(f); }
int fseek_fake(FILE *f, long off, int whence) { if (is_fake_file(f)) return -1; return fseek(f, off, whence); }
long ftell_fake(FILE *f) { if (is_fake_file(f)) return -1; return ftell(f); }
int getc_fake(FILE *f) { if (is_fake_file(f)) return -1; return getc(f); }
int fgetc_fake(FILE *f) { if (is_fake_file(f)) return -1; return fgetc(f); }
char *fgets_fake(char *s, int n, FILE *f) { if (is_fake_file(f)) return NULL; return fgets(s, n, f); }
int ungetc_fake(int c, FILE *f) { if (is_fake_file(f)) return -1; return ungetc(c, f); }
void setbuf_fake(FILE *f, char *buf) { if (is_fake_file(f)) return; setbuf(f, buf); }

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va; va_start(va, fmt);
  int ret;
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
#else
    ret = 0;
#endif
  } else {
    ret = vfprintf(f, fmt, va);
  }
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
#else
    return 0;
#endif
  }
  return vfprintf(f, fmt, va);
}

// ---------------------------------------------------------------------------
// fd routing: the native_app_glue command pipe lives in the fake-fd layer
// (android_native.c). Real files (small fds from open()) pass through to newlib.
// ---------------------------------------------------------------------------

long read_fake_unguarded(int fd, void *buf, size_t count) {
  /* Any read the game makes defers the prefetch. Marking only cached reads
   * would miss the uncached majority -- which is most of what it does while
   * loading, and exactly when the card must be left alone. */
  g_last_game_read = armTicksToNs(armGetSystemTick());
  if (asset_pack_fd_is(fd)) return asset_pack_read_fd(fd, buf, count);
  if (fakefd_is_fake(fd)) return fakefd_read(fd, buf, count);
  { struct RaCache *c = ra_find(fd);
    if (c) {
      long real_before = -1;            /* ra_read_at reports it; no second lseek */
#if DEBUG_LOG
      /* Timing experiment -- see config.h ram_delay_ms. Resident only, first
       * read after open only, so the cost is one sleep per bundle open. */
      if (bp_ram_delay_ms > 0 && c->resident) {
        struct TrEntry *t0 = tr_by_fd(fd);
        if (t0 && t0->reads_this_open == 0) svcSleepThread((u64)bp_ram_delay_ms * 1000000ull);
      }
#endif
      const long got = ra_read_at(c, fd, buf, count, &real_before);
#if DEBUG_LOG
      /* PER-READ BOOKKEEPING, DEBUG BUILDS ONLY. tr_by_fd() is a linear scan of
       * up to 96 entries, and every counter below feeds a line debugPrintf
       * cannot emit at DEBUG_LOG 0 -- so in a release build this was up to 96
       * comparisons plus the counter work on every read, producing numbers
       * nothing would ever print. */
      struct TrEntry *te = tr_by_fd(fd);
      if (te) {
        te->reads++; te->reads_this_open++;
        tr_op(te, c->resident ? 'r' : 'R', real_before, (int64_t)count, got, 0, 1);
        /* WHAT DID UNITY ACTUALLY GET. On the second open of an evicted bundle
         * Unity reads 64 bytes at offset 0, then seeks to 1 -- which is what its
         * string reader does when byte 0 is NUL -- and logs "Unable to read
         * header from archive file". The verifier said those 64 bytes matched
         * the card, and the card starts with "UnityFS". Both cannot be true of
         * the same buffer, so this prints the buffer itself, the return value
         * Unity was handed, and errno, on the first read of every open. */
        /* Only the interesting cases, so this stays rare: a header read on a
         * re-open (only evicted entries get re-opened at 0), or any first read
         * at 0 that is not UnityFS. */
        if (te->reads_this_open == 1 && real_before == 0 &&
            (te->opens >= 2 || got < 8 || memcmp(buf, "UnityFS", 7) != 0)) {
          const unsigned char *b = buf;
          char hex[64]; size_t hn = 0;
          for (size_t k = 0; k < 16 && k < (size_t)(got > 0 ? got : 0) && hn < sizeof hex - 3; k++)
            hn += (size_t)snprintf(hex + hn, sizeof hex - hn, "%02x", b[k]);
          tr_log("[evict] FIRST READ %s open#%u fd=%d at %ld: want=%zu got=%ld errno=%d "
                 "buf[0..16]=%s  %s\n", te->key, te->opens, fd, real_before, count, got, errno,
                 hn ? hex : "(none)",
                 (got >= 8 && !memcmp(buf, "UnityFS", 7)) ? "= UnityFS" : "= NOT UnityFS");
        }
        if (got >= 0 && (size_t)got < count) {
          te->short_reads++;
          /* THE ONE ANOMALY THE COUNTERS SHOW. Every evicted entry has exactly
           * one short read, and the eviction follows it. This line says which
           * read it was. A short read at pos >= size is a benign EOF probe and
           * Unity is reacting to something else; a short read with room left
           * in the file means this layer returned fewer bytes than the card
           * would have, and that IS the eviction. Rare by definition, so it
           * flushes on sight under the eviction prefix. */
          tr_log("[evict] SHORT READ %s fd=%d want=%zu got=%ld at pos=%ld "
                 "(size=%ld, room=%ld) %s%s\n",
                 te->key, fd, count, got, real_before,
                 (long)c->size, (long)c->size - real_before,
                 c->resident ? "RAM" : "window",
                 /* got == room is a correct EOF-bounded read: the card would
                  * answer the same. Only got < room is this layer's fault. */
                 got >= (long)c->size - real_before ? " -- EOF-bounded, correct" : " -- TRUNCATED");
        }
        if (got > 0) { if (c->resident) te->bytes_ram += (uint64_t)got;
                       else te->bytes_card += (uint64_t)got; }
        te->last_off = c->pos;
        /* Only the first couple of reads per entry get a line. The counters
         * below record every one, and the flood was the problem: 400 lines
         * drained 24 per frame is 400 blocking writes to a card the game is
         * reading from, which stalled the log lock past the watchdog's limit
         * twice. Shape is what these lines are for, and two reads show it. */
        if (te->reads <= 2) {
          tr_log("[trace] read  %s fd=%d want=%zu got=%ld -> pos %ld %s\n",
                 te->key, fd, count, got, c->pos, c->resident ? "(RAM)" : "(card)");
        }
      }
#else
      (void)real_before;
#endif
      return got;
    } }
  /* fsdev can return fewer bytes than requested for a large read; il2cpp's
   * global-metadata.dat loader (and others) assume a single read() fills the
   * buffer. Loop until `count` is satisfied or we hit EOF/error so the metadata
   * is never silently truncated (a short read leaves System.Object et al.
   * unresolvable -> Class::Init(NULL)). */
  size_t total = 0;
  while (total < count) {
    long r = read(fd, (char *)buf + total, count - total);
    if (r < 0) { if (total) break; return -1; }
    if (r == 0) break; /* EOF */
    total += (size_t)r;
  }
  if (count >= (1u << 20))
    if (TRACE_IO) debugPrintf("[io] read(fd=%d, %zu) -> %zu%s\n", fd, count, total,
                total < count ? "  *** SHORT READ ***" : "");
  { struct TrEntry *te = tr_by_fd(fd);
    if (te) { te->reads++;
              if (total < count) te->short_reads++;
              te->bytes_card += total;
              /* position BEFORE this read = after it, minus what it returned */
              const long after = lseek(fd, 0, SEEK_CUR);
              tr_op(te, 'C', after >= 0 ? after - (long)total : -1, (int64_t)count, (int64_t)total, 0, 2); } }
  watch_dump("read", fd, (long)count, 0, buf, (long)total);
  return (long)total;
}
long read_fake(int fd, void *buf, size_t count) {
  if (!fdg_enter(fd)) return -1;
  long r = read_fake_unguarded(fd, buf, count);
  fdg_leave(fd);
  return r;
}
long write_fake_unguarded(int fd, const void *buf, size_t count) {
  if (fakefd_is_fake(fd)) return fakefd_write(fd, buf, count);
  dlw_wrote(fd, (long)count);
  /* stdout/stderr go nowhere on Switch. Boehm writes its ABORT text to stderr
   * just before calling abort(), and the fifth run's GC abort left no message
   * at all -- it had to be recovered from disassembly. Mirror them into
   * debug.log (bounded), and still report the bytes as written. */
  if ((fd == 1 || fd == 2) && buf && count) {
    const char *s = (const char *)buf;
    int n = count > 600 ? 600 : (int)count;
    debugPrintf("[%s] %.*s%s", fd == 2 ? "stderr" : "stdout", n, s, s[n - 1] == '\n' ? "" : "\n");
    return (long)count;
  }
  return write(fd, buf, count);
}
long write_fake(int fd, const void *buf, size_t count) {
  if (!fdg_enter(fd)) return -1;
  long r = write_fake_unguarded(fd, buf, count);
  fdg_leave(fd);
  return r;
}
static int close_fake_unguarded(int fd) {
  if (asset_pack_fd_is(fd)) return asset_pack_close_fd(fd);
  { struct TrEntry *te = tr_by_fd(fd);
    if (te) {
      te->closes++;
      if (g_tr_lines < 400) { g_tr_lines++;
        tr_log("[trace] close %s fd=%d after %u read(s), %llu KB\n",
                    te->key, fd, te->reads,
                    (te->bytes_ram + te->bytes_card) >> 10); }
      tr_op(te, 'c', 0, 0, 0, 0, 0);
      tr_ops_dump(te, fd);
      /* Full checksum at close of a resident entry. Heavy: shared_stuff is
       * 118 MB, so this is ~100 ms on the closing thread. diag_io only. */
      if (bp_diag_io) { struct RaCache *cc = ra_find(fd);
        if (cc && cc->resident && cc->buf) {
          int bi = -1;
          mutexLock(&g_ram_lock);
          for (int i = 0; i < g_blob_count; i++)
            if (g_blob[i].data == cc->buf) { bi = i; break; }
          mutexUnlock(&g_ram_lock);
          if (bi >= 0) blob_report(bi, "close", 0); } }
      tr_unbind(fd);
    } }
  dlw_close(fd);
  { extern void bpn_untrack(int); bpn_untrack(fd); }
  ra_detach(fd);
  fd_ino_clear(fd);
  if (fakefd_is_fake(fd)) return fakefd_close(fd);
  int r = close(fd);
  commit_write_fd(fd);   /* flush a just-written save to the physical SD */
  return r;
}
int close_fake(int fd) {
  fdg_close_begin(fd);          /* FIRST, holding nothing -- see the descriptor guard */
  int r = close_fake_unguarded(fd);
  fdg_close_end(fd);
  return r;
}
int pipe_fake(int fds[2]) { return fakefd_pipe(fds); }
int poll_fake(void *fds, unsigned long nfds, int timeout) { (void)fds; (void)nfds; (void)timeout; return 0; }
int select_fake(int n, void *r, void *w, void *e, void *t) { (void)n; (void)r; (void)w; (void)e; (void)t; return 0; }

// ---------------------------------------------------------------------------
// networking: online play (Mobage / Silicon Studio servers) is dead. Stub the
// socket layer so connections fail and the engine stays in offline mode.
// ---------------------------------------------------------------------------

int socket_fake(int d, int t, int p) { (void)d; (void)t; (void)p; errno = EAFNOSUPPORT; return -1; }
int connect_fake(int s, const void *a, unsigned l) { (void)s; (void)a; (void)l; errno = ECONNREFUSED; return -1; }
int bind_fake(int s, const void *a, unsigned l) { (void)s; (void)a; (void)l; errno = EACCES; return -1; }
int listen_fake(int s, int b) { (void)s; (void)b; return -1; }
int accept_fake(int s, void *a, void *l) { (void)s; (void)a; (void)l; errno = EINVAL; return -1; }
long send_fake(int s, const void *b, size_t l, int f) { (void)s; (void)b; (void)l; (void)f; errno = EPIPE; return -1; }
long recv_fake(int s, void *b, size_t l, int f) { (void)s; (void)b; (void)l; (void)f; return 0; }
long sendto_fake(int s, const void *b, size_t l, int f, const void *a, unsigned al) { (void)s; (void)b; (void)l; (void)f; (void)a; (void)al; errno = EPIPE; return -1; }
long recvfrom_fake(int s, void *b, size_t l, int f, void *a, void *al) { (void)s; (void)b; (void)l; (void)f; (void)a; (void)al; return 0; }
int shutdown_fake(int s, int how) { (void)s; (void)how; return 0; }
int setsockopt_fake(int s, int lv, int n, const void *v, unsigned l) { (void)s; (void)lv; (void)n; (void)v; (void)l; return 0; }
/* socketpair: the game only needs it for optional self-pipe/wakeup plumbing; sockets are
 * stubbed offline like the rest, so fail cleanly (callers treat -1 as "no pipe" and degrade). */
int socketpair_fake(int d, int t, int p, int sv[2]) { (void)d; (void)t; (void)p; (void)sv; errno = EAFNOSUPPORT; return -1; }

/* `environ`: libil2cpp imports the POSIX environment pointer (getenv-style reads). Give it an
 * empty, NULL-terminated vector -- reads see no variables, nothing faults. The resolver binds
 * the game's `environ` data symbol to &fake_environ (address of this char** variable). */
char  *g_env_empty[1] = { NULL };
char **fake_environ   = g_env_empty;
int getsockopt_fake(int s, int lv, int n, void *v, void *l) { (void)s; (void)lv; (void)n; (void)v; (void)l; return -1; }
int getsockname_fake(int s, void *a, void *l) { (void)s; (void)a; (void)l; return -1; }
int getpeername_fake(int s, void *a, void *l) { (void)s; (void)a; (void)l; return -1; }
int getaddrinfo_fake(const char *node, const char *svc, const void *hints, void **res) { (void)node; (void)svc; (void)hints; if (res) *res = NULL; return -2 /* EAI_NONAME */; }
void freeaddrinfo_fake(void *res) { (void)res; }
int getnameinfo_fake(const void *a, unsigned al, char *h, unsigned hl, char *s, unsigned sl, int f) { (void)a; (void)al; (void)f; if (h && hl) h[0] = 0; if (s && sl) s[0] = 0; return -1; }
int gethostname_fake(char *name, size_t len) { if (name && len) snprintf(name, len, "switch"); return 0; }
void *getservbyname_fake(const char *n, const char *p) { (void)n; (void)p; return NULL; }
unsigned if_nametoindex_fake(const char *n) { (void)n; return 0; }
char *if_indextoname_fake(unsigned i, char *buf) { (void)i; if (buf) buf[0] = 0; return buf; }
static volatile int g_h_errno = 0;
int *__get_h_errno_fake(void) { return (int *)&g_h_errno; }

// ---------------------------------------------------------------------------
// process control: fork/exec/etc. are unavailable; report failure.
// ---------------------------------------------------------------------------

int fork_fake(void) { errno = ENOSYS; return -1; }
int execvp_fake(const char *f, char *const argv[]) { (void)f; (void)argv; errno = ENOSYS; return -1; }
int waitpid_fake(int pid, int *status, int opts) { (void)pid; (void)opts; if (status) *status = 0; errno = ECHILD; return -1; }
int kill_fake(int pid, int sig) { (void)pid; (void)sig; return 0; }
int getpid_fake(void) { return 1; }
int sched_yield_fake(void) { svcSleepThread(0); return 0; }
// bionic struct passwd layout (pw_dir at +0x20, as the engine derefs).
struct bionic_passwd {
  char *pw_name;     /* 0x00 */
  char *pw_passwd;   /* 0x08 */
  uint32_t pw_uid;   /* 0x10 */
  uint32_t pw_gid;   /* 0x14 */
  char *pw_gecos;    /* 0x18 */
  char *pw_dir;      /* 0x20 */
  char *pw_shell;    /* 0x28 */
};
void *getpwuid_fake(int uid) {
  (void)uid;
  static struct bionic_passwd pw;
  static char nm[] = "switch", sh[] = "/bin/sh", empty[] = "";
  /* pw_dir was a compile-time array of GAME_HOME; the root is resolved at
   * runtime now, so fill it on first use. */
  static char dir[512];
  if (!dir[0]) snprintf(dir, sizeof dir, "%s", bp_game_root());
  pw.pw_name = nm; pw.pw_passwd = empty; pw.pw_uid = 0; pw.pw_gid = 0;
  pw.pw_gecos = empty; pw.pw_dir = dir; pw.pw_shell = sh;
  return &pw;
}

// Unity computes its home/cache dir via getenv("HOME") (then getpwuid fallback).
// Serve the writable game root for HOME/TMPDIR; delegate everything else to newlib.
const char *managed_path(const char *p) {
  if (!p) return p;
  const char *c = strchr(p, ':');
  return (c && c[1] == '/') ? c + 1 : p;     // "sdmc:/switch/.." -> "/switch/.."
}
char *getenv_fake(const char *name) {
  if (name) {
    /* Unity derives its home/cache dir from these. Must track the runtime root,
     * or it writes its cache next to a folder that does not exist. */
    if (!strcmp(name, "HOME") || !strcmp(name, "TMPDIR"))
      return (char *)managed_path(bp_game_root());
  }
  return getenv(name);
}
// Report a Unix-rooted cwd ("/switch/zookeeper", no "sdmc:") so managed Path
// APIs don't treat it as relative in Path.Combine. newlib's *internal* cwd is
// unchanged, so relative file resolution still works via the default device.
char *getcwd_fake(char *buf, size_t size) {
  char *r = getcwd(buf, size);
  if (!r) return r;
  const char *c = strchr(r, ':');
  if (c && c[1] == '/') memmove(r, c + 1, strlen(c + 1) + 1);  // drop "sdmc:"
  return r;
}
int getrusage_fake(int who, void *usage) { (void)who; if (usage) memset(usage, 0, 144); return 0; }

// ---------------------------------------------------------------------------
// dlopen/dlsym over the already-loaded modules (no real dynamic loading).
// dlsym lets the engine look up its own exports / our shims.
// ---------------------------------------------------------------------------

void *dlopen_fake(const char *name, int flags) {
  (void)flags; debugPrintf("dlopen(%s)\n", name ? name : "(self)");

  /* BOUNCEMASTERS / Unity 6 GATING.
   *
   * libunity reaches these three through dlopen rather than DT_NEEDED, so
   * refusing them here is how the port selects the code paths it can actually
   * service. This is not an optimisation -- without it the engine takes paths
   * that have no backing on Switch and fails in ways that look like hangs.
   *
   *   libswappywrapper.so  Unity logs "InitSwappyWrapper() failed" and uses its
   *                        non-Swappy present path. Also avoids loading a fourth
   *                        module and binding 60 libc++ symbols, since the port
   *                        drives its own frame loop anyway.
   *   libvulkan.so         forces the GLES backend, which switch-mesa provides.
   *                        Safe because libunity.so lists libEGL.so as a hard
   *                        DT_NEEDED while Vulkan is only ever dlopen'd -- the
   *                        GLES path is linked in and cannot be absent. (The
   *                        game's own shader variants live inside a compressed
   *                        UnityFS bundle and were not counted; cloverpit_nx's
   *                        "91 GLSL sources" figure is its measurement, not one
   *                        taken from this build.)
   *   libaaudio.so         forces the FMOD/OpenSL output that opensles.c backs,
   *                        rather than an AAudio device that does not exist.
   *
   * Anything else keeps the old behaviour of succeeding with a token handle;
   * dlsym_fake ignores the handle and resolves against loaded modules + shims. */
  if (name) {
    static const char *const refuse[] = {
      "libswappywrapper.so", "libvulkan.so", "libaaudio.so",
    };
    for (size_t i = 0; i < sizeof(refuse) / sizeof(*refuse); i++) {
      if (strstr(name, refuse[i])) {
        debugPrintf("dlopen(%s) -> NULL [battd: forced fallback path]\n", name);
        return NULL;
      }
    }
  }

  return (void *)0x1;
}
int dlclose_fake(void *h) { (void)h; return 0; }
const char *dlerror_fake(void) { return NULL; }
void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol) return NULL;
  /* Firebase SWIG stub resolver (firebase_stub.c) -- see step 2b below. */
  extern void *firebase_stub_lookup(const char *symbol);
  /* 1) a real export from a loaded module (il2cpp/unity/main) */
  void *p = so_resolve_external(symbol);
  if (p) return p;
  /* 2) one of our libc/GLES/EGL shims (the engine dlopen()s libGLESv2.so etc.
   *    and dlsym()s glGetString/glGetIntegerv, which are shims, not exports) */
  uintptr_t shim = dynlib_find_export(symbol);
  if (shim) { debugPrintf("dlsym(%s) -> %p [shim]\n", symbol, (void *)shim); return (void *)shim; }
  /* 2b) EOS / AppLovin / Crashlytics P/Invokes (bp_sdk_stubs.c). MUST come
   *     before the Firebase resolver below: that one matches on the generic
   *     SWIG marker "_CSharp_", which would otherwise swallow Crashlytics'
   *     SWIG symbols and hand them a live handle. Crashlytics must get hard
   *     zeros instead -- given a real handle its managed layer registers native
   *     signal handlers, which then fight nx_crash_handler.c for the same
   *     signals and turn every crash into an unreadable one. */
  extern void *bp_sdk_stub_lookup(const char *symbol);
  void *sdk = bp_sdk_stub_lookup(symbol);
  if (sdk) return sdk;
  /* 2c) Firebase SWIG P/Invokes. The real Firebase .so files are intentionally
   *     NOT loaded (they crash our loader at boot and, lacking Play Services,
   *     could never report DependencyStatus.Available on a Switch anyway). We
   *     answer the managed SDK's native lookups with trivial stubs so the
   *     dependency check resolves to Available(0) and the bootstrap advances. */
  void *fb = firebase_stub_lookup(symbol);
  if (fb) return fb;
  /* 3) the full GLES/EGL API (~150 entry points) lives in mesa, beyond our
   *    static table -- resolve any gl or egl symbol via eglGetProcAddress. */
  if (!strncmp(symbol, "gl", 2) || !strncmp(symbol, "egl", 3)) {
    p = (void *)eglGetProcAddress(symbol);
    if (p) { debugPrintf("dlsym(%s) -> %p [egl]\n", symbol, p); return p; }
  }
  debugPrintf("dlsym(%s) -> NULL\n", symbol);
  return NULL;
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks, semaphores, timed locks
// ---------------------------------------------------------------------------

typedef struct { RwLock lock; } FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  /* Unchecked on purpose. Every caller dereferences the result at once
   * (&get_rwlock(rw)->lock) and a rdlock/wrlock has no way to report failure
   * the game would act on, so there is no honest value to return. A ~16 byte
   * calloc failing means the heap is already exhausted and the process is
   * going down regardless. See HANDOFF: "unchecked, by decision". */
  if (!*storage) { FakeRwLock *l = calloc(1, sizeof(*l)); rwlockInit(&l->lock); *storage = l; }
  return *storage;
}
int pthread_rwlock_rdlock_fake(void **rw) { RwLock *l=&get_rwlock(rw)->lock; diag_wait_enter(DIAG_W_RWLOCK,l); rwlockReadLock(l); diag_wait_exit(); return 0; }
int pthread_rwlock_wrlock_fake(void **rw) { RwLock *l=&get_rwlock(rw)->lock; diag_wait_enter(DIAG_W_RWLOCK,l); rwlockWriteLock(l); diag_wait_exit(); return 0; }
int pthread_rwlock_destroy_fake(void **rw) {   /* libil2cpp imports this */
  if (rw && *rw) { free(*rw); *rw = NULL; }
  return 0;
}
int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock)) rwlockWriteUnlock(&l->lock);
  else rwlockReadUnlock(&l->lock);
  return 0;
}

typedef struct { Semaphore sem; } FakeSem;
int sem_init_fake(void **s, int pshared, unsigned int value) { (void)pshared; FakeSem *fs = calloc(1, sizeof(*fs)); if (!fs) { errno = ENOMEM; return -1; } semaphoreInit(&fs->sem, value); *s = fs; return 0; }
int sem_destroy_fake(void **s) { if (s && *s) { free(*s); *s = NULL; } return 0; }
int sem_post_fake(void **s) { if (s && *s) semaphoreSignal(&((FakeSem *)*s)->sem); return 0; }
int sem_wait_fake(void **s) { if (s && *s) { Semaphore *sm=&((FakeSem *)*s)->sem; diag_wait_enter(DIAG_W_SEM,sm); semaphoreWait(sm); diag_wait_exit(); } return 0; }
int sem_trywait_fake(void **s) { if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem)) return 0; errno = EAGAIN; return -1; }
int sem_getvalue_fake(void **s, int *val) { if (s && *s) *val = (int)((FakeSem *)*s)->sem.count; else *val = 0; return 0; }
// no native timed wait on libnx Semaphore; poll with a short backoff to the
// deadline. The engine uses it as a yield-with-timeout in its task scheduler.
int sem_timedwait_fake(void **s, const struct timespec *abs) {
  (void)abs;
  for (int i = 0; i < 1000; i++) {
    if (sem_trywait_fake(s) == 0) return 0;
    svcSleepThread(1000000ull); // 1 ms
  }
  errno = ETIMEDOUT;
  return -1;
}

/* --- Boehm GC stop-the-world bridge -------------------------------------
 * il2cpp's Boehm GC stops the world by sending every other thread a suspend
 * signal via pthread_kill; each target's signal handler sem_posts an ack and
 * parks in sigsuspend, and GC_stop_world / GC_start_world sem_wait on those
 * acks. POSIX signals are never delivered on Switch (pthread_kill is a no-op),
 * so the acks never arrive and the first collection hangs forever inside
 * GC_stop_world -- the verified boot wall.
 *
 * sem_post/sem_wait themselves work here (real libnx Semaphore underneath), so
 * we make pthread_kill itself post the ack that the never-delivered handler
 * would have posted. Every thread the GC suspends is already parked in our own
 * shim (idle worker / background waits), so not literally suspending them is
 * fine for the brief mark window. The signal numbers, the start-world ack gate
 * and the ack semaphore are all il2cpp globals (offsets below, next to the
 * #defines). Before GC_stop_init runs they hold bdwgc's sentinels -- the two
 * signal numbers are -1 (SIGNAL_UNSET), which no live signal can equal, and the
 * ack-sem storage is NULL, on which sem_post_fake no-ops -- so the bridge is
 * inert until the GC is actually up. */
uintptr_t g_il2cpp_base = 0;
size_t    g_il2cpp_size = 0;   /* mapped size of libil2cpp; 0 = unknown (guard inert) */

/* Offsets recovered by disassembling THIS build's libil2cpp.so (Bouncemasters,
 * Unity 6000.0.58f2, arm64-v8a, 79 MB on disk / 77.7 MB mapped).
 *
 * *** BUILD-SPECIFIC. NEVER COPY THESE BETWEEN GAMES. ***
 *
 * The values here previously belonged to cloverpit_nx (0x6028fb8/fbc/fc0,
 * 0x6251e18) and were carried in by a wholesale copy of this file. Those
 * addresses lie past the end of Bouncemasters' 0x4db1e40 mapping, so the bounds
 * guard below would have caught them -- the bridge disables itself and the GC
 * stalls rather than faulting. That is the fail-safe working, not a licence to
 * skip the re-derivation.
 *
 * DERIVATION. libil2cpp is stripped, so the GC is found by its libc call sites.
 * Across the whole 77.7 MB module there are exactly TWO pthread_kill call sites,
 * and they sit in the same ~1.1 KB of .text as the only sem_wait, sem_init,
 * sem_timedwait and sigsuspend sites. That block is bdwgc's
 * pthread_stop_world.c. Each function is then named outright by the string its
 * abort path passes:
 *
 *   0x1f0a964  GC_suspend_handler_inner  "...Duplicate suspend signal..."
 *              add x0,x0,#0xd90 ; bl sem_post   @0x1f0a994
 *                                            => GC_suspend_ack_sem 0x4daed90
 *              ldr w8,[x8,#0xd60] ; gated 2nd sem_post @0x1f0a9e0
 *                                            => restart-ack gate   0x4b87d60
 *   0x1f0aa24  GC_restart_handler        "Bad signal in restart handler"
 *              ldr w8,[x8,#0xd68] ; cmp w8,w0 => GC_sig_thr_restart 0x4b87d68
 *   0x1f0ab28  GC_suspend_all            "pthread_kill failed at suspend"
 *              adrp x24,#0x4b87000 @0x1f0aa64
 *              ldr w1,[x24,#0xd64] ; bl pthread_kill @0x1f0aaa4
 *                                            => GC_sig_suspend     0x4b87d64
 *   0x1f0abf8  suspend_restart_barrier   "sem_wait failed"
 *              add x20,x20,#0xd90 ; bl sem_wait @0x1f0ac2c
 *   0x1f0ad70  GC_restart_all            "pthread_kill failed at resume"
 *              ldr w8,[x23,#0xd60]  (gate)                          0x4b87d60
 *              ldr w1,[x24,#0xd68] ; bl pthread_kill @0x1f0ad04     0x4b87d68
 *   0x1f0ad98  GC_stop_init
 *              ldr w8,[x19,#0xd64] ; cmn w8,#1 ; mov w8,#0x1e ; str  (SIGPWR 30)
 *              ldr w9,[x20,#0xd68] ; cmn w9,#1 ; mov w9,#0x18 ; str  (SIGXCPU 24)
 *              add x0,x0,#0xd90 ; bl sem_init @0x1f0ade4
 *
 * Five independent cross-checks, all of which hold for this binary:
 *  1. The three ints are contiguous -- gate 0xd60, suspend 0xd64, restart 0xd68
 *     -- the same (gate, suspend, restart) layout seen in the Bad Piggies, PvZ
 *     Fusion, Layton and CloverPit builds at their own addresses.
 *  2. The ack semaphore is reached from FIVE sites: sem_post twice in the
 *     handler (suspend ack and gated restart ack), sem_wait in the barrier,
 *     sem_timedwait in the retry path, and sem_init in GC_stop_init.
 *  3. GC_stop_init's `cmn wN,#1` is bdwgc's `== SIGNAL_UNSET (-1)` test, so
 *     these really are the two settable signal-number globals.
 *  4. The values stored are 0x1e (30, SIGPWR) and 0x18 (24, SIGXCPU) -- bdwgc's
 *     Linux defaults. At runtime the bridge should read suspend_sig=30
 *     restart_sig=24; anything else means these offsets are wrong.
 *  5. Placement is right: the three ints land in file-backed .data and the
 *     semaphore in .bss, both inside the last RW PT_LOAD
 *     (0x48ba2d0..0x4db1e40), exactly as globals must.
 *
 * Segment translation matters here. libil2cpp is NOT identity-mapped:
 * p_vaddr = p_offset + 0x4000 for .text and + 0xc000 for the last RW segment.
 * A scan that walks file offsets while comparing virtual addresses is off by
 * one segment delta and matches nothing. */
#include "bp_offsets.h"   /* GC_*_OFF + guard table, derived for THIS libil2cpp */

/* Largest offset this bridge dereferences; used for the bounds guard below.
 * 0x4daed98 against a 0x4db1e40 mapping -- inside, with ~28 KB to spare. */
#define GC_MAX_OFF         (GC_ACK_SEM_OFF + sizeof(void *))

/* GUARD WORDS. Same fail-safe pattern as every code patch in this port: assert a
 * known instruction word at each site the four offsets were read from, so that a
 * stale offset against a different libil2cpp disables the bridge instead of
 * dereferencing an address that no longer means anything. Checked once, before
 * anything is dereferenced. The encodings pin both the base register and the
 * 12-bit displacement, so together with the adrp they fully determine the
 * address -- if all six match, the offsets above cannot be wrong.
 *
 * .text takes no relocations and this port patches nothing in libil2cpp's
 * .text, so these words are identical in the file and in memory. */
static const GcGuard gc_guards[] = BP_GC_GUARDS_INIT;

/* Suspend tally for the first collection, reported once the world restarts --
 * counting here rather than logging inline keeps the pause window free of
 * anything that takes a lock. (daggerfall_nx) */
static int g_gc_paused_ok, g_gc_paused_try;
static volatile int g_gc_paused_live;   /* how many WE currently hold paused */
/* The crash handler resumes everything via diag_resume_all_gc_paused(); this
 * keeps the counter honest so a later collection is not confused by it. */
void gc_paused_live_reset(void) { g_gc_paused_live = 0; }

/* ---- doing the suspend handler's job ------------------------------------------
 * On Linux, Boehm's suspend handler runs ON the stopped thread and records
 * stop_info.stack_ptr (where GC_push_all_stacks begins scanning that thread) and
 * pushes its registers onto its own stack. No signal ever runs here, so without
 * this the collector scans every stopped thread from a stale stack pointer and
 * never sees its registers: an object held only in a register or a fresh stack
 * slot is collected while in use. That is the crash in the first hardware log --
 * a live Kongregate delegate freed and its memory reused, so Call<bool> read a
 * MethodDefinition ("Invoke") where its generic context belonged.
 *
 * The thread is really paused (diag.c) and its context read; its registers are
 * written just below its SP (scratch space on a stopped thread, exactly where a
 * signal frame would go) and stack_ptr is pointed at them, so the scan covers the
 * registers and the whole live stack. Layout from bp_offsets.h, guard-checked
 * before first use; on a mismatch publishing is disabled, loudly. */
static int g_gc_layout_ok = -1;                 /* -1 unchecked, 0 bad, 1 good */
static int g_gc_published, g_gc_unpublished;
#define GC_REGSAVE_BYTES 0x120                   /* >= 33 words, 16-aligned */
static int gc_layout_check(uintptr_t b) {
  if (g_gc_layout_ok >= 0) return g_gc_layout_ok;
  static const GcGuard tg[] = BP_GC_THREAD_GUARDS_INIT;
  g_gc_layout_ok = 1;
  for (unsigned i = 0; i < sizeof tg / sizeof tg[0]; i++) {
    uint32_t got = *(volatile uint32_t *)(b + tg[i].off);
    if (got != tg[i].word) {
      g_gc_layout_ok = 0;
      debugPrintf("[gc] STACK PUBLISHING DISABLED: thread-table guard %u failed at il2cpp+0x%x"
                  " (%s): %08x != %08x. Collections will miss stopped threads' roots --\n"
                  "     re-derive with tools/offsets/derive_gc_threads.py.\n",
                  i + 1u, (unsigned)tg[i].off, tg[i].what, (unsigned)got, (unsigned)tg[i].word);
      break;
    }
  }
  if (g_gc_layout_ok) debugPrintf("[gc] thread-table layout guards OK (%u words): stopped threads' stacks"
                                  " and registers will be published to the collector\n",
                                  (unsigned)(sizeof tg / sizeof tg[0]));
  return g_gc_layout_ok;
}
static int gc_publish_stopped(uintptr_t b, pthread_t t, const ThreadContext *ctx) {
  const uint64_t want = (uint64_t)(uintptr_t)t & (sizeof(pthread_t) >= 8 ? ~0ull : 0xffffffffull);
  uintptr_t *tab = (uintptr_t *)(b + GC_THREADS_OFF);
  for (int i = 0; i < GC_THREADS_BUCKETS; i++) {
    for (uintptr_t p = tab[i]; p; p = *(volatile uintptr_t *)(p + GC_THR_NEXT_OFF)) {
      uint64_t id = *(volatile uint64_t *)(p + GC_THR_ID_OFF);
      if ((id & (sizeof(pthread_t) >= 8 ? ~0ull : 0xffffffffull)) != want) continue;
      const uint64_t sp = ctx->sp;
      uint64_t *save = (uint64_t *)(uintptr_t)((sp - GC_REGSAVE_BYTES) & ~(uint64_t)0xF);
      for (int k = 0; k < 29; k++) save[k] = ctx->cpu_gprs[k].x;
      save[29] = ctx->fp; save[30] = ctx->lr; save[31] = sp; save[32] = ctx->pc.x;
      *(volatile uintptr_t *)(p + GC_THR_STACKPTR_OFF) = (uintptr_t)save;
      *(volatile uint64_t *)(p + GC_THR_LASTSTOP_OFF) = *(volatile uint64_t *)(b + GC_STOP_COUNT_OFF);
      return 1;
    }
  }
  return 0;
}

/* Boehm's stop-the-world, on a platform where signals are never delivered.
 *
 * The inherited bridge only posted the acknowledgement, so the "stopped" threads
 * kept running while the collector marked and swept. daggerfall_nx's audit (sec
 * 28) caught what that costs: the audio thread read an object header with the
 * collector's mark bit set and branched through it. This is daggerfall_nx's
 * bridge -- the thread is really paused (svcSetThreadActivity via diag.c) and
 * resumed -- behind this port's arming, bounds and guard checks, which make
 * stale offsets disable the bridge loudly instead of corrupting silently. */
int pthread_kill_gc(pthread_t t, int sig) {
  uintptr_t b = g_il2cpp_base;
  if (!b) {
    static int warned = 0;
    if (!warned) {
      warned = 1;
      debugPrintf("[gc] BRIDGE NOT ARMED: pthread_kill(sig=%d) with g_il2cpp_base == 0.\n"
                  "     main.c must set g_il2cpp_base/g_il2cpp_size after loading\n"
                  "     libil2cpp; until then the first collection hangs.\n", sig);
    }
    return 0;
  }
  if (g_il2cpp_size && GC_MAX_OFF > g_il2cpp_size) {
    static int logged_oob = 0;
    if (!logged_oob) { logged_oob = 1;
      debugPrintf("[gc] BRIDGE DISABLED: offsets exceed libil2cpp size (max 0x%x > 0x%zx)\n",
                  (unsigned)GC_MAX_OFF, g_il2cpp_size); }
    return 0;
  }
  static int guards_state = 0;   /* 0 = unchecked, 1 = pass, -1 = fail */
  if (guards_state == 0) {
    const unsigned n = (unsigned)(sizeof gc_guards / sizeof gc_guards[0]);
    guards_state = 1;
    for (unsigned i = 0; i < n; i++) {
      uint32_t got = *(volatile uint32_t *)(b + gc_guards[i].off);
      if (got != gc_guards[i].word) {
        guards_state = -1;
        debugPrintf("[gc] BRIDGE DISABLED: guard %u/%u failed at il2cpp+0x%x -- expected %08x"
                    " (%s), found %08x. Re-derive with tools/offsets/derive_gc_bridge.py.\n",
                    i + 1u, n, (unsigned)gc_guards[i].off, (unsigned)gc_guards[i].word,
                    gc_guards[i].what, (unsigned)got);
        break;
      }
    }
    if (guards_state > 0) { debugPrintf("[gc] bridge guards OK (%u words)\n", n); gc_layout_check(b); }
  }
  if (guards_state < 0 || !sig) return 0;

  int suspend_sig = *(volatile int *)(b + GC_SUSPEND_SIG_OFF);
  int restart_sig = *(volatile int *)(b + GC_RESTART_SIG_OFF);
  void **ack_sem  = (void **)(b + GC_ACK_SEM_OFF);
  if (sig == suspend_sig) {            /* stop-the-world: ACTUALLY suspend */
    /* LOG BEFORE PAUSING, NEVER AFTER: debugPrintf takes the stdio/heap lock,
     * and the thread just paused may hold it. sem_post_fake() is safe while
     * paused -- a bare semaphoreSignal() with no shared lock. The ack is posted
     * whatever happens: a collector that never gets one waits forever. */
    static int logged_s = 0;
    if (!logged_s) { logged_s = 1;
      debugPrintf("[gc] stop-world: suspending threads for real (ack via sem@il2cpp+0x%x)\n",
                  (unsigned)GC_ACK_SEM_OFF); }
    ThreadContext ctx;
    int pr = diag_pause_pthread_ctx((void *)t, &ctx);    /* 1 = paused + registers */
    if (pr) { g_gc_paused_ok++; g_gc_paused_live++; }
    if (pr == 1 && g_gc_layout_ok == 1 && gc_publish_stopped(b, t, &ctx)) g_gc_published++;
    else g_gc_unpublished++;
    g_gc_paused_try++;
    sem_post_fake(ack_sem);
    return 0;
  }
  if (sig == restart_sig) {            /* start-the-world: resume for real */
    /* Resume FIRST, unconditionally; the gate only decides whether the collector
     * wants an acknowledgement, never whether a thread gets to run again. Log
     * only once nothing of ours is still paused (same stdio-lock hazard). */
    if (diag_resume_pthread((void *)t) && g_gc_paused_live) g_gc_paused_live--;
    /* REPORT EVERY ROUND THAT MISSES A STACK, not just the first.
     *
     * A thread that is paused but whose stack and registers are not published
     * has its roots invisible to the collector. Anything only that thread still
     * referenced becomes free to collect, and the failure does not appear here:
     * it appears later as a live object that has been reclaimed. The crash in
     * these logs has exactly that shape -- a container holding a non-zero count
     * and a NULL pointer, walked by a marking loop.
     *
     * The first round is two threads and always clean. By the time the game is
     * loading AssetBundles it has thirty, and no round after the first was being
     * watched at all. */
    static int logged_r = 0;
    static unsigned round_no, miss_rounds;
    static int last_pub, last_unpub;
    if (g_gc_paused_live == 0) {
      const int pub = g_gc_published - last_pub;
      const int unpub = g_gc_unpublished - last_unpub;
      last_pub = g_gc_published; last_unpub = g_gc_unpublished;
      round_no++;
      if (!logged_r) { logged_r = 1;
        debugPrintf("[gc] start-world: all resumed; first round paused %d of %d threads, "
                    "published %d stacks (%d not)\n",
                    g_gc_paused_ok, g_gc_paused_try, pub, unpub); }
      else if (unpub > 0 && miss_rounds < 12) {
        miss_rounds++;
        debugPrintf("[gc] ROUND %u MISSED %d of %d stacks -- those threads' roots were "
                    "invisible to the collector, so anything only they referenced "
                    "could be freed while still live\n",
                    round_no, unpub, pub + unpub);
      }
    }
    if (*(volatile int *)(b + GC_START_ACK_OFF)) sem_post_fake(ack_sem);
    return 0;
  }
  return 0;                            /* any other signal: no-op */
}
