/* AresOS — gfx: минимальная графическая библиотека ядра. */
#include "gfx.h"
#include "../drivers/font8x8.h"
#include <string.h>

static bootinfo_fb_t g_fb;
static int g_ready;

/* Упаковка цвета по UEFI-семантике байтов (little-endian u32):
 *   FB_FORMAT_RGB = PixelRedGreenBlue: байт0=R → u32 = R | G<<8 | B<<16
 *   FB_FORMAT_BGR = PixelBlueGreenRed: байт0=B → u32 = B | G<<8 | R<<16 */
static uint32_t pack(gfx_color_t c) {
    if (g_fb.format == FB_FORMAT_RGB)
        return ((uint32_t)c.b << 16) | ((uint32_t)c.g << 8) | c.r;
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

static inline uint32_t *fbp(void) {
    return (uint32_t *)(uintptr_t)g_fb.phys_base;
}

void gfx_init(const bootinfo_fb_t *fb) {
    if (!fb || !fb->phys_base) return;
    if (fb->width < 64 || fb->height < 64 || !fb->pitch) return;  /* страховка */
    g_fb = *fb;
    g_ready = 1;
}

int gfx_ready(void) { return g_ready; }
uint32_t gfx_width(void) { return g_fb.width; }
uint32_t gfx_height(void) { return g_fb.height; }

void gfx_pixel(uint32_t x, uint32_t y, gfx_color_t c) {
    if (!g_ready || x >= g_fb.width || y >= g_fb.height) return;
    fbp()[(uint64_t)y * g_fb.pitch + x] = pack(c);
}

void gfx_pixel_xor(uint32_t x, uint32_t y, uint8_t mask) {
    if (!g_ready || !mask || x >= g_fb.width || y >= g_fb.height) return;
    fbp()[(uint64_t)y * g_fb.pitch + x] ^= 0xFFFFFF;
}

void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, gfx_color_t c) {
    if (!g_ready) return;
    uint32_t px = pack(c);
    uint32_t *fb = fbp();
    if (x + w > g_fb.width)  w = (x < g_fb.width)  ? g_fb.width - x : 0;
    if (y + h > g_fb.height) h = (y < g_fb.height) ? g_fb.height - y : 0;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t *line = fb + (uint64_t)(y + row) * g_fb.pitch;
        for (uint32_t col = 0; col < w; col++)
            line[x + col] = px;
    }
}

void gfx_gradient_v_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         gfx_color_t top, gfx_color_t bottom) {
    if (!g_ready || h == 0) return;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t t = row * 255 / (h ? h : 1);
        gfx_color_t c = {
            (uint8_t)(top.r + ((int)bottom.r - top.r) * (int)t / 255),
            (uint8_t)(top.g + ((int)bottom.g - top.g) * (int)t / 255),
            (uint8_t)(top.b + ((int)bottom.b - top.b) * (int)t / 255),
        };
        uint32_t px = pack(c);
        if (y + row >= g_fb.height) break;
        uint32_t *line = fbp() + (uint64_t)(y + row) * g_fb.pitch;
        uint32_t ww = (x + w > g_fb.width) ? (g_fb.width > x ? g_fb.width - x : 0) : w;
        for (uint32_t col = 0; col < ww; col++)
            line[x + col] = px;
    }
}

void gfx_frame_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, gfx_color_t c) {
    gfx_fill_rect(x, y, w, 1, c);
    gfx_fill_rect(x, y + h - 1, w, 1, c);
    gfx_fill_rect(x, y, 1, h, c);
    gfx_fill_rect(x + w - 1, y, 1, h, c);
}

void gfx_text(uint32_t x, uint32_t y, const char *s, gfx_color_t fg) {
    if (!g_ready) return;
    uint32_t px = pack(fg);
    for (; *s; s++, x += 8) {
        const uint8_t *glyph = font8x8_basic[(uint8_t)*s & 0x7F];
        for (uint32_t gy = 0; gy < 8; gy++) {
            if (y + gy >= g_fb.height) break;
            uint8_t bits = glyph[gy];
            if (!bits) continue;
            uint32_t *line = fbp() + (uint64_t)(y + gy) * g_fb.pitch;
            for (uint32_t gx = 0; gx < 8; gx++)
                if ((bits >> gx) & 1 && x + gx < g_fb.width)
                    line[x + gx] = px;
        }
    }
}

void gfx_text_shadow(uint32_t x, uint32_t y, const char *s, gfx_color_t fg, gfx_color_t shadow) {
    gfx_text(x + 1, y + 1, s, shadow);
    gfx_text(x, y, s, fg);
}
