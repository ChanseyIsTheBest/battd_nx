/* ---------------------------------------------------------------------------
 * firebase_stub.c -- minimal in-process replacement for the Firebase native
 * libs (libFirebaseCppApp / Analytics / Messaging / RemoteConfig).
 *
 * WHY THIS EXISTS
 * ---------------
 * The game's first scene runs a FirebaseManager whose bootstrap (StartInitializer
 * InitState::FirebaseLoading) will not advance until the managed dependency check
 * resolves to DependencyStatus.Available (== 0). On a real Android phone that is
 * answered by Google Play Services; on a Switch there is no Play Services, so even
 * a *perfectly working* libFirebaseCppApp would return UnavailableMissing (3) and
 * the game would hang at the exact same gate. The real .so files therefore cannot
 * ever reach the title screen here -- the only thing that works is to report
 * "Available", which is something only a stub can do.
 *
 * On top of that, the real libFirebaseCppApp detonates on load anyway: its
 * JNI_OnLoad logs via firebase::LogDebug through a GOT slot our loader leaves as
 * heap garbage (Instruction Abort at boot). So the real libs are pure liability.
 *
 * HOW IT WORKS
 * ------------
 * il2cpp resolves every [DllImport("FirebaseCppApp"/...)] P/Invoke through our
 * fake dlopen()/dlsym() (libc_shim.c). With the real .so files removed, dlsym for
 * any "Firebase_<module>_CSharp_*" / "SWIGRegister*" symbol lands here. We hand
 * back one of two trivial C stubs:
 *   - fb_stub_handle(): returns a non-null pointer into a shared zeroed buffer,
 *     so SWIG proxy objects (FirebaseApp, AppOptions, RemoteConfig, Future, ...)
 *     are non-null and the managed null-cPtr guards don't throw.
 *   - fb_stub_zero():   returns 0, for everything else (status ints, bools, void,
 *     getters that yield scalars).
 * Both ignore their arguments; on AArch64 a fixed-arity C function called with a
 * larger/var arg list is harmless (extra args sit in unread registers).
 *
 * The managed FirebaseApp.CheckDependencies path is synchronous (Task.Run over
 * CheckDependencies(), no Future polling), and returns Available(0) as long as a
 * FirebaseApp instance exists and nothing throws -- which our non-null
 * CreateInternal handle satisfies. Firebase here is cosmetic-only (RemoteConfig
 * banner/news textures); zeroed results just mean those images don't appear.
 *
 * Every lookup is logged so the run's debug.log enumerates exactly which Firebase
 * symbols the game asked for and which stub class we returned -- that's the tuning
 * signal if a specific call needs a different value.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"   /* debugPrintf */

/* Shared, zero-filled backing store handed out for every "object handle" the
 * SWIG layer expects. Big enough that any field reads SWIG/managed performs on a
 * proxy land on zeroed memory rather than faulting. One buffer for all types is
 * fine: our delete/dispose stubs are no-ops, so nothing is freed or aliased in a
 * way that matters. */
static uint8_t g_fb_obj[1024] __attribute__((aligned(16)));

static long  fb_stub_zero(void)   { return 0; }
static void *fb_stub_handle(void) { return g_fb_obj; }

/* The Firebase default app name constant (firebase::kDefaultAppName). The SWIG
 * string-returning name getters must hand back this exact, non-null C string:
 * the managed FirebaseApp uses it as the key into nameToProxy, and a null there
 * throws inside GetInstance() during the dependency check (faulting the task and
 * hanging the FirebaseLoading gate).
 *
 * CRITICAL: it must be a HEAP allocation, not a static. SWIG's C# string-return
 * marshaling frees the returned char* (real Firebase returns a freshly allocated
 * copy from its SWIG string helper). free() on a static/rodata pointer walks a
 * bogus malloc chunk header and faults (observed: Data Abort at 0x0 inside
 * CreateInternal's string marshal). Hand out a fresh malloc'd copy each call so
 * the marshaler's free() is valid; the getter is called rarely, so the worst
 * case (if a given call site does not free) is a few leaked bytes. */
static void *fb_stub_default_name(void) {
  static const char name[] = "__FIRAPP_DEFAULT";
  char *p = (char *)malloc(sizeof(name));
  if (p) memcpy(p, name, sizeof(name));
  return p;
}

