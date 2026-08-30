/* AresOS - PIT 8254 (канал 0): fallback системного тика, если LAPIC недоступен */
#ifndef ARES_PIT_H
#define ARES_PIT_H
#include <stdint.h>
void     pit_init_1000hz(void);  /* кв. IRQ0 (вектор 32) на 1000 Гц (тик = 1 мс) */
uint16_t pit_read_ch0(void);     /* latch-снимок счётчика ch0 (самодиагностика) */
#endif
