/* AresOS - текстовая консоль поверх framebuffer (8x8 шрифт) */
#ifndef ARES_FB_CONSOLE_H
#define ARES_FB_CONSOLE_H

#include "bootinfo.h"

void fb_console_init(const bootinfo_fb_t *fb);
int  fb_console_ready(void);
void fb_console_putc(char c);
void fb_console_write(const char *s);
void fb_console_detach(void);   /* v0.6.0: десктоп забрал экран - консоль больше не рисует */

#endif
