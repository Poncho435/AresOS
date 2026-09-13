/* AresOS - обработчик исключений: печать причины и всех регистров.
 * "Красивый крах" вместо мёртвого зависания - главный инструмент M2.
 * v0.2.5: декодирование error code, CR3/IDTR, классификация rip
 * (в т.ч. "rip внутри стека" = испорченный адрес возврата), канарейка. */
#include <stdint.h>
#include "kprintf.h"
#include "regs.h"
#include "fb_console.h"
#include "heap.h"

static const char *const exc_names[32] = {
    "Divide Error",            "Debug",                  "NMI",
    "Breakpoint",              "Overflow",               "Bound Range Exceeded",
    "Invalid Opcode",          "Device Not Available",   "Double Fault",
    "Coprocessor (legacy)",    "Invalid TSS",            "Segment Not Present",
    "Stack-Segment Fault",     "General Protection",     "Page Fault",
    "Reserved",                "x87 FP Exception",       "Alignment Check",
    "Machine Check",           "SIMD FP Exception",      "Virtualization",
    "Control Protection",      "Reserved",               "Reserved",
    "Reserved",                "Reserved",               "Reserved",
    "Reserved",                "Hypervisor Injection",   "VMM Communication",
    "Security",                "Reserved",
};

/* символы линкера/entry.S для классификации адресов */
extern char __kernel_start[], __kernel_end[];
extern char __text_start[], __text_end[];
extern char _stack_bottom[], _stack_top[], _stack_guard[];

static const char *region_of(uint64_t a) {
    if (kstack_is_guard(a))
        return "GUARD-СТРАНИЦА стека потока (ПЕРЕПОЛНЕНИЕ СТЕКА!)";
    if (a >= 0x600000000ULL && a < 0x640000000ULL)
        return "стек потока (зона kstack)";
    if (a >= 0x500000000ULL && a < 0x501000000ULL)
        return "куча (арена kmalloc)";
    if (a >= (uint64_t)_stack_bottom && a < (uint64_t)_stack_top)
        return "ВНУТРИ СТЕКА ядра (исполнение из стека - испорченный адрес возврата!)";
    if (a >= (uint64_t)__kernel_start && a < (uint64_t)__kernel_end)
        return "внутри образа ядра";
    if (a < 0x100000) return "нижняя память (<1 MiB)";
    return "вне ядра";
}

static void print_error_code(regs_t *r) {
    uint64_t e = r->error;
    kprintf("  error=%#lx", e);
    if (r->vector == 13 || r->vector == 8 || (r->vector >= 10 && r->vector <= 14) ||
        r->vector == 17 || r->vector == 21 || r->vector == 29 || r->vector == 30) {
        if (e) {
            kprintf(" [EXT=%lu IDT=%lu TI=%lu selector=%#lx]",
                    e & 1, (e >> 1) & 1, (e >> 2) & 1, e & ~7ULL);
            if ((e >> 1) & 1) {
                uint64_t idx = (e >> 3) & 0x1FFF;
                kprintf(" - указывает на IDT[%lu]", idx);
            }
        } else {
            kprintf(" [0: не сегментная - неканонический адрес / прив. инструкция / лимит]");
        }
    }
    if (r->vector == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("\n  cr2=%p (адрес, вызвавший #PF)  P=%lu W/R=%lu U/S=%lu RSV=%lu I/D=%lu",
                (void *)cr2, e & 1, (e >> 1) & 1, (e >> 2) & 1,
                (e >> 3) & 1, (e >> 4) & 1);
    }
    kprintf("\n");
}

