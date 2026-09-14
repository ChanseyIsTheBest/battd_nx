/* ---------------------------------------------------------------------------
 * bp_tate_glue.c -- wires clayjam_nx's portrait presentation (bp_tate.c) into
 * this tree. The three base edits PORTING_CLAYJAM.md §D.3 calls for are:
 *   1. imports.c eglSwapBuffers wrapper -> bp_tate_swap_hook() before the swap
 *   2. imports.c GL table: glBindFramebuffer -> bp_gl_BindFramebuffer_tate
 *   3. android_native_update_mode(): render size PORTRAIT, window LANDSCAPE
 * plus one this port adds: eglQuerySurface reports the render size while TATE
 * is on, so the engine never sees the landscape swapchain's dimensions.
 * MIT.
 * ------------------------------------------------------------------------- */
#include <switch.h>
#include <GLES3/gl3.h>
#include "android_native_unity.h"
#include "bp_tate.h"
#include "config.h"
#include "util.h"

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40   /* GLES 2.0+; only for minimal header stubs */
#endif

int bp_tate_enabled(void) { return BP_TATE_ENABLE; }

void bp_tate_bind_overlay(void) {
  if (bp_tate_active()) glBindFramebuffer(GL_FRAMEBUFFER, bp_tate_fbo());
}

/* Called from imports.c's eglSwapBuffers wrapper, immediately before the swap.
 * ASK THE WINDOW HOW BIG IT IS (clayjam learned this the hard way): handing
 * init a config value instead of the real NWindow size is what once rotated a
 * portrait image into a portrait window. */
void bp_tate_swap_hook(void) {
#if BP_TATE_ENABLE
  static int s_logged;
  if (!bp_tate_active()) {
    u32 ww = 0, wh = 0;
    NWindow *nw = nwindowGetDefault();
    if (nw) nwindowGetDimensions(nw, &ww, &wh);
    if (!ww || !wh) { ww = android_native_window_width(); wh = android_native_window_height(); }
    if (!s_logged) {
      debugPrintf("[tate] real window %ux%u, engine renders %dx%d, rot=%d\n",
                  ww, wh, screen_width, screen_height, BP_TATE_ROT);
      s_logged = 1;
    }
    bp_tate_init(screen_width, screen_height, (int)ww, (int)wh, BP_TATE_ROT);
  }
  bp_tate_present();
#endif
}

void bp_tate_map_stick(float dx, float dy, float *rx, float *ry) {
  /* Screen-space cursor delta (y down) -> render space. The console is held so
   * the portrait image is upright, so the stick turns with it. */
#if BP_TATE_ENABLE
  if (BP_TATE_ROT == 1)      { *rx =  dy; *ry = -dx; return; }
  else if (BP_TATE_ROT == 2) { *rx = -dy; *ry =  dx; return; }
#endif
  *rx = dx; *ry = dy;
}