/* Does this dlsym name belong to the Firebase SWIG surface we're replacing? */
static int fb_is_firebase_symbol(const char *s) {
  if (!s) return 0;

  /* ENABLED FOR BOUNCEMASTERS.
   *
   * cloverpit_nx hard-disabled this resolver with an unconditional `return 0`,
   * because CloverPit ships no Firebase at all -- `firebase` and `_CSharp_`
   * both occur zero times in its libil2cpp.so, so the matcher was dead code and
   * an active hazard: the `_CSharp_` test below is a generic SWIG marker, not a
   * Firebase-specific one, and any other SWIG-wrapped plugin would silently get
   * a stub returning 0 instead of an honest dlsym failure.
   *
   * Bouncemasters is the opposite case, and the numbers are not close:
   *
   *     "firebase"  in libil2cpp.so : 114 occurrences
   *     "_CSharp_"  in libil2cpp.so : 108 occurrences
   *
   * plus Firebase.App, Firebase.Crashlytics, Firebase.RemoteConfig,
   * Firebase.Platform and Firebase.TaskExtension in ScriptingAssemblies.json,
   * and libFirebaseCppApp-12_10_1.so / libFirebaseCppCrashlytics.so /
   * libFirebaseCppRemoteConfig.so in the APK. The managed SDK is live and its
   * SWIG P/Invokes must resolve to something or the dependency check never
   * reports Available and the boot gate never opens.
   *
   * The generic-SWIG-marker hazard is real but acceptable here: the only other
   * SWIG surface in this build is Crashlytics (CrashlyticsInternalPINVOKE),
   * which bp_sdk_stubs.c claims first and answers with hard zeros -- deliberately,
   * so it never gets a live handle it could register signal handlers through.
   * Anything reaching this resolver is Firebase's.
   *
   * Re-enabled by deleting cloverpit_nx's `return 0;`. */
  if (strstr(s, "_CSharp_"))                 return 1;  /* Firebase_<Mod>_CSharp_* */
  if (!strncmp(s, "Firebase_", 9))           return 1;
  if (!strncmp(s, "SWIGRegister", 12))       return 1;  /* exception/string cb reg */
  if (!strncmp(s, "SWIG", 4) && strstr(s, "Firebase")) return 1;
  return 0;
}

/* A symbol whose managed return is an object/handle/pointer (must be non-null so
 * the proxy's cPtr guard passes). SWIG factory/accessor naming conventions:
 *   new_X, X_CreateInternal, ...Create..., ...GetInstance..., DefaultInstance,
 *   GetReference..., ...Future... (future handles), ..._SWIGUpcast (base ptr). */
