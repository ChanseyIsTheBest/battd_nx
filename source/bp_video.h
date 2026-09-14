/* bp_video.h -- splash video playback. NOT IMPLEMENTED FOR THIS GAME.
 *
 * WHY THESE ARE STUBS
 * -------------------
 * cloverpit_nx carries a full ffmpeg-backed player here, because CloverPit
 * opens with two intro clips it drives through UnityEngine.VideoPlayer, and the
 * engine cannot play them: libunity reaches VideoPlayer through the Android NDK
 * media API, and bp_imports_extra.c answers all forty of those entry points
 * with AMEDIA_ERROR_UNSUPPORTED.
 *
 * Bouncemasters does not need any of that. The APK does ship assets/meta.mp4,
 * but it is not reachable from the engine:
 *
 *   - "meta.mp4" appears ZERO times in global-metadata.dat
 *   - "meta.mp4" appears ZERO times in libil2cpp.so
 *
 * so no managed code names it. It sits next to meta-logo.png and the four
 * loader_*.png splash images, which is the signature of an Android-side splash
 * shown by the Activity before Unity starts -- a layer this port replaces
 * outright rather than reproduces.
 *
 * VideoClip and VideoPlayer types do exist in the metadata, but that only means
 * UnityEngine.VideoModule was included in the build; nothing here points a
 * VideoPlayer at a clip on the boot path.
 *
 * So: no ffmpeg dependency, and the Makefile links none. These stubs exist to
 * satisfy the two inherited call sites -- imports.c's eglSwapBuffers wrapper
 * and the FMOD audio pump in jni_fake.c -- which are written to call a player
 * unconditionally and do the right thing when it reports nothing to show.
 *
 * IF THIS TURNS OUT TO BE WRONG and a clip is needed, do not write a new
 * player. Lift cloverpit_video.c wholesale along with the ffmpeg block from
 * that repo's Makefile; it is 45 KB of solved problems including the YUV
 * shader path and the audio mixing, and it already degrades safely when a
 * codec is missing.
 *
 * MIT.
 */
#ifndef BP_VIDEO_H
#define BP_VIDEO_H

/* Draw the newest decoded frame over the current framebuffer, immediately
 * before eglSwapBuffers. Called on the GL thread every frame. No-op here. */
void bp_video_draw(void);

/* Mix pending movie audio into an S16 interleaved buffer already holding the
 * engine's PCM. Called from the FMOD pump. Returns frames mixed -- always 0
 * here, which the caller reads as "nothing to add", leaving its buffer
 * untouched. MUST NOT LOG: it runs on the audio thread. */
int bp_video_mix_audio(short *dst, int frames, int channels);

/* Whether a clip is decoding. Diagnostic only. */
int bp_video_is_playing(void);

#endif /* BP_VIDEO_H */
