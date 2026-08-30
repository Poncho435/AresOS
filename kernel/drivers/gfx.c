/* AresOS - gfx: минимальная графическая библиотека ядра. */
#include "gfx.h"
#include "fontex.h"
#include <string.h>

static bootinfo_fb_t g_fb;
static int g_ready;

/* v0.6.0: двойная буферизация. g_target != NULL -> весь вывод идёт в
 * RAM-буфер (pitch = g_tw), на экран выливается региона через gfx_flush.
 * Рендер в RAM быстрый (видеопамять uncached), мерцания нет в принципе. */
static uint32_t *g_target;
static uint32_t  g_tw, g_th;

/* Упаковка цвета по UEFI-семантике байтов (little-endian u32):
 *   FB_FORMAT_RGB = PixelRedGreenBlue: байт0=R -> u32 = R | G<<8 | B<<16
 *   FB_FORMAT_BGR = PixelBlueGreenRed: байт0=B -> u32 = B | G<<8 | R<<16 */
static uint32_t pack(gfx_color_t c) {
    if (g_fb.format == FB_FORMAT_RGB)
        return ((uint32_t)c.b << 16) | ((uint32_t)c.g << 8) | c.r;
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

static inline uint32_t *fbp(void) {
    return (uint32_t *)(uintptr_t)g_fb.phys_base;
}

/* строка текущей цели рендера (RAM-буфер или сам экран) */
static inline uint32_t *lineptr(uint32_t y) {
    if (g_target) return g_target + (uint64_t)y * g_tw;
    return fbp() + (uint64_t)y * g_fb.pitch;
}

/* распаковка пикселя обратно в цвет (инверсия pack) */
static inline gfx_color_t unpack(uint32_t px) {
    gfx_color_t c;
    if (g_fb.format == FB_FORMAT_RGB) {
        c.r = (uint8_t)(px & 0xFF); c.g = (uint8_t)((px >> 8) & 0xFF); c.b = (uint8_t)((px >> 16) & 0xFF);
    } else {
        c.b = (uint8_t)(px & 0xFF); c.g = (uint8_t)((px >> 8) & 0xFF); c.r = (uint8_t)((px >> 16) & 0xFF);
    }
    return c;
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
    lineptr(y)[x] = pack(c);
}

void gfx_pixel_xor(uint32_t x, uint32_t y, uint8_t mask) {
    if (!g_ready || !mask || x >= g_fb.width || y >= g_fb.height) return;
    fbp()[(uint64_t)y * g_fb.pitch + x] ^= 0xFFFFFF;
}

void gfx_fill_rect(uint32_t x_, uint32_t y_, uint32_t w_, uint32_t h_, gfx_color_t c) {
    if (!g_ready) return;
    /* v0.6.3: КРАХ "окно за краем" - координаты приходят отрицательными
     * (тень/рамка окна при x<0), а в uint32 они оборачиваются в ~4 ГиБ и
     * проверка границ пропускала запись МИМО буфера (порча кучи -> паника).
     * Клип строго в int64 до любой адресной арифметики. */
    int64_t x = (int32_t)x_, y = (int32_t)y_;
    int64_t w = w_, h = h_;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int64_t)g_fb.width)  w = (int64_t)g_fb.width - x;
    if (y + h > (int64_t)g_fb.height) h = (int64_t)g_fb.height - y;
    if (w <= 0 || h <= 0) return;
    uint32_t px = pack(c);
    uint64_t px2 = ((uint64_t)px << 32) | px;      /* заливаем парами - в 2 раза быстрее */
    for (int64_t row = 0; row < h; row++) {
        uint32_t *p = lineptr((uint32_t)(y + row)) + x;
        int64_t left = w;
        /* v0.6.2: нечётная голова - одиночной записью, дальше u64 выровнен */
        if ((uintptr_t)p & 4) { *p++ = px; left--; }
        while (left >= 2) { *(uint64_t *)(void *)p = px2; p += 2; left -= 2; }
        if (left) *p = px;
    }
}

