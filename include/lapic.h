/* AresOS - LAPIC (M4): включение local APIC, EOI, таймер 100 Гц.
 * IOAPIC: перенаправление IRQ1/IRQ12 на LAPIC (иначе fallback на 8259). */
#ifndef ARES_LAPIC_H
#define ARES_LAPIC_H

#include <stdint.h>

int  lapic_init(void);            /* enable + откалибровать таймер (PIT ch2); 0 = fail */
int  lapic_active(void);          /* LAPIC-путь включён (меняет EOI в irq.c) */
void lapic_eoi(void);
void lapic_fallback_off(void);    /* уснуть при откате на PIC (иначе INTR не доходит!) */

int  ioapic_route_irq(uint32_t irq, uint8_t vector);  /* 0 = ioapic недоступен */

#endif
