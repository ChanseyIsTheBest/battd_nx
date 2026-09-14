/* clayjam_tate.c -- portrait presentation on a swapchain stuck in landscape.
 *
 * WHY THIS EXISTS
 *   Clay Jam Classic is portrait-only. The Switch will not give us a portrait
 *   window: geometry set BEFORE mesa creates the EGL surface appears to
 *   succeed, but afterwards the window is the panel (1280x720) and
 *   nwindowSetDimensions fails outright (rc=0xf59 on hardware, per
 *   papersplease_nx). So the swapchain is landscape, and a 720x1280 image
 *   cannot be pushed through it:
 *
 *     - crop (0,0,720,1280) on a 1280x720 window is out of bounds, so
 *       nvnflinger rejects every present with BAD_VALUE -> black screen;
 *     - clamping the crop to fit shows the top 720 rows only -> vertical squash.
 *
 *   Neither is fixable at the window layer, because the window is not ours to
 *   resize. So rotate in GL, where we own everything:
 *
 *     1. Give the engine a portrait framebuffer object. It thinks it has a
 *        portrait screen and uses its portrait layout.
 *     2. At present time, bind the REAL framebuffer and draw one full-screen
 *        quad sampling that texture with rotated texture coordinates.
 *
 *   Cost is one textured full-screen pass per frame.
 *
 * HOW THE ENGINE ENDS UP IN OUR FBO
 *   Unity returns to the default framebuffer with glBindFramebuffer(target, 0).
 *   The GL import table is ours, so that call is intercepted and our FBO
 *   substituted. bp_tate_present() calls the real glBindFramebuffer directly,
 *   so the present path is unaffected.
 *
 * THE BLIT IS 1:1 IN BOTH MODES
 *   handheld  720x1280 texture -> 1280x720 window, rotated
 *   docked   1080x1920 texture -> 1920x1080 window, rotated
 *   Each texture axis maps to an equal-length screen axis, so no scaling is
 *   involved and NEAREST is exact. That is why NEAREST is the default even
 *   though Clay Jam's claymation art would tolerate LINEAR: a rotated
 *   full-screen quad can land sample points a hair off texel centres, and
 *   LINEAR then blends neighbours for a slight softening across the whole
 *   image. BP_TATE_LINEAR in config.h flips it if you prefer the softer look.
 *
 * *** UNTESTED ON HARDWARE, like the papersplease_nx module it derives from.
 *     BP_TATE_ENABLE in config.h turns it off. ***
 *
 * MIT, same as the rest of the tree.
 */

#include <string.h>
#include <switch.h>
#include <GLES3/gl3.h>

#include "util.h"
#include "config.h"
#include "bp_tate.h"

static int    s_ready = 0, s_failed = 0;
static GLuint s_fbo = 0, s_tex = 0, s_depth = 0, s_prog = 0;
static GLint  s_loc_tex = -1;
static int    s_rw = 0, s_rh = 0;     /* render (portrait) size  */
static int    s_ww = 0, s_wh = 0;     /* window (landscape) size */
static int    s_rot = 1;              /* 1 = 90 CW, 2 = 90 CCW   */

/* ------------------------------------------------------------------ shaders */