void gfx_gradient_v_rect(uint32_t x_, uint32_t y_, uint32_t w_, uint32_t h_,
                         gfx_color_t top, gfx_color_t bottom) {
    if (!g_ready || h_ == 0) return;
    int64_t x = (int32_t)x_, y = (int32_t)y_;   /* v0.6.3: клип в int64 */
    int64_t w = w_, h = h_;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int64_t)g_fb.width)  w = (int64_t)g_fb.width - x;
    if (y + h > (int64_t)g_fb.height) h = (int64_t)g_fb.height - y;
    if (w <= 0 || h <= 0) return;
    for (int64_t row = 0; row < h; row++) {
        uint32_t t = (uint32_t)row * 255 / (uint32_t)h;
        gfx_color_t c = {
            (uint8_t)(top.r + ((int)bottom.r - top.r) * (int)t / 255),
            (uint8_t)(top.g + ((int)bottom.g - top.g) * (int)t / 255),
            (uint8_t)(top.b + ((int)bottom.b - top.b) * (int)t / 255),
        };
        uint32_t px = pack(c);
        uint32_t *line = lineptr((uint32_t)(y + row)) + x;
        for (int64_t col = 0; col < w; col++)
            line[col] = px;
    }
}

void gfx_frame_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, gfx_color_t c) {
    gfx_fill_rect(x, y, w, 1, c);
    gfx_fill_rect(x, y + h - 1, w, 1, c);
    gfx_fill_rect(x, y, 1, h, c);
    gfx_fill_rect(x + w - 1, y, 1, h, c);
}

void gfx_text(uint32_t x_, uint32_t y_, const char *s, gfx_color_t fg) {
    if (!g_ready) return;
    uint32_t px = pack(fg);
    /* v0.6.3: окно может быть за левым/верхним краем - считаем в int64,
     * иначе отрицательный x оборачивается и портит чужие строки */
    int64_t x = (int32_t)x_;
    int64_t y = (int32_t)y_;
    int st = 0;
    for (; *s; s++, x += 8) {
        int slot = fontex_slot(&st, (uint8_t)*s);
        if (slot < 0) { x -= 8; continue; }   /* ждём второй байт UTF-8 */
        const uint8_t *glyph = fontex_glyph(slot);
        for (int64_t gy = 0; gy < 8; gy++) {
            if (y + gy < 0) continue;
            if (y + gy >= (int64_t)g_fb.height) break;
            uint8_t bits = glyph[gy];
            if (!bits) continue;
            uint32_t *line = lineptr((uint32_t)(y + gy));  /* v0.6.2: lineptr,
                                                   а НЕ fbp() - иначе текст
                                                   затирался dirty_flush */
            for (int64_t gx = 0; gx < 8; gx++)
                if ((bits >> gx) & 1 && x + gx >= 0 &&
                    x + gx < (int64_t)g_fb.width)
                    line[x + gx] = px;
        }
    }
}

void gfx_text_shadow(uint32_t x, uint32_t y, const char *s, gfx_color_t fg, gfx_color_t shadow) {
    gfx_text(x + 1, y + 1, s, shadow);
    gfx_text(x, y, s, fg);
}

/* v0.5.0: "жирный" текст - два прохода со сдвигом на 1px */
void gfx_text_bold(uint32_t x, uint32_t y, const char *s, gfx_color_t fg) {
    gfx_text(x, y, s, fg);
    gfx_text(x + 1, y, s, fg);
}

uint32_t gfx_pack(gfx_color_t c) { return pack(c); }

/* обои из кэша: весь экран / прямоугольник - прямые записи packed-пикселей */
void gfx_blit_rows_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           const uint32_t *rowpx) {
    if (!g_ready) return;
    if (x + w > g_fb.width)  w = (x < g_fb.width)  ? g_fb.width - x : 0;
    if (y + h > g_fb.height) h = (y < g_fb.height) ? g_fb.height - y : 0;
    for (uint32_t r = 0; r < h; r++) {
        uint32_t px = rowpx[y + r];
        uint32_t *p = lineptr(y + r) + x;
        uint32_t left = w;
        /* v0.6.3: как в fill - нечётная голова одиночной, пары выровнены */
        if (left && ((uintptr_t)p & 4)) { *p++ = px; left--; }
        while (left >= 2) {
            uint64_t px2 = ((uint64_t)px << 32) | px;
            memcpy(p, &px2, 8); p += 2; left -= 2;
        }
        if (left) *p = px;
    }
}
void gfx_blit_rows(const uint32_t *rowpx) {
    gfx_blit_rows_region(0, 0, g_fb.width, g_fb.height, rowpx);
}

