/* AresOS — TESTPE.EXE: первое тестовое PE-приложение (M3/M8-пробник).
 *
 * Собирается в плоский бинарник (tools/mkpe.py заворачивает его в PE32+),
 * вшивается в ядро и запускается PE-загрузчиком ядра (kernel/pe.c).
 *
 * ABI-контракт этого этапа (prototype): SysV — ctx приходит в RDI, т.к. и ядро,
 * и приложение собраны gcc. Настоящий WinAPI-слой (M8) даст MS x64 ABI.
 * Глобальных данных нет → нет ни одной абсолютной релокации: код исполняется
 * по любому ImageBase (проверка ImageBase-независимости — часть теста).
 */
#include <stdint.h>

typedef struct {
    uint64_t fb_base;   /* физадрес framebuffer (identity-mapped) */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;     /* пикселей на строку */
    uint32_t format;    /* 0 = байт0=R (RGB), 1 = байт0=B (BGR) */
    uint32_t magic;     /* 'ARES' — контроль контракта */
    uint32_t draw_x;    /* куда рисовать шахматный квадрат 96x96 */
    uint32_t draw_y;
} ares_api_t;

#define ARES_MAGIC 0x41524553u     /* 'ARES' */
#define TEST_OK    0x0000A2E5      /* успех: число видно в окне десктопа */

__attribute__((used))
int ares_main(const ares_api_t *api) {
    if (!api || api->magic != ARES_MAGIC) return -1;
    if (!api->fb_base || !api->pitch || !api->width || api->format > 1) return -2;

    uint32_t *fb = (uint32_t *)(uintptr_t)api->fb_base;
    for (uint32_t dy = 0; dy < 96; dy++)
        for (uint32_t dx = 0; dx < 96; dx++) {
            uint32_t x = api->draw_x + dx - 32;
            uint32_t y = api->draw_y + dy - 32;
            if (x >= api->width || y >= api->height) continue;
            uint8_t r, g, b;
            if (((dx / 12) + (dy / 12)) & 1) { r = 0x28; g = 0x2C; b = 0x48; }   /* клетка */
            else                             { r = 0xFF; g = 0x9E; b = 0x49; }   /* оранж AresOS */
            /* упаковка по UEFI-семантике байтов (как в ядре) */
            uint32_t v = api->format
                       ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | b   /* BGR: байт0=B */
                       : ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;  /* RGB: байт0=R */
            fb[(uint64_t)y * api->pitch + x] = v;
        }
    return TEST_OK;
}
