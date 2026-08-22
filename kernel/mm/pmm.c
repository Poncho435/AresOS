/* AresOS — PMM: битовая карта физических страниц 4 КиБ.
 * Источник данных — карта памяти UEFI (через bootinfo).
 * Бит = 1 → страница занята/зарезервирована, бит = 0 → свободна.
 * Линейный поиск — пока достаточно; buddy-аллокатор придёт на M3+ вместе с VMM. */
#include "pmm.h"
#include "kprintf.h"
#include <string.h>

/* дескриптор памяти UEFI (EFI_MEMORY_DESCRIPTOR) */
typedef struct {
    uint32_t type;
    uint32_t _pad;
    uint64_t phys;
    uint64_t virt;
    uint64_t pages;
    uint64_t attr;
} efi_mem_desc_t;

#define EFI_CONVENTIONAL_MEMORY 7   /* свободная ОЗУ */
#define LOW_MEM_LIMIT  0x100000ULL  /* всё ниже 1 МиБ — резерв (BIOS-наследие) */

extern char __kernel_start[], __kernel_end[];  /* символы из linker.ld */

static uint8_t  *g_bitmap;
static uint64_t  g_total_pages;   /* всего страниц в пространстве [0, top) */
static uint64_t  g_free_pages;
static uint64_t  g_hint;          /* с чего начинать поиск (простейшая оптимизация) */

static inline void bit_set(uint64_t i)   { g_bitmap[i / 8] |=  (uint8_t)(1u << (i % 8)); }
static inline void bit_clear(uint64_t i) { g_bitmap[i / 8] &= (uint8_t)~(1u << (i % 8)); }
static inline int  bit_test(uint64_t i)  { return (g_bitmap[i / 8] >> (i % 8)) & 1; }

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

void pmm_init(const bootinfo_t *bi) {
    const uint8_t *end = (const uint8_t *)(uintptr_t)bi->mmap_phys + bi->mmap_size;

    /* 1. верхняя граница ОЗУ — по обычной (conventional) памяти */
    uint64_t top = 0;
    for (const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_phys; p < end; p += bi->mmap_desc_size) {
        const efi_mem_desc_t *d = (const efi_mem_desc_t *)p;
        if (d->type != EFI_CONVENTIONAL_MEMORY) continue;
        uint64_t region_end = d->phys + d->pages * PMM_PAGE_SIZE;
        if (region_end > top) top = region_end;
    }
    g_total_pages = top >> 12;
    uint64_t bitmap_bytes = align_up(g_total_pages / 8 + 1, PMM_PAGE_SIZE);

    /* 2. битмап размещаем сразу за ядром — там точно есть физпамять,
          потому что загрузчик положил ядро в conventional-регион */
    g_bitmap = (uint8_t *)(uintptr_t)align_up((uint64_t)__kernel_end, PMM_PAGE_SIZE);

    /* 3. всё занято... */
    memset(g_bitmap, 0xFF, bitmap_bytes);
    g_free_pages = 0;

    /* 4. ...кроме conventional-регионов выше 1 МиБ */
    for (const uint8_t *p = (const uint8_t *)(uintptr_t)bi->mmap_phys; p < end; p += bi->mmap_desc_size) {
        const efi_mem_desc_t *d = (const efi_mem_desc_t *)p;
        if (d->type != EFI_CONVENTIONAL_MEMORY) continue;
        uint64_t base = d->phys;
        uint64_t last = d->phys + d->pages * PMM_PAGE_SIZE;
        if (base < LOW_MEM_LIMIT) base = LOW_MEM_LIMIT;
        if (last > top) last = top;
        for (uint64_t addr = base; addr < last; addr += PMM_PAGE_SIZE) {
            bit_clear(addr >> 12);
            g_free_pages++;
        }
    }

    /* 5. резервируем: битмап и само ядро */
    uint64_t bitmap_start = (uint64_t)g_bitmap;
    for (uint64_t a = bitmap_start; a < bitmap_start + bitmap_bytes; a += PMM_PAGE_SIZE) {
        if (!bit_test(a >> 12)) { bit_set(a >> 12); g_free_pages--; }
    }
    uint64_t kstart = (uint64_t)__kernel_start & ~0xFFFULL;
    for (uint64_t a = kstart; a < (uint64_t)__kernel_end; a += PMM_PAGE_SIZE) {
        if (!bit_test(a >> 12)) { bit_set(a >> 12); g_free_pages--; }
    }
    g_hint = LOW_MEM_LIMIT >> 12;

    kprintf("[pmm] RAM top: %lu MiB, total pages %lu, free %lu MiB, bitmap %lu KiB @ %p\n",
            top >> 20, g_total_pages, (g_free_pages * PMM_PAGE_SIZE) >> 20,
            bitmap_bytes >> 10, (void *)g_bitmap);
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t i = g_hint; i < g_total_pages; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            g_free_pages--;
            g_hint = i;
            return i << 12;
        }
    }
    /* wrap-around: поиск с начала */
    for (uint64_t i = LOW_MEM_LIMIT >> 12; i < g_hint; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            g_free_pages--;
            g_hint = i;
            return i << 12;
        }
    }
    return 0;  // OOM
}

void pmm_free_page(uint64_t phys) {
    uint64_t i = phys >> 12;
    if (i >= g_total_pages || !bit_test(i)) return;  // защита от мусора
    bit_clear(i);
    g_free_pages++;
    if (i < g_hint) g_hint = i;
}

uint64_t pmm_total_pages(void) { return g_total_pages; }
uint64_t pmm_free_pages(void)  { return g_free_pages; }
