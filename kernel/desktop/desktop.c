/* AresOS — прототип рабочего стола.
 * Обои-градиент, верхняя панель, док, окно «About AresOS» (таскается за заголовок),
 * курсор стрелка (XOR). Полноценный оконный менеджер — после M6, это демо-каркас. */
#include "desktop.h"
#include "gfx.h"
#include "mouse.h"
#include "pic.h"
#include "keyboard.h"
#include "proc.h"
#include "kprintf.h"
#include "pe.h"
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
static void strcpy_small(char *dst, const char *src);
static void u32dec(uint32_t v, char *out);

typedef struct {
    int32_t x, y, w, h;
    char l1[48], l2[48], l3[48], l4[48], l5[48], l6[48];
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
    /* PE-тест (M3): результат TESTPE.EXE */
    if (w->l6[0])
        gfx_text(w->x + 12, w->y + WIN_TITLE_H + 110, w->l6, GFX_RGB(0x11, 0x77, 0x33));
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
    gfx_text(g_scr_w - 96 - 8, 11, "v0.4.0", COL_PANEL_TXT);
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

/* v0.3.4: статус мыши — в ПАНЕЛЬ (не в консоль! спам консоли «уезжал вниз») */
static void draw_mouse_status(void) {
    char buf[40], num[12];
    char *p = buf;
    *p++ = 'x'; *p++ = '=';
    u32dec((uint32_t)mouse_x(), num);   strcpy_small(p, num); while (*p) p++;
    *p++ = ' '; *p++ = 'y'; *p++ = '=';
    u32dec((uint32_t)mouse_y(), num);   strcpy_small(p, num); while (*p) p++;
    *p++ = ' '; *p++ = 'i'; *p++ = 'r'; *p++ = 'q'; *p++ = '=';
    u32dec(mouse_irq_count(), num);     strcpy_small(p, num);
    gfx_fill_rect(g_scr_w - 300, 2, 196, PANEL_H - 4, COL_PANEL);
    gfx_text(g_scr_w - 296, 11, buf, COL_PANEL_TXT);
}

/* ================= диспетчер задач (M5) =================
 * F2 — открыть/закрыть. Таблица процессов ядра: id, имя, состояние,
 * тиков CPU, тип (normal/background). Обновляется 4 раза в секунду. */
#define TM_W   480
#define TM_H   320

static int      g_taskman;                 /* окно открыто? */
static int32_t  g_tm_x, g_tm_y, g_tm_h;
static uint64_t g_tm_stamp;                /* последний рефреш (тики 100 Гц) */

static void taskman_geom(void) {
    g_tm_x = g_scr_w > TM_W ? (int32_t)(g_scr_w - TM_W) / 2 : 0;
    g_tm_y = PANEL_H + 24;
    g_tm_h = TM_H;
    int32_t maxh = (int32_t)g_scr_h - DOCK_H - g_tm_y - 4;
    if (g_tm_h > maxh) g_tm_h = maxh;
}

/* строка таблицы: смещения в символах (шрифт 8px) —
 * id@0, name@4, state@18, ticks@25 (вправо), type@36 */
static void tm_build_row(const proc_info_t *pi, char *out) {
    char num[20];
    char *p = out;
    int i;
    u32dec((uint32_t)pi->id, num);
    for (i = 0; num[i]; i++);
    while (i++ < 2) *p++ = ' ';
    for (i = 0; num[i]; i++) *p++ = num[i];
    *p++ = ' '; *p++ = ' ';
    for (i = 0; i < 13 && pi->name[i]; i++) *p++ = pi->name[i];
    while (i++ < 13) *p++ = ' ';
    *p++ = ' ';
    { const char *s = proc_state_name(pi->state); while (*s) *p++ = *s++; }
    *p++ = ' '; *p++ = ' ';
    u32dec((uint32_t)pi->ticks, num);
    for (i = 0; num[i]; i++);
    while (i++ < 9) *p++ = ' ';
    for (i = 0; num[i]; i++) *p++ = num[i];
    *p++ = ' '; *p++ = ' ';
    { const char *t = (pi->flags & PROC_F_BACKGROUND) ? "background" : "normal    ";
      while (*t) *p++ = *t++; }
    *p = 0;
}

static void draw_taskman(void) {
    int32_t x = g_tm_x, y = g_tm_y, h = g_tm_h;
    gfx_fill_rect(x + 4, y + 4, TM_W, h, GFX_RGB(0, 0, 0));          /* тень */
    gfx_fill_rect(x, y, TM_W, h, COL_WIN_BODY);
    gfx_fill_rect(x, y, TM_W, WIN_TITLE_H, COL_WIN_BAR);
    gfx_text(x + 8, y + 9, "AresOS Task Manager [F2]", COL_WIN_BAR_T);

    /* аптайм справа в заголовке */
    {
        char buf[24], num[16];
        char *p = buf;
        strcpy_small(p, "up ");
        p += 3;
        u32dec((uint32_t)(sched_ticks() / 100), num);
        strcpy_small(p, num);
        while (*p) p++;
        *p++ = ' '; *p++ = 's'; *p = 0;
        gfx_text(x + TM_W - 96, y + 9, buf, COL_WIN_BAR_T);
    }

    /* шапка таблицы (позиции = tm_build_row) */
    const gfx_color_t HC = GFX_RGB(0x8A, 0x3C, 0x00);
    int32_t hy = y + WIN_TITLE_H + 10;
    gfx_fill_rect(x + 8, hy - 2, TM_W - 16, 14, GFX_RGB(0xE6, 0xDF, 0xD3));
    gfx_text(x + 16,        hy, "id",    HC);
    gfx_text(x + 16 + 4*8,  hy, "name",  HC);
    gfx_text(x + 16 + 18*8, hy, "state", HC);
    gfx_text(x + 16 + 26*8, hy, "ticks", HC);
    gfx_text(x + 16 + 36*8, hy, "type",  HC);

    /* строки процессов */
    proc_info_t pi[16];
    int n = proc_list(pi, 16);
    int maxrows = (int)((h - WIN_TITLE_H - 44) / 14);
    if (n > maxrows) n = maxrows;
    for (int i = 0; i < n; i++) {
        char row[56];
        int32_t ry = hy + 16 + i * 14;
        if (pi[i].state == PROC_RUNNING)
            gfx_fill_rect(x + 8, ry - 2, TM_W - 16, 13, GFX_RGB(0xFF, 0xDF, 0xBD));
        tm_build_row(&pi[i], row);
        gfx_text(x + 16, ry, row, COL_WIN_TXT);
    }
    gfx_text(x + 16, y + h - 16, "F2 close | bg = background daemons",
             GFX_RGB(0x66, 0x66, 0x77));
    gfx_frame_rect(x, y, TM_W, h, GFX_RGB(0x0F, 0x14, 0x30));
}

static void erase_taskman(void) {
    gfx_gradient_v_rect(g_tm_x, g_tm_y, TM_W + 4, g_tm_h + 4, COL_BG_TOP, COL_BG_BOT);
    draw_window(&g_win);      /* «About» мог частично прятаться под диспетчером */
}

/* ------- док: аптайм + последняя клавиша (видно, что клавиатура жива) ------- */
static uint64_t g_uptime_sec;
static void draw_uptime(void) {
    char buf[28], num[16];
    char *p = buf;
    strcpy_small(p, "up ");
    p += 3;
    u32dec((uint32_t)g_uptime_sec, num);
    strcpy_small(p, num);
    while (*p) p++;
    strcpy_small(p, " s | F2: tasks");
    gfx_fill_rect(g_scr_w - 262, g_scr_h - 24, 232, 14, COL_PANEL);
    gfx_text(g_scr_w - 256, g_scr_h - 21, buf, COL_PANEL_TXT);
}

static int g_last_key = -1;
static void draw_key_label(void) {
    char buf[24];
    char *p = buf;
    strcpy_small(p, "key=");
    p += 4;
    if (g_last_key < 0) {
        strcpy_small(p, "-");
    } else if (g_last_key >= KEY_F1 && g_last_key <= KEY_F10) {
        char num[8];
        *p++ = 'F';
        u32dec((uint32_t)(g_last_key - KEY_F1 + 1), num);
        strcpy_small(p, num);
    } else if (g_last_key >= 32 && g_last_key < 127) {
        *p++ = '\''; *p++ = (char)g_last_key; *p++ = '\''; *p = 0;
    } else {
        strcpy_small(p, "(special)");
    }
    gfx_fill_rect(150, g_scr_h - 24, 100, 14, COL_PANEL);
    gfx_text(154, g_scr_h - 21, buf, COL_PANEL_TXT);
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
    strcpy_small(g_win.l1, "Kernel 0.4.0 (x86-64)");
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
    strcpy_small(g_win.l4, "Mouse+KBD: PS/2, timer: LAPIC/PIT 100Hz");

    strcpy_small(g_win.l5, "F2: Task Manager | drag window");

    g_win.w = 400;
    g_win.h = WIN_TITLE_H + 126;
    g_win.x = (int32_t)(g_scr_w > 400 ? (g_scr_w - 400) / 2 : 0);
    g_win.y = (int32_t)(g_scr_h > 220 ? (g_scr_h - 220) / 2 : 0);
    g_win.l6[0] = 0;

    taskman_geom();
    g_uptime_sec = 0;
    draw_screen();
    draw_uptime();
    draw_key_label();
    kprintf("[desktop] %lux%lu ready; window drag enabled, F2=TaskManager\n", g_scr_w, g_scr_h);

    /* ===== M3: запуск TESTPE.EXE через PE-загрузчик ядра =====
     * Квадрат рисуется ПОСЛЕ обоев — иначе градиент его сотрёт. */
    {
        int ret = pe_demo_run();
        static const char hex[] = "0123456789ABCDEF";
        char *p = g_win.l6;
        if ((uint32_t)ret == ARES_PE_TEST_OK) {
            strcpy_small(g_win.l6, "PE test: OK (ret=0xA2E5)");
        } else {
            strcpy_small(g_win.l6, "PE test: FAIL ret=");
            while (*p) p++;
            uint32_t v = (uint32_t)ret;
            *p++ = v & 0x80000000u ? '-' : '0';
            if (!(v & 0x80000000u)) {
                for (int i = 28; i >= 0; i -= 4) *p++ = hex[(v >> i) & 0xF];
            } else {
                v = (uint32_t)(-(int32_t)v);
                for (int i = 28; i >= 4; i -= 4) *p++ = hex[(v >> i) & 0xF];
            }
            *p = 0;
        }
        draw_window(&g_win);   /* обновить окно: появилась строка l6 */
    }

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

        /* ---- клавиатура (M4): разбираем очередь нажатий ---- */
        {
            int k;
            int kd = 0;
            while ((k = keyboard_getch()) >= 0) {
                g_last_key = k;
                kd = 1;
                if (k == KEY_F2) {
                    if (g_cur_on) cursor_flip(g_cur_x, g_cur_y);
                    g_taskman ^= 1;
                    if (g_taskman) {
                        draw_taskman();
                        kprintf("[taskman] open (процессов в списке)\n");
                    } else {
                        erase_taskman();
                        kprintf("[taskman] closed\n");
                    }
                    cursor_flip(g_cur_x, g_cur_y);
                    g_cur_on = 1;
                    g_tm_stamp = sched_ticks();
                }
            }
            if (kd) draw_key_label();
        }

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

        /* v0.3.4: статус мыши — в панель, а не спамом в консоль */
        {
            static uint32_t last_irq = 0;
            uint32_t c = mouse_irq_count();
            if (c != last_irq) {
                if (!last_irq)
                    kprintf("[mouse] IRQ12 живы — CS-фикс v0.3.3 победил (дальше молчу)\n");
                last_irq = c;
                draw_mouse_status();
            }
        }

        if (moved)
            draw_mouse_status();

        /* ---- M5: живой диспетчер задач + аптайм в доке ---- */
        {
            uint64_t t = sched_ticks();
            if (g_taskman && t - g_tm_stamp >= 25) {      /* 4 раза/сек */
                g_tm_stamp = t;
                if (g_cur_on) cursor_flip(g_cur_x, g_cur_y);
                draw_taskman();
                cursor_flip(g_cur_x, g_cur_y);
                g_cur_on = 1;
            }
            if (t / 100 != g_uptime_sec) {
                g_uptime_sec = t / 100;
                draw_uptime();
            }
        }

        __asm__ volatile ("hlt");   /* таймер 100 Гц / IRQ12 / IRQ1 нас разбудят */
    }
}
