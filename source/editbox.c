/* editbox.c -- the Switch software keyboard (swkbd) behind Unity's soft input. MIT.
 *
 * Borrowed from daggerfall_nx and pvz_fusion_en_nx (same design in both): swkbdShow()
 * runs the system keyboard applet and BLOCKS until the player confirms or cancels,
 * and the result goes back to Unity through the soft-input natives the JNI layer
 * captured at RegisterNatives (kbd_push_result() in jni_fake.c).
 *
 * Two changes for Unity 2020.3's TouchScreenKeyboard, which the age gate
 * (TMP_InputField ageGateInput) uses:
 *
 *  - TIMING. On Android, UnityPlayer.showSoftInput() returns at once and the Java
 *    UI thread later calls nativeSetInputString / nativeSoftInputClosed. The
 *    reference ports ran the keyboard INSIDE showSoftInput and delivered the result
 *    before Unity had finished opening its keyboard state, which its game (a
 *    String-returning keyboard call) tolerated. Here showSoftInput only records the
 *    request (editbox_request); editbox_pump() shows the keyboard at the next frame
 *    boundary and delivers in Android's order.
 *
 *  - SHAPE. Unity's keyboard type, password flag, placeholder and character limit
 *    reach swkbd: NumberPad / PhonePad / DecimalPad get the number pad.
 */
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include "editbox.h"
#include "util.h"

extern unsigned g_kbd_trace;                                    /* jni_fake.c */
extern void kbd_push_result(const char *text, int cancelled);   /* jni_fake.c */

#define EDITBOX_TEXT_CAP 1024

static char g_text[EDITBOX_TEXT_CAP];
static volatile int g_open;
static int  g_cancelled = 1, g_engine_driven;
static Mutex g_lock;                         /* one applet at a time: a second crashes the console */

static Mutex g_req_lock;
static volatile int g_req_pending;
static struct {
  char initial[EDITBOX_TEXT_CAP];
  char placeholder[256];
  int  type, secure, multiline, limit;
} g_req;

/* Unity TouchScreenKeyboardType -> swkbd. */
static const char *type_name(int t) {
  switch (t) {
    case 0: return "Default"; case 1: return "ASCIICapable"; case 2: return "NumbersAndPunctuation";
    case 3: return "URL"; case 4: return "NumberPad"; case 5: return "PhonePad"; case 6: return "NamePhonePad";
    case 7: return "EmailAddress"; case 11: return "DecimalPad"; default: return "other";
  }
}

/* Returns 1 confirmed, 0 cancelled, -1 refused (a keyboard is already up). */
static int run_swkbd(const char *initial, int type, int secure, int multiline,
                     const char *placeholder, int limit) {
  if (!mutexTryLock(&g_lock)) {
    debugPrintf("[kbd] refused: a keyboard is already up (a second applet would crash the console)\n");
    return -1;
  }
  g_open = 1;
  g_cancelled = 1;                           /* assume cancel until confirmed */
  snprintf(g_text, sizeof g_text, "%s", initial ? initial : "");
  if (limit <= 0 || limit >= EDITBOX_TEXT_CAP) limit = EDITBOX_TEXT_CAP - 1;

  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_SUCCEEDED(rc)) {
    char result[EDITBOX_TEXT_CAP];
    snprintf(result, sizeof result, "%s", g_text);
    swkbdConfigMakePresetDefault(&kbd);
    if (type == 4 || type == 5 || type == 11) {          /* NumberPad, PhonePad, DecimalPad */
      swkbdConfigSetType(&kbd, SwkbdType_NumPad);
      if (type == 11) swkbdConfigSetLeftOptionalSymbolKey(&kbd, ".");
    }
    if (secure) swkbdConfigSetPasswordFlag(&kbd, 1);
    if (multiline) swkbdConfigSetTextDrawType(&kbd, SwkbdTextDrawType_Box);
    if (placeholder && placeholder[0]) swkbdConfigSetGuideText(&kbd, placeholder);
    swkbdConfigSetInitialText(&kbd, g_text);
    swkbdConfigSetStringLenMax(&kbd, (u32)limit);
    rc = swkbdShow(&kbd, result, sizeof result);        /* blocks: system applet */
    if (R_SUCCEEDED(rc)) {
      snprintf(g_text, sizeof g_text, "%s", result);
      g_cancelled = 0;
    }
    swkbdClose(&kbd);
  } else {
    debugPrintf("[kbd] swkbdCreate failed (0x%x)\n", rc);
  }
  g_open = 0;
  mutexUnlock(&g_lock);
  debugPrintf("[kbd] swkbd closed: %s, text=\"%s\"\n", g_cancelled ? "cancelled" : "confirmed", g_text);
  return g_cancelled ? 0 : 1;
}

void editbox_request(const char *initial, int unity_type, int secure, int multiline,
                     const char *placeholder, int char_limit) {
  mutexLock(&g_req_lock);
  snprintf(g_req.initial, sizeof g_req.initial, "%s", initial ? initial : "");
  snprintf(g_req.placeholder, sizeof g_req.placeholder, "%s", placeholder ? placeholder : "");
  g_req.type = unity_type; g_req.secure = secure; g_req.multiline = multiline; g_req.limit = char_limit;
  g_req_pending = 1;
  mutexUnlock(&g_req_lock);
  debugPrintf("[kbd] showSoftInput: type=%s secure=%d multiline=%d limit=%d placeholder=\"%s\" "
              "-> system keyboard at the next frame\n",
              type_name(unity_type), secure, multiline, char_limit, g_req.placeholder);
}

void editbox_pump(void) {
  if (!g_req_pending) return;
  char initial[EDITBOX_TEXT_CAP], placeholder[256];
  int type, secure, multiline, limit;
  mutexLock(&g_req_lock);
  if (!g_req_pending) { mutexUnlock(&g_req_lock); return; }
  memcpy(initial, g_req.initial, sizeof initial);
  memcpy(placeholder, g_req.placeholder, sizeof placeholder);
  type = g_req.type; secure = g_req.secure; multiline = g_req.multiline; limit = g_req.limit;
  g_req_pending = 0;
  mutexUnlock(&g_req_lock);
  const int r = run_swkbd(initial, type, secure, multiline, placeholder, limit);
  if (r < 0) return;
  g_kbd_trace = 60;                          /* log the engine's follow-up JNI calls */
  kbd_push_result(g_text, g_cancelled);      /* setInputString, selection, visible=0, closed|canceled */
}

void editbox_show(const char *initial, int maxlen) {
  if (run_swkbd(initial, 0, 0, 0, NULL, maxlen) < 0) return;
  g_kbd_trace = 60;
  kbd_push_result(g_text, g_cancelled);
}

int  editbox_is_open(void) { return g_open || g_req_pending; }
const char *editbox_text(void) { return g_text; }
int  editbox_cancelled(void) { return g_cancelled; }
void editbox_close(void) {
  if (g_req_pending) {                       /* not shown yet: just drop it */
    mutexLock(&g_req_lock); g_req_pending = 0; mutexUnlock(&g_req_lock);
    debugPrintf("[kbd] hideSoftInput before the keyboard was shown: request dropped\n");
  }
}
void editbox_mark_engine_driven(void) { g_engine_driven = 1; }
int  editbox_engine_drives(void)      { return g_engine_driven; }
