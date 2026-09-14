/* ---------------------------------------------------------------------------
 * main.c -- entry point, heap layout, module loading, boot sequence.
 *
 * The order in here matters more than anything else in the port, and most of it
 * is not obvious from the code alone. Each step carries the reason it sits
 * where it does.
 *
 * SCOPE. This is the Bouncemasters equivalent of cloverpit_nx's main.c, written
 * against only what this tree actually provides. CloverPit's version also
 * drives a stack-region overcommit arena, a splash-video decoder, an il2cpp
 * runtime-API binder, a managed Time driver and a set of il2cpp hooks. None of
 * those modules were ported, so none of them are called here. Where their
 * absence is likely to matter on this game, there is a note saying so.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#include "bp_config.h"
#include "bp_savetool.h"
#include "battd_video.h"
#include "battd_il2cpp.h"
#include "asset_pack.h"
#include "battd_save_probe.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "android_native_unity.h"
#include "bp_assets.h"
#include "bp_managed.h"
#include "bp_net.h"
#include "bp_root.h"
#include "config.h"
#include "diag.h"
#include "error.h"      /* fatal_error */
#include "imports.h"
#include "jni_fake.h"
#include "libc_shim.h"
#include "opensles.h"
#include <dirent.h>
#include <sys/stat.h>
#include "so_util.h"
#include "util.h"

/* --------------------------------------------------------------------------
 * Globals the rest of the tree expects from here
 * ------------------------------------------------------------------------ */

so_module main_mod, unity_mod, il2cpp_mod;

static uint32_t g_frame_count;
uint32_t port_frame_count(void) { return g_frame_count; }
void     port_frame_tick(void)  { g_frame_count++; }

/* Main-thread Bionic TLS.
 *
 * The engine reads its stack guard through tpidr_el0. libnx uses that register
 * for its own thread pointer, so every thread that will run engine code needs a
 * Bionic-shaped block installed there first. Anything that creates a thread or
 * touches HID can move it, so the pointer is kept here and re-asserted at the
 * two points that matter -- see bp_reassert_main_tls() callers. */
static void *g_main_tls;
void bp_set_main_tls(void *buf)   { g_main_tls = buf; }
void bp_reassert_main_tls(void)   { if (g_main_tls) install_bionic_tls(g_main_tls); }

/* unity_glue.c. Declared here because it has no header of its own -- it is a
 * single function whose only caller is this file. */
void unity_environment_init(const char *data_root);

/* Where the game's modules are mapped.
 *
 * so_load() takes (base, max_size) and REFUSES a module whose aligned load_size
 * exceeds max_size. The three modules are placed end to end out of one reserved
 * region, so load_module() bumps the base and shrinks the limit after each one.
 * They are NOT all loaded at the same address. */
static void  *heap_so_base;
static size_t heap_so_limit;

/* Carved out of the heap in __libnx_initheap, ahead of newlib.
 *
 * Measured need for this build, straight from the boot log: libmain ~0 MB,
 * libunity 26 MB, libil2cpp 77 MB -- 103 MB with page rounding. 160 MB leaves
 * ~55 MB of headroom for a content update.
 *
 * Was 240 MB (cloverpit_nx's figure). Trimmed because the cost of over-reserving
 * is heap newlib does not get, and newlib is where Unity's 512 MB Dynamic Heap
 * has to come from. The cost of under-reserving is so_load returning -3, which
 * load_module now reports by name along with how much room was left. */
#define SO_REGION_BYTES (160u * 1024 * 1024)

/* Provided by bp_boot.c / bp_gpuarena.c / bp_patches.c / bp_vsync.c */
int  bp_boot_and_run(void);
void bp_boot_pause(void);
void bp_boot_resume(void);
void bp_boot_focus(int focused);
void bp_gpua_enable(void);
int  bp_apply_patches(void);
int  bp_vsync_init(void);

