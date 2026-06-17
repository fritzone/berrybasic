// ---------------------------------------------------------------------------
// AArch64 EL1 exception vector table.
//
// The CPU jumps here (via VBAR_EL1, set in boot.S) whenever an exception is
// taken. Until this existed, a fault (e.g. an unaligned access with the MMU
// off) jumped to address 0 and hung silently. Now every fault lands in
// exception_handler(), which prints the cause over UART and halts.
//
// The table must be 2KB aligned and holds 16 entries of 128 bytes each.
// ---------------------------------------------------------------------------

.macro VENTRY type
.align 7                 // each entry is 0x80 (128) bytes
    mov     x0, #\type
    b       exc_common
.endm

.section ".text.vectors", "ax"
.align 11                // 2KB-aligned table base
.global vector_table
vector_table:
    // Current EL with SP_EL0
    VENTRY 0             // Synchronous
    VENTRY 1             // IRQ
    VENTRY 2             // FIQ
    VENTRY 3             // SError
    // Current EL with SP_ELx  (this is the one we normally hit)
    VENTRY 4             // Synchronous
    VENTRY 5             // IRQ
    VENTRY 6             // FIQ
    VENTRY 7             // SError
    // Lower EL, AArch64
    VENTRY 8
    VENTRY 9
    VENTRY 10
    VENTRY 11
    // Lower EL, AArch32
    VENTRY 12
    VENTRY 13
    VENTRY 14
    VENTRY 15

// Common tail: gather the syndrome registers and call the C handler.
// We do not return — the handler reports the fault and halts.
exc_common:
    mrs     x1, esr_el1          // exception syndrome
    mrs     x2, elr_el1          // faulting instruction address
    mrs     x3, far_el1          // faulting data/instruction address
    bl      exception_handler
1:  wfe
    b       1b
