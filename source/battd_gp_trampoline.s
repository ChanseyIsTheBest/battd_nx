// battd_gp_trampoline.s -- frame-correct entry into PasswordGenerator::GetPassword's
// body, so the probe can observe the return value without replacing the function.
//
// NAMED battd_gp_trampoline.s, NOT battd_save_probe.s, AND THAT MATTERS.
// devkitPro's rules build objects from basenames: OFILES_SRC is
// $(SFILES:.s=.o) ... $(CFILES:.c=.o), so foo.c and foo.s both yield foo.o.
// The link then sees that object twice (multiple definition of everything in
// it) and make builds only ONE of the two sources, so whatever was in the other
// is an undefined reference. Both errors at once, from one name clash. The
// lineage already dodges this by accident -- bp_tm_trampoline.s pairs with
// bp_tmclock.c -- and the Makefile now checks for it explicitly.
//
// WHY THIS EXISTS AT ALL
// hook_arm64 REPLACES a function: it overwrites the first 16 bytes with
// LDR X17,#8 / BR X17 / .quad dst, leaving no way back to the original body.
// The first version of this probe worked around that by un-patching, calling,
// and re-patching under a lock. That was wrong in three ways, all of which this
// file removes:
//
//   1. A RACE THE LOCK CANNOT COVER. The mutex only serialises callers that
//      arrive through our hook. A thread already executing the first 16 bytes
//      when we swap them executes a HYBRID prologue -- e.g. it runs the detour's
//      LDR X17 and then, after our write, continues into the real prologue at
//      word 1, skipping `str x21, [sp, #-0x30]!`. The function then pops a frame
//      it never pushed. That is a crash, and stopping it would need every core
//      halted, not a mutex.
//   2. BLOCKING I/O IN A HOOK. so_patch_code ends with a debugPrintf, which
//      takes the debug-log lock and flushes to the SD card. The old probe did
//      that TWICE per call, on the main thread, while holding its own lock --
//      the exact shape that deadlocked this lineage before (PORTING.md sec 10).
//   3. VIRTUAL-ADDRESS CHURN. so_patch_code reserves and releases an ASLR alias
//      mapping under the global virtmem lock on every call.
//
// This replaces all of it with the pattern bp_tm_trampoline.s already uses:
// patch ONCE at install, and reach the original body through a static
// trampoline that reproduces its prologue.
//
// WHY THE PROLOGUE CAN BE COPIED VERBATIM
// GetPassword opens with
//     str  x21,      [sp, #-0x30]!     ; f81d0ff5
//     stp  x20, x19, [sp, #0x10]       ; a9014ff4
//     stp  x29, x30, [sp, #0x20]       ; a9027bfd
//     add  x29, sp,  #0x20             ; 910083fd
// Four instructions, none PC-relative -- no ADRP, no branch, no literal load --
// so moving them changes nothing. (That is NOT true of IL2CPP prologues in
// general, which is why this is checked rather than assumed: the four words are
// pinned by BP_GUARD_GetPassword and re-verified against this file at install.)
// Assembled with keystone to exactly those four words.
//
// WHY THIS IS TRANSPARENT TO EXCEPTIONS
// We enter with BL (x30 = return into the C hook), build the frame, then BR --
// never BL -- into the body at +0x10. x30 is untouched, so the body's own
// epilogue (`ldp x29, x30, [sp, #0x20]` ... `ret`) returns straight to the C
// hook, which then reads the result from x0. If the body throws instead, the
// unwinder uses the BODY's FDE, which describes exactly the frame we built, and
// recovers x30 from [CFA-8] -- a PC inside the C hook. This file never appears
// as an unwind frame, so it needs no CFI. GetPassword CAN throw (its bounds
// check is `cmp length, version ; b.le <throw>`), which is why that matters.
//
// void *battd_gp_call_body(void *self, int version, void *method);
    .text
    .align 2
    .global battd_gp_call_body
    .type   battd_gp_call_body, %function
battd_gp_call_body:
    str     x21, [sp, #-0x30]!
    stp     x20, x19, [sp, #0x10]
    stp     x29, x30, [sp, #0x20]
    add     x29, sp, #0x20
    adrp    x16, g_gp_body_target
    add     x16, x16, :lo12:g_gp_body_target
    ldr     x16, [x16]
    br      x16
    .size   battd_gp_call_body, .-battd_gp_call_body
