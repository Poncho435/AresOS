/* AresOS — PIT 8254 channel 0: периодический тик 100 Гц (fallback M4).
 * 1193182 Гц / 11932 ≈ 100 Гц. Используется, только если LAPIC недоступен. */
#include "io.h"

void pit_init_100hz(void) {
    outb(0x43, 0x36);          /* ch0, lo/hi, mode 3 (square), binary */
    outb(0x40, 0x9C);          /* 11932 = 0x2E9C */
    outb(0x40, 0x2E);
}