void exception_handler(regs_t *r) {
    uint64_t cr3;
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("sidt %0" : "=m"(idtr));

    fb_console_emergency();      /* v0.6.2: дамп краха обязан попасть на экран */
    kprintf("\n*** CPU EXCEPTION %lu: %s ***\n", r->vector, exc_names[r->vector & 31]);
    print_error_code(r);
    kprintf("  rip=%#lx  cs=%#lx  rflags=%#lx\n", r->rip, r->cs, r->rflags);
    kprintf("  rsp=%#lx  ss=%#lx\n", r->rsp, r->ss);
    kprintf("  rax=%#lx rbx=%#lx rcx=%#lx rdx=%#lx\n", r->rax, r->rbx, r->rcx, r->rdx);
    kprintf("  rsi=%#lx rdi=%#lx rbp=%#lx\n", r->rsi, r->rdi, r->rbp);
    kprintf("  r8=%#lx r9=%#lx r10=%#lx r11=%#lx\n", r->r8, r->r9, r->r10, r->r11);
    kprintf("  r12=%#lx r13=%#lx r14=%#lx r15=%#lx\n", r->r12, r->r13, r->r14, r->r15);
    kprintf("  rip зона: %s\n", region_of(r->rip));
    kprintf("  rsp зона: %s\n", region_of(r->rsp));
    kprintf("  cr3=%#lx  IDT base=%#lx limit=%#lx\n", cr3, idtr.base, (uint64_t)idtr.limit);

    /* ---- v0.8.2: BACKTRACE ----
     * Frame pointer при -O2 выключен, поэтому идём по стеку СКАНОМ: любое
     * 8-байтное слово, попадающее в .text ядра, - кандидат в адрес возврата.
     * Этого достаточно, чтобы увидеть цепочку вызовов, съевшую стек. */
    {
        uint64_t lo = 0, hi = 0;
        if (kstack_bounds(r->rsp, &lo, &hi)) {
            kprintf("  backtrace (стек %#lx..%#lx, занято %lu из %lu Б):\n",
                    lo, hi, hi - r->rsp, hi - lo);
        } else {
            lo = r->rsp & ~0xFFFULL;
            hi = (uint64_t)_stack_top;
            if (r->rsp < (uint64_t)_stack_bottom || r->rsp >= hi) hi = lo + 0x4000;
            kprintf("  backtrace (скан %#lx..%#lx):\n", r->rsp, hi);
        }
        uint64_t tstart = (uint64_t)__text_start, tend = (uint64_t)__text_end;
        int shown = 0;
        for (uint64_t a = r->rsp; a < hi && shown < 12; a += 8) {
            if (a < lo) continue;
            uint64_t v = *(const uint64_t *)(uintptr_t)a;
            if (v >= tstart && v < tend) {
                kprintf("    [%2d] %#lx  (слот %#lx)\n", shown, v, a);
                shown++;
            }
        }
        if (!shown) kprintf("    (адресов возврата не найдено)\n");
    }

    /* канарейка под дном стека (v0.2.5): BROKEN = стек переполнен в BSS */
    const uint64_t *g = (const uint64_t *)_stack_guard;
    int ok = 1;
    for (int i = 0; i < 8; i++)
        if (g[i] != 0xA9E5C0FFEE15DA7AULL) ok = 0;
    /* v0.8.1: #PF по guard-странице = переполнение стека потока. Печатаем это
     * прямым текстом - раньше такой случай уходил в #DF -> triple fault и
     * выглядел как "VirtualBox молча перезагрузил ВМ". */
    if (r->vector == 14) {
        uint64_t cr2v;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2v));
        if (kstack_is_guard(cr2v))
            kprintf("  >>> ПЕРЕПОЛНЕНИЕ СТЕКА ПОТОКА: запись по %#lx попала в "
                    "guard-страницу.\n      Увеличь PROC_STACK в kernel/proc.c "
                    "или уменьши локальные буферы в цепочке вызовов.\n", cr2v);
    }

    kprintf("  stack canary: %s (guard[0]=%#lx, стек %lu КиБ, bottom=%p top=%p)\n",
            ok ? "OK - переполнения не было" : "*** BROKEN - СТЕК ПЕРЕПОЛНЕН! ***",
            g[0], (uint64_t)(_stack_top - _stack_bottom) >> 10,
            _stack_bottom, _stack_top);

    kpanic("unhandled CPU exception");
}
