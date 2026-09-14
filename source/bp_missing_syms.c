/* ---------------------------------------------------------------------------
 * bp_missing_syms.c -- the complete delta between what Bouncemasters' ARM64
 * libraries import and what the inherited shim tables already provide.
 *
 * A symbol diff of the three loaded modules (libmain / libunity / libil2cpp,
 * 498 distinct undefined dynamic symbols) against the combined cloverpit_nx +
 * killerbean_nx import tables leaves exactly four unresolved names, all of them
 * imported by libunity.so:
 *
 *     AImage_getWidth            NDK media image accessor
 *     __android_log_buf_write    liblog, buffer-targeted variant
 *     nextafter                  libm, double precision
 *     writev                     scatter/gather write
 *
 * That is 99.1% coverage inherited for free, and none of the four is load-
 * bearing. Add the table rows at the bottom of this file to dynlib_functions[]
 * in imports.c and the native import surface is closed.
 *
 * Register these BEFORE the first so_resolve() call. An unresolved import is
 * not a link error here -- so_util taints the slot and the game takes an
 * Instruction Abort at the first call site instead, which is a far more
 * confusing failure than a missing-symbol log line.
 * ------------------------------------------------------------------------- */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#include "libc_shim.h"   /* struct nx_iovec */


/* --------------------------------------------------------------------------
 * AImage_getWidth
 *
 * Part of the NDK AImageReader path, which libunity.so imports unconditionally
 * because the Android player supports camera-backed textures (WebCamTexture,
 * ARCore). Bouncemasters never opens a camera -- there is no camera to open --
 * so this is dead weight that only has to exist, not work.
 *
 * media_status_t, negative on error. AMEDIA_ERROR_UNSUPPORTED == -10002
 * (AMEDIA_ERROR_BASE is -10000). Returning that rather than AMEDIA_OK matters:
 * a caller that got OK would then trust *width, which we never set.
 * ------------------------------------------------------------------------ */
#define AMEDIA_ERROR_UNSUPPORTED (-10002)

int AImage_getWidth(void *image, int32_t *width) {
  (void)image;
  if (width) *width = 0;
  return AMEDIA_ERROR_UNSUPPORTED;
}

/* --------------------------------------------------------------------------
 * __android_log_buf_write
 *
 * Same as __android_log_write but targeting a named log buffer (main, radio,
 * events, crash). We have one sink, so the buffer id is discarded. Delegates to
 * the existing __android_log_write in imports.c rather than reimplementing the
 * formatting, so both paths stay consistent when DEBUG_LOG is toggled.
 * ------------------------------------------------------------------------ */
extern int __android_log_write(int prio, const char *tag, const char *text);

int __android_log_buf_write(int bufID, int prio, const char *tag, const char *text) {
  (void)bufID;
  return __android_log_write(prio, tag, text);
}

/* --------------------------------------------------------------------------
 * nextafter
 *
 * newlib ships this; it is absent from the inherited tables only because
 * neither reference game imported it. A direct passthrough keeps the host
 * libm's exact semantics (subnormals, the x == y case, NaN propagation)
 * instead of inviting a hand-rolled bit-twiddling version to get an edge case
 * wrong in physics code. Bouncemasters is a distance-scoring game; a
 * one-ULP error in the wrong place is the kind of bug that costs a weekend.
 * ------------------------------------------------------------------------ */
double bp_nextafter(double x, double y) {
  return nextafter(x, y);
}

/* --------------------------------------------------------------------------
 * writev
 *
 * Sequential write of the iovec array.
 *
 * Takes struct nx_iovec, not struct iovec: newlib has no <sys/uio.h>, so the
 * layout is declared in libc_shim.h. Same two fields, same ABI -- libunity
 * passes a Bionic iovec and it lands correctly. Bionic's writev is atomic with respect
 * to other writers on the same fd; ours is not, and cannot be over the fake-fd
 * layer in fakefd.c. That is acceptable here because libunity.so's only use is
 * log and pipe traffic on descriptors nothing else holds.
 *
 * Two details worth keeping:
 *   - Short writes are propagated rather than retried. A caller that ignores a
 *     short return is already broken; masking it hides that.
 *   - A zero-length iovec entry is skipped, not passed to write(). write(fd, p, 0)
 *     is defined but some fake-fd backends treat it as EOF.
 * ------------------------------------------------------------------------ */
ssize_t bp_writev(int fd, const struct nx_iovec *iov, int iovcnt) {
  if (!iov || iovcnt < 0) { errno = EINVAL; return -1; }

  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    size_t len = iov[i].iov_len;
    if (len == 0) continue;

    ssize_t n = write(fd, iov[i].iov_base, len);
    if (n < 0) {
      /* Report the error only if nothing was written at all; otherwise the
       * partial count is the more useful answer and errno stays set. */
      return total > 0 ? total : -1;
    }
    total += n;
    if ((size_t)n < len) break;   /* short write -- stop, report progress */
  }
  return total;
}

/* ---------------------------------------------------------------------------
 * Add these four rows to dynlib_functions[] in imports.c:
 *
 *   { "AImage_getWidth",         (uintptr_t)&AImage_getWidth },
 *   { "__android_log_buf_write", (uintptr_t)&__android_log_buf_write },
 *   { "nextafter",               (uintptr_t)&bp_nextafter },
 *   { "writev",                  (uintptr_t)&bp_writev },
 *
 * Then re-run tools/symcheck.py against your extracted libs; it should report
 * zero unresolved imports. Do that before every build after a game update --
 * a new APK can introduce new imports, and symcheck catches in one second what
 * would otherwise be an unexplained abort ten minutes into a boot attempt.
 * ------------------------------------------------------------------------- */
