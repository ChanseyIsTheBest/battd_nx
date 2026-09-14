/* error.h -- error handler
 *
 * Copyright (C) 2021 fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ERROR_H__
#define __ERROR_H__

void fatal_error(const char *fmt, ...) __attribute__((noreturn));

/* Boot-time progress on the console, for work that takes long enough that a
 * black screen reads as a hang. The asset pack build is the reason this exists:
 * it takes roughly a minute on first boot for this game's 1,224 files, and
 * without a message on screen that is indistinguishable from a freeze.
 *
 * Only safe BEFORE the game's graphics stack owns the display -- consoleInit()
 * re-enters vi/nwindow and aborts inside libnx once EGL holds the surface. Call
 * startup_status_end() before any GL/EGL setup. */
void startup_status_begin(const char *message);
void startup_status_update(const char *message);
void startup_status_end(void);

#endif
