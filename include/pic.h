/* AresOS — программирование legacy PIC 8259 (пока нет APIC; M4 заменит) */
#ifndef ARES_PIC_H
#define ARES_PIC_H

#include <stdint.h>

void pic_remap(void);
void pic_set_mask(uint8_t master, uint8_t slave);
void pic_eoi(int vector);

#endif
