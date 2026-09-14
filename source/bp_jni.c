/* bp_jni.c -- android.os.Vibrator / android.os.VibrationEffect over Switch HID.
 *
 * Derived from cloverpit_jni.c (MIT), reduced to the classes this game uses.
 * See bp_jni.h for why the InputDevice/InputManager half is not here.
 *
 * The mapping is not one-to-one, and the mismatch is the interesting part:
 * Android's Vibrator.vibrate(ms) is edge-triggered and fire-and-forget -- the
 * platform stops the motor when the duration expires. The Switch's
 * hidSendVibrationValues is level-triggered: it sets an amplitude that holds
 * until something sets a different one. So every vibrate() here has to arm a
 * deadline, and bp_jni_rumble_tick() has to be called once per frame to
 * disarm it. Skip that call and the first buzz in the game runs forever.
 *
 * MIT.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bp_jni.h"
#include "diag.h"
#include "util.h"   /* debugPrintf */
#include "jni_fake.h"

/* Must match the FakeID layout in jni_fake.c / unity_jni.c EXACTLY. That file
 * typedefs an anonymous struct, so this tagged declaration is a distinct type
 * with the same layout rather than a redefinition -- which is what lets the
 * dispatch chain pass it across as a const void *. If the layout there ever
 * changes, this silently misreads every method name. */
struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };

static int has(const char *hay, const char *needle) {
  return hay && needle && strstr(hay, needle) != NULL;
}

/* ------------------------------------------------------------ handle types */

enum { BPJ_TAG = 0x424d4a31 /* 'BMJ1' */ };
enum { BPJ_VIBRATOR, BPJ_VIBEFFECT };

typedef struct {
  uint32_t tag;
  int      kind;
  int32_t  ints[4];      /* VibrationEffect: [0] = ms, [1] = amplitude */
  int32_t  n;
} BmHandle;

static BmHandle *bpj_new(int kind) {
  BmHandle *h = calloc(1, sizeof *h);
  if (!h) return NULL;
  h->tag  = BPJ_TAG;
  h->kind = kind;
  return h;
}

/* Tag check before dereferencing. These pointers come back to us through the
 * managed side as opaque jobjects, and a handle from a class we do not own
 * would otherwise be read as one of ours. */
static int is_bmj(void *p, int kind) {
  BmHandle *h = p;
  return h && h->tag == BPJ_TAG && h->kind == kind;
}

/* ----------------------------------------------------------------- rumble */

static HidVibrationDeviceHandle g_vib[2];
static int g_vib_ready;
static int g_vib_tried;

static void rumble_init(void) {
  if (g_vib_tried) return;
  g_vib_tried = 1;

  /* Handheld and detached Joy-Cons use different device handles. Try handheld
   * first and fall back, matching how the pad is actually configured. */
  Result rc = hidInitializeVibrationDevices(g_vib, 2, HidNpadIdType_Handheld,
                                            HidNpadStyleTag_NpadHandheld);
  if (R_FAILED(rc))
    rc = hidInitializeVibrationDevices(g_vib, 2, HidNpadIdType_No1,
                                       HidNpadStyleTag_NpadFullKey);
  g_vib_ready = R_SUCCEEDED(rc);
  debugPrintf("[jni] rumble %s\n", g_vib_ready ? "ready" : "unavailable");
}

/* amplitude is Android's 1..255, or -1 for "default". The Switch wants 0.0..1.0
 * at two frequency bands. 160/320 Hz is the pair libnx's own samples use and
 * sits where the LRA is most responsive. */
static void rumble_set(int amplitude) {
  rumble_init();
  if (!g_vib_ready) return;

  float amp = (amplitude < 0) ? 0.5f : (float)amplitude / 255.0f;
  if (amp > 1.0f) amp = 1.0f;
  if (amp < 0.0f) amp = 0.0f;

  HidVibrationValue v[2];
  memset(v, 0, sizeof v);
  for (int i = 0; i < 2; i++) {
    v[i].amp_low   = amp;
    v[i].freq_low  = 160.0f;
    v[i].amp_high  = amp;
    v[i].freq_high = 320.0f;
  }
  hidSendVibrationValues(g_vib, v, 2);
}

static void rumble_stop(void) {
  rumble_init();
  if (!g_vib_ready) return;

  /* Zero amplitude but keep the frequencies set. Sending an all-zero value
   * including frequency is treated as an invalid packet by some firmware
   * revisions rather than as "stop". */
  HidVibrationValue v[2];
  memset(v, 0, sizeof v);
  for (int i = 0; i < 2; i++) {
    v[i].freq_low  = 160.0f;
    v[i].freq_high = 320.0f;
  }
  hidSendVibrationValues(g_vib, v, 2);
}

static uint64_t g_rumble_until_ns;

static uint64_t now_ns(void) {
  return armTicksToNs(armGetSystemTick());
}

