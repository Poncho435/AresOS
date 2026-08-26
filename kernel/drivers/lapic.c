/* AresOS - LAPIC + IOAPIC (M4).
 * Ставки: регистры LAPIC identity-mapped в нашей VMM (0..4G).
 * Калибровка таймера - по PIT каналу 2 (одноразовый ~50 мс, gate на порту 0x61). */
#include "lapic.h"
#include "acpi.h"
#include "io.h"
#include "kprintf.h"
#include <stdint.h>

#define MSR_APIC_BASE 0x1B
#define APIC_EN       (1ULL << 11)

/* смещения регистров LAPIC */
#define L_ID    0x020
#define L_TPR   0x080
#define L_EOI   0x0B0
#define L_SVR   0x0F0
#define L_LVT_TMR 0x320
#define L_LVT_LINT0 0x350
#define L_LVT_LINT1 0x360
#define L_TMRDIV  0x3E0
#define L_TMRINIT 0x380
#define L_TMRCUR  0x390

#define TIMER_VECTOR 0x40     /* тот же вектор, что и irq64 в IDT */

static volatile uint32_t *g_lapic;
static int g_active;

static inline uint32_t lrd(uint32_t off) { return g_lapic[off / 4]; }
static inline void lwr(uint32_t off, uint32_t v) { g_lapic[off / 4] = v; (void)g_lapic[off / 4]; }

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

/* PIT ch2, один импульс ~50 мс без прерываний (классическая калибровка osdev) */
static int pit_one_shot_50ms(void) {
    enum { TICKS = 59659 };   /* 1.193182 МГц × 50 мс */
    uint8_t a = inb(0x61);
    outb(0x61, (uint8_t)(a & ~0x01));          /* gate off */
    outb(0x43, 0xB2);                          /* ch2, lobyte/hibyte, mode 0 */
    outb(0x42, TICKS & 0xFF);
    outb(0x42, (TICKS >> 8) & 0xFF);
    a = inb(0x61);
    outb(0x61, (uint8_t)(a | 0x01));           /* gate on -> старт */
    for (uint64_t i = 0; i < 100000000; i++)
        if (inb(0x61) & 0x20) return 1;        /* OUT=1: отсчёт кончился */
    return 0;
}

int lapic_init(void) {
    uint64_t base = acpi_lapic_base();
    if (!base) return 0;

    uint64_t msr = rdmsr(MSR_APIC_BASE);
    if (!(msr & APIC_EN)) wrmsr(MSR_APIC_BASE, (msr & ~0xFFFFFULL) | base | APIC_EN);

    g_lapic = (volatile uint32_t *)(uintptr_t)base;
    lwr(L_SVR, 0x100 | 0xFF);        /* APIC enable, spurious vector 0xFF */
    lwr(L_TPR, 0);                   /* принимаем все приоритеты */
    lwr(L_LVT_LINT0, 0x10000);       /* LINT0/1 маскируем (NMI пока не нужны) */
    lwr(L_LVT_LINT1, 0x10000);
    lwr(L_TMRDIV, 0x3);              /* делитель 16 */
    lwr(L_LVT_TMR, TIMER_VECTOR);    /* one-shot */

    /* --- калибровка: ДВА замера по PIT ch2, должны совпасть +-5% ---
     * v0.5.1: на части VM (VirtualBox/NEM) TMRCUR/гейт ведут себя странно и
     * одиночный замер врал в сотни раз -> таймер улетал на ~10 МГц. Лучше
     * честный откат на PIT, чем варп-часы. */
    uint32_t delta[2];
    for (int t = 0; t < 2; t++) {
        lwr(L_TMRINIT, 0xFFFFFFFF);
        uint32_t start = lrd(L_TMRCUR);
        if (!pit_one_shot_50ms()) { kprintf("[lapic] PIT ch2 не ответил - откат на PIC\n"); lapic_fallback_off(); return 0; }
        delta[t] = start - lrd(L_TMRCUR);
        lwr(L_TMRINIT, 0);
    }
    lwr(L_LVT_TMR, 0x10000);         /* маскируем пока не уверены */

    uint32_t d = delta[1];
    uint32_t diff = delta[0] > delta[1] ? delta[0] - delta[1] : delta[1] - delta[0];
    if (d < 20000 || d > 100000000u) {
        kprintf("[lapic] замер вне диапазона (delta=%lu) - откат на PIC\n", (uint64_t)d);
        lapic_fallback_off();
        return 0;
    }
    if (diff > d / 20) {
        kprintf("[lapic] два замера разошлись (%lu vs %lu) - LAPIC таймеру"
                " в этой VM верить нельзя, откат на PIC\n",
                (uint64_t)delta[0], (uint64_t)delta[1]);
        lapic_fallback_off();
        return 0;
    }
    uint32_t per_10ms = d / 5;       /* 50 мс -> 10 мс (100 Гц) */
    if (per_10ms < 4000) {           /* ~>25 МГц после /16 - чушь для bus clock */
        kprintf("[lapic] per_10ms=%lu слишком мал - откат на PIC\n", (uint64_t)per_10ms);
        lapic_fallback_off();
        return 0;
    }
    lwr(L_LVT_TMR, TIMER_VECTOR | 0x20000);   /* periodic */
    lwr(L_TMRINIT, per_10ms);

    g_active = 1;
    kprintf("[lapic] OK: таймер 100 Гц (замеры %lu/%lu, init=%lu), vector=0x40\n",
            (uint64_t)delta[0], (uint64_t)delta[1], (uint64_t)per_10ms);
    return 1;
}

