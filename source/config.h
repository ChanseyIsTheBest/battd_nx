/* ---------------------------------------------------------------------------
 * config.h -- build-time switches for the Bloons Adventure Time TD loader.
 *
 * Game:   com.ninjakiwi.btdadventuretime 1.7.7 (2823), built 2025-08-26
 * Engine: Unity 2020.3.40f1 (ba48d4efcef1), IL2CPP, metadata v27, arm64-v8a
 *
 * WHY THE bp_ PREFIX SURVIVES
 * This tree is bloonspop_nx retargeted, which is itself bouncemasters_nx
 * retargeted, back to the Vita/Switch so-loader tradition. Every port in the
 * lineage keeps the prefix it inherited so a diff against its parent stays
 * readable -- that is how a fix found in one port reaches the others. The prefix
 * means "base port", not "Bloons Pop". Only GAME IDENTITY was rewritten, because
 * leaving a donor's identity behind is a real bug class: bloonspop_nx shipped a
 * build that reported CLONE HERO's package name to Unity and to the publisher's
 * servers, because it adopted a JNI layer and never re-read those strings.
 * ------------------------------------------------------------------------- */
#ifndef BP_CONFIG_H
#define BP_CONFIG_H

/* ---- diagnostics --------------------------------------------------------
 * DEBUG_LOG 1 writes debug.log next to the .nro and enables every diagnostic
 * (module map, JNI ledger, patch/hook results, crash dumper, save-password
 * probe). At 0 debugPrintf is a no-op: no SD traffic and no log lock anywhere
 * in the frame loop.
 *
 * Turn it back to 1 before reporting a problem. It is the only instrument this
 * port has, and every diagnosis in PORTING.md came out of it. */
#define DEBUG_LOG        0

/* CRASH_DUMP is deliberately NOT tied to DEBUG_LOG.
 *
 * A release build wants no debug.log and no per-frame instruments, but it
 * still wants a crash report -- "it crashed and there is nothing to read" is
 * the worst possible state to ship, and it is the state a DEBUG_LOG 0 build
 * was in. crash.log costs nothing until a thread actually faults. */
#define CRASH_DUMP       0
#define DEBUG_JNI_TRACE  0
#define TRACE_IO         0
#define TRACE_MMAP       0

/* ---- game files (the folder name is resolved at runtime) --------------- */
#define BP_LIB_MAIN   "libmain.so"
#define BP_LIB_UNITY  "libunity.so"
#define BP_LIB_IL2CPP "libil2cpp.so"
#define BP_ASSET_DIR  "assets"
#define GAME_HOME     "sdmc:/switch/battd"
#define LOG_NAME      "sdmc:/switch/battd/debug.log"

/* ---- application identity ------------------------------------------------
 * Read from assets/crashlytics-build.properties and PlayerSettings. SDK and
 * service init compare the package name against their own config, and Unity
 * derives persistentDataPath from it, so these are not cosmetic. 2020.3 ships
 * no assets/bin/Data/unity_app_guid; storage follows the package name. */
#define CS_PACKAGE       "com.ninjakiwi.btdadventuretime"
#define CS_VERSION_NAME  "1.7.7"
#define CS_VERSION_CODE  2823
#define CS_APP_GUID      ""

/* ---------------------------------------------------------------------------
 * PRESENTATION: LANDSCAPE, NO ROTATION
 *
 * The single biggest difference from bloonspop_nx. Bloons Pop was portrait-only
 * and needed clayjam_nx's render-to-texture TATE layer. BATTD's PlayerSettings
 * allow LandscapeLeft and LandscapeRight and forbid both portrait orientations,
 * with a 1280x720 default -- exactly the handheld panel. So the engine renders
 * straight into the real swapchain at 1:1 and the rotation layer switches off.
 *
 * bp_tate.c/.h and bp_tate_glue.c are KEPT, not deleted. With BP_TATE_ENABLE 0
 * every entry point is already inert by construction:
 *   bp_tate_enabled()      -> 0, so eglQuerySurface reports the real surface
 *   bp_tate_swap_hook()    -> compiles to nothing
 *   bp_tate_bind_overlay() -> no-op (bp_tate_active() false; init never runs)
 *   bp_tate_map_stick()    -> identity
 *   bp_tate_map_pointer()  -> plain panel->render scale, which is 1:1 here
 *   bp_gl_BindFramebuffer_tate() -> passes fb 0 through untouched
 * Deleting them would mean editing seven call sites across imports.c,
 * nx_pointer.c and android_native_unity.c to chase a few hundred bytes of dead
 * code, in a tree that cannot be compiled here. Leaving them costs nothing.
 * ------------------------------------------------------------------------ */
