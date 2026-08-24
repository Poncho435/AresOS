/* AresOS — kmain: главная функция ядра.
 * Порядок инициализации:
 *   serial → GDT → IDT → FB-консоль → карта памяти → PMM → тест →
 *   PIC+мышь → рабочий стол (или halt, если нет графики). */
#include "bootinfo.h"
#include "serial.h"
#include "kprintf.h"
#include "fb_console.h"
#include "pmm.h"
#include "pic.h"
#include "mouse.h"
#include "desktop.h"
#include <stdint.h>

extern void gdt_init(void);
extern void idt_init(void);

/* дескриптор UEFI — такой же layout, как в pmm.c */
typedef struct {
    uint32_t type;
    uint32_t _pad;
    uint64_t phys;
    uint64_t virt;
    uint64_t pages;
    uint64_t attr;
} efi_mem_desc_t;

static const char *mem_type_name(uint32_t t) {
    static const char *const names[] = {
        "Reserved", "LoaderCode", "LoaderData", "BSCode", "BSData",
        "RTCode", "RTData", "Conventional", "Unusable", "ACPIReclaim",
        "ACPINVS", "MMIO", "MMIOPortSpace?", "PalCode", "Persistent",
    };
    if (t == 12) return "MMIO(IOPort)";
    if (t < 15) return names[t];
    return "Other";
}

static void print_memory_summary(const bootinfo_t *bi) {
    const uint8_t *end = (const uint8_t *)(uintptr_t)bi->mmap_phys + bi->mmap_size;
    uint64_t usable = 0, entries = 0;

    kprintf("[mem] UEFI memory map (%lu bytes, desc %lu bytes):\n",
            bi->mmap_size, bi->mmap_desc_size);
    for (const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_phys;
         p < end && entries < 64; p += bi->mmap_desc_size, entries++) {
        const efi_mem_desc_t *d = (const efi_mem_desc_t *)p;
        uint64_t bytes = d->pages * 4096ULL;
        if (d->type == 7) usable += bytes;
        kprintf("  [%2lu] %p - %p  %8lu KiB  %s\n",
                entries, (void *)(uintptr_t)d->phys,
                (void *)(uintptr_t)(d->phys + bytes - 1),
                bytes >> 10, mem_type_name(d->type));
    }
    kprintf("[mem] usable RAM total: %lu MiB\n", usable >> 20);
}

#define KERNEL_VERSION "0.2.3-diag"

void kmain(bootinfo_t *bi) {
    /* M5 diag-маркер (v0.2.2): первая инструкция ядра — циановая полоса.
       Видна, только если загрузчик дошёл до ExitBootServices и прыгнул сюда. */
    if (bi && bi->fb.phys_base && bi->fb.format <= FB_FORMAT_BGR &&
        bi->fb.pitch && bi->fb.width && bi->fb.height > 128) {
        uint32_t v = (bi->fb.format == FB_FORMAT_RGB) ? 0x0000C0C0u : 0x00C0C000u;
        uint32_t *fb = (uint32_t *)(uintptr_t)bi->fb.phys_base;
        for (uint32_t y = 96; y < 128; y++) {
            uint32_t *line = fb + (uint64_t)y * bi->fb.pitch;
            for (uint32_t x = 0; x < bi->fb.width; x++) line[x] = v;
        }
    }

    serial_init();   /* первым делом — канал отладки */

    kprintf("\n");
    kprintf("  ============================================\n");
    kprintf("   A r e s O S   kernel %s\n", KERNEL_VERSION);
    kprintf("   x86-64 bare-metal | custom UEFI loader\n");
    kprintf("  ============================================\n\n");

    if (!bi || bi->magic != BOOTINFO_MAGIC)
        kpanic("bad bootinfo (ptr=%p) — loader/kernel ABI mismatch", (void *)bi);

    gdt_init();
    kprintf("[gdt] GDT loaded (kcode=0x08, kdata=0x10)\n");

    idt_init();
    kprintf("[idt] IDT loaded: 32 exception handlers armed\n");

    fb_console_init(&bi->fb);
    if (fb_console_ready()) {
        kprintf("[fb] framebuffer console: %lu x %lu (pitch %lu, fmt %lu) @ %p\n",
                bi->fb.width, bi->fb.height, bi->fb.pitch,
                bi->fb.format, (void *)(uintptr_t)bi->fb.phys_base);
    } else {
        kprintf("[fb] no framebuffer provided by loader (serial only)\n");
    }

    print_memory_summary(bi);

    pmm_init(bi);

    /* дымовой тест аллокатора */
    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();
    kprintf("[pmm] test: alloc a=%p b=%p\n", (void *)a, (void *)b);
    pmm_free_page(a);
    pmm_free_page(b);
    kprintf("[pmm] test: freed both, free pages=%lu (%lu MiB)\n",
            pmm_free_pages(), (pmm_free_pages() * 4096ULL) >> 20);

    uint64_t free_mib = (pmm_free_pages() * 4096ULL) >> 20;
    uint64_t total_mib = (pmm_total_pages() * 4096ULL) >> 20;

    if (fb_console_ready()) {
        kprintf("\n[desktop] starting AresOS Desktop prototype...\n");
        pic_remap();
        mouse_init();
        /* IRQ12 (bit4 slave) + cascade IRQ2 (bit2 master); остальное замаскировано */
        pic_set_mask((uint8_t)~0x04, (uint8_t)~0x10);
        desktop_enter(bi, total_mib, free_mib);   /* не возвращается */
    }

    kprintf("\nAresOS init complete. No framebuffer — halting.\n");
    for (;;) __asm__ volatile ("hlt");
}