int  lapic_active(void) { return g_active; }
void lapic_eoi(void)    { if (g_lapic) lwr(L_EOI, 0); }

/* Откат на legacy PIC (8259) при неудачной калибровке LAPIC.
 * КРИТИЧНО: LAPIC обязан уснуть! Иначе включённый LAPIC с замаскированным
 * LINT0 перехватывает линию INTR процессора - и ни таймер PIT, ни клавиатура,
 * ни мышь от 8259 до CPU НИКОГДА не доходят: система вечно спит в hlt
 * (баг по фото пользователя: [m5] scheduler ON - и тишина, ни одного тика).
 * SVR бит 8 = 0 это software-disable: INTR/NMI снова идут напрямую от PIC. */
void lapic_fallback_off(void) {
    if (!g_lapic) return;
    lwr(L_TMRINIT, 0);
    lwr(L_LVT_TMR,   0x10000);   /* LVT-таймер masked */
    lwr(L_LVT_LINT0, 0x10000);   /* LINT0 masked */
    lwr(L_LVT_LINT1, 0x10000);   /* LINT1 masked */
    lwr(L_TPR, 0xFF);            /* ничего не принимать */
    lwr(L_SVR, 0xFF);            /* software-DISABLE, spurious vector 0xFF */
    g_active = 0;
    kprintf("[lapic] усыплён (SVR.EN=0): INTR от PIC снова доходит до CPU\n");
}

/* ---------- IOAPIC ---------- */
static volatile uint32_t *g_ioapic;
static inline uint32_t iord(uint8_t reg) { g_ioapic[0] = reg; return g_ioapic[4]; }
static inline void iowr(uint8_t reg, uint32_t v) { g_ioapic[0] = reg; g_ioapic[4] = v; }

int ioapic_route_irq(uint32_t irq, uint8_t vector) {
    if (!g_ioapic) {
        uint64_t b = acpi_ioapic_base();
        if (!b) return 0;
        g_ioapic = (volatile uint32_t *)(uintptr_t)b;
        uint32_t ver = iord(0x01);
        kprintf("[ioapic] @ %#lx, max redir=%lu\n", b, (uint64_t)((ver >> 16) & 0xFF));
    }
    uint32_t gsi = acpi_gsi_for_irq(irq);
    uint32_t lo = vector;                 /* fixed, physical, edge, active-high */
    uint32_t hi = acpi_bsp_apic_id() << 24;
    iowr(0x10 + gsi * 2,     lo);
    iowr(0x10 + gsi * 2 + 1, hi);
    kprintf("[ioapic] IRQ%lu -> GSI%lu, vector=%#x, dest=BSP\n",
            (uint64_t)irq, (uint64_t)gsi, vector);
    return 1;
}
