/* linker.ld — AIOS v2 kernel
 *
 * HARD CONTRACT: Kernel loads at physical 1MB.
 * p_vaddr = p_paddr = addresses in this script.
 * The bootloader loads to p_paddr. Paging is OFF when kernel starts.
 * All addresses here ARE physical addresses. */

ENTRY(kernel_main)

SECTIONS
{
    . = 0x100000;

    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }

    .kaos_export : {
        __kaos_export_start = .;
        KEEP(*(.kaos_export))
        __kaos_export_end = .;
    }

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;

    /* Discard unneeded sections */
    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
