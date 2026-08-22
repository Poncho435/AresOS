/* AresOS — linker script ядра.
 * Пока (M1-M2): физическая загрузка по 2 МиБ, identity-map от UEFI действует.
 * На M3 ядро переедет в higher half (0xFFFFFFFF80000000) со своими страницами. */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x200000;
    __kernel_start = .;

    .text : ALIGN(0x1000) {
        *(.text .text.*)
    }

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata .rodata.* .srodata .srodata.*)
    }

    . = ALIGN(0x1000);
    .data : {
        *(.data .data.* .sdata .sdata.*)
    }

    . = ALIGN(0x1000);
    .bss : {
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(0x1000);
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
