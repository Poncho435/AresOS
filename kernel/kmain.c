/* AresOS — kmain: главная функция ядра.
 * Порядок инициализации:
 *   serial → GDT → IDT → FB-консоль → карта памяти → PMM → тест →
 *   PIC+мышь → рабочий стол (или halt, если нет графики). */
#include "bootinfo.h"
#include "serial.h"
#include "kprintf.h"
#include "fb_console.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "pe.h"
#include "pic.h"
#include "mouse.h"
#include "keyboard.h"
#include "acpi.h"
#include "lapic.h"
#include "proc.h"
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

#define KERNEL_VERSION "0.5.2"

/* ---- фоновые демоны M5 (диспетчер задач покажет их в списке) ---- */
#include "gfx.h"

static void heartbeat_proc(void *arg) {
    (void)arg;
    int on = 0;
    for (;;) {
        /* мигающий огонёк в правом нижнем углу (правее пилюли со статусом) */
        uint32_t x = gfx_width() - 20, y = gfx_height() - 28;
        gfx_fill_rect(x, y, 11, 11, on ? GFX_RGB(0xFF, 0x9E, 0x49) : GFX_RGB(0x28, 0x2C, 0x40));
        on ^= 1;
        proc_sleep(500);
    }
}

static void sysmon_proc(void *arg) {
    (void)arg;
    for (;;) {
        /* ТОЛЬКО serial: консоль держим чистой (статус виден в диспетчере задач) */
        char num[24]; int i = 0;
        uint64_t v = sched_ticks() / 100;
        serial_write("[sysmon] bg alive, uptime=");
        if (!v) serial_write("0");
        while (v) { num[i++] = (char)('0' + v % 10); v /= 10; }
        while (i) { char c[2] = { num[--i], 0 }; serial_write(c); }
        serial_write(" s\n");
        proc_sleep(5000);
    }
}

/* обёртка: десктоп как обычный процесс (bi прячем глобально) */
static bootinfo_t *g_bi;
static uint64_t   g_total_mib, g_free_mib;
static void desktop_proc(void *arg) {
    (void)arg;
    desktop_enter(g_bi, g_total_mib, g_free_mib);   /* не возвращается */
}

void kmain(bootinfo_t *bi) {
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

    /* v0.2.4: IDT (все 256 векторов) — ДО первой записи в framebuffer.
       Любой залётный вектор/исключение теперь ловится нашим обработчиком,
       а не улетает в висячий IDT прошивки или в triple fault. */
    idt_init();
    kprintf("[idt] IDT loaded: 256 vectors armed (exc + PIC IRQ + spurious-safe)\n");

    /* M5 diag-маркер: ядро живо и прошло инициализацию IDT — циановая полоса. */
    if (bi->fb.phys_base && bi->fb.format <= FB_FORMAT_BGR &&
        bi->fb.pitch && bi->fb.width && bi->fb.height > 128) {
        uint32_t v = (bi->fb.format == FB_FORMAT_RGB) ? 0x00C0C000u : 0x0000C0C0u;
        uint32_t *fb = (uint32_t *)(uintptr_t)bi->fb.phys_base;
        for (uint32_t y = 96; y < 128; y++) {
            uint32_t *line = fb + (uint64_t)y * bi->fb.pitch;
            for (uint32_t x = 0; x < bi->fb.width; x++) line[x] = v;
        }
    }

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

    /* ===== M3: своя виртуальная память + куча ===== */
    vmm_init(bi);          /* свои таблицы: identity+HHDM, null-guard, NX, WP */
    heap_init();           /* арена кучи через VMM */
    heap_stress_test();    /* DoD M3: миллион случайных alloc/free */
    pe_demo_init(&bi->fb); /* готовим контекст для TESTPE.EXE (запустит десктоп) */

    /* дымовой тест аллокатора */
    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();
    kprintf("[pmm] test: alloc a=%p b=%p\n", (void *)a, (void *)b);
    pmm_free_page(a);
    pmm_free_page(b);
    kprintf("[pmm] test: freed both, free pages=%lu (%lu MiB)\n",
            pmm_free_pages(), (pmm_free_pages() * 4096ULL) >> 20);

    g_bi = bi;
    g_free_mib = (pmm_free_pages() * 4096ULL) >> 20;
    g_total_mib = (pmm_total_pages() * 4096ULL) >> 20;

    if (!fb_console_ready()) {
        kprintf("\nAresOS init complete. No framebuffer — halting.\n");
        for (;;) __asm__ volatile ("hlt");
    }

    /* ===== M4: IRQ-модель (IOAPIC+LAPIC при наличии ACPI, иначе PIC+PIT) ===== */
    int lapic_ok = acpi_init(bi->rsdp_phys) && lapic_init();
    keyboard_init();
    pic_remap();
    if (lapic_ok) {
        ioapic_route_irq(1, 33);
        ioapic_route_irq(12, 44);
        pic_set_mask(0xFF, 0xFF);              /* PIC уходит с поля — всё через LAPIC */
        kprintf("[irq] model: IOAPIC → LAPIC (kbd=33, mouse=44), timer=LAPIC 100Hz\n");
    } else {
        /* PIT ch0 как системный тик 100 Гц + IRQ1 (клавиатура) + IRQ12 (мышь) */
        {
            extern void pit_init_100hz(void);
            pit_init_100hz();
        }
        pic_set_mask((uint8_t)~0x07, (uint8_t)~0x10);  /* IRQ0,IRQ1,cascade | IRQ12 */
        kprintf("[irq] model: legacy PIC 8259, PIT tick 100 Hz, kbd=33, mouse=44\n");
    }
    mouse_init();

    /* ===== M5: планировщик + процессы ===== */
    proc_init();                                       /* kmain → процесс "idle" */
    proc_spawn(heartbeat_proc, NULL, "heartbeat", PROC_F_BACKGROUND);
    proc_spawn(sysmon_proc,    NULL, "sysmon",    PROC_F_BACKGROUND);
    proc_spawn(desktop_proc,   NULL, "desktop",   0);

    kprintf("\n[m5] scheduler ON: %s модель, тик 100 Гц — посмотри диспетчер (F2)!\n",
            lapic_ok ? "LAPIC" : "PIC");

    __asm__ volatile ("sti");
    for (;;) __asm__ volatile ("hlt");          /* idle-поток кmain: паркуемся */

    kprintf("\nAresOS init complete. No framebuffer — halting.\n");
    for (;;) __asm__ volatile ("hlt");
}
