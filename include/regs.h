/* AresOS - кадр регистров при исключении/IRQ (совпадает с push-порядком стабов) */
#ifndef ARES_REGS_H
#define ARES_REGS_H

#include <stdint.h>

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} regs_t;

#endif
