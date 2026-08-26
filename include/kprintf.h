/* AresOS - форматированный вывод ядра (serial + консоль framebuffer) */
#ifndef ARES_KPRINTF_H
#define ARES_KPRINTF_H

#include <stdarg.h>

int kprintf(const char *fmt, ...);
int kvprintf(const char *fmt, va_list ap);

/* паника ядра: печать + остановка навсегда */
__attribute__((noreturn)) void kpanic(const char *fmt, ...);

#endif
