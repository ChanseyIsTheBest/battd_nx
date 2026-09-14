/* battd_il2cpp.h -- managed-side hooks installed into the game's libil2cpp.
 * Currently one: VideoPlayer::Play -> battd_video.c. See the .c for why the
 * engine cannot play these clips itself.
 * MIT. */
#ifndef BATTD_IL2CPP_H
#define BATTD_IL2CPP_H

#include "so_util.h"

/* Call once, after libil2cpp.so is mapped and relocated and BEFORE the engine
 * starts. Returns the number of hooks installed (0 is not fatal -- the intro
 * just stays a black screen). Verifies guard words and skips loudly. */
int battd_il2cpp_install(so_module *il2cpp);

/* Call once per frame on the thread that owns the GL context (the engine's main
 * thread). Raises SplashScreenVideo.EndReached when our decoder finishes a clip
 * -- without it the splash would wait forever, because replacing PlayVideoNow
 * silences both of the handlers the game normally leaves on. */
void battd_il2cpp_pump(void);

#endif /* BATTD_IL2CPP_H */
