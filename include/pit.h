/* AresOS - PIT 8254 (канал 0): fallback системного тика, если LAPIC недоступен */
#ifndef ARES_PIT_H
#define ARES_PIT_H
void pit_init_100hz(void);   /* кв. IRQ0 (вектор 32) на 100 Гц */
#endif