#define BP_TATE_ENABLE   0
#define BP_TATE_ROT      0
#define BP_TATE_LINEAR   0

/* Landscape resolution from config.txt at boot (bp_config.c): 1280x720 up to
 * 1920x1080, snapped to a 16:9 size. Render and window are the SAME buffer. */
extern int bp_res_w, bp_res_h, bp_portrait_rot;   /* rot unused: landscape */
/* config.txt ui_scale / memory_mb -- see bp_screen.c for what they select. */
extern float bp_ui_dpi;
extern int   bp_memory_mb;
#define BP_RENDER_W      bp_res_w
#define BP_RENDER_H      bp_res_h
#define BP_WINDOW_W      bp_res_w     /* landscape: window == render */
#define BP_WINDOW_H      bp_res_h
#define BP_FORCE_SCREEN_W BP_RENDER_W
#define BP_FORCE_SCREEN_H BP_RENDER_H
extern int screen_width;
extern int screen_height;

/* ---- rendering ------------------------------------------------------------
 * GLES via mesa/nouveau; libc_shim.c's dlopen refuse list keeps Vulkan off. */
#define BP_FORCE_GLES   1
#define BP_PATCH_VSYNC  1             /* start the vsync pump (bp_vsync.c) */
#define BP_VSYNC_PERIOD_NS 16666667ull
#define BP_RENDER_FALSE_EXIT_FRAMES 600u

/* ---------------------------------------------------------------------------
 * NETWORK -- required, not optional, for this game
 *
 * BATTD ships base bundles inside the APK (assets/Bundles/Android/, with
 * __nk_manifest__.json) but still fetches content over HTTPS into Unity's own
 * cache at files/UnityCache/. It uses UnityWebRequest + AssetBundle caching
 * directly; there is NO Addressables catalog in this build (0 references in
 * dump.cs), so none of bloonspop_nx's aa/ handling applies.
 *
 * bp_net_shim.c is a real bionic-ABI socket layer; bp_net.c brings up BSD
 * sockets and nifm, answers Application.internetReachability, and installs a CA
 * bundle into unitytls' default list so HTTPS verifies.
 * ------------------------------------------------------------------------ */
#define BP_NET_ENABLE          1
#define BP_NET_BSD_SESSIONS    8
/* libnx's default sb_efficiency (4) gives ~2 MB of transfer memory and each
 * downloading TCP socket grows toward 256 KB of receive buffer, so a handful of
 * concurrent transfers exhausts the pool and new connections are refused. 8
 * doubles it; bp_net_init() falls back to the default config if refused. */
#define BP_NET_SB_EFFICIENCY   8
/* socketpair(): 0 = in-process pipe pair. Its only callers are curl's resolver
 * signal and multi wake-up pair, both one-way. The loopback-TCP form churns a
 * socket slot and a TIME_WAIT entry per DNS lookup and is what ran the socket
 * service out of resources in bloonspop_nx's ninth run. */
#define BP_NET_SOCKETPAIR_LOOPBACK 0
#define BP_NET_REACH_CACHE_MS  1000
#define BP_NET_BLOCK_ADS       1   /* ad/attribution/telemetry hosts -> no such host */
#define BP_NET_TRACE           0
#define BP_NET_CONNECT_TIMEOUT_MS 0   /* 0 = acpc_nx's proven blocking connect */

