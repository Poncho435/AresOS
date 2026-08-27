/* AresOS - PIT 8254 channel 0: периодический тик 100 Гц (fallback M4).
 * 1193182 Гц / 11932 ~ 100 Гц. Используется, только если LAPIC недоступен. */
#include "io.h"

void pit_init_100hz(void) {
    outb(0x43, 0x36);          /* ch0, lo/hi, mode 3 (square), binary */
    outb(0x40, 0x9C);          /* 11932 = 0x2E9C */
    outb(0x40, 0x2E);
}

/* Текущий счётчик канала 0 (latch + чтение). Считает ДАЖЕ если IRQ0
 * никуда не доходит - позволяет отличить "PIT мёртв" от "доставка мертва". */
uint16_t pit_read_ch0(void) {
    outb(0x43, 0x00);          /* latch чтение счётчика ch0 */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}
