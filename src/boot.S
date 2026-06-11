.section ".text.boot"
.global _start

_start:
    // Park secondary cores (cores 1-3)
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary_core
secondary_park:
    wfe
    b       secondary_park

primary_core:
    // Check current exception level and drop to EL1
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    beq     from_el3
    cmp     x0, #2
    beq     from_el2
    b       el1_entry

from_el3:
    // Allow EL2 (AArch64), enable HVC, mark non-secure
    mov     x0, #0x0531         // SCR_EL3: NS|IRQ|FIQ|EA|RW|HCE
    msr     scr_el3, x0
    mov     x0, #0x3C9          // SPSR_EL3: EL2h + DAIF masked
    msr     spsr_el3, x0
    adr     x0, from_el2
    msr     elr_el3, x0
    eret

from_el2:
    // EL1 will be AArch64
    mov     x0, #(1 << 31)      // HCR_EL2.RW = 1
    msr     hcr_el2, x0
    mov     x0, #0x3C5          // SPSR_EL2: EL1h + DAIF masked
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

el1_entry:
    // Set stack below load address
    ldr     x0, =_start
    mov     sp, x0

    // Install the exception vector table
    ldr     x0, =vector_table
    msr     vbar_el1, x0
    isb

    // Enable FP/SIMD at EL1 (CPACR_EL1.FPEN = 0b11) so floating point doesn't trap
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    // Zero BSS
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    sub     x1, x1, x0
    cbz     x1, jump_to_main
clear_bss:
    str     xzr, [x0], #8
    subs    x1, x1, #8
    bgt     clear_bss

jump_to_main:
    bl      kernel_main

halt:
    wfe
    b       halt