/* ---------------------------------------------------------------------------
 * OFFLINE-WHEN-CACHED
 *
 * The internet is switched off once the cache has SETTLED -- once a launch
 * finds it byte-for-byte what the launch before found, which means the previous
 * session downloaded nothing and there is nothing left to fetch. Offline means
 * internetReachability reports NotReachable, getaddrinfo fails every lookup,
 * and connect refuses anything but loopback.
 *
 * This replaced a baked-in manifest of content hashes. That manifest went stale
 * the moment the game cached a different version of anything, and the only cure
 * was pulling the SD card and re-running a tool -- so the port sat online long
 * after everything had in fact been downloaded, with nothing to tell the player
 * why. The settled test needs no tooling and converges by itself: a launch that
 * downloads something comes up online, the next downloads nothing, and the one
 * after that goes offline. The fingerprint counts __data entries and bytes, so
 * Unity rewriting its __info bookkeeping does not disturb it.
 *
 * An empty file <root>/force_online overrides it for a launch. Regenerate the
 * manifest after a content change:
 *     python3 tools/gen_cache_manifest.py <sd>/switch/battd/files/UnityCache --manifest
 *
 * (An earlier revision documented a BP_CACHE_MIN_BUNDLES "count mode" here.
 * bp_net.c never read it -- the setting did nothing and the comment described
 * behaviour that did not exist. Removed rather than left to mislead.)
 * ------------------------------------------------------------------------ */
#define BP_OFFLINE_WHEN_CACHED 1

/* config.txt "online" chooses what happens ONCE the cache is complete:
 *   online = false  (default)  stay offline and play from the cache
 *   online = true              keep the internet on, so content updates arrive
 *
 * An earlier revision defaulted this to true, on the theory that the game needed
 * a server round-trip to resolve its DLC and that offline was therefore
 * destructive. That was wrong. The bundles were being corrupted by a fortified
 * read bypassing the read-ahead layer (see __read_chk_fake in libc_shim.c);
 * Unity then re-fetched and evicted them, and offline mode took the blame.
 * It has NO EFFECT until the cache has settled -- the first run has to
 * download, and a setting that could block that would just look like a broken
 * game. Until then the port is online regardless, and the log says so. */
extern int bp_allow_online;

/* config.txt "mt_sample": the [mt] managed-frame sampler in diag.c.
 *
 * DEFAULTS OFF. It pauses UnityMain, grabs its context, resumes it, then walks
 * 16 KB of a stack that is live again while it walks -- and emits each frame to
 * BOTH logs, so every line takes g_stall_lock and g_log_lock in turn. Two boots
 * in a row ended with the watchdog stopping partway through one of these dumps
 * and never polling again, at the same point in the splash. That is not proof
 * the sampler is at fault; it is the reason to be able to take it out of the
 * picture in one line rather than argue about it. Set to 1 to turn it back on. */
extern int bp_mt_sample;

/* config.txt "ram_delay_ms": a timing experiment, default 0 (off).
 *
 * The syscall sequence on an evicted bundle is IDENTICAL between the RAM path
 * and the card path for 293 ops -- same reads, same seeks, same positions,
 * verified same bytes -- and then Unity closes the file on RAM and keeps
 * loading on the card. The one input that differs is time: the card spends
 * seconds on an 11 MB bundle and the RAM path spends microseconds, so on RAM
 * every bundle is in flight at once and finishes before the splash. When set,
 * the RAM path sleeps this long before answering the FIRST read after each
 * open of a cache entry. If that alone stops the eviction, Unity is reacting
 * to speed and the next question is what it does in that window. */
extern int bp_ram_delay_ms;

/* config.txt "ram_skip_tail": 1 = leave bundles whose blocks-info is at the
 * END of the file (the three DLC bundles Unity kept rejecting when served from
 * the old per-file blob store) on the card. 0 = make them resident too, from
 * the single immutable image. Default 0: the image is the fix under test, and
 * this is the switch back to the known-working state if it is not. */
extern int bp_ram_skip_tail;

/* config.txt "diag_io": 0 (default) = only the instruments that write nothing
 * until they fire. 1 = the full set: the per-close syscall op log to io.log
 * with a commit per block, the whole-blob CRC at close, the 2 s watchdog
 * beacon with a commit each, the 1 Hz debug.log flush. The full set was on
 * for every run that died at the map-scene load, and it is a plausible cause
 * of the console going down: dozens of fsFsCommit calls in the burst where
 * the game loads its largest scene. */
extern int bp_diag_io;

