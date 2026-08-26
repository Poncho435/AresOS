/* AresOS - port I/O (x86-64) */
#ifndef ARES_IO_H
#define ARES_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("out %0, %1" :: "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("out %0, %1" :: "a"(val), "Nd"(port));
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("out %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("in %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("in %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("in %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
/* маленькая пауза для древнего железа */
static inline void io_wait(void) { outb(0x80, 0); }

#endif