static int fb_returns_handle(const char *s) {
  /* Future completion pollers must read as 0: kFutureStatusComplete == 0 and
   * "no error" == 0. Returning a non-null pointer here would make any Task that
   * polls a Future (RemoteConfig/Messaging fetches) spin forever. Catch these
   * before the "Future" handle rule below. */
  if (strstr(s, "GetStatus"))      return 0;
  if (strstr(s, "GetError"))       return 0;
  /* The SWIG Future surface uses lowercase property getters: FutureBase_status()
   * must read kFutureStatusComplete(0) and FutureBase_error() "no error"(0), or
   * the Future->Task bridge polls forever (e.g. RemoteConfig SetDefaultsAsync).
   * These contain "Future" so must be caught before the "Future"->handle rule. */
  if (strstr(s, "FutureBase_status")) return 0;
  if (strstr(s, "FutureBase_error"))  return 0;   /* incl. error_message: null is fine when error==0 */
  if (strstr(s, "new_"))            return 1;
  if (strstr(s, "Create"))         return 1;   /* CreateInternal / Create__SWIG_* */
  if (strstr(s, "GetInstance"))    return 1;
  if (strstr(s, "DefaultInstance"))return 1;
  if (strstr(s, "Instance"))       return 1;   /* *_Instance, GetInstanceInternal */
  if (strstr(s, "App_get"))        return 1;   /* RemoteConfig/Messaging .App -> the app object */
  if (strstr(s, "GetReference"))   return 1;
  if (strstr(s, "Future"))         return 1;   /* future handle objects */
  if (strstr(s, "SWIGUpcast"))     return 1;   /* base-class pointer cast */
  if (strstr(s, "GetTask"))        return 1;

  /* FUTURE-RETURNING METHODS THAT NAME NEITHER "Future" NOR "Async".
   *
   * This is what stopped the boot at the splash. Firebase's SWIG layer names
   * the RemoteConfig calls plainly:
   *
   *     FirebaseRemoteConfigInternal_SetDefaultsInternal
   *     FirebaseRemoteConfigInternal_EnsureInitialized
   *     FirebaseRemoteConfigInternal_Activate
   *
   * Each returns a native Future that C# wraps as `new FutureVoid(cPtr, true)`.
   * None matches any rule above, so all three fell through to zero -- and a
   * FutureVoid built on a null cPtr throws the moment it is touched:
   *
   *     ArgumentNullException: Value cannot be null.
   *     Parameter name: Object is disposed
   *       at Firebase.FutureVoid.ThrowIfDisposed ()
   *
   * which killed the loading coroutine with the log reading
   * "Game loading progress : 1" and "Config: Set Defaults" immediately above.
   * The game had finished loading; it died on the next line.
   *
   * Listed explicitly rather than matched on "Internal": most *Internal methods
   * return void or a scalar, and handing those a pointer would be its own bug.
   */
  if (strstr(s, "SetDefaultsInternal"))  return 1;
  if (strstr(s, "EnsureInitialized"))    return 1;
  if (strstr(s, "_Activate"))            return 1;
  if (strstr(s, "FetchAndActivate"))     return 1;
  if (strstr(s, "_Fetch"))               return 1;
  if (strstr(s, "Async"))                return 1;   /* any future-returning Async */
  /* Firebase Unity 7.2.0 (this game): FixAndroidDependencies returns the native
   * Future behind FirebaseApp.FixDependenciesAsync(). It names neither "Future"
   * nor "Async", so it fell through to 0 -- and a FutureVoid built on a null
   * cPtr throws "Object is disposed" on first touch (bouncemasters §30). It is
   * only reached if CheckAndroidDependencies is not Available(0), which ours
   * always is -- but a path that is merely unlikely should not be fatal. */
  if (strstr(s, "FixAndroidDependencies"))  return 1;

  /* DERIVED, NOT GUESSED.
   *
   * The rules above grew one crash at a time. Extracting every Firebase SWIG
   * P/Invoke from dump.cs with its C# return type shows 24 of 95 are declared
   * `IntPtr`, and every one of those MUST come back non-null: the C# wrapper
   * builds a SWIG proxy around the pointer and throws on first touch when it is
   * zero, which is exactly how SetDefaultsInternal killed the boot.
   *
   * Six were still classified zero after the hand-written rules. They are named
   * here rather than pattern-matched, because "GetInfo" and "data_get" are the
   * kind of substrings that would sweep up unrelated scalar getters:
   *
   *   FirebaseRemoteConfigInternal_GetInfo         -> ConfigInfo proxy
   *   FirebaseRemoteConfigInternal_GetKeys         -> StringList proxy
   *   FirebaseRemoteConfigInternal_GetValueInternal-> ConfigValue proxy
   *   ConfigValueInternal_data_get                 -> CharVector proxy
   *   StackFrames_getitem                          -> frame proxy
   *   StringStringMap_create_iterator_begin        -> iterator proxy
   *
   * The empty-collection case is still correct: these hand back the shared
   * zeroed buffer, and the size/count accessors report 0, so C# sees a valid
   * empty StringList rather than a null one.
   *
   * Re-derive with:
   *   grep -oE 'static extern IntPtr \w*(Firebase|Future|Config|String)\w*\(' dump.cs
   */
  if (strstr(s, "_GetInfo"))                    return 1;
  if (strstr(s, "_GetKeys"))                    return 1;   /* incl. GetKeysByPrefix */
  if (strstr(s, "GetValueInternal"))            return 1;
  if (strstr(s, "ConfigValueInternal_data_get"))return 1;
  if (strstr(s, "StackFrames_getitem"))         return 1;
  if (strstr(s, "create_iterator_begin"))       return 1;
  return 0;
}