/* config.txt "mmap_arena_mb": the anonymous-mmap arena carved from the heap at
 * first use (default 1408).
 *
 * BIGGER IS NOT BETTER, and the measurements say so plainly:
 *
 *     arena   fallback   total   outcome
 *       640       1231    1871   ran 30300 frames
 *      1280        292    1572   ran fine
 *      1792        142    1934   memalign FAILED at frame 1140
 *
 * Two things make this counter-intuitive:
 *
 * 1. THE ARENA IS A RATCHET. "now" equals "peak" in every [mem] sample ever
 *    taken -- it never releases a page. So it fills to whatever capacity it is
 *    given, and the total is always arena + whatever still spills over. Giving
 *    it more does not reduce Unity's demand, it just moves where the memory
 *    sits and raises the total.
 *
 * 2. THE ARENA IS COMMITTED IN FULL, UP FRONT, at the first big mmap. The
 *    fallback path grows on demand. So a large arena starves newlib of the
 *    contiguous space the fallback needs -- and the fallback allocates
 *    memalign(64 MB, 128 MB), which is the largest single request in the
 *    process. At 1792 that memalign failed outright; the GPU arena's reserve
 *    rescued one and the next one had nowhere to go.
 *
 * BUT THE PEAK IS NOT THE SAME EACH BOOT. Two consecutive boots of the same
 * build doing the same thing (title screen -> level) asked for 16 and 17 maps
 * and peaked at 1161 and 1212 MB. Whatever varies -- allocation order, address
 * layout -- it moves the peak by ~50 MB, which is enough to cross a tight
 * ceiling and start spilling. So leave a margin for the variance on top of the
 * highest peak seen, rather than sizing to the best run.
 *
 * Size this to the minimum that keeps the spill small, not to Unity's total
 * appetite. Watch "ARENA FULL" together with the [mem] fallback figure: a few
 * spills are fine, a failed memalign is not. */
extern int bp_mmap_arena_mb;

/* config.txt "ram_max_file_mb": cache bundles larger than this stay on the
 * card (default 16). The point of residency is the small per-character
 * bundles the game streams mid-play; shared_stuff at 118 MB is loaded once,
 * behind a loading screen, and holding it costs a sixth of what Unity has
 * left. 0 = no limit. */
extern int bp_ram_max_file_mb;

/* config.txt "gpu_arena_mb": the contiguous slab nouveau's 64KB..64MB buffers
 * come from (default 430 -- 90 MB, ~26%, over the measured peak).
 *
 * SIZE THIS FROM A LONG SESSION, NOT A SHORT ONE. The peak climbs with
 * playtime: 302 MB at 21k frames, 340 MB at 30k, but only 191 MB in a run that
 * stopped at 1.7k. Sizing against that short run gave 320, which is BELOW the
 * real figure and would have wedged the compositor.
 *
 * Undersizing is not a degradation. nouveau_bo_new returns NULL, GL reports
 * GL_OUT_OF_MEMORY, the framebuffer comes back incomplete and the console goes
 * down -- a hard crash, no crash report. Overshooting just costs heap, and
 * there is headroom. So this keeps a deliberately generous margin: 448 is
 * ~32% over the measured 340.
 *
 * All of that was measured at 720p. A higher "resolution" makes every render
 * target bigger, so raise this alongside it and watch the [mem] line's peak.
 * Floor is 96. */
extern int bp_gpu_arena_mb;

/* ---- TLS ------------------------------------------------------------------
 * BP_TLS_VERIFY_DIAG logs refused chains (flags, subject/issuer per cert) and
 * saves them as DER under <root>/tls/.
 * BP_TLS_INSECURE_FALLBACK accepts chains refused ONLY as NOT_TRUSTED; hostname
 * and dates are still checked. bloonspop_nx turned this on because mbedtls 2.x
 * rejects Let's Encrypt's 2025/26 cross-signed hierarchy even with ISRG Root X1
 * installed, and that is the same CA estate this CDN sits behind.
 *
 * READ THIS BEFORE LEAVING IT ON. BATTD has a Ninja Kiwi LiNK account path
 * (Ninjakiwi.LiNK.dll) and in-app purchases. With the fallback on, anyone on
 * your network can impersonate the server. It is 1 here because the first
 * bring-up target is getting content to download at all; set it to 0 before you
 * sign in to anything. */
#define BP_TLS_VERIFY_DIAG       1
#define BP_TLS_INSECURE_FALLBACK 1

/* ---- audio (opensles.c + the FMOD pump) -----------------------------------
 * BP_AUDIO_PERIOD_FRAMES and BP_AUDIO_UPFRONT_BUFFERS are the two values the
 * FMOD patch writes into the engine (bp_patches.c). The game's own AudioManager
 * asks for a 512-frame DSP buffer at the system rate; 256 is the lineage's
 * known-good period and what the patch forces. */
