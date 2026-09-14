// bp_tm_trampoline.s -- frame-correct entry into TimeManager::Update's body.
//
// Unity 2020.3.40f1 edition (Bloons Adventure Time TD).
//
// THIS FILE IS BUILD-SPECIFIC AND IS THE EASIEST THING IN THE PORT TO GET
// WRONG. The trampoline must rebuild BYTE-FOR-BYTE the same stack frame the
// real prologue builds, because we jump into the middle of the function and its
// epilogue pops whatever the prologue pushed. A frame that is 16 bytes too
// small returns to garbage.
//
// THIS build's prologue (game libunity.so, TimeManager::Update @ 0x042a2bc):
//     stp  d9, d8,   [sp, #-0x30]!     ; 6dbd23e9
//     str  x20,      [sp, #0x10]       ; f9000bf4
//     stp  x19, x30, [sp, #0x20]       ; a9027bf3
// i.e. a 0x30 frame saving BOTH d9 and d8 -- the 2020.3.39f1 (badpiggies_nx)
// shape, which is what this file now reproduces.
//
// IT IS NOT bloonspop_nx's. That port targets 2020.3.15f2, whose prologue is
//     str  d8,       [sp, #-0x20]!
//     str  x20,      [sp, #8]
//     stp  x19, x30, [sp, #0x10]
// -- a 0x20 frame saving only d8. Carrying that version here pops 0x10 bytes
// that were never pushed. The three words above were verified with keystone to
// assemble to exactly 6dbd23e9 / f9000bf4 / a9027bf3, and bp_offsets.h's
// 15-word guard refuses to install the hook if the game's prologue ever differs.
//
// The body (entry + BP_TM_BODY_ENTRY = +0x3c) leaves through the shared
// epilogue at +0x2c:
//     ldp  x19, x30, [sp, #0x20]
//     ldr  x20,      [sp, #0x10]
//     ldp  d9, d8,   [sp], #0x30
//     ret
// so with OUR return address sitting in x30's slot, the body's own epilogue
// returns to our caller. We never return here ourselves.
//
// void bp_tm_call_body(void *tm /* x0 */, double newTime /* d0 */);
    .text
    .align 2
    .global bp_tm_call_body
    .type   bp_tm_call_body, %function
bp_tm_call_body:
    stp     d9, d8, [sp, #-0x30]!
    str     x20, [sp, #0x10]
    stp     x19, x30, [sp, #0x20]
    adrp    x16, g_tm_body_target
    add     x16, x16, :lo12:g_tm_body_target
    ldr     x16, [x16]
    br      x16
    .size   bp_tm_call_body, .-bp_tm_call_body