uint32_t gfx_peek(uint32_t x, uint32_t y) {
    if (!g_ready || x >= g_fb.width || y >= g_fb.height) return 0;
    return lineptr(y)[x];
}
void gfx_poke(uint32_t x, uint32_t y, uint32_t packed) {
    if (!g_ready || x >= g_fb.width || y >= g_fb.height) return;
    lineptr(y)[x] = packed;
}

/* доступ именно к РЕАЛЬНОМУ экрану (курсор рисуется поверх, минуя буфер) */
uint32_t gfx_peek_fb(uint32_t x, uint32_t y) {
    if (!g_ready || x >= g_fb.width || y >= g_fb.height) return 0;
    return fbp()[(uint64_t)y * g_fb.pitch + x];
}
void gfx_poke_fb(uint32_t x, uint32_t y, uint32_t packed) {
    if (!g_ready || x >= g_fb.width || y >= g_fb.height) return;
    fbp()[(uint64_t)y * g_fb.pitch + x] = packed;
}

/* ================= v0.7.0: масштабный текст =================
 * Каждый исходный пиксель 8x8 покрывает блок (mag10/10)x(mag10/10):
 * dst0 = src*mag10/10, dst1 = (src+1)*mag10/10 (без дробей - целые границы). */
void gfx_text_mag(uint32_t x_, uint32_t y_, const char *s, gfx_color_t fg, int mag10) {
    if (!g_ready) return;
    if (mag10 <= 10) { gfx_text(x_, y_, s, fg); return; }
    uint32_t px = pack(fg);
    int64_t x = (int32_t)x_;
    int64_t y = (int32_t)y_;
    int adv = 8 * mag10 / 10;
    int st = 0;
    for (; *s; s++, x += adv) {
        int slot = fontex_slot(&st, (uint8_t)*s);
        if (slot < 0) { x -= adv; continue; }
        const uint8_t *glyph = fontex_glyph(slot);
        for (int gy = 0; gy < 8; gy++) {
            uint8_t bits = glyph[gy];
            if (!bits) continue;
            int64_t dy0 = y + (int64_t)gy * mag10 / 10;
            int64_t dy1 = y + (int64_t)(gy + 1) * mag10 / 10;
            if (dy1 <= 0) continue;
            if (dy0 >= (int64_t)g_fb.height) break;
            if (dy0 < 0) dy0 = 0;
            if (dy1 > (int64_t)g_fb.height) dy1 = g_fb.height;
            for (int gx = 0; gx < 8; gx++) {
                if (!((bits >> gx) & 1)) continue;
                int64_t dx0 = x + (int64_t)gx * mag10 / 10;
                int64_t dx1 = x + (int64_t)(gx + 1) * mag10 / 10;
                if (dx0 < 0) dx0 = 0;
                if (dx1 > (int64_t)g_fb.width) dx1 = g_fb.width;
                for (int64_t yy = dy0; yy < dy1; yy++) {
                    uint32_t *line = lineptr((uint32_t)yy);
                    for (int64_t xx = dx0; xx < dx1; xx++) line[xx] = px;
                }
            }
        }
    }
}

void gfx_text_bold_mag(uint32_t x, uint32_t y, const char *s, gfx_color_t fg, int mag10) {
    gfx_text_mag(x, y, s, fg, mag10);
    gfx_text_mag(x + (mag10 > 10 ? mag10 / 10 : 1), y, s, fg, mag10);
}

/* прямоугольник со скруглёнными углами: k-я полоса от края имеет inset RIN[r][k].
 * Стороны не перерисовываются: середина + r верхних/нижних полос = ровно h строк. */
