// AArch64 setjmp / longjmp for the pod-libc.
//
// The callee-saved state a C function may rely on across a longjmp: x19..x28,
// the frame pointer x29, the link register x30, the stack pointer, and the
// callee-saved SIMD registers d8..d15. jmp_buf is long[32] (256 bytes), which
// is ample. setjmp returns 0; longjmp makes the matching setjmp return `val`
// (or 1 if val is 0).
    .text
    .align 2
    .global setjmp
    .type setjmp, %function
setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x2, sp
    str x2, [x0, #96]
    stp d8,  d9,  [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov w0, #0
    ret

// The C wrapper longjmp() (misc.c) passes an already-nonzero value in w1, so no
// compare is needed here (tcc's assembler lacks cmp/csinc). This just restores
// and returns w1 as setjmp's value.
    .align 2
    .global __pod_longjmp
    .type __pod_longjmp, %function
__pod_longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    mov sp, x2
    ldp d8,  d9,  [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    mov w0, w1
    ret
