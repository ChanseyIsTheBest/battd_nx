#include "bp_offsets.h"
#include "bp_managed.h"
/* ---------------------------------------------------------------------------
 * bp_input.c -- expose the Switch touchscreen to UnityEngine.Input.
 *
 * Bouncemasters is a touchscreen title. Every interaction is a tap or a drag on
 * a button the game draws itself, so there is nothing to map buttons onto --
 * what it needs is for Input.touchCount / Input.GetTouch / Input.mousePosition
 * to describe the Switch panel.
 *
 * THE PRODUCER ALREADY EXISTS
 * ---------------------------
 * android_native_feed_hid() merges the touchscreen, the stick-driven cursor,
 * gyro aiming and a USB mouse into one stream in render space with a
 * bottom-left origin, and bp_touch.c turns that into per-finger DOWN / MOVE / UP
 * phases. This file is only the consumer: it answers managed queries from
 * bp_touch_poll(). No new input plumbing, no second source of truth.
 *
 * WHY HOOK Input RATHER THAN INJECT MOTIONEVENTS
 * ----------------------------------------------
 * Injecting through android::NewInput::ProcessTouchEvent would be the other
 * route, and killerbean_nx takes it. It needs a per-build native RVA and a
 * correctly shaped MotionEvent. Hooking the managed getters needs neither: the
 * addresses come from the same dump.cs as everything else here, the values are
 * plain scalars, and what the game reads is exactly what we wrote. For a game
 * whose input is entirely touch, the managed layer is where the truth needs to
 * be anyway.
 *
 * BOTH TOUCH AND MOUSE ARE ANSWERED
 * ---------------------------------
 * Unity UI (uGUI) drives its StandaloneInputModule from mousePosition +
 * GetMouseButton when touchSupported is false, and from touch when it is true.
 * Which one a given build uses depends on the EventSystem configuration, which
 * we cannot see from here -- so both are answered consistently from the same
 * finger. A tap is simultaneously "touch 0 in phase Began" and "mouse button 0
 * went down at that position", which is what a real Android device reports too.
 *
 * COORDINATES
 * -----------
 * bp_touch already delivers bottom-left origin in render pixels, which is
 * exactly Unity's screen space. No flip here; if the Y axis ever looks
 * inverted, the bug is in android_native_feed_hid's flip, not in this file.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <string.h>

#include "bp_managed.h"
#include "bp_touch.h"

uint32_t port_frame_count(void);
#include "config.h"
#include "diag.h"
#include "so_util.h"
#include "util.h"

/* UnityEngine.Vector3 / Vector2 -- plain float triples/pairs. */
typedef struct { float x, y, z; } V3;
typedef struct { float x, y; }    V2;

/* UnityEngine.Touch, field offsets straight from dump.cs:
 *   0x00 int      m_FingerId
 *   0x04 Vector2  m_Position
 *   0x0C Vector2  m_RawPosition
 *   0x14 Vector2  m_PositionDelta
 *   0x1C float    m_TimeDelta
 *   0x20 int      m_TapCount
 *   0x24 TouchPhase m_Phase
 *   0x28 TouchType  m_Type
 *   0x2C float    m_Pressure
 *   0x30 float    m_maximumPossiblePressure
 *   0x34 float    m_Radius
 *   0x38 float    m_RadiusVariance
 *   0x3C float    m_AltitudeAngle
 *   0x40 float    m_AzimuthAngle
 * Total 0x44. Laid out explicitly rather than trusting struct packing. */
typedef struct {
  int32_t fingerId;
  V2      position;
  V2      rawPosition;
  V2      positionDelta;
  float   timeDelta;
  int32_t tapCount;
  int32_t phase;
  int32_t type;
  float   pressure;
  float   maxPressure;
  float   radius;
  float   radiusVariance;
  float   altitudeAngle;
  float   azimuthAngle;
} UTouch;

/* UnityEngine.TouchPhase */
enum { UTP_BEGAN = 0, UTP_MOVED = 1, UTP_STATIONARY = 2, UTP_ENDED = 3, UTP_CANCELED = 4 };

/* ------------------------------------------------------------ frame state */

static bp_touch g_cur[NX_MAX_TOUCH];
static int      g_n;
static V2       g_prev_pos[NX_MAX_TOUCH];
static int      g_prev_valid[NX_MAX_TOUCH];

/* Last known pointer position. Held across lift so mousePosition does not snap
 * to the origin the instant a finger leaves -- uGUI reads it on the release
 * frame to decide whether the pointer is still over the button it pressed. */
