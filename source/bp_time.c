#include "bp_offsets.h"
#include "bp_managed.h"
/* ---------------------------------------------------------------------------
 * bp_time.c -- drive UnityEngine.Time from our own clock.
 *
 * WHY THIS EXISTS ALONGSIDE bp_vsync.c
 * ------------------------------------
 * bp_vsync.c advances the engine's NATIVE vsync state, which is what stops
 * WaitVSync blocking and what the native TimeManager derives from. That is
 * necessary and it stays.
 *
 * It is not sufficient on Unity 6. cloverpit_nx measured that TimeManager::Update
 * never runs at all on this engine generation, so anything downstream of it is
 * inert -- and UnityEngine.Time's getters are downstream of it.
 *
 * This works one level up, at the managed boundary. Time's getters are ordinary
 * compiled methods in libil2cpp, so hooking them makes C# see time advance no
 * matter what the native subsystem is doing. The two paths do not fight: both
 * derive from the same monotonic base, so if TimeManager ever does run it steps
 * native time with the same clock these getters report.
 *
 * The symptom is specific and matches what this port is stuck on: a loading
 * spinner driven by `transform.Rotate(0, 0, speed * Time.deltaTime)` renders one
 * frame of motion and then sits still forever, because deltaTime reads 0 on
 * every frame after the first.
 *
 * IL2CPP CALLING CONVENTION
 * -------------------------
 * A static managed method compiles to a native function taking a hidden trailing
 * `const MethodInfo*`. Every hook here ignores its arguments and returns a
 * value, so the extra parameter is harmless -- it sits unread in the next
 * register. Float returns come back in s0, which is what these signatures give.
 *
 * bp_time_tick() MUST be called once per rendered frame. It is what samples
 * deltaTime; without it every delta reads as the clamp floor.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <switch.h>

#include "bp_managed.h"
#include "config.h"
#include "diag.h"
#include "so_util.h"
#include "util.h"

/* Clamp per-frame delta. A synchronous scene load can take seconds; reporting
 * that as one delta makes physics and animation explode on the frame after it.
 * 100 ms is about six frames -- enough that a hitch stays a hitch. The floor
 * keeps anything computing a rate from dividing by zero. */
#define BPT_MIN_DELTA 0.001f
#define BPT_MAX_DELTA 0.100f

#define BPT_FIXED_DELTA (1.0f / 60.0f)

static uint64_t g_base_ns;      /* captured at install */
static uint64_t g_last_ns;
static float    g_delta = BPT_FIXED_DELTA;   /* REAL elapsed, unscaled */
static float    g_smooth = BPT_FIXED_DELTA;

/* Unity keeps two clocks, and the difference is the whole point of timeScale.
 *
 *   Time.time / deltaTime / smoothDeltaTime   SCALED by timeScale
 *   Time.unscaledTime / unscaledDeltaTime     real
 *   Time.realtimeSinceStartup                 real, never scaled
 *   Time.fixedDeltaTime                       the fixed step, not scaled
 *
 * The first version reported the real delta for both, on the reasoning that
 * Unity applies timeScale itself and scaling here would apply it twice. That is
 * true of a real engine, where TimeManager does the scaling before anything
 * reads Time.deltaTime -- but this port replaces the getters outright, so
 * nothing else is left to do it. Whatever the getter returns IS deltaTime.
 *
 * The symptom was exact: Bouncemasters slows down on the opening swings by
 * setting timeScale, the log showed `dt=16.84 ms scale=0.34` frame after frame,
 * and nothing ever slowed -- which makes those shots impossible to aim and the
 * game impossible to progress. Two accumulators now, so scaled and unscaled
 * time diverge the way managed code expects. */
static double   g_scaled_s;      /* accumulated SCALED seconds   */
static double   g_unscaled_s;    /* accumulated real seconds     */
static uint32_t g_frames;
static int      g_ready;
static float    g_time_scale = 1.0f;   /* set via Time.timeScale; see below */

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* Real seconds since install. Never scaled -- this is what
 * realtimeSinceStartup means, and what the AsDouble variants report. */
static float elapsed_s(void) {
  if (!g_ready) return 0.0f;
  return (float)((double)(now_ns() - g_base_ns) * 1e-9);
}

