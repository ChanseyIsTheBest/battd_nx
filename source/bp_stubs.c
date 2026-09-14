/* bp_stubs.c -- link-only bodies for the text2bitmap / movie-player / editbox
 * helpers that jni_fake.c's fallback handlers reference.
 *
 * These paths belong to the Chaos Rings 3 MVGL engine the substrate was
 * originally written for, and travel down the lineage as declarations that must
 * resolve. Unity drives none of them: it has no engine-side dynamic text
 * rendering, and its software keyboard goes through the SoftInputProvider stub
 * in unity_jni.c rather than through editbox. They exist so jni_fake.c links
 * without dragging in FreeType and FFmpeg.
 *
 * Bouncemasters specifically has no FMV -- assets/meta.mp4 is an Android-side
 * Activity splash that nothing managed names (see bp_video.h) -- so the movie
 * bodies are no-ops here for the same reason they are in cloverpit_nx, not
 * merely by inheritance.
 *
 * data_dir() is the one live function in this file. It feeds the JNI data-path
 * getters, and resolving it to the runtime-resolved game root rather than the
 * compiled-in GAME_HOME is what lets the user name the SD folder anything.
 *
 * MIT.
 */

#include <stddef.h>

#include "config.h"
#include "bp_root.h"
#include "data.h"
#include "text2bitmap.h"
#include "movie_player.h"
#include "editbox.h"

/* Game data root, resolved at runtime from the .nro's own location -- see
 * bp_root.c. bp_game_root() falls back to GAME_HOME before resolution, so this
 * is safe to call early; it just may answer with the default until main.c has
 * run bp_resolve_game_root(). */
const char *data_dir(void) { return bp_game_root(); }

/* Engine-thread-finished hook. Our main loop uses jni_quit_requested instead. */
void android_mark_main_finished(void) {}

/* ---- text rendering (engine dynamic text path; Unity does not use it) ---- */
FakeBitmap *text2bitmap_render(const char *text, int pixel_size) { (void)text; (void)pixel_size; return NULL; }
int  text2bitmap_measure_width (const char *text, int pixel_size) { (void)text; (void)pixel_size; return 0; }
int  text2bitmap_measure_height(const char *text, int pixel_size) { (void)text; (void)pixel_size; return 0; }
void text2bitmap_free(FakeBitmap *bmp) { (void)bmp; }

/* ---- FMV playback (unused; see bp_video.h) ---- */
void movie_set_db(const char *db_path) { (void)db_path; }
int  movie_play(const char *name, int looping) { (void)name; (void)looping; return 0; }
void movie_stop(void)   {}
void movie_pause(void)  {}
void movie_resume(void) {}
int  movie_is_playing(void) { return 0; }

/* ---- software keyboard (Unity routes through SoftInputProvider) ---- */
/* editbox: real implementation in editbox.c (swkbd). */