static const char *kVS =
  "#version 300 es\n"
  "layout(location=0) in vec2 aPos;\n"
  "layout(location=1) in vec2 aUV;\n"
  "out vec2 vUV;\n"
  "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *kFS =
  "#version 300 es\n"
  "precision mediump float;\n"
  "in vec2 vUV;\n"
  "uniform sampler2D uTex;\n"
  "out vec4 oCol;\n"
  "void main(){ oCol = texture(uTex, vUV); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
        debugPrintf("[tate] shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* ------------------------------------------------------------------- setup */

int bp_tate_init(int render_w, int render_h, int window_w, int window_h, int rot)
{
    if (s_ready || s_failed) return s_ready;
#if !BP_TATE_ENABLE
    debugPrintf("[tate] disabled at compile time (BP_TATE_ENABLE 0)\n");
    s_failed = 1;
    return 0;
#endif
    if (render_w <= 0 || render_h <= 0 || window_w <= 0 || window_h <= 0) {
        debugPrintf("[tate] refusing init: bad geometry render=%dx%d window=%dx%d\n",
                    render_w, render_h, window_w, window_h);
        s_failed = 1;
        return 0;
    }
    /* A portrait render target on a landscape window is the whole point; if
     * they are both the same orientation something upstream is confused and
     * rotating would make it worse, not better. */
    if ((render_w > render_h) == (window_w > window_h)) {
        debugPrintf("[tate] not rotating: render %dx%d and window %dx%d are both "
                    "%s -- the compositor already gave us the orientation we "
                    "wanted, so the engine renders straight to the window\n",
                    render_w, render_h, window_w, window_h,
                    (render_h > render_w) ? "portrait" : "landscape");
        s_failed = 1;
        return 0;
    }

    s_rw = render_w; s_rh = render_h;
    s_ww = window_w; s_wh = window_h;
    s_rot = (rot == 2) ? 2 : 1;

    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_rw, s_rh, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#if BP_TATE_LINEAR
    const GLint filt = GL_LINEAR;
#else
    const GLint filt = GL_NEAREST;   /* blit is 1:1; see file header */
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Depth+stencil is required even though Clay Jam is mostly 2D: Unity's UI
     * and its own clears assume a complete framebuffer, and an FBO without
     * depth changes behaviour silently rather than failing loudly. */
    glGenRenderbuffers(1, &s_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s_rw, s_rh);

    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, s_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, s_depth);

    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        /* 0x8CDD = UNSUPPORTED, which on this stack usually means the GPU
         * arena could not place the attachments -- a memory problem wearing a
         * framebuffer costume. Check the [gpua] beacons before blaming TATE. */
        debugPrintf("[tate] FBO incomplete (0x%x) at %dx%d -- portrait disabled, "
                    "falling back to unrotated\n", st, s_rw, s_rh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        s_failed = 1;
        return 0;
    }

    GLuint vs = compile(GL_VERTEX_SHADER, kVS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) { s_failed = 1; glBindFramebuffer(GL_FRAMEBUFFER, 0); return 0; }
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "aPos");
    glBindAttribLocation(s_prog, 1, "aUV");
    glLinkProgram(s_prog);
    GLint ok = 0;
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(s_prog, sizeof log - 1, NULL, log);
        debugPrintf("[tate] program link failed: %s\n", log);
        s_failed = 1;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }
    s_loc_tex = glGetUniformLocation(s_prog, "uTex");

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);   /* engine starts here */
    s_ready = 1;
    debugPrintf("[tate] portrait ready: engine renders %dx%d -> window %dx%d, "
                "rot=%d (%s), filter=%s\n",
                s_rw, s_rh, s_ww, s_wh, s_rot,
                s_rot == 2 ? "90 CCW" : "90 CW",
                (filt == GL_LINEAR) ? "LINEAR" : "NEAREST");
    return 1;
}

int    bp_tate_active(void) { return s_ready; }
GLuint bp_tate_fbo(void)    { return s_fbo; }

void bp_tate_shutdown(void)
{
    if (!s_ready) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (s_prog)  glDeleteProgram(s_prog);
    if (s_fbo)   glDeleteFramebuffers(1, &s_fbo);
    if (s_tex)   glDeleteTextures(1, &s_tex);
    if (s_depth) glDeleteRenderbuffers(1, &s_depth);
    s_prog = s_fbo = s_tex = s_depth = 0;
    s_ready = 0;
    debugPrintf("[tate] shut down\n");
}

/* ==========================================================================
 *  THE ROTATION -- forward (display) and inverse (pointer) together.
 *
 *  Screen corners in triangle-strip order are BL, BR, TL, TR (GL y-up). The
 *  table below says which TEXTURE corner each screen corner samples, and that
 *  is the entire rotation.
 *
 *  CW (rot=1): the portrait image's TOP edge ends up on the screen's RIGHT.
 *      screen BL <- tex bottom-right    screen BR <- tex top-right
 *      screen TL <- tex bottom-left     screen TR <- tex top-left
 *
 *  CCW (rot=2) walks the cycle the other way.
 *
 *  The inverse, used for pointer input, is directly below and MUST agree.
 *  For CW, a panel point (px,py) in a panel_w x panel_h space maps to
 *      game_x = py            scaled to render_w
 *      game_y = panel_w - px  scaled to render_h
 *  which is what you get by reading the corner table backwards. If you change
 *  one of these, change the other in the same edit.
 * ========================================================================== */

