/* AresOS — диспетчер аппаратных прерываний (IRQ 0..15 → вектора 32..47) */
#include "regs.h"
#include "pic.h"
#include "mouse.h"
#include "kprintf.h"

#define IRQ_MOUSE_VECTOR 44   /* IRQ12 (slave) + база 0x20 */

void irq_dispatch(regs_t *r) {
    switch (r->vector) {
    case IRQ_MOUSE_VECTOR:
        mouse_irq_handler();
        break;
    default:
        /* спурьё и неподключённые IRQ игнорируем — лог только для загадочных */
        break;
    }
    pic_eoi((int)r->vector);
}
