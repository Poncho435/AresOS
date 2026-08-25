/* AresOS — kprintf: мини-printf ядра.
 * Поддержка: %d %i %u %x %X %p %s %c %%, модификаторы l/ll, ширина с '0' и '-'.
 * Вывод дублируется: serial (всегда) + framebuffer-консоль (после её init). */
#include "kprintf.h"
#include "serial.h"
#include "fb_console.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

static void emit(char c) {
    serial_putc(c);
    if (fb_console_ready()) fb_console_putc(c);
}

static void emit_str(const char *s) {
    if (!s) s = "(null)";
    while (*s) emit(*s++);
}

static int ull_to_str(char *buf, uint64_t v, unsigned base, bool upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) {
        tmp[i++] = digits[v % base];
        v /= base;
    }
    int n = i;
    while (i > 0) *buf++ = tmp[--i];
    return n;
}

/* локальная, чтобы не тянуть string.h ради одного цикла */
static size_t strlen_min(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int kvprintf(const char *fmt, va_list ap) {
    int written = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(*fmt); written++; continue; }
        fmt++;
        bool left = false, zero = false, alt = false;
        int width = 0, longs = 0;
        while (*fmt == '-' || *fmt == '0' || *fmt == '#') {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else alt = true;                 /* '#' — альтернативная форма (0x… для hex) */
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        while (*fmt == 'l') { longs++; fmt++; }

        char num[32];
        int len = 0;
        bool neg = false;
        const char *str = NULL;
        char ch = 0;

        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            uint64_t uv = (uint64_t)v;
            if (v < 0) { neg = true; uv = (uint64_t)(-v); }
            len = ull_to_str(num, uv, 10, false);
            break;
        }
        case 'u': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            len = ull_to_str(num, v, 10, false);
            break;
        }
        case 'x': case 'X': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            if (alt) { emit('0'); emit(*fmt); written += 2; }
            len = ull_to_str(num, v, 16, *fmt == 'X');
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            emit_str("0x"); written += 2;
            len = ull_to_str(num, v, 16, false);
            break;
        }
        case 's': str = va_arg(ap, const char *); if (!str) str = "(null)"; break;
        case 'c': ch = (char)va_arg(ap, int); break;
        case '%': ch = '%'; break;
        default: ch = *fmt; break;  // неизвестный спецификатор — печатаем как есть
        }

        if (str) { emit_str(str); written += (int)strlen_min(str); continue; }
        if (ch) { emit(ch); written++; continue; }

        int pad = width - len - (neg ? 1 : 0);
        if (!left && pad > 0 && !zero) { while (pad--) { emit(' '); written++; } }
        if (neg) { emit('-'); written++; }
        if (!left && pad > 0 && zero) { while (pad--) { emit('0'); written++; } }
        for (int i = 0; i < len; i++) { emit(num[i]); written++; }
        if (left && pad > 0) { while (pad--) { emit(' '); written++; } }
    }
    return written;
}

int kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

__attribute__((noreturn)) void kpanic(const char *fmt, ...) {
    va_list ap;
    serial_write("\n*** KERNEL PANIC ***\n");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    serial_write("\n*** system halted ***\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}