static const GLfloat kPos[8]   = { -1.f,-1.f,   1.f,-1.f,  -1.f, 1.f,   1.f, 1.f };
static const GLfloat kUvCw[8]  = {  1.f, 0.f,   1.f, 1.f,   0.f, 0.f,   0.f, 1.f };
static const GLfloat kUvCcw[8] = {  0.f, 1.f,   0.f, 0.f,   1.f, 1.f,   1.f, 0.f };

void bp_tate_map_pointer(float px, float py,
                         float panel_w, float panel_h,
                         float *out_x, float *out_y)
{
    /* Fall back to the believed render size if we have not initialised yet, so
     * early input is merely unrotated rather than divided by zero. */
    const float rw = s_ready ? (float)s_rw : (float)screen_width;
    const float rh = s_ready ? (float)s_rh : (float)screen_height;
    if (panel_w <= 0.f || panel_h <= 0.f) { *out_x = px; *out_y = py; return; }

    if (!s_ready || !BP_TATE_ROT) {       /* plain stretch, no rotation */
        *out_x = px * (rw / panel_w);
        *out_y = py * (rh / panel_h);
        return;
    }
    if (s_rot == 2) {                          /* 90 CCW */
        *out_x = (panel_h - py) * (rw / panel_h);
        *out_y =  px            * (rh / panel_w);
    } else {                                   /* 90 CW  */
        *out_x =  py            * (rw / panel_h);
        *out_y = (panel_w - px) * (rh / panel_w);
    }
}

/* ----------------------------------------------------------------- present */

void bp_tate_present(void)
{
    if (!s_ready) return;

    /* --- save whatever the engine left bound --------------------------- */
    GLint prev_prog = 0, prev_vao = 0, prev_tex = 0, prev_ab = 0, vp[4] = {0,0,0,0};
    glGetIntegerv(GL_CURRENT_PROGRAM,      &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,   &prev_tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_ab);
    glGetIntegerv(GL_VIEWPORT, vp);
    const GLboolean had_depth   = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean had_blend   = glIsEnabled(GL_BLEND);
    const GLboolean had_cull    = glIsEnabled(GL_CULL_FACE);
    const GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);

    /* --- rotated blit into the real window ----------------------------- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);      /* the genuine default FB */
    glViewport(0, 0, s_ww, s_wh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    /* GLES3 forbids client-side vertex arrays while a non-zero VAO is bound,
     * and the engine leaves one bound. Bind 0 for the draw. */
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    const GLfloat *uv = (s_rot == 2) ? kUvCcw : kUvCw;

    glUseProgram(s_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    if (s_loc_tex >= 0) glUniform1i(s_loc_tex, 0);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kPos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    /* --- restore, and hand the engine its FBO back --------------------- */
    glUseProgram((GLuint)prev_prog);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_ab);
    glBindVertexArray((GLuint)prev_vao);
    if (had_depth)   glEnable(GL_DEPTH_TEST);
    if (had_blend)   glEnable(GL_BLEND);
    if (had_cull)    glEnable(GL_CULL_FACE);
    if (had_scissor) glEnable(GL_SCISSOR_TEST);
    glViewport(vp[0], vp[1], vp[2], vp[3]);

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
}

/* ---------------------------------------------------------------- plumbing */

/* Install in the GL import table INSTEAD of glBindFramebuffer:
 *     { "glBindFramebuffer", (uintptr_t)&bp_gl_BindFramebuffer_tate },
 * This is the single point where the substitution happens. */
void bp_gl_BindFramebuffer_tate(GLenum target, GLuint fb)
{
    if (fb == 0 && bp_tate_active()) fb = bp_tate_fbo();
    glBindFramebuffer(target, fb);
}
