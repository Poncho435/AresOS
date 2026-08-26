/* AresOS — драйвер мыши PS/2 (IRQ12) */
#ifndef ARES_MOUSE_H
#define ARES_MOUSE_H

#include <stdint.h>

void mouse_init(void);
void mouse_irq_handler(void);
void mouse_set_limits(uint32_t max_x, uint32_t max_y);

int32_t mouse_x(void);
int32_t mouse_y(void);
int     mouse_left(void);    /* левая кнопка нажата */
int     mouse_moved(void);   /* был ли сдвиг с прошлого опроса (флаг сбрасывается) */
uint32_t mouse_irq_count(void); /* сколько IRQ12 доставлено (диагностика) */
/* v0.5.0: программное движение курсора (клавиатурный режим Ctrl+стрелки) */
void    mouse_nudge(int dx, int dy);

#endif
