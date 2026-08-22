/* AresOS — COM1 (0x3F8), 115200 8N1.
 * Пишется в терминал при запуске QEMU с `-serial stdio`.
 * Работает опросом (polling), без прерываний — просто и надёжно для логов. */
#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);  // выключить прерывания
    outb(COM1 + 3, 0x80);  // DLAB=1 — доступ к делителю скорости
    outb(COM1 + 0, 0x01);  // делитель = 1 → 115200 бод
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);  // 8 бит, без чётности, 1 стоп-бит
    outb(COM1 + 2, 0xC7);  // FIFO включён, сброшен, порог 14 байт
    outb(COM1 + 4, 0x0B);  // DTR + RTS + OUT2
}

static int serial_tx_ready(void) {
    return inb(COM1 + 5) & 0x20;  // THR empty
}

void serial_putc(char c) {
    while (!serial_tx_ready()) {}
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');  // CRLF для терминалов
        serial_putc(*s++);
    }
}
