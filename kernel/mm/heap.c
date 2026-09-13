/* AresOS - kernel heap (M3): K&R-аллокатор поверх VMM-арены. */
#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include <stdint.h>
#include <string.h>

#define HEAP_VA     0x500000000ULL          /* 20 ГиБ - вне identity-зон и HHDM-RAM */
#define HEAP_PAGES  4096                    /* 16 МиБ арены (v0.6.1: было 2 МиБ -
                                               бэкбуферу стола 1024x768x32 нужно 3 МиБ
                                               единым куском; запас на будущее) */
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

/* ===================== v0.8.1: стеки потоков с guard-страницей =====================
 *
 * ПОЧЕМУ ЭТО ПОЯВИЛОСЬ. В VirtualBox 7.2 ядро падало в "Guru Meditation"
 * (VINF_EM_TRIPLE_FAULT). Разбор дампа: rip=0x220dde (memcpy, инструкция
 * `mov %rdi,0x10(%rsp)`), rsp=0x4ffffffb8, cr2=0x4ffffffa8. Ключ: арена кучи
 * начинается ровно с 0x500000000, то есть rsp был НА 72 БАЙТА НИЖЕ начала
 * арены. Стек потока (kmalloc(32 КиБ) из proc.c) переполнился, уехал вниз за
 * первый блок кучи - в никуда не отображённые адреса - и любая запись на стек
 * давала #PF; обработчик #PF пытался положить кадр на тот же убитый стек ->
 * #DF -> triple fault. Отсюда и "перезагрузка без единого сообщения".
 *
 * ЛЕЧЕНИЕ ДВУСТОРОННЕЕ:
 *   1) TSS.IST (gdt.c + idt.c) - у #PF/#DF свой заведомо валидный стек, так
 *      что даже полное разрушение стека потока даёт нормальную панику.
 *   2) Стеки потоков больше НЕ берутся из общей арены kmalloc. Каждый стек -
 *      отдельный диапазон VA со своими физ. страницами и НЕзамапленной
 *      страницей-часовым снизу. Переполнение ловится на первой же странице
 *      за пределом стека, а не уносит соседние объекты кучи.                 */

#define KSTACK_AREA   0x600000000ULL    /* 24 ГиБ: своя зона, вне арены кучи */
#define KSTACK_SLOTS  64
#define KSTACK_STRIDE 0x100000ULL       /* 1 МиБ на слот: стек + guard + запас */

static uint8_t  g_kstack_used[KSTACK_SLOTS];
static uint64_t g_kstack_guard[KSTACK_SLOTS];
static uint64_t g_kstack_size[KSTACK_SLOTS];

void *kstack_alloc(unsigned size, unsigned long *out_guard) {
    if (!size) return NULL;
    uint64_t pages = (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if ((pages + 1) * PMM_PAGE_SIZE > KSTACK_STRIDE) return NULL;

    for (int s = 0; s < KSTACK_SLOTS; s++) {
        if (g_kstack_used[s]) continue;
        uint64_t guard = KSTACK_AREA + (uint64_t)s * KSTACK_STRIDE;
        uint64_t base  = guard + PMM_PAGE_SIZE;      /* дно стека - НАД часовым */
        for (uint64_t i = 0; i < pages; i++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {                              /* откат частичного стека */
                while (i--) {
                    uint64_t va = base + i * PMM_PAGE_SIZE;
                    pmm_free_page(vmm_get_phys(va));
                    vmm_unmap_4k(va);
                }
                return NULL;
            }
            vmm_map_4k(base + i * PMM_PAGE_SIZE, phys, VMM_P | VMM_W | VMM_NX);
        }
        /* guard-страницу НЕ мапим: попадание туда = #PF (а не порча памяти) */
        vmm_unmap_4k(guard);
        g_kstack_used[s]  = 1;
        g_kstack_guard[s] = guard;
        g_kstack_size[s]  = pages * PMM_PAGE_SIZE;
        if (out_guard) *out_guard = (unsigned long)guard;
        return (void *)(uintptr_t)base;
    }
    return NULL;
}

void kstack_free(void *base, unsigned size) {
    if (!base) return;
    uint64_t b = (uint64_t)(uintptr_t)base;
    if (b < KSTACK_AREA) return;
    int s = (int)((b - KSTACK_AREA) / KSTACK_STRIDE);
    if (s < 0 || s >= KSTACK_SLOTS || !g_kstack_used[s]) return;
    uint64_t pages = (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = b + i * PMM_PAGE_SIZE;
        uint64_t pa = vmm_get_phys(va);
        if (pa) pmm_free_page(pa);
        vmm_unmap_4k(va);
    }
    g_kstack_used[s] = 0;
}

int kstack_is_guard(unsigned long addr) {
    uint64_t a = (uint64_t)addr;
    if (a < KSTACK_AREA || a >= KSTACK_AREA + KSTACK_SLOTS * KSTACK_STRIDE) return 0;
    int s = (int)((a - KSTACK_AREA) / KSTACK_STRIDE);
    if (!g_kstack_used[s]) return 0;
    return (a & ~(PMM_PAGE_SIZE - 1)) == g_kstack_guard[s];
}

/* v0.8.2: границы стека, которому принадлежит адрес (для backtrace в панике) */
int kstack_bounds(unsigned long addr, unsigned long *lo, unsigned long *hi) {
    uint64_t a = (uint64_t)addr;
    if (a < KSTACK_AREA || a >= KSTACK_AREA + KSTACK_SLOTS * KSTACK_STRIDE) return 0;
    int s = (int)((a - KSTACK_AREA) / KSTACK_STRIDE);
    if (s < 0 || s >= KSTACK_SLOTS || !g_kstack_used[s]) return 0;
    uint64_t base = g_kstack_guard[s] + PMM_PAGE_SIZE;
    if (lo) *lo = (unsigned long)base;
    if (hi) *hi = (unsigned long)(base + g_kstack_size[s]);
    return 1;
}