/* Used by nx_crash_handler.c to name an address in a backtrace. */
int crash_resolve_module(uintptr_t addr, char *name_out, size_t name_cap,
                         uintptr_t *base_out) {
  so_module *m = so_find_module_by_addr((const void *)addr);
  if (!m) return 0;
  /* m->name is char[64], so it can never be NULL and `m->name ? ... : "?"` is
   * dead -- GCC's -Waddress is right. The case that actually occurs is an EMPTY
   * name, and an unnamed module in a crash backtrace should be visibly unnamed
   * rather than a blank gap. Test the first byte. */
  snprintf(name_out, name_cap, "%s", m->name[0] ? m->name : "?");
  if (base_out) *base_out = (uintptr_t)m->load_virtbase;
  return 1;
}

/* --------------------------------------------------------------------------
 * Heap
 *
 * Called by libnx's crt0 before main(). The stock margin is 2 MB, which starves
 * switch-mesa: a driver allocation failing in the compositor path wedges the
 * whole console rather than failing the process. Hold back GFX_RESERVE_MB.
 *
 * cloverpit_nx additionally carves a stack-region overcommit arena out of the
 * reservation here (overcommit_setup(), which lives in its main.c and was not
 * ported). Without it the mmap shim in libc_shim.c falls back to its
 * heap-backed arena, which is the documented fallback path and works -- it just
 * spends real RAM on reservations the overcommit version would not.
 * ------------------------------------------------------------------------ */
/* __libnx_initheap runs BEFORE main(), before the game root is resolved and
 * therefore before debug.log exists. Anything it prints goes nowhere. Stash the
 * numbers and let main() report them once logging works -- the alternative is a
 * boot that dies here with no output at all, which cloverpit_nx records costing
 * a full test cycle. */
static struct {
  size_t total, used, granted, newlib, so_zone;
  int    heap_override;
} g_heapinfo;

void __libnx_initheap(void) {
  void  *addr = NULL;
  size_t size = 0;

  if (envHasHeapOverride()) {
    /* hbmenu path. The heap is hbloader's, not ours, and in APPLET mode it is
     * only a few hundred MB -- nowhere near what this game needs. main() checks
     * and says so. */
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
    g_heapinfo.heap_override = 1;
  } else {
    size_t mem_available = 0, mem_used = 0;
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used,      InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    g_heapinfo.total = mem_available;
    g_heapinfo.used  = mem_used;

    const size_t gfx_reserve = (size_t)GFX_RESERVE_MB * 1024 * 1024;
    if (mem_available > mem_used + gfx_reserve)
      size = (mem_available - mem_used - gfx_reserve) & ~0x1FFFFFull;
    if (size == 0)
      size = 0x2000000 * 16;   /* 512 MB floor, applet mode or a bad query */

    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  /* Split the reservation: newlib gets the front, the module region the tail.
   *
   * so_load() maps the game's code into heap_so_base, so this region must NOT
   * be inside newlib's arena -- newlib would hand the same pages out again. */
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2) so_zone = size / 2;
  const size_t fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  g_heapinfo.granted = size;
  g_heapinfo.newlib  = fake_heap_size;
  g_heapinfo.so_zone = so_zone;

  /* The mmap shim in libc_shim.c reads these. Left at zero here, which selects
   * its heap-backed fallback arena -- the same path CloverPit takes whenever
   * overcommit_setup() fails.
   *
   * The alternative is to carve an alias-mapped region out of the reservation
   * above and set g_overcommit, which lets large reservations stay uncommitted
   * until touched. That is worth doing if the game runs short of memory; it is
   * not worth doing speculatively, because a wrong arena base here corrupts
   * every mmap the engine makes and the symptom appears somewhere unrelated. */
  extern void  *g_mmap_arena_base;
  extern size_t g_mmap_arena_size;
  extern int    g_overcommit;
  g_mmap_arena_base = NULL;
  g_mmap_arena_size = 0;
  g_overcommit      = 0;
}

/* Definitions for the three above. libc_shim.c declares them extern with the
 * note "set by __libnx_initheap (main.c)", so this file owns them. */
