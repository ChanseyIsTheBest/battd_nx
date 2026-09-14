/* bp_touch.h -- current pointer/touch state, with Android-style phases.
 *
 * REPLACES unity_input_hook.h/.c from badpiggies_nx.
 *
 * That file was 449 lines whose real job was patching nineteen hard-coded
 * UnityEngine.Input and PlayerPrefs RVAs inside Bad Piggies' libil2cpp.so. None
 * of those addresses mean anything in Bouncemasters, so the hooks would be
 * wrong. (cloverpit_nx additionally had Rewired, which does not read
 * UnityEngine.Input at all; this game ships no Rewired, but it does not need
 * the hooks either -- it is a touchscreen title driven entirely by injected
 * touches.) Deleted rather than disabled.
 *
 * What was worth keeping is the small part underneath: the multi-touch slot
 * tracker that turns "here is the set of fingers currently down" into per-finger
 * DOWN / MOVE / UP phases. That is exactly what building an Android MotionEvent
 * needs, so it survives here with the same type and function names the inherited
 * android_native_unity.c already calls.
 *
 * The producer is android_native_feed_hid() (android_native_unity.c), which
 * already merges the touchscreen, the stick cursor, gyro aiming and a USB mouse
 * into one stream via nx_pointer.c, in render space with a bottom-left origin.
 * The consumer is bloonspop_input.c.
 *
 * MIT.
 */
#ifndef BP_TOUCH_H
#define BP_TOUCH_H

/* Switch tracks up to 16 fingers; 10 is plenty and matches Android's practical
 * pointer limit. */
#define NX_MAX_TOUCH 10

/* One finger in Unity screen space (bottom-left origin, render px). `id` is the
 * raw HID finger id and must stay stable while the finger is down so phases
 * track correctly across frames. */
typedef struct { int id; float x, y; } NxTouchIn;

/* Android MotionEvent action codes, so the consumer does not have to translate. */
typedef enum {
  BP_TOUCH_NONE = -1,
  BP_TOUCH_DOWN = 0,   /* AMOTION_EVENT_ACTION_DOWN */
  BP_TOUCH_UP   = 1,   /* AMOTION_EVENT_ACTION_UP   */
  BP_TOUCH_MOVE = 2    /* AMOTION_EVENT_ACTION_MOVE */
} bp_touch_phase;

typedef struct {
  int            id;      /* densified slot index, 0..NX_MAX_TOUCH-1 */
  int            raw_id;  /* the HID finger id                       */
  float          x, y;
  bp_touch_phase phase;
} bp_touch;

/* Push EVERY finger currently down (n may be 0). Phases are derived by matching
 * raw ids against the previous frame: unseen -> DOWN, seen -> MOVE, missing ->
 * UP for exactly one frame. Called once per frame by android_native_feed_hid.
 * Name and signature kept from unity_input_hook.h so the inherited caller in
 * android_native_unity.c needs no edit beyond its #include. */
void nx_input_hook_update_multi(const NxTouchIn *in, int n);

/* Single-finger convenience wrapper (stick-cursor path). */
void nx_input_hook_update(int active, float ux, float uy);

/* Read the current frame's fingers, phases included. Returns how many were
 * written. UP entries appear for exactly one frame after a finger lifts, which
 * is the frame in which the consumer must emit ACTION_UP. */
int bp_touch_poll(bp_touch *out, int max);

/* 1 if any finger is currently down. */
int bp_touch_any_down(void);

#endif /* BP_TOUCH_H */
