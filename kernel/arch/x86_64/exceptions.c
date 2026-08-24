/* AresOS — обработчик исключений: печать причины и всех регистров.
 * "Красивый крах" вместо мёртвого зависания — главный инструмент M2. */
#include <stdint.h>
#include "kprintf.h"
#include "regs.h"

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

void exception_handler(regs_t *r) {
    uint64_t cr2 = 0;
    if (r->vector == 14)
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));  /* адрес, вызвавший #PF */

    kprintf("\n*** CPU EXCEPTION %lu: %s (error %#lx) ***\n",
            r->vector, exc_names[r->vector & 31], r->error);
    kprintf("  rip=%p  cs=%#lx  rflags=%#lx\n", (void *)r->rip, r->cs, r->rflags);
    kprintf("  rsp=%p  ss=%#lx\n", (void *)r->rsp, r->ss);
    if (r->vector == 14)
        kprintf("  cr2=%p (faulting address)\n", (void *)cr2);
    kprintf("  rax=%#lx rbx=%#lx rcx=%#lx rdx=%#lx\n", r->rax, r->rbx, r->rcx, r->rdx);
    kprintf("  rsi=%#lx rdi=%#lx rbp=%#lx\n", r->rsi, r->rdi, r->rbp);
    kprintf("  r8=%#lx r9=%#lx r10=%#lx r11=%#lx\n", r->r8, r->r9, r->r10, r->r11);
    kprintf("  r12=%#lx r13=%#lx r14=%#lx r15=%#lx\n", r->r12, r->r13, r->r14, r->r15);

    kpanic("unhandled CPU exception");
}
