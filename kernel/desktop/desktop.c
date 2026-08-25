/* AresOS — прототип рабочего стола.
 * Обои-градиент, верхняя панель, док, окно «About AresOS» (таскается за заголовок),
 * курсор стрелка (XOR). Полноценный оконный менеджер — после M6, это демо-каркас. */
#include "desktop.h"
#include "gfx.h"
#include "mouse.h"
#include "pic.h"
#include "kprintf.h"
#include <stdint.h>

#define PANEL_H 30
#define DOCK_H  46
#define WIN_TITLE_H 26

static const gfx_color_t COL_BG_TOP    = GFX_RGB(0x12, 0x18, 0x3D);
static const gfx_color_t COL_BG_BOT    = GFX_RGB(0x0B, 0x3D, 0x3A);
static const gfx_color_t COL_PANEL     = GFX_RGB(0x14, 0x16, 0x25);
static const gfx_color_t COL_PANEL_TXT = GFX_RGB(0xE8, 0xE8, 0xF0);
static const gfx_color_t COL_ACCENT    = GFX_RGB(0xFF, 0x9E, 0x49);  /* "огненный" акцент AresOS */
static const gfx_color_t COL_WIN_BODY  = GFX_RGB(0xF2, 0xF2, 0xF2);
static const gfx_color_t COL_WIN_TXT   = GFX_RGB(0x22, 0x22, 0x33);
static const gfx_color_t COL_WIN_BAR   = GFX_RGB(0x2B, 0x3E, 0x7A);
static const gfx_color_t COL_WIN_BAR_T = GFX_RGB(0xFF, 0xFF, 0xFF);

/* ---------------- окно ---------------- */
typedef struct {
    int32_t x, y, w, h;
    char l1[48], l2[48], l3[48], l4[48], l5[48];
} win_t;

static win_t g_win;
static uint32_t g_scr_w, g_scr_h;

static void draw_window(const win_t *w) {
    /* тень */
    gfx_fill_rect(w->x + 4, w->y + 4, w->w, w->h, GFX_RGB(0, 0, 0));
    /* рамка + тело */
    gfx_fill_rect(w->x, w->y, w->w, w->h, COL_WIN_BODY);
    /* заголовочная панель */
    gfx_fill_rect(w->x, w->y, w->w, WIN_TITLE_H, COL_WIN_BAR);
    gfx_text(w->x + 8, w->y + 9, "About AresOS", COL_WIN_BAR_T);
    /* "кнопка закрытия" — пока декоративная */
    gfx_fill_rect(w->x + w->w - 22, w->y + 6, 14, 14, GFX_RGB(0xE0, 0x4F, 0x3F));
    /* содержимое */
    gfx_text(w->x + 12, w->y + WIN_TITLE_H + 14, w->l1, COL_WIN_TXT);
    gfx_text(w->x + 12, w->y + WIN_TITLE_H + 30, w->l2, COL_WIN_TXT);
    gfx_text(w->x + 12, w->y + WIN_TITLE_H + 46, w->l3, COL_WIN_TXT);
    gfx_text(w->x + 12, w->y + WIN_TITLE_H + 62, w->l4, COL_WIN_TXT);
    gfx_text(w->x + 12, w->y + WIN_TITLE_H + 86, w->l5, GFX_RGB(0x66, 0x66, 0x77));
    gfx_frame_rect(w->x, w->y, w->w, w->h, GFX_RGB(0x0F, 0x14, 0x30));
}

static void erase_window(const win_t *w) {
    /* вернуть обои на месте окна (+тень) */
    gfx_gradient_v_rect(w->x, w->y, w->w + 4, w->h + 4, COL_BG_TOP, COL_BG_BOT);
}

/* ---------------- панель и док ---------------- */
static void draw_panel(void) {
    gfx_fill_rect(0, 0, g_scr_w, PANEL_H, COL_PANEL);
    gfx_fill_rect(0, PANEL_H - 2, g_scr_w, 2, COL_ACCENT);
    gfx_text_shadow(10, 11, "AresOS", COL_ACCENT, GFX_RGB(0, 0, 0));
    gfx_text(88, 11, "Desktop prototype", COL_PANEL_TXT);
    gfx_text(g_scr_w - 96 - 8, 11, "v0.2.5", COL_PANEL_TXT);
}

static void draw_dock(void) {
    uint32_t y = g_scr_h - DOCK_H;
    gfx_fill_rect(0, y, g_scr_w, DOCK_H, COL_PANEL);
    gfx_fill_rect(0, y, g_scr_w, 1, GFX_RGB(0x33, 0x33, 0x44));
    /* три декоративных "значка" */
    gfx_color_t c1 = GFX_RGB(0xFF, 0x9E, 0x49);
    gfx_color_t c2 = GFX_RGB(0x4F, 0xC3, 0x7B);
    gfx_color_t c3 = GFX_RGB(0x4A, 0x9D, 0xFF);
    gfx_fill_rect(16, y + 8, 30, 30, c1);
    gfx_fill_rect(56, y + 8, 30, 30, c2);
    gfx_fill_rect(96, y + 8, 30, 30, c3);
    gfx_text(16, g_scr_h - 10, "kernel", COL_PANEL_TXT);
}