void bp_time_tick(void) {
  if (!g_ready) return;

  uint64_t t = now_ns();
  float d = (float)((double)(t - g_last_ns) * 1e-9);
  g_last_ns = t;

  if (d < BPT_MIN_DELTA) d = BPT_MIN_DELTA;
  if (d > BPT_MAX_DELTA) d = BPT_MAX_DELTA;
  g_delta = d;

  /* Unity's smoothDeltaTime is a low-pass of deltaTime. Matching that keeps
   * anything using it for camera smoothing from juddering. */
  g_smooth += (d - g_smooth) * 0.2f;

  /* Advance both clocks. The scaled one is what Time.time reports, so a game
   * paused with timeScale = 0 sees Time.time stop while unscaledTime keeps
   * running -- which is exactly what pause menus rely on. */
  g_unscaled_s += (double)d;
  g_scaled_s   += (double)d * (double)g_time_scale;

  g_frames++;

#if DEBUG_LOG
  /* Once a second. There was previously NO way to tell from a log whether
   * managed time was advancing -- only the vsync pump reported, and that is a
   * different clock for a different consumer. When the vsync pump was stalling
   * at its 1 us floor the managed clock was fine, and nothing in the log said
   * so; the two had to be told apart by reasoning rather than by reading.
   * They are separate lines now. */
  if ((g_frames % 60u) == 0u)
    debugPrintf("[time] frame %u  t=%.2fs (real %.2fs)  dt=%.2f ms "
                "(real %.2f)  scale=%.2f\n",
                (unsigned)g_frames, g_scaled_s, g_unscaled_s,
                (double)(g_delta * g_time_scale * 1000.0f),
                (double)(g_delta * 1000.0f), (double)g_time_scale);
#endif
}

/* ------------------------------------------------------------------ hooks */

static float bpt_time(void)                 { return (float)g_scaled_s; }
static float bpt_deltaTime(void)            { return g_delta * g_time_scale; }
static float bpt_unscaledTime(void)         { return (float)g_unscaled_s; }
static float bpt_unscaledDeltaTime(void)    { return g_delta; }
/* Unity's fixedDeltaTime is the configured step, NOT scaled -- the physics
 * loop applies timeScale by running fewer steps, not by shortening them. */
static float bpt_fixedDeltaTime(void)       { return BPT_FIXED_DELTA; }
static float bpt_smoothDeltaTime(void)      { return g_smooth * g_time_scale; }
static float bpt_timeScale(void)            { return g_time_scale; }
static int   bpt_frameCount(void)           { return (int)g_frames; }
static float bpt_realtimeSinceStartup(void) { return elapsed_s(); }

/* timeSinceLevelLoad should reset per scene. We have no scene-load signal, so
 * it tracks total elapsed. Anything using it for a fade-in gets a value that
 * only ever grows, which is right on the first scene and harmless after --
 * far better than the 0 it reads today. */
static float bpt_timeSinceLevelLoad(void)   { return (float)g_scaled_s; }

/* ---- the double-precision variants ----
 *
 * These read the NATIVE clock, which is the one that does not advance. Leaving
 * them unhooked while the float getters are hooked is worse than leaving both:
 * managed code that mixes Time.time with Time.timeAsDouble would see two clocks
 * disagreeing, one moving and one frozen. dump.cs shows this game references
 * them, so they are not hypothetical. */
static double bpt_timeAsDouble(void) { return g_scaled_s; }
static double bpt_realtimeSinceStartupAsDouble(void) {
  if (!g_ready) return 0.0;
  return (double)(now_ns() - g_base_ns) * 1e-9;   /* real, never scaled */
}

/* ---- timeScale: a value, not a constant ----
 *
 * get_timeScale used to return a hardcoded 1.0f. That is wrong the moment the
 * game pauses: a pause menu sets Time.timeScale = 0, our setter was not hooked
 * so the assignment went to the frozen native TimeManager, and our getter kept
 * answering 1.0 -- so the pause would never take effect and gameplay would keep
 * running underneath the menu.
 *
 * Hooking both makes them coherent: the setter records, the getter reports what
 * was set. Note deltaTime deliberately stays UNSCALED here -- Unity applies
 * timeScale to deltaTime itself, and applying it twice would make a 0.5x slow
 * motion run at 0.25x. Unity's own contract is that unscaledDeltaTime ignores
 * it, which is what we return for both; if the game turns out to depend on
 * deltaTime being scaled, multiply in bpt_deltaTime, not here. */
static void  bpt_set_timeScale(float v) {
  if (v < 0.0f) v = 0.0f;          /* Unity clamps negatives to 0 */
  g_time_scale = v;
}

/* ---- RationalTime ----
 *
 * struct RationalTime { long m_Count; TicksPerSecond m_TicksPerSecond; } where
 * TicksPerSecond is { uint m_Numerator; uint m_Denominator; }. 16 bytes.
 *
 * Report elapsed time in nanosecond ticks: count = ns, rate = 1e9/1. That is
 * exact, needs no rounding, and matches what the float getters report. */
