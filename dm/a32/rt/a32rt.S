// Runtime glue between translated AArch32 code (dm/a32/translate.py) and the C host (a32host.c).
// Guest registers: r0-r14 = x0-x14 (w views), x28 = guest memory base, x27 = ThreadCtx*, x26 = TPIDRURO, x25 = TPIDRURW,
// x24 = FPSCR condition flags, w21 = a guest address handed to a stub. Scratch used by translated code: x15-x17, x19-x21.
// A "frame" (352 bytes on the host stack) holds r0-r14 (u32 at 0), nzcv (u32 at 60), next pc (u32 at 64, 0 = none), q0-q15 (at 80), x29/x30 (at 336).
    .text

.macro FRAME_SAVE
    sub sp, sp, #352
    stp w0, w1, [sp, #0]
    stp w2, w3, [sp, #8]
    stp w4, w5, [sp, #16]
    stp w6, w7, [sp, #24]
    stp w8, w9, [sp, #32]
    stp w10, w11, [sp, #40]
    stp w12, w13, [sp, #48]
    str w14, [sp, #56]
    mrs x17, nzcv
    str w17, [sp, #60]
    str wzr, [sp, #64]
    stp q0, q1, [sp, #80]
    stp q2, q3, [sp, #112]
    stp q4, q5, [sp, #144]
    stp q6, q7, [sp, #176]
    stp q8, q9, [sp, #208]
    stp q10, q11, [sp, #240]
    stp q12, q13, [sp, #272]
    stp q14, q15, [sp, #304]
    stp x29, x30, [sp, #336]
.endmacro

.macro FRAME_RESTORE
    ldp q0, q1, [sp, #80]
    ldp q2, q3, [sp, #112]
    ldp q4, q5, [sp, #144]
    ldp q6, q7, [sp, #176]
    ldp q8, q9, [sp, #208]
    ldp q10, q11, [sp, #240]
    ldp q12, q13, [sp, #272]
    ldp q14, q15, [sp, #304]
    ldr w17, [sp, #60]
    msr nzcv, x17
    ldp w0, w1, [sp, #0]
    ldp w2, w3, [sp, #8]
    ldp w4, w5, [sp, #16]
    ldp w6, w7, [sp, #24]
    ldp w8, w9, [sp, #32]
    ldp w10, w11, [sp, #40]
    ldp w12, w13, [sp, #48]
    ldr w14, [sp, #56]
    ldr w21, [sp, #64]
    ldp x29, x30, [sp, #336]
    add sp, sp, #352
.endmacro

// Indirect branch: w21 = guest address. Addresses from HOOK_BASE up are host stubs; the rest is translated module text.
    .globl _a32_dispatch
    .p2align 4
_a32_dispatch:
#ifndef A32_NO_RING
    adrp x19, _a32_ring@PAGE                  // debugging aid: the last 256 dispatch targets with the link register, dumped when the guest dies
    add x19, x19, _a32_ring@PAGEOFF
    ldr w20, [x19]
    add w15, w20, #1
    and w15, w15, #255
    str w15, [x19]
    add x19, x19, x20, lsl #2
    str w21, [x19, #8]
    str w14, [x19, #1032]
#endif
    tst w21, #3
    b.ne _a32_bad_target
    movz w17, #0xf000, lsl #16
    cmp w21, w17
    b.hs _a32_hook_entry
    adrp x17, _a32_modtab@PAGE
    add x17, x17, _a32_modtab@PAGEOFF
1:  ldp w15, w19, [x17]
    cbz w19, _a32_bad_target
    sub w20, w21, w15
    cmp w20, w19
    b.lo 2f
    add x17, x17, #16
    b 1b
2:  ldr x15, [x17, #8]
    lsr w20, w20, #2
    add x15, x15, x20, lsl #4
    br x15

// Guest code branched to a hook address (an import the host implements, or HOOK_RET, the return to the host).
    .globl _a32_hook_entry
    .p2align 4
_a32_hook_entry:
    movz w17, #0xf000, lsl #16
    cmp w21, w17
    b.eq _a32_return_to_host
    FRAME_SAVE
    mov w0, w21
    mov x1, sp
    bl _a32_hook_c
    FRAME_RESTORE
    b _a32_dispatch

// svc: w21 = svc number; returns to the guest instruction after it (x30), or dispatches to frame.next when the handler set it.
    .globl _a32_svc_gate
    .p2align 4
_a32_svc_gate:
    FRAME_SAVE
    mov w0, w21
    mov x1, sp
    bl _a32_svc_c
    FRAME_RESTORE
    cbnz w21, _a32_dispatch
    ret

// trace gate (translate.py --trace): w21 = guest address of the instruction about to run
    .globl _a32_trace_gate
    .p2align 4
_a32_trace_gate:
    FRAME_SAVE
    mov w0, w21
    mov x1, sp
    bl _a32_trace_c
    FRAME_RESTORE
    ret

// call counting (translate.py --count-calls): w21 = guest address of the bl / blx about to run; a32_callcnt points at the module's table biased by its base so that the address indexes it
    .globl _a32_count_gate
    .p2align 4
_a32_count_gate:
    adrp x16, _a32_callcnt@PAGE
    ldr x16, [x16, _a32_callcnt@PAGEOFF]
    lsr w17, w21, #2
    add x16, x16, x17, lsl #2
    ldr w17, [x16]
    add w17, w17, #1
    str w17, [x16]
    ret

    .globl _a32_unsup
    .p2align 4
_a32_unsup:
    FRAME_SAVE
    mov w0, w21
    mov x1, sp
    bl _a32_unsup_c
    brk #0

    .globl _a32_bad_target
    .p2align 4
_a32_bad_target:
    FRAME_SAVE
    mov w0, w21
    mov x1, sp
    bl _a32_bad_target_c
    brk #0

// uint32_t a32_enter(ThreadCtx *ctx, const uint32_t *regs /* r0-r14 at 0, nzcv at 60 */, uint32_t pc)
// ThreadCtx: +0 host_sp, +16 guest memory base, +24 guest TLS pointer (TPIDRURO), +28 TPIDRURW. Runs guest code until it returns to HOOK_RET; returns r0.
    .globl _a32_enter
    .p2align 4
_a32_enter:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    ldr x17, [x0]
    str x17, [sp, #160]
    mov x17, sp
    str x17, [x0]
    mov x27, x0
    mov w21, w2
    ldr x28, [x27, #16]
    ldr w26, [x27, #24]
    ldr w25, [x27, #28]
    mov x24, xzr
    mov x15, x1
    ldr w17, [x15, #60]
    msr nzcv, x17
    ldp w0, w1, [x15, #0]
    ldp w2, w3, [x15, #8]
    ldp w4, w5, [x15, #16]
    ldp w6, w7, [x15, #24]
    ldp w8, w9, [x15, #32]
    ldp w10, w11, [x15, #40]
    ldp w12, w13, [x15, #48]
    ldr w14, [x15, #56]
    b _a32_dispatch

_a32_return_to_host:
    ldr x17, [x27]
    mov sp, x17
    ldr x17, [sp, #160]
    str x17, [x27]
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #176
    ret