static const int8_t RIN[11][10] = {
    {0}, {0},
    {1,0}, {2,1,0}, {3,1,0,0}, {4,2,1,0,0}, {5,3,2,1,0,0},
    {4,2,1,1,0,0,0},          /* r=7 */
    {5,3,2,1,1,0,0,0},        /* r=8 */
    {5,3,2,1,1,0,0,0,0},      /* r=9 */
    {6,4,2,2,1,1,0,0,0,0},    /* r=10 */
};
void gfx_fill_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t r, gfx_color_t c) {
    if (r > 10) r = 10;
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    if (h > 2 * r)
        gfx_fill_rect(x, y + r, w, h - 2 * r, c);
    for (uint32_t k = 0; k < r; k++) {
        uint32_t ins = (uint32_t)RIN[r][k];
        gfx_fill_rect(x + ins, y + k,         w - 2 * ins, 1, c);
        gfx_fill_rect(x + ins, y + h - 1 - k, w - 2 * ins, 1, c);
    }
}

/* ================= v0.6.0: двойная буферизация + прозрачность ================= */

void gfx_set_target(uint32_t *buf, uint32_t width, uint32_t height) {
    g_target = buf;
    g_tw = width;
    g_th = height; (void)g_th;
}

/* выливка региона из RAM-буфера на настоящий экран - атомарно для глаза */
void gfx_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_ready || !g_target) return;
    if (x + w > g_fb.width)  w = (x < g_fb.width)  ? g_fb.width - x : 0;
    if (y + h > g_fb.height) h = (y < g_fb.height) ? g_fb.height - y : 0;
    if (!w || !h) return;
    for (uint32_t r = 0; r < h; r++) {
        const uint32_t *s = g_target + (uint64_t)(y + r) * g_tw + x;
        uint32_t *d = fbp() + (uint64_t)(y + r) * g_fb.pitch + x;
        uint32_t left = w;
        /* v0.6.2: пары через memcpy - GCC всё равно даст один mov, но без UB */
        while (left >= 2) { uint64_t v; memcpy(&v, s, 8); memcpy(d, &v, 8);
                            d += 2; s += 2; left -= 2; }
        if (left) *d = *s;
    }
}

/* альфа-блендинг (a: 0..255) поверх текущей цели - честное стекло в RAM */
static inline void blend_px_at(uint32_t *line, uint32_t x, gfx_color_t c, uint8_t a) {
    gfx_color_t d = unpack(line[x]);
    uint32_t ia = 255 - a;
    d.r = (uint8_t)((d.r * ia + c.r * a + 127) / 255);
    d.g = (uint8_t)((d.g * ia + c.g * a + 127) / 255);
    d.b = (uint8_t)((d.b * ia + c.b * a + 127) / 255);
    line[x] = pack(d);
}

void gfx_blend_rect(uint32_t x_, uint32_t y_, uint32_t w_, uint32_t h_,
                    gfx_color_t c, uint8_t a) {
    if (!g_ready) return;
    int64_t x = (int32_t)x_, y = (int32_t)y_;   /* v0.6.3: клип в int64 */
    int64_t w = w_, h = h_;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int64_t)g_fb.width)  w = (int64_t)g_fb.width - x;
    if (y + h > (int64_t)g_fb.height) h = (int64_t)g_fb.height - y;
    if (w <= 0 || h <= 0) return;
    for (int64_t row = 0; row < h; row++) {
        uint32_t *line = lineptr((uint32_t)(y + row)) + x;
        for (int64_t col = 0; col < w; col++)
            blend_px_at(line, (uint32_t)col, c, a);
    }
}

void gfx_blend_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t r, gfx_color_t c, uint8_t a) {
    if (r > 10) r = 10;
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    if (h > 2 * r)
        gfx_blend_rect(x, y + r, w, h - 2 * r, c, a);
    if (x + w > g_fb.width)  w = (x < g_fb.width)  ? g_fb.width - x : 0;
    if (y + h > g_fb.height) h = (y < g_fb.height) ? g_fb.height - y : 0;
    for (uint32_t k = 0; k < r; k++) {
        uint32_t ins = (uint32_t)RIN[r][k];
        gfx_blend_rect(x + ins, y + k,         w - 2 * ins, 1, c, a);
        gfx_blend_rect(x + ins, y + h - 1 - k, w - 2 * ins, 1, c, a);
    }
}