typedef struct { int64_t count; uint32_t num, den; } URationalTime;

static void bpt_rational_fill(URationalTime *r) {
  if (!r) return;
  r->count = g_ready ? (int64_t)(now_ns() - g_base_ns) : 0;
  r->num   = 1000000000u;
  r->den   = 1u;
}
static void bpt_timeAsRational_Injected(URationalTime *ret) { bpt_rational_fill(ret); }

/* The non-Injected form returns the 16-byte struct by value. On AArch64 a
 * 16-byte POD of two 8-byte-or-smaller fields is returned in x0:x1, not through
 * x8 -- so returning it directly is correct and no hidden pointer is involved. */
static URationalTime bpt_timeAsRational(void) {
  URationalTime r; bpt_rational_fill(&r); return r;
}

/* ---------------------------------------------------------------- install */

typedef void *(*bpt_gtm_fn)(void);
static bpt_gtm_fn g_gtm;
/* Pass-through: the fixed timestep stays the ENGINE's value (physics owns it).
 * Same read the stock binding does: GetTimeManager()->+0x48 (float). */
static float bpt_fixedDeltaTime_pt(void) {
  void *tm = g_gtm ? g_gtm() : NULL;
  return tm ? *(const float *)((const char *)tm + 0x48) : 0.02f;
}

/* Unity 2020.3: hook the libunity icall BINDINGS, not the managed getters.
 * Every managed Time.* call in libil2cpp resolves to these, so one hook covers
 * every caller. This build registers exactly 11 Time icalls (engine code
 * stripping removed the other 18, incl. every *AsDouble variant) -- see
 * bp_offsets.h. RationalTime does not exist before Unity 6. */
typedef void (*bpt_sts_fn)(void *tm, float v);
static bpt_sts_fn g_sts;
/* Fallback mode only: record the scale for our managed clock AND hand it to the
 * engine's TimeManager, so Animator/particles/physics pause with the C# side. */
static void bpt_set_timeScale_fwd(float v) {
  bpt_set_timeScale(v);
  void *tm = g_gtm ? g_gtm() : NULL;
  if (tm && g_sts) g_sts(tm, v);
}

int bp_time_install(so_module *unity) {
#if !BP_TIME_ICALL_HOOKS
  if (bp_tmclock_active()) {
    debugPrintf("[time] engine clock live (TimeManager hook): the 11 Time icalls are\n"
                "       left to the engine. BP_TIME_ICALL_HOOKS 1 forces the hooks.\n");
    return 0;
  }
#endif
  g_base_ns = g_last_ns = now_ns();
  g_ready = 1;
  if (bp_guard_ok(unity, BP_RVA_GetTimeManager, BP_GUARD_GetTimeManager, 4, "GetTimeManager"))
    g_gtm = (bpt_gtm_fn)((uintptr_t)unity->load_virtbase + BP_RVA_GetTimeManager);
  if (g_gtm && bp_guard_ok(unity, BP_RVA_TimeManager_SetTimeScale, BP_GUARD_TimeManager_SetTimeScale, 4,
                          "TimeManager::SetTimeScale"))
    g_sts = (bpt_sts_fn)((uintptr_t)unity->load_virtbase + BP_RVA_TimeManager_SetTimeScale);
  const int N = BP_NSITES(BP_TIME_SITES);
  int n = 0;
#define H(nm, fn) n += bp_icall_hook(unity, BP_TIME_SITES, N, nm, (void *)&fn)
  H("get_time",                 bpt_time);
  H("get_timeSinceLevelLoad",   bpt_timeSinceLevelLoad);
  H("get_deltaTime",            bpt_deltaTime);
  H("get_unscaledTime",         bpt_unscaledTime);
  H("get_unscaledDeltaTime",    bpt_unscaledDeltaTime);
  H("get_fixedDeltaTime",       bpt_fixedDeltaTime_pt);
  H("get_smoothDeltaTime",      bpt_smoothDeltaTime);
  H("get_timeScale",            bpt_timeScale);
  H("set_timeScale",            bpt_set_timeScale_fwd);
  H("get_frameCount",           bpt_frameCount);
  H("get_realtimeSinceStartup", bpt_realtimeSinceStartup);
#undef H
  debugPrintf("[time] %d/%d Time icall bindings hooked (all this build registers)\n", n, N);
  if (n && n < N)
    debugPrintf("[time] %d NOT installed -- mixed clocks are worse than none;\n"
                "       check the guard mismatches above\n", N - n);
  return n;
}