#define BP_AUDIO_DEVICE_RATE       24000
#define BP_AUDIO_FRAMES_PER_BUFFER 64
#define BP_AUDIO_CALLBACK_FRAMES   1024
#define BP_AUDIO_PERIOD_FRAMES     256
#define BP_AUDIO_UPFRONT_BUFFERS   4

/* ---- input ----------------------------------------------------------------
 * nx_pointer owns the touch panel, the stick/gyro cursor and USB mice. BATTD
 * uses legacy UnityEngine.Input and uGUI (its assemblies include
 * UnityEngine.InputLegacyModule and no Unity.InputSystem), so the Input icall
 * hooks are the correct layer. */
#define BP_ENABLE_POINTER_INPUT 1
#define BP_LOG_BUTTONS          0

/* ---- memory ---------------------------------------------------------------
 * MMAP_ARENA_ALIGN MUST equal BP_REGION_GRANULARITY_MB (bp_patch_granularity.h).
 * 64 MB is the lineage's known-good floor; 16 MB corrupted the Dynamic Heap.
 * libunity 18 MB + libil2cpp 48 MB mapped here. */
#define MMAP_ARENA_ALIGN ((size_t)64 * 1024 * 1024)
#ifndef LOAD_ADDRESS
#define LOAD_ADDRESS 0xC0000000
#endif
/* ---------------------------------------------------------------------------
 * RAM CACHE
 *
 * The console grants this port about 2.9 GB of newlib heap and the whole game
 * is far smaller, so the files it reads repeatedly are held in memory:
 *
 *   assets.nxpack   ~83 MB   every asset read becomes a memcpy, and the global
 *                            pack I/O mutex drops out of the hot path entirely
 *   UnityCache      ~244 MB  each AssetBundle is read from the card once per
 *                            boot no matter how often Unity opens and closes it
 *
 * Both draw on ONE budget so they cannot between them overrun it. Anything that
 * does not fit falls back to the 1 MB read-ahead window, which is what the port
 * did before -- residency is an optimisation, never a requirement.
 *
 * config.txt "ram_cache" overrides the megabyte figure at runtime; 0 turns
 * residency off. BP_RAM_RESIDENT_MAX_MB caps any single file, so one unexpected
 * giant file cannot swallow the whole budget. */
/* 512, because everything is loaded at boot now and everything is 326 MB:
 * 83 MB of packed assets plus 243 MB of downloaded content. At 256 the boot
 * load would stop two thirds of the way through and the rest would be read from
 * the card all session, which is the situation this replaced. */
#define BP_RAM_CACHE_MB         512

/* ---------------------------------------------------------------------------
 * VERIFY EVERY CACHED READ AGAINST THE FILE
 *
 * Five rounds of this cache have each ended the same way: a real bug found, a
 * fix that looked sound, and something else broken on the next hardware run.
 * The reason is that a wrong cached read is INVISIBLE -- it does not fault or
 * return an error, it returns plausible bytes, and the first symptom is Unity
 * deciding an AssetBundle is corrupt several seconds later. Every diagnosis so
 * far has been reconstructed backwards from that, and most were wrong.
 *
 * With this on, every read served from RAM is compared against the same range
 * read from the real file. A mismatch is reported with the descriptor, the
 * offset, the length and the first differing byte -- which turns "Unity says
 * the bundle is corrupt" into "this read at this offset returned these bytes
 * instead of those". It costs a second read per cached read, so it is a
 * debugging aid rather than a shipping setting, but it is the only way to stop
 * guessing.
 *
 * BACK ON, and this time it can actually prove something. It was switched off
 * after reporting zero failures -- but that was while the fb_key bug had the
 * cache serving 1-2% of reads, so it verified almost nothing. The hit rate is
 * now 100%, and an A/B says the game evicts entries with the cache on and does
 * not with it off. Either the bytes served differ from the file, or they do
 * not and the cause is elsewhere; this answers that in one run instead of
 * another round of theories.
 *
 * Set to 0 once the cache is proven. */
