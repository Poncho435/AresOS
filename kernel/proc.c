/* AresOS — планировщик (M5): round-robin, вытеснение тиком таймера.
 * Кадры одинаковой формы (irq_common): переключаемся подменой RSP перед
 * «pop/iretq» — idt_stubs.S читает g_sched_new_rsp/g_sched_old_rsp_slot. */
#include "proc.h"
#include "heap.h"
#include "kprintf.h"
#include <string.h>

#define MAX_PROC 32
#define PROC_STACK 32768       /* 32 КиБ (v0.5.1: было 16 — pe_demo+kprintf глубокие) */
#define HZ 100            /* таймер 100 Гц → тик 10 мс */
#define TIMESLICE 4       /* 40 мс квант */

/* читает irq_common (idt_stubs.S) — НЕ static! */
uint64_t  g_sched_new_rsp;
uint64_t *g_sched_old_rsp_slot;

typedef struct {
    int      id, state, flags;
    char     name[16];
    uint64_t rsp;           /* сохранённый указатель стека при вытеснении */
    uint64_t stack_base;
    uint64_t ticks, wake_tick;
    proc_fn  fn;
    void    *arg;
} proc_t;

static proc_t g_p[MAX_PROC];
static int    g_n = 1, g_cur;
static uint64_t g_ticks;

static void proc_trampoline(void);

int proc_init(void) {
    memset(g_p, 0, sizeof(g_p));
    g_n = 1;                       /* критично: сбрасываем ВСЁ, не только таблицу */
    g_cur = 0;
    g_ticks = 0;
    g_sched_new_rsp = 0;
    g_sched_old_rsp_slot = 0;
    g_p[0].id = 0;
    g_p[0].state = PROC_RUNNING;
    strcpy(g_p[0].name, "idle");
    return 0;
}

int proc_spawn(proc_fn fn, void *arg, const char *name, int flags) {
    if (g_n >= MAX_PROC) return -1;
    proc_t *p = &g_p[g_n];
    memset(p, 0, sizeof(*p));
    p->id = g_n;
    p->state = PROC_READY;
    p->flags = flags;
    {   /* bounded copy: strncpy в нашей mini-libc нет */
        size_t i = 0;
        while (i < sizeof(p->name) - 1 && name[i]) { p->name[i] = name[i]; i++; }
        p->name[i] = 0;
    }
    p->fn = fn;
    p->arg = arg;
    p->stack_base = (uint64_t)kmalloc(PROC_STACK);
    if (!p->stack_base) { kprintf("[proc] spawn '%s': нет памяти под стек\n", name); return -1; }

    /* строим фейковый кадр прерывания в вершине стека — точно как irq_common:
     * [r15..rax][vector][error][rip cs rflags rsp ss] */
    uint64_t top = (p->stack_base + PROC_STACK) & ~0xFULL;
    uint64_t *st = (uint64_t *)(uintptr_t)(top - 22 * 8);
    /* st[0..14] = GPR = 0 */
    st[15] = 0;                              /* vector */
    st[16] = 0;                              /* error */
    st[17] = (uint64_t)proc_trampoline;      /* rip */
    st[18] = 0x08;                           /* cs = kernel code */
    st[19] = 0x202;                          /* rflags: IF=1 */
    st[20] = top;                            /* rsp */
    st[21] = 0x10;                           /* ss = kernel data */
    p->rsp = (uint64_t)(uintptr_t)st;

    kprintf("[proc] spawn #%d '%s'%s → стек %lu KiB @ %#lx\n",
            p->id, name, flags & PROC_F_BACKGROUND ? " [bg]" : "",
            (uint64_t)PROC_STACK >> 10, p->stack_base);
    return g_n++;
}

/* вызывается как ПЕРВЫЙ код нового потока (через фейковый iretq-кадр) */
static void proc_trampoline(void) {
    proc_t *me = &g_p[g_cur];
    me->state = PROC_RUNNING;
    me->fn(me->arg);
    me->state = PROC_DEAD;
    kprintf("[proc] #%d '%s' завершился\n", me->id, me->name);
    for (;;) __asm__ volatile ("hlt");   /* DEAD потоки тик не планирует */
}

int proc_list(proc_info_t *out, int max) {
    int n = g_n < max ? g_n : max;
    for (int i = 0; i < n; i++) {
        out[i].id = g_p[i].id;
        memcpy(out[i].name, g_p[i].name, 16);
        out[i].state = g_p[i].state;
        out[i].flags = g_p[i].flags;
        out[i].ticks = g_p[i].ticks;
    }
    return n;
}

const char *proc_state_name(int s) {
    switch (s) {
    case PROC_READY:   return "готов ";
    case PROC_RUNNING: return "работ.";
    case PROC_SLEEP:   return "спит  ";
    default:           return "мёртв ";
    }
}

uint64_t sched_ticks(void) { return g_ticks; }

void proc_sleep(uint32_t ms) {
    g_p[g_cur].wake_tick = g_ticks + (ms + 9) / 10;
    g_p[g_cur].state = PROC_SLEEP;
    while (g_p[g_cur].state == PROC_SLEEP)
        __asm__ volatile ("hlt");
}

void proc_yield(void) { proc_sleep(10); }

void sched_tick(void) {
    g_ticks++;
    proc_t *cur = &g_p[g_cur];
    cur->ticks++;

    /* будим проспавших */
    for (int i = 0; i < g_n; i++)
        if (g_p[i].state == PROC_SLEEP && g_ticks >= g_p[i].wake_tick)
            g_p[i].state = PROC_READY;

    /* квант не вышел — текущий продолжает (правило одно для всех) */
    if (cur->state == PROC_RUNNING && (cur->ticks % TIMESLICE) != 0)
        return;
    if (cur->state == PROC_RUNNING) cur->state = PROC_READY;

    for (int k = 1; k <= g_n; k++) {
        int i = (g_cur + k) % g_n;
        if (g_p[i].state == PROC_READY) {
            if (i == g_cur) { g_p[i].state = PROC_RUNNING; return; }
            g_sched_old_rsp_slot = &g_p[g_cur].rsp;
            g_p[i].state = PROC_RUNNING;
            g_sched_new_rsp = g_p[i].rsp;     /* irq_common подменит RSP */
            g_cur = i;
            return;
        }
    }
    /* Некуда переключаться. Текущий SLEEP/DEAD — оставляем как есть
     * (его разбудит wake-scan на будущих тиках). Force-RUNNING тут был бы
     * ранним пробуждением: проспавший увидел бы RUNNING и вышел бы из
     * proc_sleep раньше срока. READY — возвращаем в RUNNING сами. */
    if (cur->state == PROC_READY) cur->state = PROC_RUNNING;
}
