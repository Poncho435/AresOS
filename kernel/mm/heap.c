/* AresOS - kernel heap (M3): K&R-аллокатор поверх VMM-арены. */
#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include <stdint.h>
#include <string.h>

#define HEAP_VA     0x500000000ULL          /* 20 ГиБ - вне identity-зон и HHDM-RAM */
#define HEAP_PAGES  512                     /* 2 МиБ арены */
#define BLOCK_MAGIC 0xB10CB10CB10CB10CULL
#define ALIGN16(n) (((n) + 15) & ~15ULL)

typedef struct block {
    uint64_t      magic;
    uint64_t      size;       /* байт полезной нагрузки */
    int           free;
    struct block *next;       /* список ВСЕХ блоков по адресу (для coalesce) */
} block_t;

static block_t  *g_head;
static uint64_t  g_arena_size = (uint64_t)HEAP_PAGES * PMM_PAGE_SIZE;
static uint64_t  g_free_bytes;

void heap_init(void) {
    /* арена: 512 произвольных физ. страниц -> непрерывные VA через VMM */
    for (uint64_t i = 0; i < HEAP_PAGES; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) kpanic("heap: out of pages for arena");
        vmm_map_4k(HEAP_VA + i * PMM_PAGE_SIZE, phys, VMM_P | VMM_W | VMM_NX);
    }
    g_head = (block_t *)(uintptr_t)HEAP_VA;
    g_head->magic = BLOCK_MAGIC;
    g_head->size  = g_arena_size - sizeof(block_t);
    g_head->free  = 1;
    g_head->next  = NULL;
    g_free_bytes  = g_head->size;
    kprintf("[heap] arena %lu MiB @ %#lx (virt), header %lu bytes\n",
            g_arena_size >> 20, (uint64_t)HEAP_VA, (uint64_t)sizeof(block_t));
}

void *kmalloc(size_t n) {
    if (!n) return NULL;
    uint64_t want = ALIGN16(n);
    for (block_t *b = g_head; b; b = b->next) {
        if (b->magic != BLOCK_MAGIC) kpanic("heap: corrupted block header");
        if (!b->free || b->size < want) continue;
        if (b->size >= want + sizeof(block_t) + 16) {
            /* split: откусываем хвост в новый свободный блок */
            block_t *rest = (block_t *)((uint8_t *)b + sizeof(block_t) + want);
            rest->magic = BLOCK_MAGIC;
            rest->size  = b->size - want - sizeof(block_t);
            rest->free  = 1;
            rest->next  = b->next;
            b->next     = rest;
            b->size     = want;
            g_free_bytes -= sizeof(block_t);
        }
        b->free = 0;
        g_free_bytes -= b->size;
        return (uint8_t *)b + sizeof(block_t);
    }
    return NULL;   /* не хватило - NULL, паника только при порче структур */
}

void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)((uint8_t *)p - sizeof(block_t));
    if (b->magic != BLOCK_MAGIC) kpanic("kfree: bad pointer %p (magic)", p);
    if (b->free) kpanic("kfree: double free %p", p);
    b->free = 1;
    g_free_bytes += b->size;
    /* coalesce: склеиваем все пары соседних свободных блоков (список по адресам).
       v0.3.2: был баг - "перезапуск" через c=g_head в for-цикле ПРОПУСКАЛ пару
       (head, head->next): инкремент цикла сразу перепрыгивал её. Теперь честные
       полные проходы до стабилизации. */
    int merged;
    do {
        merged = 0;
        for (block_t *c = g_head; c && c->next; c = c->next) {
            if (!c->free || !c->next->free) continue;
            if ((uint8_t *)c + sizeof(block_t) + c->size == (uint8_t *)c->next) {
                c->size += sizeof(block_t) + c->next->size;
                c->next = c->next->next;
                g_free_bytes += sizeof(block_t);
                merged = 1;
                break;
            }
        }
    } while (merged);
}

size_t heap_free_bytes(void) { return (size_t)g_free_bytes; }

/* ---------------- стресс-тест (DoD M3): миллион случайных alloc/free ---------------- */
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

void heap_stress_test(void) {
    enum { SLOTS = 64, OPS = 1000000 };
    static void    *ptr[SLOTS];
    static uint16_t len[SLOTS];            /* v0.3.2: ПОЛНЫЙ размер (был баг: младший байт) */
    uint64_t live_blocks = 0, max_live = 0, allocs = 0, frees = 0;

    for (int op = 0; op < OPS; op++) {
        int s = (int)(rnd() % SLOTS);
        if (!ptr[s]) {
            size_t n = (size_t)(8 + rnd() % 1016);   /* 8..1023 байт */
            ptr[s] = kmalloc(n);
            if (!ptr[s]) continue;                   /* фрагментация - допустимо */
            len[s] = (uint16_t)n;
            memset(ptr[s], (int)(n & 0xFF), n);      /* паттерн для проверки */
            live_blocks++; allocs++;
            if (live_blocks > max_live) max_live = live_blocks;
        } else {
            uint8_t *q = ptr[s];
            size_t n = (size_t)len[s];
            uint8_t pat = (uint8_t)(n & 0xFF);
            /* начало/середина/конец паттерна - ловит и перезапись, и Ctrl-C */
            if (q[0] != pat || q[n / 2] != pat || q[n - 1] != pat)
                kpanic("heap stress: pattern broken @ %p len=%lu pat=%#x (op %d)",
                       q, (uint64_t)n, (unsigned)pat, op);
            kfree(ptr[s]);
            ptr[s] = NULL;
            live_blocks--; frees++;
        }
    }
    for (int s = 0; s < SLOTS; s++)
        if (ptr[s]) { kfree(ptr[s]); ptr[s] = NULL; }

    /* после полной очистки куча должна собраться в один блок И ВЕРНУТЬ все байты */
    if (!(g_head && g_head->free && !g_head->next))
        kpanic("heap stress: Leak/fragment - арена не собралась обратно");
    if (g_free_bytes != g_arena_size - sizeof(block_t))
        kpanic("heap stress: accounting %lu != arena %lu - hdr %lu",
               g_free_bytes, g_arena_size, (uint64_t)sizeof(block_t));

    kprintf("[heap] STRESS OK: 1,000,000 ops (allocs=%lu frees=%lu, peak live=%lu), leaks=0\n",
            allocs, frees, max_live);
}
