/* AresOS — текстовая консоль поверх линейного framebuffer от GOP.
 * Шрифт 8x8, скролл переносом памяти. Цвета: светло-серый по тёмно-синему. */
#include "fb_console.h"
#include "fontex.h"
#include <string.h>
#include <stdint.h>

#define GLYPH_W 8
#define GLYPH_H 8
#define FG_R 0xE6
#define FG_G 0xE6
#define FG_B 0xE6
#define BG_R 0x10
#define BG_G 0x14
#define BG_B 0x22   /* тёмно-синий "темinal" AresOS */

static bootinfo_fb_t g_fb;
static int    g_ready;
static uint32_t g_cols, g_rows;
static uint32_t g_cur_x, g_cur_y;
static uint32_t g_fg, g_bg;

static uint32_t make_pixel(uint8_t r, uint8_t g, uint8_t b) {
    /* UEFI-семантика байтов: RGB → байт0=R (u32 = R|G<<8|B<<16);
     * BGR → байт0=B (u32 = B|G<<8|R<<16). */
    if (g_fb.format == FB_FORMAT_RGB)
        return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t *fb_ptr(void) {
    return (uint32_t *)(uintptr_t)g_fb.phys_base;  /* identity-map действует до M3 */
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t *fb = fb_ptr();
    for (uint32_t row = y; row < y + h; row++) {
        uint32_t *line = fb + (uint64_t)row * g_fb.pitch;
        for (uint32_t col = x; col < x + w; col++) line[col] = color;
    }
}

void fb_console_init(const bootinfo_fb_t *fb) {
    if (!fb || !fb->phys_base) return;  // нет графики — остаётся serial
    /* защита от нулевой/мусорной геометрии (иначе scroll в бесконечность) */
    if (fb->width < GLYPH_W * 2 || fb->height < GLYPH_H * 4 || !fb->pitch) {
        return;
    }
    g_fb = *fb;
    g_cols = g_fb.width / GLYPH_W;
    g_rows = g_fb.height / GLYPH_H;
    g_cur_x = g_cur_y = 0;
    g_fg = make_pixel(FG_R, FG_G, FG_B);
    g_bg = make_pixel(BG_R, BG_G, BG_B);
    fill_rect(0, 0, g_fb.width, g_fb.height, g_bg);
    g_ready = 1;
}

int fb_console_ready(void) { return g_ready; }

static void scroll(void) {
    uint32_t *fb = fb_ptr();
    uint64_t row_bytes = (uint64_t)g_fb.pitch * 4;
    uint64_t move_rows = (uint64_t)(g_rows - 1) * GLYPH_H;
    memmove(fb, fb + (uint64_t)GLYPH_H * g_fb.pitch, move_rows * row_bytes);
    fill_rect(0, (g_rows - 1) * GLYPH_H, g_fb.width, GLYPH_H, g_bg);
}

static void newline(void) {
    g_cur_x = 0;
    if (++g_cur_y >= g_rows) {
        g_cur_y = g_rows - 1;
        scroll();
    }
}

static void draw_glyph_slot(int slot, uint32_t cx, uint32_t cy) {
    const uint8_t *glyph = fontex_glyph(slot);
    uint32_t *fb = fb_ptr();
    uint32_t px = cx * GLYPH_W, py = cy * GLYPH_H;
    for (uint32_t y = 0; y < GLYPH_H; y++) {
        uint8_t bits = glyph[y];
        uint32_t *line = fb + (uint64_t)(py + y) * g_fb.pitch;
        for (uint32_t x = 0; x < GLYPH_W; x++)
            line[px + x] = (bits >> x) & 1 ? g_fg : g_bg;
    }
}
static void draw_glyph(char c, uint32_t cx, uint32_t cy) {
    draw_glyph_slot((uint8_t)c & 0x7F, cx, cy);
}

void fb_console_putc(char c) {
    static int g_utf;                      /* состояние UTF-8 между байтами */
    if (!g_ready) return;
    if (c == '\n') { newline(); return; }
    if (c == '\r') { g_cur_x = 0; return; }
    if (c == '\t') {
        do { fb_console_putc(' '); } while (g_cur_x % 8 != 0);
        return;
    }
    if (c == '\b') {
        if (g_cur_x) g_cur_x--;
        draw_glyph(' ', g_cur_x, g_cur_y);
        return;
    }
    int slot = fontex_slot(&g_utf, (uint8_t)c);
    if (slot < 0) return;                  /* половина UTF-8 — ждём второй байт */
    draw_glyph_slot(slot, g_cur_x, g_cur_y);
    if (++g_cur_x >= g_cols) newline();
}

void fb_console_write(const char *s) {
    while (*s) fb_console_putc(*s++);
}