/* TWO CHECKS, TWO SWITCHES. They were one, and that hid a cost.
 *
 * BP_RAM_VERIFY       -- at load, on the prefetch thread, once per file: the
 *                        whole blob compared against the card before anything
 *                        can read it. Cheap, off the game's threads, and the
 *                        one that catches a bad load. Stays ON.
 *
 * BP_RAM_VERIFY_READS -- per READ, on the game's own thread: re-read the same
 *                        range from the card and compare. This is the one the
 *                        comment below was written about, and it was left at 1.
 *                        It makes every RAM-served read ALSO a card read, so a
 *                        cache meant to remove SD I/O from the bundle-load burst
 *                        instead doubles it there, from six Background Job
 *                        workers at once, into ONE shared static 64 KB buffer
 *                        with no lock (the reports it produced contradicted
 *                        each other against the reference archive for exactly
 *                        that reason). With ram_cache = 0 none of this runs and
 *                        the game plays through; with 512 it freezes at the end
 *                        of the splash, when that burst begins. This is the
 *                        only code on the read path that differs between those
 *                        two configurations besides a memcpy.
 *
 *                        OFF. Turn it to 1 only to re-test correctness, and
 *                        expect it to cost the boot while it is on.
 *
 * Original note, kept because it was right: "it reads the same range from the
 * file for EVERY cached read, so the cache saves no I/O at all and doubles the
 * syscall load on a card the game is already waiting on." */
#define BP_RAM_VERIFY       1
#define BP_RAM_VERIFY_READS 0
/* Per-file cap. It exists to stop one pathological file swallowing the budget,
 * NOT to exclude the biggest real one -- at 64 MB it was rejecting
 * shared_stuff (112.5 MB), which is the single most-read bundle the game has.
 * The largest observed is 112.5 MB, the whole working set is 327 MB (83 MB pack
 * + 244 MB cache), so 192 leaves room for a bundle to grow and still refuses
 * anything absurd. */
/* 64, not 192. The 112 MB shared_stuff blob fragments the heap as much as it
 * fills it, and a single allocation that large is exactly what the game was
 * later refused. Big files still get the read-ahead window. */
/* 192, so shared_stuff (112 MB) is included -- at 64 the single largest and
 * most-read bundle in the game was excluded. Holding a block that size was a
 * fragmentation risk before; there are now two escapes from a failed large
 * allocation (the cache releases everything and the GPU arena lends its
 * reserve), so it is affordable. */
#define BP_RAM_RESIDENT_MAX_MB  192
extern int bp_ram_cache_mb;      /* config.txt ram_cache, defaults to the above */

#define BP_GPU_ARENA_MB 320
#define GFX_RESERVE_MB  192u

/* ---------------------------------------------------------------------------
 * ENGINE PATCHES -- all offsets derived from the stock 2020.3.40f1 reference
 *
 * BATTD ships the UNMODIFIED Unity Android player: its libunity.so has the same
 * BuildID as the stock 2020.3.40f1 arm64 build (6d0adf1c159f63eb...) and a
 * byte-identical .text. Every RVA in bp_offsets.h was read straight off the
 * reference symbol table rather than fingerprinted, which is why this port
 * starts with all patches armed instead of bloonspop_nx's derive-then-verify
 * bring-up. Each patch still checks the game's own instruction words first and
 * SKIPS itself with a named log line on mismatch.
 * ------------------------------------------------------------------------ */

/* Swappy: androidUseSwappy is off in BATTD's PlayerSettings, so these should be
 * inert anyway. Belt and braces -- Swappy's Java choreographer thread would wait
 * for callbacks that cannot arrive. All seven entry points located by symbol. */
#define BP_PATCH_SWAPPY 1

/* Engine clock (badpiggies_nx's design, re-derived for 2020.3.40f1).
 * TimeManager::Update is detoured onto a monotonic clock. THIS BUILD'S PROLOGUE
 * IS THE 0x30 FRAME SAVING d9/d8 -- the 2020.3.39f1 shape, not 2020.3.15f2's
 * 0x20/d8-only frame -- so badpiggies_nx's trampoline transfers unchanged and
 * bloonspop_nx's does NOT. See bp_tm_trampoline.s and BP_TM_PROLOGUE. */
#define BP_TM_CLOCK          1
#define BP_TM_CLOCK_THREAD   1
/* 0 = hook the Time icalls only if the TimeManager clock could not be installed.
 * All 29 bindings exist in this build (bloonspop_nx's stripped libunity had 11);
 * the managed side uses 10. With the engine clock live they are redundant, and
 * the engine's own Time values also reach Animator, particles and
 * WaitForSeconds, which icall hooks never do. */
