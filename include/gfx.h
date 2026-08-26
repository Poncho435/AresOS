/* AresOS — gfx: графические примитивы поверх framebuffer (для десктопа) */
#ifndef ARES_GFX_H
#define ARES_GFX_H

#include <stdint.h>
#include "bootinfo.h"

typedef struct { uint8_t r, g, b; } gfx_color_t;

#define GFX_RGB(rr, gg, bb) ((gfx_color_t){ (rr), (gg), (bb) })

void gfx_init(const bootinfo_fb_t *fb);
int  gfx_ready(void);
uint32_t gfx_width(void);
uint32_t gfx_height(void);

void gfx_pixel(uint32_t x, uint32_t y, gfx_color_t c);
void gfx_pixel_xor(uint32_t x, uint32_t y, uint8_t mask);  /* XOR для курсора */
void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, gfx_color_t c);
void gfx_gradient_v_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         gfx_color_t top, gfx_color_t bottom);
void gfx_frame_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, gfx_color_t c);
/* текст 8x8 из font8x8 */
void gfx_text(uint32_t x, uint32_t y, const char *s, gfx_color_t fg);
void gfx_text_shadow(uint32_t x, uint32_t y, const char *s, gfx_color_t fg, gfx_color_t shadow);
/* v0.5.0: современный UI */
void gfx_text_bold(uint32_t x, uint32_t y, const char *s, gfx_color_t fg);
void gfx_fill_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, gfx_color_t c);
uint32_t gfx_pack(gfx_color_t c);   /* упаковка цвета в пиксель fb */
/* v0.5.0: быстрая заливка обоев из кэша упакованных цветов по строкам */
void gfx_blit_rows(const uint32_t *rowpx);
void gfx_blit_rows_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *rowpx);
/* packed доступ к одному пикселю (для save/restore курсора) */
uint32_t gfx_peek(uint32_t x, uint32_t y);
void     gfx_poke(uint32_t x, uint32_t y, uint32_t packed);

#endif