/* --------------------------------------------------------------------------
 * Future completion
 *
 * A non-null handle stops ThrowIfDisposed, but it does not complete the Task.
 * Firebase's C# bridge does not poll -- FutureVoid.GetTask() registers a
 * delegate and waits. A no-op registration means the Task never completes and
 * the awaiting coroutine parks forever: the null-pointer crash becomes a silent
 * hang, which is worse.
 *
 * THE SIGNATURE, READ FROM THE DUMP RATHER THAN ASSUMED:
 *
 *     IntPtr FutureVoid_SWIG_OnCompletion(HandleRef future,
 *                                         SWIG_CompletionDelegate cb,
 *                                         int key)
 *     private static void SWIG_CompletionDispatcher(int key)
 *
 * So the callback takes an INT KEY, not (future, user_data). A first attempt
 * here called `cb(future, user_data)`, which on AArch64 would have put a
 * pointer in w0 where the dispatcher expects a small integer key -- dispatching
 * against a garbage key, in the middle of the boot sequence. Worth stating,
 * because "stub returns nothing" and "stub calls the right function wrongly"
 * fail very differently and only one of them is obvious in a log.
 *
 * WHY DEFERRED RATHER THAN IMMEDIATE
 *
 * Firing from inside SWIG_OnCompletion would re-enter managed code before that
 * call has returned, and the C# side registers `key` in its dispatch table
 * around this call. Dispatching a key that is not in the table yet does
 * nothing at best. So completions are queued and fired from
 * firebase_stub_pump(), once per frame, by which point registration is
 * certainly finished. One frame of latency for a future that was always going
 * to be "already complete" is not a cost worth avoiding.
 *
 * The return value is an IntPtr the C# side keeps and later hands to
 * SWIG_FreeCompletionData. A non-null value is returned so that path has
 * something valid to release; our FreeCompletionData stub ignores it.
 * ------------------------------------------------------------------------ */

typedef void (*fb_completion_cb)(int key);

#define FB_MAX_PENDING 32
static struct { fb_completion_cb cb; int key; } g_fb_pending[FB_MAX_PENDING];
static int g_fb_pending_n;

static void *fb_stub_on_completion(void *future, void *cb, int key) {
  (void)future;
  if (!cb) return (void *)g_fb_obj;
  if (g_fb_pending_n < FB_MAX_PENDING) {
    g_fb_pending[g_fb_pending_n].cb  = (fb_completion_cb)cb;
    g_fb_pending[g_fb_pending_n].key = key;
    g_fb_pending_n++;
    debugPrintf("[fb-stub] queued Future completion key=%d (%d pending)\n",
                key, g_fb_pending_n);
  } else {
    debugPrintf("[fb-stub] completion queue FULL, dropping key=%d -- a Task "
                "will hang. Raise FB_MAX_PENDING.\n", key);
  }
  return (void *)g_fb_obj;
}

/* Call once per frame, from the render loop. */
void firebase_stub_pump(void) {
  while (g_fb_pending_n > 0) {
    /* Take a copy and clear the slot BEFORE dispatching: the callback runs
     * managed code that can register further completions, and re-entering this
     * queue mid-iteration would either lose them or double-fire. */
    fb_completion_cb cb = g_fb_pending[0].cb;
    int              key = g_fb_pending[0].key;
    for (int i = 1; i < g_fb_pending_n; i++) g_fb_pending[i - 1] = g_fb_pending[i];
    g_fb_pending_n--;

    debugPrintf("[fb-stub] completing Future key=%d\n", key);
    cb(key);
  }
}

void *firebase_stub_lookup(const char *symbol) {
  if (!fb_is_firebase_symbol(symbol)) return NULL;
  /* App-identity name getters must return the non-null default-app name string,
   * not 0/null -- see fb_stub_default_name above. Covers DefaultName_get,
   * NameInternal_get and the FirebaseApp Name getter. */
  if (strstr(symbol, "DefaultName") ||
      strstr(symbol, "NameInternal") ||
      strstr(symbol, "_Name_get")    ||
      strstr(symbol, "get_Name")) {
    debugPrintf("[fb-stub] %s -> \"__FIRAPP_DEFAULT\"\n", symbol);
    return (void *)&fb_stub_default_name;
  }
  /* Completion registration must actually complete the Task -- see above. */
  if (strstr(symbol, "SWIG_OnCompletion")) {
    debugPrintf("[fb-stub] %s -> queue-and-complete\n", symbol);
    return (void *)&fb_stub_on_completion;
  }

  if (fb_returns_handle(symbol)) {
    debugPrintf("[fb-stub] %s -> handle\n", symbol);
    return (void *)&fb_stub_handle;
  }
  debugPrintf("[fb-stub] %s -> 0\n", symbol);
  return (void *)&fb_stub_zero;
}
