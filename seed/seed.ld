/* Linker script for BerryBasiC native seeds.
 *
 * Lays a seed out as one flat, position-independent image:
 *   - the seed_header first (offset 0),
 *   - the entry function immediately after it (so entry_off == sizeof header),
 *   - then the rest of the code and read-only data.
 * .bss is NOLOAD: the loader zeroes the whole destination slot, so zero-init
 * data lands in already-cleared memory right after the loaded bytes.
 *
 * Base address 0 + a 4 KiB-aligned load buffer keeps any adrp/add page maths
 * valid at the load address (and -mcmodel=tiny avoids adrp entirely anyway).
 */

SECTIONS
{
    . = 0;

    .seed : ALIGN(8) {
        KEEP(*(.seed.header))     /* seed_header at offset 0 */
        . = 16;                   /* header is 16 bytes                          */
        KEEP(*(.seed.keyword))    /* optional seed_keyword descriptor (32 B)     */
        . = ALIGN(16);            /* the entry is a 16-aligned function: keep the */
                                  /*   offset deterministic so it matches the     */
                                  /*   header's entry_off (16, or 48 with a kw)   */
        KEEP(*(.seed.entry))      /* entry function next                          */
        *(.text .text.*)
        *(.rodata .rodata.* .srodata .srodata.*)
        *(.data .data.* .sdata .sdata.*)
    }

    .bss (NOLOAD) : ALIGN(8) {
        *(.bss .bss.* .sbss .sbss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.ARM.attributes)
        *(.ARM.exidx .ARM.exidx.*)
    }
}