static V2 g_last_pos = { 0.0f, 0.0f };

/* Poll the tracker, but advance per-frame bookkeeping only ONCE per frame.
 *
 * Every hook calls refresh(), and uGUI calls several of them per frame --
 * get_touchCount, GetTouch, get_mousePosition, GetMouseButton* all land here.
 * bp_touch_poll itself is non-destructive so repeated reads are safe, but the
 * delta bookkeeping in bpi_GetTouch_fill is NOT: it overwrites g_prev_pos with
 * the current position, so a second GetTouch in the same frame reports a
 * deltaPosition of zero.
 *
 * Touch.get_deltaPosition has 4 call sites in this game, and a drag whose delta
 * randomly reads zero depending on how many times the module polled is exactly
 * the kind of bug that looks like "input is flaky" rather than "input is
 * wrong". Gate on the frame counter. */
static uint32_t g_last_refresh_frame = 0xffffffffu;
static int      g_frame_is_new;

static void refresh(void) {
  uint32_t f = port_frame_count();
  g_frame_is_new = (f != g_last_refresh_frame);
  g_last_refresh_frame = f;

  g_n = bp_touch_poll(g_cur, NX_MAX_TOUCH);

#if DEBUG_LOG
  /* Prove the path end to end, once each. Silence here was ambiguous for a
   * whole cycle: "the hooks are installed" told us nothing about whether the
   * engine ever calls them, or whether anything ever reaches the tracker. */
  {
    static int said_polled, said_touch;
    if (!said_polled) { said_polled = 1;
      debugPrintf("[input] first Input poll from the engine "
                  "(hooks are live)\n"); }
    if (!said_touch && g_n > 0) { said_touch = 1;
      debugPrintf("[input] first finger: n=%d id=%d at %.0f,%.0f phase=%d\n",
                  g_n, g_cur[0].id, (double)g_cur[0].x, (double)g_cur[0].y,
                  g_cur[0].phase); }
  }
#endif
  for (int i = 0; i < g_n; i++)
    if (g_cur[i].phase != BP_TOUCH_UP) { g_last_pos.x = g_cur[i].x; g_last_pos.y = g_cur[i].y; break; }
}

/* A finger counts as present for touchCount while it is DOWN or MOVE, and also
 * on its single UP frame -- Unity expects to see the finger one last time in
 * phase Ended, and a UI that only handles Ended never fires without it. */
static int live_count(void) {
  int n = 0;
  for (int i = 0; i < g_n; i++)
    if (g_cur[i].phase != BP_TOUCH_NONE) n++;
  return n;
}

/* ------------------------------------------------------------------ hooks */

static int32_t bpi_touchCount(void)     { refresh(); return live_count(); }
static int32_t bpi_touchSupported(void) { return 1; }
static int32_t bpi_mousePresent(void)   { return 1; }

static void bpi_GetTouch_fill(int32_t index, UTouch *ret);   /* defined below */

/* The wrapper the game actually calls. Vector3 is an HFA -- three floats back
 * in s0/s1/s2 -- which is exactly what returning this struct by value produces.
 * See bp_managed.h for why the _Injected hook below is not enough. */
static V3 bpi_mousePosition(void) {
  refresh();
  V3 v; v.x = g_last_pos.x; v.y = g_last_pos.y; v.z = 0.0f;
  return v;
}

/* Ditto for GetTouch. The 0x44-byte Touch comes back through the indirect-result
 * register x8, which is what the compiler emits for a struct this size. */
static UTouch bpi_GetTouch(int32_t index) {
  UTouch t;
  bpi_GetTouch_fill(index, &t);
  return t;
}

static void bpi_mousePosition_Injected(V3 *ret) {
  refresh();
  if (!ret) return;
  ret->x = g_last_pos.x;
  ret->y = g_last_pos.y;
  ret->z = 0.0f;
}

static void bpi_GetTouch_Injected(int32_t index, UTouch *ret) {
  bpi_GetTouch_fill(index, ret);
}