void bp_jni_rumble_tick(void) {
  if (g_rumble_until_ns && now_ns() >= g_rumble_until_ns) {
    g_rumble_until_ns = 0;
    rumble_stop();
  }
}

static void rumble_for(int ms, int amplitude) {
  if (ms <= 0) {
    g_rumble_until_ns = 0;
    rumble_stop();
    return;
  }
  rumble_set(amplitude);
  g_rumble_until_ns = now_ns() + (uint64_t)ms * 1000000ull;
}

/* -------------------------------------------------------- class ownership */

int bp_jni_owns_class(const char *cls) {
  if (!cls) return 0;
  return has(cls, "os/Vibrator") ||
         has(cls, "os/VibrationEffect");
}

/* Effective class, same reasoning as unity_jni.c's eff_cls(): Unity 6 hands us
 * java/lang/Object for ~90% of method IDs, so gating on id->cls alone means
 * these handlers never fire and every vibrate() call silently does nothing.
 * tools/audit_jni.py flagged both classes here. */
static const char *bpj_cls(const struct FakeID *id, void *recv) {
  const char *c = id->cls;
  if (c && *c && !has(c, "java/lang/Object")) return c;
  if (recv) {
    uint32_t tag = *(uint32_t *)recv;
    if (tag == 0x434c5331u || tag == 0x4f424a31u) {
      const char *rc = (const char *)recv + 4;
      if (*rc) return rc;
    }
  }
  return c ? c : "";
}

/* ------------------------------------------------------- object dispatch */

void *bp_jni_dispatch_object(void *recv, const void *id_, va_list va) {
  const struct FakeID *id = id_;
  const char *cls = bpj_cls(id, recv), *m = id->name;
  (void)recv;

  if (has(cls, "os/VibrationEffect")) {
    /* createOneShot(long ms, int amplitude) carries the real parameters; the
     * others are named presets whose timing Android never exposes. */
    if (has(m, "createOneShot")) {
      BmHandle *h = bpj_new(BPJ_VIBEFFECT);
      if (h) {
        h->ints[0] = (int32_t)va_arg(va, long long);
        h->ints[1] = va_arg(va, int);
        h->n = 2;
      }
      return h;
    }
    if (has(m, "createPredefined") || has(m, "createWaveform")) {
      /* 40 ms at default strength. EFFECT_CLICK and friends are short taps on
       * real hardware, and a waveform's envelope cannot be reproduced through
       * a single level-triggered amplitude anyway -- so approximate it with one
       * short buzz rather than pretending to follow the pattern. */
      BmHandle *h = bpj_new(BPJ_VIBEFFECT);
      if (h) { h->ints[0] = 40; h->ints[1] = -1; h->n = 2; }
      return h;
    }
    return NULL;
  }

  /* getSystemService(VIBRATOR_SERVICE) lands in unity_jni.c, which hands back a
   * generic object. Anything asked of the Vibrator instance itself is answered
   * by the int/void dispatchers below, so no handle is needed here. */
  return NULL;
}

/* ---------------------------------------------------------- int dispatch */

uint64_t bp_jni_dispatch_int(void *recv, const void *id_, va_list va) {
  const struct FakeID *id = id_;
  const char *cls = bpj_cls(id, recv), *m = id->name;
  (void)recv; (void)va;

  if (has(cls, "os/Vibrator")) {
    /* Both true: the console has an LRA and it takes a continuous amplitude.
     * Reporting hasAmplitudeControl false would push Modules.Haptic down its
     * fallback path, which uses fixed-length presets and loses the intensity
     * the game asked for. */
    if (has(m, "hasVibrator"))         return 1;
    if (has(m, "hasAmplitudeControl")) return 1;
    return 0;
  }
  return 0;
}

/* --------------------------------------------------------- void dispatch */

void bp_jni_dispatch_void(void *recv, const void *id_, va_list va) {
  const struct FakeID *id = id_;
  const char *cls = bpj_cls(id, recv), *m = id->name;
  (void)recv;

  if (has(cls, "os/Vibrator")) {
    if (has(m, "cancel")) {
      rumble_for(0, 0);
      return;
    }
    if (has(m, "vibrate")) {
      /* Two shapes reach here:
       *   vibrate(long milliseconds)
       *   vibrate(VibrationEffect effect)
       * Read the JNI signature to tell them apart -- guessing from the varargs
       * would misparse one of the two and pull a pointer as a long. */
      if (has(id->sig, "Landroid/os/VibrationEffect;")) {
        void *eff = va_arg(va, void *);
        if (is_bmj(eff, BPJ_VIBEFFECT)) {
          BmHandle *h = eff;
          rumble_for(h->ints[0], h->ints[1]);
        } else {
          rumble_for(40, -1);   /* unknown effect object: short default buzz */
        }
      } else {
        long long ms = va_arg(va, long long);
        rumble_for((int)ms, -1);
      }
      return;
    }
  }
}