void  *g_mmap_arena_base;
size_t g_mmap_arena_size;
int    g_overcommit;

/* --------------------------------------------------------------------------
 * Pre-flight checks
 * ------------------------------------------------------------------------ */

/* so_util maps the game's code with svcMapProcessCodeMemory and makes it
 * executable with svcSetProcessMemoryPermission. Both need the process to have
 * been launched with the right permissions, which title override gives and the
 * homebrew menu's applet mode does not. Checking here turns "it crashes on
 * launch" into one line naming the cause. */
static void check_syscalls(void) {
  u64 dummy = 0;
  Result rc = svcGetInfo(&dummy, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  if (R_FAILED(rc))
    fatal_error("svcGetInfo failed (0x%08x).\n\n"
                "Launch by title override -- hold R while starting an installed\n"
                "game. Applet mode does not grant the memory permissions this\n"
                "loader needs.", rc);

}

/* Unity's Dynamic Heap allocator asks for one ~512 MB block during MemoryManager
 * init, before it renders anything. Add the 110 MB of mapped modules and this
 * game cannot start in less than roughly 768 MB of heap. Applet mode gives a few
 * hundred. */
#define BP_MIN_NEWLIB_MB 768u

static void check_memory(void) {
  const size_t MB = 1024 * 1024;

  debugPrintf("[heap] granted %u MB -> newlib %u MB + module region %u MB%s\n",
              (unsigned)(g_heapinfo.granted / MB),
              (unsigned)(g_heapinfo.newlib  / MB),
              (unsigned)(g_heapinfo.so_zone / MB),
              g_heapinfo.heap_override ? "   [hbloader heap override]" : "");
  if (g_heapinfo.total)
    debugPrintf("[heap] process total %u MB, used %u MB at init\n",
                (unsigned)(g_heapinfo.total / MB),
                (unsigned)(g_heapinfo.used  / MB));

  /* Report the applet type, do NOT gate on it.
   *
   * An earlier version of this made a non-Application applet type fatal. That
   * was wrong twice over: hbmenu's title override does not necessarily report
   * AppletType_Application, so the check could kill a setup that works, and the
   * applet type is only ever a proxy for the thing that actually matters, which
   * is how much heap we got. Check the heap. */
  AppletType at = appletGetAppletType();
  debugPrintf("[heap] applet type %d%s\n", (int)at,
              (at == AppletType_Application ||
               at == AppletType_SystemApplication) ? " (application)" : "");

  /* THE check. Unity's Dynamic Heap allocator asks for one ~512 MB block during
   * MemoryManager init, before it renders anything, and gets it from newlib
   * through memalign. If newlib's arena is smaller than that the boot cannot
   * survive, and it is far better to say so here with the number than to fail
   * inside the engine's allocator with a message about a GPU arena that was
   * never involved. */
  if (g_heapinfo.newlib / MB < BP_MIN_NEWLIB_MB) {
    fatal_error(
        "Only %u MB of heap for newlib; this game needs about %u MB.\n\n"
        "Unity's Dynamic Heap allocator requests a single 512 MB block during\n"
        "startup, so a smaller arena cannot boot.\n\n"
        "If you launched from the homebrew menu, use TITLE OVERRIDE instead:\n"
        "hold R while starting an installed game.\n\n"
        "If you are ALREADY using title override, this is a sizing bug, not a\n"
        "launch-mode one -- lower SO_REGION_BYTES (currently %u MB) or\n"
        "GFX_RESERVE_MB (currently %u MB) in the source and rebuild.",
        (unsigned)(g_heapinfo.newlib / MB), BP_MIN_NEWLIB_MB,
        (unsigned)(SO_REGION_BYTES / MB), GFX_RESERVE_MB);
  }

  debugPrintf("[heap] ok for a %u MB Unity Dynamic Heap\n", 512u);
}

/* Recursive delete, for dropping the loose asset tree once it is packed.
 * unlink() after rmdir() because a path can be either here. (laracroftgo_nx) */
static void remove_tree(const char *path) {
  DIR *dir = opendir(path);
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      char child[832];
      snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
      struct stat st;
      if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
      else unlink(child);
    }
    closedir(dir);
  }
  rmdir(path);
  unlink(path);
}