static void bpi_GetTouch_fill(int32_t index, UTouch *ret) {
  if (!ret) return;
  memset(ret, 0, sizeof *ret);
  refresh();

  /* Walk to the index-th live finger rather than indexing g_cur directly:
   * bp_touch's slots are densified but a NONE can still appear between live
   * entries, and Unity's index is over live touches only. */
  int seen = 0;
  for (int i = 0; i < g_n; i++) {
    if (g_cur[i].phase == BP_TOUCH_NONE) continue;
    if (seen++ != index) continue;

    int slot = g_cur[i].id;
    if (slot < 0 || slot >= NX_MAX_TOUCH) slot = 0;

    ret->fingerId    = g_cur[i].id;
    ret->position.x  = g_cur[i].x;
    ret->position.y  = g_cur[i].y;
    ret->rawPosition = ret->position;

    if (g_prev_valid[slot]) {
      ret->positionDelta.x = g_cur[i].x - g_prev_pos[slot].x;
      ret->positionDelta.y = g_cur[i].y - g_prev_pos[slot].y;
    }
    /* Only on the frame's FIRST poll -- see refresh(). Otherwise a second
     * GetTouch in the same frame would compare the position against itself. */
    if (g_frame_is_new) {
      g_prev_pos[slot]   = ret->position;
      g_prev_valid[slot] = (g_cur[i].phase != BP_TOUCH_UP);
    }

    switch (g_cur[i].phase) {
      case BP_TOUCH_DOWN: ret->phase = UTP_BEGAN; break;
      case BP_TOUCH_UP:   ret->phase = UTP_ENDED; break;
      /* A finger that has not moved this frame is Stationary, not Moved.
       * A drag-threshold check that sees Moved every frame starts a drag on a
       * stationary finger, which makes buttons impossible to press. */
      default:
        ret->phase = (ret->positionDelta.x == 0.0f && ret->positionDelta.y == 0.0f)
                   ? UTP_STATIONARY : UTP_MOVED;
        break;
    }

    ret->tapCount    = 1;
    ret->timeDelta   = 1.0f / 60.0f;
    ret->pressure    = 1.0f;
    ret->maxPressure = 1.0f;
    ret->type        = 0;                  /* TouchType.Direct */
    return;
  }
  /* Out of range: a zeroed Touch with fingerId -1, which is what Unity's own
   * bounds failure produces. */
  ret->fingerId = -1;
}

/* Mouse emulation, same finger. Button 0 only; 1 and 2 are always up. */
static int32_t bpi_GetMouseButton(int32_t button) {
  if (button != 0) return 0;
  refresh();
  for (int i = 0; i < g_n; i++)
    if (g_cur[i].phase == BP_TOUCH_DOWN || g_cur[i].phase == BP_TOUCH_MOVE) return 1;
  return 0;
}

static int32_t bpi_GetMouseButtonDown(int32_t button) {
  if (button != 0) return 0;
  refresh();
  for (int i = 0; i < g_n; i++) if (g_cur[i].phase == BP_TOUCH_DOWN) return 1;
  return 0;
}

static int32_t bpi_GetMouseButtonUp(int32_t button) {
  if (button != 0) return 0;
  refresh();
  for (int i = 0; i < g_n; i++) if (g_cur[i].phase == BP_TOUCH_UP) return 1;
  return 0;
}

/* anyKey covers "is the user touching anything at all", which some menus use to
 * skip a splash. anyKeyDown is the edge. */
static int32_t bpi_anyKey(void)     { refresh(); return bp_touch_any_down(); }
static int32_t bpi_anyKeyDown(void) { return bpi_GetMouseButtonDown(0); }

/* ---------------------------------------------------------------- install */

/* libunity icall bindings (see bp_time.c for why bindings, not managed RVAs).
 * Input.get_anyKey/anyKeyDown are not registered by this build. */
int bp_input_install(so_module *unity) {
  const int N = BP_NSITES(BP_INPUT_SITES);
  int n = 0, want = 0;
#define H(nm, fn) do { want++; n += bp_icall_hook(unity, BP_INPUT_SITES, N, nm, (void *)&fn); } while (0)
  H("get_mousePosition_Injected", bpi_mousePosition_Injected);
  H("get_mousePresent",           bpi_mousePresent);
  H("get_touchSupported",         bpi_touchSupported);
  H("get_touchCount",             bpi_touchCount);
  H("GetMouseButton",             bpi_GetMouseButton);
  H("GetMouseButtonDown",         bpi_GetMouseButtonDown);
  H("GetMouseButtonUp",           bpi_GetMouseButtonUp);
  H("GetTouch_Injected",          bpi_GetTouch_Injected);
#undef H
  debugPrintf("[input] %d/%d Input icall bindings hooked (touch + mouse emulation)\n", n, want);
  if (n == 0)
    debugPrintf("[input] NONE installed -- the game will be unplayable. Regenerate\n"
                "        bp_offsets.h with tools/offsets/run_all.sh.\n");
  return n;
}