#define BP_TIME_ICALL_HOOKS  0

/* EnableFrameTimeTracker -> ret. Its ctor starts an Android Looper over JNI and
 * waits for a thread that cannot exist; this is the 2020.3 frame-2 deadlock. */
#define BP_PATCH_FRAMETIMETRACKER 1

/* FMOD OpenSL output: force the DSP period and up-front buffer count and defeat
 * the buffer-geometry bound check. The four patch sites sit at the SAME offsets
 * from FMOD::OutputOpenSL::init as in 2020.3.15f2 and in bouncemasters_nx --
 * verified word for word, so 2020.3's FMOD really is instruction-identical. */
#define BP_PATCH_FMOD 1

/* ---------------------------------------------------------------------------
 * SPLASH VIDEO (from cloverpit_nx)
 *
 * BATTD ships four clips in assets/: CNGamesLogo-{720,1080}24fps.mp4 and
 * NK_splash_sound{,_720}.mp4. libunity reaches UnityEngine.Video through the
 * Android NDK media API, and imports.c answers those entry points with
 * AMEDIA_ERROR_UNSUPPORTED, so every clip reports unplayable. battd_video.c
 * bypasses VideoPlayer entirely: it decodes with ffmpeg on its own thread and
 * draws YUV planes as one letterboxed quad from the eglSwapBuffers wrapper.
 *
 * SAFE TO TURN OFF. With BATTD_VIDEO 0 the hooks are never installed and the
 * intro is a black screen for its duration, then the game continues. Requires
 * switch-ffmpeg; the Makefile checks and says so. The clips must be staged --
 * tools/stage_sd.py writes videos/ and videos/manifest.txt. */
#define BATTD_VIDEO 1
#define BATTD_VIDEO_MAX_W 1280
#define BATTD_VIDEO_MAX_H 720
#define BATTD_VIDEO_DECODE_THREADS 2

/* Skip H.264's deblocking filter and allow FFmpeg's non-bit-exact shortcuts
 * (AV_CODEC_FLAG2_FAST) for the splash clips. Every clip is decoded at 1080p
 * and scaled down to 720p, and the downscale smooths away exactly the block
 * edges the filter exists to hide. Measured on the game's own clips (host
 * decode + the same 720p scale, PSNR of the displayed frame vs a normal
 * decode): 15-55% faster, 58-68 dB average, worst single frame 51.5 dB --
 * visually identical; the error that builds up on reference frames is reset
 * at each keyframe and stays small. The NK splash plays while Unity boots and
 * has run as slow as 14.2 fps against 24. Set to 0 to restore a bit-exact
 * decode. See tools/vbench.c. */
#define BATTD_VIDEO_FAST_DECODE 1
/* SplashScreenVideo drives the intro. The first attempt hooked PlayVideoNow;
 * the hardware log showed it never firing, because the real order is
 *
 *   Start() -> TryPlayNextVideoInWaterfall()
 *                -> VideoPlayer.set_url(...)
 *                -> VideoPlayer.Prepare()          <-- fails here, every time
 *                -> (on prepareCompleted) PlayVideoNow() -> Play()
 *
 * PlayVideoNow is DOWNSTREAM of Prepare succeeding -- all it does is subscribe
 * EndReached to loopPointReached and call Play. With the media backend stubbed,
 * Prepare always fails, so nothing below it runs and the engine raises
 * errorReceived instead ("VideoPlayer cannot play url").
 *
 * So hook the driver. TryPlayNextVideoInWaterfall is entered from Start (a tail
 * branch at +0x34c) and again from OnVideoError, and it owns the waterfall array
 * and index -- everything needed to choose the clip. When our decoder finishes
 * we call TriggerAnimationExit, the game's own way out of the splash: it clears
 * isPlaying and fires the exit animation. */
#define BATTD_RVA_TryPlayNextVideoInWaterfall 0x0D2999Cu
#define BATTD_RVA_TriggerAnimationExit        0x0D29C58u
/* Field offsets on SplashScreenVideo (dump.cs, TypeDefIndex 9202). */
#define BATTD_OFF_videoClipPathWaterfall 0x28u   /* string[]    */
#define BATTD_OFF_video                  0x30u   /* VideoPlayer */
#define BATTD_OFF_isPlaying              0x40u   /* bool        */
#define BATTD_OFF_currentClipIdx         0x44u   /* int32       */

