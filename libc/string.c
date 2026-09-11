/* AresOS mini-libc - реализация базовых функций памяти/строк.
 * Без зависимостей от ОС (freestanding). GCC может сам генерировать вызовы
 * memcpy/memset - поэтому эти функции обязаны существовать всегда.
 * v0.8.0: побайтовые циклы заменены на 8-байтные слова - полноэкранный
 * сброс кадра (8+ МиБ при 1920x1080) стал в разы быстрее, курсор летает. */
#include <string.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t  b = (uint8_t)c;
    uint64_t w = b; w |= w << 8; w |= w << 16; w |= w << 32;
    while (n && ((uintptr_t)d & 7)) { *d++ = b; n--; }   /* выравнивание до 8 */
    while (n >= 64) {                                    /* развёрнутый ход */
        uint64_t *p = (uint64_t *)d;
        p[0] = w; p[1] = w; p[2] = w; p[3] = w;
        p[4] = w; p[5] = w; p[6] = w; p[7] = w;
        d += 64; n -= 64;
    }
    while (n >= 8) { uint64_t v; *(uint64_t *)d = w; (void)v; d += 8; n -= 8; }
    while (n--) *d++ = b;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    /* чтение невыровненного источника - только байтовым доступом (UB-safe) */
    while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
    while (n >= 32) {
        uint64_t a, b, c, e;
        memcpy(&a, s,      8); memcpy(&b, s + 8,  8);
        memcpy(&c, s + 16, 8); memcpy(&e, s + 24, 8);
        uint64_t *p = (uint64_t *)d;
        p[0] = a; p[1] = b; p[2] = c; p[3] = e;
        d += 32; s += 32; n -= 32;
    }
    while (n >= 8) { uint64_t a; memcpy(&a, s, 8); *(uint64_t *)d = a; d += 8; s += 8; n -= 8; }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i])
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}
