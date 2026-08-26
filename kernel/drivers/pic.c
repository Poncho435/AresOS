/* AresOS - legacy PIC (8259A): ремап векторов 0x20/0x28, маски, EOI.
 * Временно: на M4 заменяется связкой LAPIC/IOAPIC. */
#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI_CMD 0x20

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

void pic_remap(void) {
    /* ICW1: начать инициализацию, будет ICW4 */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    /* ICW2: базовые векторы - master 0x20 (32), slave 0x28 (40) */
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    /* ICW3: master - slave на IRQ2; slave - cascade identity 2 */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    /* ICW4: режим 8086 */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();
    /* по умолчанию - всё замаскировано */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_set_mask(uint8_t master, uint8_t slave) {
    outb(PIC1_DATA, master);
    outb(PIC2_DATA, slave);
}

void pic_eoi(int vector) {
    if (vector >= 0x28)
        outb(PIC2_CMD, PIC_EOI_CMD);   /* IRQ8..15 живут на slave */
    outb(PIC1_CMD, PIC_EOI_CMD);
}
