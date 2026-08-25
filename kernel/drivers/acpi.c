/* AresOS — ACPI discovery (M4). Ищем RSDP в BIOS-области 0xE0000..0x100000
 * (EBDA не трогаем: страница 0 под null-guard, стоять ей), без магии. */
#include "acpi.h"
#include "kprintf.h"
#include <string.h>

static uint64_t g_lapic, g_ioapic;
static uint32_t g_bsp_id = 0xFFFFFFFF;
static int      g_ok;

/* overrides: type2 entries мапят ISA IRQ → GSI */
#define MAX_ISO 16
static struct { uint32_t irq, gsi; uint16_t flags; } g_iso[MAX_ISO];
static int g_iso_n;

static int csum(const void *p, uint32_t n) {
    const uint8_t *b = p; uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) s += b[i];
    return (s & 0xFF) == 0;
}

static void parse_madt(const uint8_t *t) {
    /* MADT: 36B ACPI-header, +36 LAPIC base (4B), +40 flags (4B), +44 → entries */
    g_lapic = (uint64_t)*(const uint32_t *)(t + 36);
    const uint8_t *e = t + 44;
    const uint8_t *end = t + *(const uint32_t *)(t + 4);
    while (e + 2 <= end) {
        uint8_t type = e[0], len = e[1];
        if (len < 2 || e + len > end) break;
        switch (type) {
        case 0: {    /* Processor Local APIC */
            uint32_t id = e[3], flags = *(const uint32_t *)(e + 4);
            if ((flags & 1) && g_bsp_id == 0xFFFFFFFF) g_bsp_id = id;
            break;
        }
        case 1: {    /* I/O APIC */
            if (!g_ioapic) g_ioapic = *(const uint32_t *)(e + 4);
            break;
        }
        case 2: {    /* Interrupt Source Override */
            if (g_iso_n < MAX_ISO) {
                g_iso[g_iso_n].irq   = e[3];
                g_iso[g_iso_n].gsi   = *(const uint32_t *)(e + 4);
                g_iso[g_iso_n].flags = *(const uint16_t *)(e + 8);
                g_iso_n++;
            }
            break;
        }
        default: break;
        }
        e += len;
    }
}

static int rsdp_valid(const uint8_t *p) {
    if (memcmp(p, "RSD PTR ", 8) != 0) return 0;
    uint32_t rev = p[15];
    uint32_t len = rev >= 2 ? *(const uint32_t *)(p + 20) : 20;
    return csum(p, 20) && csum(p, len);
}

int acpi_init(uint64_t rsdp_hint) {
    const uint8_t *rsdp = NULL;
    /* 1) надёжный путь: RSDP из EFI ConfigurationTable (передал загрузчик) */
    if (rsdp_hint && rsdp_valid((const uint8_t *)(uintptr_t)rsdp_hint)) {
        rsdp = (const uint8_t *)(uintptr_t)rsdp_hint;
        kprintf("[acpi] RSDP от загрузчика @ %#lx\n", rsdp_hint);
    }
    /* 2) запасной путь: скан legacy BIOS-области (для BIOS-загрузок) */
    if (!rsdp) {
        for (uint64_t a = 0xE0000; a < 0x100000; a += 16) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)a;
            if (rsdp_valid(p)) { rsdp = p; break; }
        }
    }
    if (!rsdp) { kprintf("[acpi] RSDP не найден — остаёмся на legacy PIC\n"); return 0; }

    const uint8_t *root = NULL; uint64_t nent = 0; int xsdt = 0;
    if (rsdp[15] >= 2) {
        root = (const uint8_t *)(uintptr_t)*(const uint64_t *)(rsdp + 24);
        nent = (*(const uint32_t *)(root + 4) - 36) / 8; xsdt = 1;
    } else {
        root = (const uint8_t *)(uintptr_t)*(const uint32_t *)(rsdp + 16);
        nent = (*(const uint32_t *)(root + 4) - 36) / 4;
    }
    kprintf("[acpi] RSDP @ %#lx, %s, записей=%lu\n",
            (uint64_t)(uintptr_t)rsdp, xsdt ? "XSDT" : "RSDT", nent);

    const uint8_t *madt = NULL;
    for (uint64_t i = 0; i < nent; i++) {
        uint64_t addr = xsdt ? *(const uint64_t *)(root + 36 + i * 8)
                             : (uint64_t)*(const uint32_t *)(root + 36 + i * 4);
        const uint8_t *t = (const uint8_t *)(uintptr_t)addr;
        if (memcmp(t, "APIC", 4) == 0) { madt = t; break; }
    }
    if (!madt) { kprintf("[acpi] MADT не найден — PIC-only путь\n"); return 0; }

    parse_madt(madt);
    if (!g_lapic || !g_ioapic) { kprintf("[acpi] MADT без LAPIC/IOAPIC — PIC-only\n"); return 0; }

    g_ok = 1;
    kprintf("[acpi] MADT: LAPIC @ %#lx, IOAPIC @ %#lx, BSP id=%lu, overrides=%d\n",
            g_lapic, g_ioapic, (uint64_t)g_bsp_id, g_iso_n);
    return 1;
}

uint64_t acpi_lapic_base(void)  { return g_lapic; }
uint64_t acpi_ioapic_base(void) { return g_ok ? g_ioapic : 0; }
uint32_t acpi_bsp_apic_id(void) { return g_bsp_id; }

uint32_t acpi_gsi_for_irq(uint32_t irq) {
    for (int i = 0; i < g_iso_n; i++)
        if (g_iso[i].irq == irq) return g_iso[i].gsi;
    return irq;
}
