/* AresOS — диспетчер аппаратных прерываний (IRQ 0..15 → вектора 32..47)
 * v0.3.1: sanity-проверка ВОЗВРАТНОГО кадра — защита от «iretq в мусор»,
 * которым VBox/NEM поражает нас после первого IRQ12 (rip летел в .bss). */
#include "regs.h"
#include "pic.h"
#include "mouse.h"
#include "kprintf.h"

#define IRQ_MOUSE_VECTOR 44   /* IRQ12 (slave) + база 0x20 */

extern char __text_start[], __text_end[];

static int frame_sane(const regs_t *r) {
    /* кадр iretq должен указывать в наш .text и наши селекторы */
    if (r->rip >= (uint64_t)__text_start && r->rip < (uint64_t)__text_end &&
        r->cs == 0x08 && r->ss == 0x10 && (r->rflags & 2))
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
    case IRQ_MOUSE_VECTOR:
        mouse_irq_handler();
        break;
    default:
        /* спурьё и неподключённые IRQ игнорируем */
        break;
    }
    pic_eoi((int)r->vector);

    /* ПОСЛЕ-обработчика: если кадр испорчен во время обработки — не iretq-имся в мусор */
    if (!frame_sane(r)) {
        kprintf("\n[irq] !!! КАДР ИСПОРЧЕН ВО ОБРАБОТЧИКЕ: vec=%lu rip=%#lx cs=%#lx ss=%#lx\n",
                r->vector, r->rip, r->cs, r->ss);
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }
}