static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", bp_game_root(), name);

  debugPrintf("[boot] loading %s  (region %p, %u MB free)\n",
              path, heap_so_base, (unsigned)(heap_so_limit >> 20));

  int rc = so_load(mod, path, heap_so_base, heap_so_limit);
  if (rc < 0) {
    /* so_load's return codes, spelled out -- they are the difference between
     * "the file is missing" and "the file is fine but the region is full", and
     * those want completely different fixes. */
    const char *why =
        rc == -1 ? "cannot open the file, or it is not an ELF"
      : rc == -2 ? "out of memory reading it"
      : rc == -3 ? "module is larger than the remaining module region "
                   "(raise SO_REGION_BYTES)"
      : rc == -4 ? "too many program headers"
                 : "unknown";
    debugPrintf("[boot] so_load(%s) failed rc=%d: %s\n", path, rc, why);
    if (rc == -3)
      debugPrintf("[boot]   needs %u MB, %u MB left in the region\n",
                  (unsigned)(mod->load_size >> 20),
                  (unsigned)(heap_so_limit >> 20));
    return -1;
  }

  /* Advance the bump allocator. Without this the next module is placed on top
   * of this one. */
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base  = (char *)heap_so_base + used;
  heap_so_limit -= used;

  if (so_relocate(mod) < 0) {
    debugPrintf("[boot] so_relocate failed for %s\n", name);
    return -1;
  }

  /* crx_resolve_imports, NOT so_resolve against dynlib_functions alone.
   *
   * imports.c builds a combined table from four sources: the base libc/GLES
   * shims, unity_imports (the Unity 6 engine surface), firebase_extra, and
   * bp_imports_extra (the media NDK). Resolving against only the first would
   * leave the other three unresolved and tainted, and the game would abort at
   * the first AImageReader or Firebase call with no indication why. */
  crx_resolve_imports(mod);

  debugPrintf("[boot]   %s at %p (%u MB)\n", name, mod->load_virtbase,
              (unsigned)(mod->load_size >> 20));
  return 0;
}

/* --------------------------------------------------------------------------
 * Applet lifecycle
 * ------------------------------------------------------------------------ */

static AppletHookCookie g_applet_cookie;

static void nx_applet_hook(AppletHookType hook, void *param) {
  (void)param;
  switch (hook) {
    case AppletHookType_OnExitRequest:
      debugPrintf("[applet] exit requested\n");
      jni_quit_requested = 1;
      break;
    case AppletHookType_OnFocusState: {
      /* Sleep and the home menu both land here. The engine has to be told, or
       * it keeps running its clock across a suspend and integrates the whole
       * gap into one frame on the way back. */
      u8 focus = appletGetFocusState();
      int focused = (focus == AppletFocusState_InFocus);
      debugPrintf("[applet] focus=%d\n", focused);
      bp_boot_focus(focused);
      if (focused) bp_boot_resume(); else bp_boot_pause();
      break;
    }
    case AppletHookType_OnOperationMode:
      /* Docked <-> handheld. The render resolution is fixed (see config.h), so
       * the compositor scales and nothing here has to change. Logged because if
       * something DOES go wrong at a dock transition, this is the first thing
       * to want in the log. */
      debugPrintf("[applet] operation mode changed\n");
      break;
    default:
      break;
  }
}