static void draw_screen(void) {
    gfx_gradient_v_rect(0, 0, g_scr_w, g_scr_h, COL_BG_TOP, COL_BG_BOT);
    draw_panel();
    draw_dock();
    draw_window(&g_win);
}

/* ---------------- курсор (XOR-стрелка 12x17) ---------------- */
static const uint16_t CURSOR[17] = {
    0b000000000001, 0b000000000011, 0b000000000101, 0b000000001001,
    0b000000010001, 0b000000100001, 0b000001000001, 0b000010000001,
    0b000100000001, 0b001000000001, 0b011111100001, 0b000100100101,
    0b000010101001, 0b000010100011, 0b000010100001, 0b000010100000,
    0b000011000000,
};

static int g_cur_on;
static int32_t g_cur_x, g_cur_y;

static void cursor_flip(int32_t x, int32_t y) {
    /* XOR-белым: вызов поверх старого положения = стирание */
    for (uint32_t row = 0; row < 17; row++)
        for (uint32_t col = 0; col < 12; col++)
            if ((CURSOR[row] >> col) & 1)
                gfx_pixel_xor(x + col, y + row, 1);
}

/* ---------------- десктоп ---------------- */
static uint8_t g_buttons_old;

static void strcpy_small(char *dst, const char *src) {
    while ((*dst++ = *src++)) {}
}
static void u32dec(uint32_t v, char *out) {
    char tmp[16]; int i = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[i++] = '0' + v % 10; v /= 10; }
    int j = 0;
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
}

__attribute__((noreturn)) void desktop_enter(const bootinfo_t *bi,
                                             uint64_t total_mib,
                                             uint64_t free_mib) {
    gfx_init(&bi->fb);
    if (!gfx_ready()) {
        kpanic("desktop: no framebuffer");
    }
    g_scr_w = gfx_width();
    g_scr_h = gfx_height();
    mouse_set_limits(g_scr_w, g_scr_h);

    /* тексты окна */
    strcpy_small(g_win.l1, "Kernel 0.2.5 (x86-64)");
    {
        char buf[48];
        char num[16];
        u32dec((uint32_t)total_mib, num);
        strcpy_small(buf, "RAM usable: ");
        char *p = buf;
        while (*p) p++;
        strcpy_small(p, num);
        while (*p) p++;
        strcpy_small(p, " MiB");
        strcpy_small(g_win.l2, buf);
    }
    {
        char buf[48], num[16];
        u32dec((uint32_t)free_mib, num);
        strcpy_small(buf, "RAM free:   ");
        char *p = buf + 12; strcpy_small(p, num); while (*p) p++;
        strcpy_small(p, " MiB (bitmap PMM)");
        strcpy_small(g_win.l3, buf);
    }
    strcpy_small(g_win.l4, "Mouse: PS/2 over IRQ12 (PIC 8259)");

    strcpy_small(g_win.l5, "[ drag me by title bar! ]");

    g_win.w = 400;
    g_win.h = WIN_TITLE_H + 110;
    g_win.x = (int32_t)(g_scr_w > 400 ? (g_scr_w - 400) / 2 : 0);
    g_win.y = (int32_t)(g_scr_h > 220 ? (g_scr_h - 220) / 2 : 0);

    draw_screen();
    kprintf("[desktop] %lux%lu ready; window drag enabled\n", g_scr_w, g_scr_h);

    /* включить прерывания: PIC запрограммирован, мышь ARM-нута */
    __asm__ volatile ("sti");

    int dragging = 0;
    int32_t drag_dx = 0, drag_dy = 0;
    g_cur_x = mouse_x(); g_cur_y = mouse_y();
    cursor_flip(g_cur_x, g_cur_y);
    g_cur_on = 1;

    for (;;) {
        int moved = mouse_moved();
        int left = mouse_left();

        if (!dragging && left && !(g_buttons_old & 1)) {
            /* нажатие: попали в заголовок окна? */
            int32_t mx = mouse_x(), my = mouse_y();
            if (mx >= g_win.x && mx < g_win.x + g_win.w &&
                my >= g_win.y && my < g_win.y + WIN_TITLE_H) {
                dragging = 1;
                drag_dx = mx - g_win.x;
                drag_dy = my - g_win.y;
            }
        }
        if (dragging && !left) dragging = 0;

        if (moved || dragging) {
            int32_t nx = mouse_x(), ny = mouse_y();

            /* перерисовать курсор: стереть старый */
            if (g_cur_on) cursor_flip(g_cur_x, g_cur_y);

            if (dragging) {
                int32_t wx = nx - drag_dx;
                int32_t wy = ny - drag_dy;
                if (wy < PANEL_H) wy = PANEL_H;
                if (wy > (int32_t)g_scr_h - DOCK_H - 20) wy = (int32_t)g_scr_h - DOCK_H - 20;
                if (wx < 0) wx = 0;
                if (wx > (int32_t)g_scr_w - 60) wx = (int32_t)g_scr_w - 60;
                if (wx != g_win.x || wy != g_win.y) {
                    erase_window(&g_win);
                    g_win.x = wx;
                    g_win.y = wy;
                    draw_window(&g_win);
                }
            }

            cursor_flip(nx, ny);
            g_cur_x = nx; g_cur_y = ny; g_cur_on = 1;
        }

        g_buttons_old = (uint8_t)left;
        __asm__ volatile ("hlt");   /* спим до следующего IRQ */
    }
}
