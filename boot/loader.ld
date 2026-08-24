/*
 * AresOS — линкер-скрипт UEFI-загрузчика (static-PIE ELF → elf2efi → PE32+).
 *
 * ЗАЧЕМ ОН: elf2efi.py отражает PT_LOAD-сегменты ELF в PE-секции один-в-один
 * (RVA = vaddr + 0x1000). PE/COFF требует, чтобы VirtualAddress КАЖДОЙ секции
 * был кратен SectionAlignment (0x1000) — иначе UEFI-загрузчик образа (PeCoff)
 * отклоняет файл ещё ДО запуска (статус «failed to load … Not Found», экран
 * чёрный, ни одной нашей строки). Дефолтный скрипт GNU ld для static-pie
 * кладёт RW-сегмент (.dynamic/.got/.data) в середину страницы — ровно это
 * и убивало загрузку на VirtualBox. Здесь начало каждого сегмента жёстко
 * выровнено по границе 0x1000; elf2efi дублирует проверку ассертом.
 *
 * Символы __bss_start / _end использует boot/main.c (самоочистка BSS).
 */

ENTRY(efi_main)

SECTIONS
{
    /* vaddr 0 — ld кладёт сюда служебные таблицы static-PIE (R-сегмент).
       elf2efi развернёт его в .rdata сразу за PE-заголовками (RVA 0x1000). */
    . = 0;
    .gnu.hash : { *(.gnu.hash) }
    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }

    . = ALIGN(0x1000);
    .text : {
        *(.text) *(.text.*)
    }

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata) *(.rodata.*)
        *(.srodata) *(.srodata.*)
        *(.data.rel.ro) *(.data.rel.ro.*)
        *(.rela.dyn) *(.rela.plt) *(.rela.*)
    }

    /* RW-сегмент: начало — строго на границе страницы */
    . = ALIGN(0x1000);
    .dynamic : { *(.dynamic) }
    .got     : { *(.got) *(.got.plt) }
    .data    : {
        *(.sdata) *(.sdata.*)
        *(.data) *(.data.*)
    }

    /* BSS слит с .data в один RW-сегмент (старт сегмента уже выровнен).
       Нули в PE получаем бесплатно: VirtualSize > SizeOfRawData,
       а цикл в main.c просто повторяет очистку — безвредно. */
    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.sbss) *(.sbss.*) *(.scommon)
        *(.bss) *(.bss.*)
        __bss_end = .;
    }
    . = ALIGN(8);
    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note) *(.note.*)
        *(.eh_frame) *(.eh_frame.*)
        *(.interp)
    }
}
