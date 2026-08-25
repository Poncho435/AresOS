/* AresOS — VMM (M3): собственные таблицы страниц.
 * После vmm_init прошивочные таблицы больше не используются:
 *   PML4[0]   → identity 0..4 ГиБ (2 МиБ, кроме зоны ядра и страницы 0)
 *   PML4[256] → тот же PDPT: HHDM 0xFFFF800000000000 + phys
 * Зона ядра [0x200000..0x400000) мапится постранично с защитой секций. */
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include <string.h>

#define PTE_P  (1ULL << 0)
#define PTE_W  (1ULL << 1)
#define PTE_PS (1ULL << 7)
#define PTE_NX (1ULL << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

#define IDENTITY_TOP 0x100000000ULL      /* 4 ГиБ */
#define KERNEL_ZONE_LO 0x200000ULL
#define KERNEL_ZONE_HI 0x400000ULL

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[];

static uint64_t *g_pml4;

static inline void invlpg(uint64_t va) {
    __asm__ volatile ("invlpg (%0)" :: "r"(va) : "memory");
}
static inline uint64_t read_cr0(void) {
    uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline void write_cr0(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr0" :: "r"(v) : "memory");
}
static inline void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

/* во время постройки таблиц действует identity — физ. адрес = вирт. адрес */
static uint64_t *new_table(void) {
    uint64_t p = pmm_alloc_page();
    if (!p) kpanic("vmm: out of pages for page tables");
    memset((void *)(uintptr_t)p, 0, PMM_PAGE_SIZE);
    return (uint64_t *)(uintptr_t)p;
}

static uint64_t *next_level(uint64_t *tbl, int idx, uint64_t add_flags) {
    if (!(tbl[idx] & PTE_P)) {
        uint64_t t = (uint64_t)(uintptr_t)new_table();
        tbl[idx] = t | PTE_P | PTE_W | add_flags;
    }
    return (uint64_t *)(uintptr_t)(tbl[idx] & PTE_ADDR);
}

void vmm_map_4k(uint64_t virt, uint64_t phys, uint64_t flags) {
    int i4 = (int)((virt >> 39) & 0x1FF);
    int i3 = (int)((virt >> 30) & 0x1FF);
    int i2 = (int)((virt >> 21) & 0x1FF);
    int i1 = (int)((virt >> 12) & 0x1FF);
    if (!g_pml4) kpanic("vmm_map_4k before vmm_init");
    uint64_t *pdpt = next_level(g_pml4, i4, 0);
    uint64_t *pd   = next_level(pdpt, i3, 0);
    if (pd[i2] & PTE_PS)
        kpanic("vmm: 4K map inside 2MiB page %#lx — реализуй split позже", virt);
    uint64_t *pt   = next_level(pd, i2, 0);
    pt[i1] = (phys & PTE_ADDR) | flags;
    invlpg(virt);
}

static uint64_t *vmm_walk(uint64_t virt) {
    /* вернуть указатель на PTE страницы 4 КиБ, или NULL */
    int i4 = (int)((virt >> 39) & 0x1FF);
    int i3 = (int)((virt >> 30) & 0x1FF);
    int i2 = (int)((virt >> 21) & 0x1FF);
    if (!g_pml4 || !(g_pml4[i4] & PTE_P)) return NULL;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(g_pml4[i4] & PTE_ADDR);
    if (!(pdpt[i3] & PTE_P)) return NULL;
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[i3] & PTE_ADDR);
    if (!(pd[i2] & PTE_P) || (pd[i2] & PTE_PS)) return NULL;
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[i2] & PTE_ADDR);
    return pt + ((virt >> 12) & 0x1FF);
}

void vmm_unmap_4k(uint64_t virt) {
    uint64_t *pte = vmm_walk(virt);
    if (pte && (*pte & PTE_P)) { *pte = 0; invlpg(virt); }
}

