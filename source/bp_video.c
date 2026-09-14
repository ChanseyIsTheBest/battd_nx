/* bp_video.c -- no-op splash video player. See bp_video.h for why.
 *
 * Every function here is deliberately empty. The two inherited call sites --
 * the eglSwapBuffers wrapper in imports.c and the FMOD pump in jni_fake.c --
 * call a player unconditionally each frame and are written to handle "nothing
 * to show". Keeping the symbols means neither of them needs an #ifdef, and
 * dropping in cloverpit_video.c later is a file swap rather than a patch.
 *
 * MIT.
 */

#include "bp_video.h"

void bp_video_draw(void) {
  /* Nothing to draw. Deliberately does not touch GL state: the caller runs this
   * between the engine's last draw and eglSwapBuffers, so a stray bind or
   * enable here would land on the frame the engine just finished composing. */
}

int bp_video_mix_audio(short *dst, int frames, int channels) {
  (void)dst; (void)frames; (void)channels;
  /* 0 == "no movie audio this block". The caller keeps its buffer as-is, so the
   * engine's own PCM passes through untouched. Returning anything non-zero
   * would claim we had written that many frames of silence over it.
   *
   * No logging: this runs on the audio pump thread. */
  return 0;
}

int bp_video_is_playing(void) {
  return 0;
}