/* ---------------------------------------------------------------------------
 * FORCED ART QUALITY
 *
 * VariantBootstrap.GetActiveVariants computes a tier and the tier picks the
 * asset-bundle variant. Rather than steer it indirectly through Screen.dpi and
 * the reported RAM -- which also move UI scaling and Unity's own budgeting --
 * the tier itself is forced:
 *
 *   +0x13c   bl   <tier(density, size)>   ->  movz w0, #N
 *   +0x148   b.ne <skip memory check>     ->  b <skip memory check>
 *
 * The second patch matters: with N == 2 the untouched code would fall into the
 * GetPhysicalMemoryMB() >= 1536 test and knock the tier back down to 1.
 *
 * WHY 2 IS THE TOP. On this screen (min(w,h) = 720 >= 600) the tier function
 * can only return 1 or 2, and it returns 2 for the HIGHER density
 * (csinc w0, #2, wzr, ge). The first hardware log shows tier 1 loading
 * ui/splash-variants.low, so 2 is unambiguously the better of the two.
 * config.txt art_quality overrides it without a rebuild; "auto" removes the
 * patch entirely and restores the game's own choice.
 * ------------------------------------------------------------------------ */
#define BATTD_RVA_VariantTier_Call 0x0EB1EE8u   /* bl   tier()   */
#define BATTD_RVA_VariantTier_Skip 0x0EB1EF4u   /* b.ne +0x1c    */
extern int bp_art_quality;                      /* -1 = auto, else 0..2 */

/* ---------------------------------------------------------------------------
 * JNI layer (daggerfall_nx, from clonehero_nx)
 *   LEDGER     records every JNI call the layer had to approximate.
 *   QUARANTINE holds freed local refs this deep before reuse, so a
 *              use-after-free reads a dead tag instead of another object.
 *   SDK_NULL   third-party SDK classes answer inert instead of plausible fakes.
 * ------------------------------------------------------------------------ */
/* ---------------------------------------------------------------------------
 * 2D ART QUALITY
 *
 * The game picks its asset-bundle variant in VariantBootstrap.GetActiveVariants:
 *
 *     A = min(Screen.width, Screen.height)        -- 720 here
 *     B = Screen.dpi / 160.0                      -- the Android density bucket
 *     if A >= 600:  tier = (B >= 1.5) ? 2 : 1
 *     else:         tier = (B >= 2.5) ? 2 : (B >= 1.5) ? 1 : 0
 *     if tier == 2: tier = (SystemInfo.GetPhysicalMemoryMB() >= 1536) ? 2 : 1
 *
 * Both gates were failing. The port reported the panel's true 237 dpi, which is
 * density 1.48125 -- three dpi short of the 1.5 cut -- and 512 MB of RAM, well
 * under 1536. The result was the LOW-detail bundles
 * (ui/splash-variants.low, ui-splash-variants-christmas.low, ...) on every boot.
 *
 * Both are now config.txt settings rather than constants, because the honest
 * values are the wrong ones here and the right ones are a judgement call.
 * ------------------------------------------------------------------------ */
#define BP_PATCH_PHYSMEM 1

#define BP_JNI_LEDGER     1
#define BP_JNI_QUARANTINE 512
#define BP_JNI_SDK_NULL   1

/* ---------------------------------------------------------------------------
 * SAVE EDITING -- on.
 *
 * bp_savetool.c applies save.txt to Profile.Save before the engine starts. The
 * format is fully derived and the password was recovered on hardware, so this
 * is no longer guesswork; see that file's header. The first launch writes a
 * save.txt with every option commented out, so turning it on changes nothing
 * until you edit that file.
 * ------------------------------------------------------------------------ */
#define BP_SAVETOOL 1

/* Log the real Profile.Save password once, from inside the game. The container
 * is fully derived but the password string is not, and a static search over
 * millions of candidates did not find it -- so measure instead. Costs one
 * guarded hook and a few log lines; changes nothing. */
#define BP_SAVE_PROBE 1

/* ---- inherited knobs, inert here ---------------------------------------- */
#define BP_EOS_INIT_RESULT         14
#define BP_FORCE_OFFLINE           0
#define BP_TRACE_LOADING_STEPS     0
#define BP_TRACE_REMOTECONFIG_BIND 0

#endif /* BP_CONFIG_H */
