/* AresOS - PIT 8254 channel 0: периодический тик (fallback M4).
 * v0.7.0: 1000 Гц (тик 1 мс) - основа плавных 60 fps: рендер-пэйсинг 16 мс.
 * 1193182 Гц / 1193 ~ 1000.15 Гц (ошибка 0.015% - часы не уплывут).
 * Используется, только если LAPIC недоступен. */
#include "io.h"

void pit_init_1000hz(void) {
    outb(0x43, 0x36);          /* ch0, lo/hi, mode 3 (square), binary */
    outb(0x40, 0xA9);          /* 1193 = 0x04A9 */
    outb(0x40, 0x04);
}

/* Текущий счётчик канала 0 (latch + чтение). Считает ДАЖЕ если IRQ0
 * никуда не доходит - позволяет отличить "PIT мёртв" от "доставка мертва". */
uint16_t pit_read_ch0(void) {
    outb(0x43, 0x00);          /* latch чтение счётчика ch0 */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}
