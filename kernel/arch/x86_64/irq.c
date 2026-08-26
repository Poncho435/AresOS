/* AresOS — диспетчер аппаратных прерываний.
 * v0.3.1: sanity-кадра; v0.4.0 (M4/M5): вектора
 *   0x20 (32) PIT-тик (fallback), 0x21 (33) клавиатура, 0x2C (44) мышь,
 *   0x40 (64) LAPIC-таймер → sched_tick (планировщик может сменить поток!).
 * EOI: по активной модели (IOAPIC/LAPIC или legacy PIC). */
#include "regs.h"
#include "pic.h"
#include "mouse.h"
#include "keyboard.h"
#include "lapic.h"
#include "proc.h"
#include "kprintf.h"

#define IRQ_PIT_VECTOR   32
#define IRQ_KBD_VECTOR   33
#define IRQ_MOUSE_VECTOR 44
#define VECTOR_LAPIC_TMR 64

extern char __text_start[], __text_end[];

/* куда может указывать rip в момент прерывания:
 *  1) текст ядра [__text_start..__text_end)
 *  2) область тестового PE-приложения 0x400000000.. (TESTPE.EXE исполняется
 *     в ring0 — легальный кадр; v0.5.0 детектор ошибочно считал его мусором
 *     и вешал систему прямо посреди PE-теста) */
#define PE_CODE_LO 0x400000000ULL
#define PE_CODE_HI 0x408000000ULL

static int frame_sane(const regs_t *r) {
    int rip_ok = (r->rip >= (uint64_t)__text_start && r->rip < (uint64_t)__text_end) ||
                 (r->rip >= PE_CODE_LO && r->rip < PE_CODE_HI);
    if (rip_ok && r->cs == 0x08 && r->ss == 0x10 && (r->rflags & 2))
        return 1;
    return 0;
}

void irq_dispatch(regs_t *r) {
    if (!frame_sane(r)) {
        kprintf("\n[irq] !!! ИСПОРЧЕННЫЙ КАДР: vec=%lu rip=%#lx cs=%#lx ss=%#lx rflags=%#lx\n",
                r->vector, r->rip, r->cs, r->ss, r->rflags);
        kprintf("[irq] iretq НЕ выполняем — разбор у разработчика (фото сюда).\n");
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }

    switch (r->vector) {
    case IRQ_PIT_VECTOR:                      /* PIT-тик (fallback-путь) */
        pic_eoi((int)r->vector);
        sched_tick();
        return;
    case IRQ_KBD_VECTOR:
        keyboard_irq_handler();
        break;
    case IRQ_MOUSE_VECTOR:
        mouse_irq_handler();
        break;
    case VECTOR_LAPIC_TMR:                    /* LAPIC-таймер → планировщик */
        lapic_eoi();
        sched_tick();
        return;
    default:
        break;
    }

    if (lapic_active()) lapic_eoi();
    else pic_eoi((int)r->vector);

    /* ПОСЛЕ-обработчика: если кадр испорчен во время обработки — не iretq-имся в мусор */
    if (!frame_sane(r)) {
        kprintf("\n[irq] !!! КАДР ИСПОРЧЕН ВО ОБРАБОТЧИКЕ: vec=%lu rip=%#lx cs=%#lx ss=%#lx\n",
                r->vector, r->rip, r->cs, r->ss);
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }
}