void vmm_protect_4k(uint64_t virt, uint64_t flags) {
    uint64_t *pte = vmm_walk(virt);
    if (pte && (*pte & PTE_P)) { *pte = (*pte & PTE_ADDR) | flags; invlpg(virt); }
}

uint64_t vmm_get_phys(uint64_t virt) {
    uint64_t *pte = vmm_walk(virt);
    return (pte && (*pte & PTE_P)) ? (*pte & PTE_ADDR) : 0;
}

void vmm_init(const bootinfo_t *bi) {
    (void)bi;
    g_pml4 = new_table();
    uint64_t *pdpt = new_table();
    g_pml4[0] = ((uint64_t)(uintptr_t)pdpt) | PTE_P | PTE_W;

    /* 4 PD на 0..4 ГиБ */
    uint64_t *pd[4];
    for (int d = 0; d < 4; d++) {
        pd[d] = new_table();
        pdpt[d] = ((uint64_t)(uintptr_t)pd[d]) | PTE_P | PTE_W;
        for (int i = 0; i < 512; i++) {
            uint64_t base = ((uint64_t)d << 30) + ((uint64_t)i << 21);
            pd[d][i] = base | PTE_P | PTE_W | PTE_PS | PTE_NX;
        }
    }

    /* HHDM: тот же PDPT под PML4[256] → физ. 0..4 ГиБ видны и там */
    g_pml4[(VMM_HHDM_BASE >> 39) & 0x1FF] = ((uint64_t)(uintptr_t)pdpt) | PTE_P | PTE_W;
    kprintf("[vmm] HHDM: phys 0..4 GiB дублируется на %#lx\n", (uint64_t)VMM_HHDM_BASE);

    /* ---- страница 0: НЕ замаплена (null-pointer guard) ---- */
    uint64_t *pt_lo = new_table();          /* PT для самых первых 2 МиБ */
    pd[0][0] = ((uint64_t)(uintptr_t)pt_lo) | PTE_P | PTE_W;   /* сбрасывает PS */
    for (int i = 1; i < 512; i++)           /* i=0 пропускаем: null → #PF */
        pt_lo[i] = ((uint64_t)i << 12) | PTE_P | PTE_W | PTE_NX;

    /* ---- зона ядра 0x200000..0x400000 постранично ---- */
    uint64_t *pt_k = new_table();
    pd[0][KERNEL_ZONE_LO >> 21] = ((uint64_t)(uintptr_t)pt_k) | PTE_P | PTE_W;
    for (int i = 0; i < 512; i++) {
        uint64_t va = KERNEL_ZONE_LO + ((uint64_t)i << 12);
        uint64_t flags;
        if (va >= (uint64_t)__text_start && va < (uint64_t)__text_end)
            flags = PTE_P;                            /* RX: код, только чтение */
        else if (va >= (uint64_t)__rodata_start && va < (uint64_t)__rodata_end)
            flags = PTE_P | PTE_NX;                   /* R: rodata */
        else
            flags = PTE_P | PTE_W | PTE_NX;           /* RW+NX: данные/стек/куча-зона */
        pt_k[i] = va | flags;
    }
    kprintf("[vmm] kernel zone: .text %lu KiB RX, .rodata %lu KiB R, rest RW+NX\n",
            ((uint64_t)__text_end - (uint64_t)__text_start) >> 10,
            ((uint64_t)__rodata_end - (uint64_t)__rodata_start) >> 10);

    /* NX на всё: EFER.NXE */
    uint64_t efer = rdmsr(0xC0000080);
    if (!(efer & (1ULL << 11))) wrmsr(0xC0000080, efer | (1ULL << 11));

    /* WP=1: запрет записи в RO-страницы даже для ring-0 */
    write_cr0(read_cr0() | (1ULL << 16));

    /* 💥 переключение на СВОИ таблицы */
    write_cr3((uint64_t)(uintptr_t)g_pml4);

    kprintf("[vmm] CR3 переключён на свои таблицы @ %p (identity 0..4G + HHDM, null-page guard, NX, WP)\n",
            (void *)(uintptr_t)g_pml4);
}
