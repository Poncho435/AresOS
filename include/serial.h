/* AresOS - последовательный порт COM1 (главный канал отладки ядра) */
#ifndef ARES_SERIAL_H
#define ARES_SERIAL_H

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

#endif