/* --------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
  /* FIRST. debug.log lives under the resolved root, so nothing may log before
   * this returns -- including anything that fails. */
  bp_resolve_game_root(argc, argv);
  battd_video_init(bp_game_root());   /* reads videos/manifest.txt; starts nothing */
  bp_config_load();   /* config.txt: resolution, before anything sizes a buffer */

  bp_net_init();   /* sockets + nifm; see bp_net.c */
  bp_savetool_run();   /* save.txt -> Profile.Save, before the engine reads it */
  debugPrintf("[boot] === battd_nx (Bloons Adventure Time TD, Unity 2020.3.40f1 / IL2CPP) ===\n");
  bp_root_report(argc, argv);

  /* Title override leaves the cwd at the .nro folder or at the SD root
   * depending on how it was launched, and the engine opens plenty of things by
   * relative path. Pin it. */
  if (chdir(bp_game_root()) != 0)
    debugPrintf("[boot] WARNING: chdir(%s) failed\n", bp_game_root());

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  check_memory();

  /* FastLoad clocks for the load path. Module loading, relocation and the first
   * scene are all CPU-bound and single-threaded; this is the difference between
   * a boot that feels broken and one that feels slow. Turned off before the
   * frame loop so it does not cost battery for the whole session. */
  cpu_boost(1);
  debugPrintf("[boot] CPU boost ON for the load path\n");

  /* Verify the staged tree BEFORE loading anything. A missing catalog.bin is a
   * five-second fix if it is named now, and an unexplained hang forty seconds
   * in if it is not. */
  /* ---- asset pack ------------------------------------------------------
   * This game ships 1,224 loose asset files (83 MB). Horizon is slow to open
   * and stat files individually and fsdev holds a handle per open, so a tree
   * that size makes loading crawl and puts real pressure on the handle table.
   * The pack (from laracroftgo_nx, unchanged) concatenates the tree into one
   * assets.nxpack plus an assets.nxidx index; libc_shim.c serves
   * open/read/lseek/stat/fstat/fopen/opendir/readdir out of it. One fd for the
   * whole asset tree, and a hash lookup instead of a directory walk.
   *
   * ORDER. Try the existing pack FIRST. Building is keyed on "no pack AND a
   * loose tree is present", and the loose tree is removed once the pack is
   * live -- otherwise every boot would see loose files and rebuild, and the SD
   * card would carry both copies forever. */
  {
    char adir[768], probe[832];
    snprintf(adir, sizeof adir, "%s/assets", bp_game_root());
    snprintf(probe, sizeof probe, "%s/bin/Data/globalgamemanagers", adir);

    asset_pack_open_existing(bp_game_root());

    struct stat loose;
    const int have_loose = (stat(probe, &loose) == 0 && S_ISREG(loose.st_mode));

    if (!asset_pack_active() && have_loose) {
      /* First boot: about a minute for 83 MB. A black screen for that long
       * reads as a hang, so say what is happening. */
      startup_status_begin("Optimizing game assets (first boot, about a minute)");
      debugPrintf("[pack] building asset pack from %s\n", adir);
      if (!asset_pack_build(adir, bp_game_root())) {
        startup_status_end();
        fatal_error("Could not optimize the extracted assets.\n%s\n\n"
                    "The original files were kept, so you can\n"
                    "simply try again.", asset_pack_error());
      }
      /* Only drop the loose tree once the pack is open and verified -- the
       * build path re-reads and checksums what it wrote before returning. */
      if (asset_pack_active() && stat(probe, &loose) == 0) {
        startup_status_update("Removing unpacked asset files");
        remove_tree(adir);
      }
      startup_status_end();
    } else if (asset_pack_active() && have_loose) {
      /* Both present: a re-stage after packing. The pack wins (it is what the
       * shim serves); say so rather than silently ignoring the new files. */
      debugPrintf("[pack] NOTE: assets.nxpack AND a loose assets/ tree are both "
                  "present. The pack is being served. Delete assets.nxpack and "
                  "assets.nxidx to rebuild from the loose tree.\n");
    }

    if (asset_pack_active())
      debugPrintf("[pack] active: %u entries from assets.nxpack\n",
                  (unsigned)asset_pack_entry_count());
    else
      debugPrintf("[pack] not active -- serving loose files from %s\n", adir);
  }

  /* Verify the staged tree. With the pack live this checks the index; without
   * it, the loose files. Either way a missing file is named now rather than
   * surfacing as an unexplained hang forty seconds into the boot. */
  if (bp_assets_init() < 0)
    fatal_error("No assets found under %s.\n\n"
                "Stage the game from your own APK:\n"
                "  python3 tools/stage_sd.py base.apk split_config.arm64_v8a.apk -o out/battd\n"
                "then copy out/battd/ next to the .nro.", bp_game_root());

  SDL_SetMainReady();

  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   BP_LIB_MAIN)   < 0) fatal_error("Could not load %s", BP_LIB_MAIN);
  if (load_module(&unity_mod,  BP_LIB_UNITY)  < 0) fatal_error("Could not load %s", BP_LIB_UNITY);
  if (load_module(&il2cpp_mod, BP_LIB_IL2CPP) < 0) fatal_error("Could not load %s", BP_LIB_IL2CPP);

  /* Hand libc_shim the il2cpp mapping BEFORE anything can run engine code.
   *
   * pthread_kill is routed to pthread_kill_gc(), the Boehm GC stop-the-world
   * bridge. libnx has no POSIX signals, so bdwgc's suspend/restart signals are
   * never delivered and its threads never acknowledge -- GC_suspend_all then
   * spins in usleep() forever waiting for acks. The bridge answers on their
   * behalf by posting GC_suspend_ack_sem directly.
   *
   * It reads the ack semaphore and the two signal-number globals through
   * g_il2cpp_base, and it refuses to dereference anything when that is zero.
   * Leaving it unset is therefore SILENT: no [gc] line is ever printed, every
   * pthread_kill is a no-op, and the first collection wedges the main thread in
   * GC_suspend_all with nothing in the log to say why. That is exactly what
   * happened -- the boot got as far as FMOD playing and then stopped, with the
   * watchdog showing UnityMain parked in usleep under GC_suspend_all+0xb0.
   *
   * Set from load_virtbase, not load_base: the offsets in the bridge are
   * virtual addresses within the mapped module. */
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;
  g_il2cpp_size = il2cpp_mod.load_size;
  debugPrintf("[gc] il2cpp base=%p size=%u MB (GC stop-the-world bridge armed)\n",
              il2cpp_mod.load_virtbase, (unsigned)(il2cpp_mod.load_size >> 20));

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  debugPrintf("[boot] modules finalized + flushed\n");

  /* Patches go in after finalize (the pages are mapped and executable, and
   * so_patch_code aliases them RW to write) and before any engine code runs.
   * Each one verifies a guard word first; see bp_patches.c. */
  bp_apply_patches();

  /* MAIN-THREAD BIONIC TLS -- must precede any engine code. */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);
  bp_set_main_tls(main_tls);
  debugPrintf("[boot] main-thread bionic TLS @ %p\n", (void *)main_tls);

  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);

  so_free_temp(&main_mod);
  so_free_temp(&unity_mod);
  so_free_temp(&il2cpp_mod);
  debugPrintf("[boot] init arrays run; staging buffers freed\n");

  jni_init();

  /* Creates fake_unityplayer_thiz / fake_context_obj / fake_surface_obj and
   * registers the asset, PlayerPrefs and Display handlers against the data
   * root. Without it those three stay NULL and initJni is handed a null
   * Context, which it dereferences. */
  unity_environment_init(bp_game_root());

  /* Order matters. update_mode() sizes the NWindow from screen_width/height;
   * input_init() then binds HID against that size. Calling input_init() alone
   * leaves the window at its portrait default while the engine renders 1080p
   * landscape, and the result is cropped and rotated. */
  screen_width  = BP_FORCE_SCREEN_W;
  screen_height = BP_FORCE_SCREEN_H;
  android_native_update_mode();
  android_native_input_init();
  appletHook(&g_applet_cookie, nx_applet_hook, NULL);

  /* jni_init, HID and applet setup all ran since the TLS was installed. */
  install_bionic_tls(main_tls);

  /* libunity's JNI_OnLoad is what calls RegisterNatives, and that capture in
   * jni_fake.c is the ONLY way bp_boot.c can find initJni/nativeRender -- they
   * are file-local in libunity and never exported. If this is skipped, the boot
   * fails with "a required UnityPlayer native did not register". */
  {
    typedef int (*fn_jnionload)(void *, void *);
    fn_jnionload unity_onload =
        (fn_jnionload)so_try_find_addr_rx(&unity_mod, "JNI_OnLoad");
    if (!unity_onload) fatal_error("libunity JNI_OnLoad not found");
    debugPrintf("[boot] libunity JNI_OnLoad(fake_vm)...\n");
    int jver = unity_onload(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }
  {
    typedef int (*fn_jnionload)(void *, void *);
    fn_jnionload il2cpp_onload =
        (fn_jnionload)so_try_find_addr_rx(&il2cpp_mod, "JNI_OnLoad");
    if (il2cpp_onload) {
      debugPrintf("[boot] libil2cpp JNI_OnLoad(fake_vm)...\n");
      il2cpp_onload(fake_vm, NULL);
    } else {
      debugPrintf("[boot] libil2cpp JNI_OnLoad absent (managed JNI may fail)\n");
    }
  }

  /* Resolve the vsync state block now that libunity is mapped and relocated. */
  bp_vsync_init();

  /* Managed hooks. These patch generated C# in libil2cpp, so they must go in
   * after its init arrays have run (the code is there from load, but hooking
   * before JNI_OnLoad would race the runtime bringing itself up) and before the
   * first frame, since Time and Screen are read during the first scene load.
   *
   * Each verifies three guard words and skips itself on a mismatch, so a stale
   * bp_managed.h costs the feature rather than corrupting an unrelated method. */
  bp_time_install(&unity_mod);   /* libunity icall bindings */
  bp_input_install(&unity_mod);   /* libunity icall bindings */
  bp_screen_install(&unity_mod);   /* libunity icall bindings */
  bp_physmem_install(&unity_mod);  /* art-quality gate: report config.txt memory_mb */
  bp_prefs_install(&il2cpp_mod);
  battd_il2cpp_install(&il2cpp_mod);   /* VideoPlayer::Play -> ffmpeg splash player */
  battd_save_probe_install(&il2cpp_mod);  /* log the save password once */
  /* Arm the GPU arena only HERE. Everything before this point -- module
   * loading, relocation, the metadata read -- makes large page-aligned
   * allocations that are not graphics buffers. cloverpit_nx records that
   * arming it earlier reserved half a gigabyte for nothing and regressed the
   * game to a frame-0 freeze. From here on, the only allocator making
   * page-aligned requests of this size class is libdrm_nouveau. */
  bp_gpua_enable();

  cpu_boost(0);
  debugPrintf("[boot] CPU boost OFF (load path complete)\n");

  diag_thread_register(NULL, 1);          /* this is the engine main thread */
  diag_set_name(NULL, "battd-main");
  diag_watchdog_start();                  /* backtrace if a frame wedges */

  /* HTTPS trust. unitytls asks Android's KeyStore for roots over JNI and gets
   * nothing here, so append a real CA bundle to its default list (ACPC's
   * method). Without it every HTTPS request fails certificate verification. */
  bp_net_install_ca(&unity_mod);

  /* The TimeManager detour went in with the patches; its stall thread starts
   * now that libunity is initialised (bp_tmclock.c). */
  bp_tmclock_start();

  int rc = bp_boot_and_run();

  nx_sd_flush();
  debugPrintf("[boot] exit rc=%d\n", rc);
  opensles_shutdown();
  SDL_Quit();
  socketExit();

  /* The engine leaves threads and atexit handlers behind that fault if the
   * normal C runtime teardown runs. Leave the way the rest of this lineage
   * does. */
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
