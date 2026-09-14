/* bp_jni.h -- the Java classes Bouncemasters reaches through AndroidJavaObject.
 *
 * Slots into jni_fake.c's dispatch chain ahead of unity_jni.c, on the same
 * contract: claim a class by name, then answer its calls by return kind.
 *
 * SCOPE, AND WHY IT IS NARROWER THAN cloverpit_nx's
 * -------------------------------------------------
 * CloverPit's equivalent covers four classes: android.os.Vibrator,
 * android.os.VibrationEffect, android.hardware.input.InputManager and
 * android.view.InputDevice. The latter two exist because CloverPit ships
 * Rewired, which enumerates physical gamepads through InputDevice and reads
 * "no controller attached" when those calls return 0/null.
 *
 * Bouncemasters ships no Rewired -- zero references in global-metadata.dat --
 * and is a touchscreen title whose input arrives as synthesised touches through
 * android::NewInput::ProcessTouchEvent. Claiming InputDevice here would answer
 * questions nothing asks, and would mean maintaining a fake controller identity
 * that no code path consults. Dropped.
 *
 * What IS needed is vibration. The game ships Modules.Haptic.dll and the
 * metadata carries VibrationManager, VibrationEffect, VibrationEffectClass,
 * HapticsSupported, Vibrate and vibrateAndroid -- so the haptics path is live,
 * reaches Java, and would otherwise fall through to generic no-op stubs and
 * silently do nothing.
 *
 * If the boot log shows unhandled classes worth claiming, add them here rather
 * than widening unity_jni.c: that file is inherited substrate shared with the
 * other ports in this lineage, and game-specific claims belong on this side of
 * the chain.
 *
 * MIT.
 */
#ifndef BP_JNI_H
#define BP_JNI_H

#include <stdarg.h>
#include <stdint.h>

/* 1 if this module answers for `cls`. Checked before every dispatch. */
int bp_jni_owns_class(const char *cls);

void     *bp_jni_dispatch_object(void *recv, const void *id, va_list va);
uint64_t  bp_jni_dispatch_int   (void *recv, const void *id, va_list va);
void      bp_jni_dispatch_void  (void *recv, const void *id, va_list va);

/* Stop rumble once its requested duration expires. Android's vibrate(ms) is
 * fire-and-forget with an implicit stop; the Switch's is level-triggered and
 * runs until told otherwise, so this must be called once per frame from the
 * render loop or a single buzz never ends. */
void bp_jni_rumble_tick(void);

/* provided by jni_fake.c */
extern void *jni_make_intarray(const int32_t *vals, int n);

#endif /* BP_JNI_H */
