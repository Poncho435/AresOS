/* AresOS — рабочий стол v0.5.0: современная тёмная тема «midnight aurora».
 *  - кэшированные строки обоев → мгновенная перерисовка мира
 *  - верхняя панель со «стеклом», часы, хинты F1/F2/F3
 *  - плавающий док: 3 значка-приложения, открываются мышкой и клавишами
 *  - окна со z-порядком: перетаскивание, raise по клику, красная точка закрытия
 *  - приложения: About (F1), Диспетчер задач (F2), Логи (F3) — журнал ядра
 *  - курсор: save/restore, чёрный контур + белое тело; Ctrl+стрелки = клавиатурная мышь */
#include "desktop.h"
#include "gfx.h"
#include "mouse.h"
#include "keyboard.h"
#include "proc.h"
#include "logbuf.h"
#include "kprintf.h"
#include "pe.h"
#include "heap.h"
#include <stdint.h>

#define PANEL_H     34
#define DOCK_H      58
#define DOCK_ICON   44
#define DOCK_STEP   56
#define WIN_BAR_H   30
#define TERM_LINE_H 10
#define TM_REFRESH  33            /* 3 раза/сек */

/* ---------- палитра ---------- */
static const gfx_color_t C_PANEL   = GFX_RGB(0x0E, 0x10, 0x18);
static const gfx_color_t C_PLINE   = GFX_RGB(0x2A, 0x30, 0x48);
static const gfx_color_t C_TXT     = GFX_RGB(0xE7, 0xEA, 0xF3);
static const gfx_color_t C_TXT2    = GFX_RGB(0x8B, 0x92, 0xA9);
static const gfx_color_t C_ACCENT  = GFX_RGB(0xFF, 0x9E, 0x49);
static const gfx_color_t C_GREEN   = GFX_RGB(0x4F, 0xC3, 0x7B);
static const gfx_color_t C_BLUE    = GFX_RGB(0x4A, 0x9D, 0xFF);
static const gfx_color_t C_CYAN    = GFX_RGB(0x56, 0xC2, 0xE8);
static const gfx_color_t C_PURPLE  = GFX_RGB(0xB0, 0x8A, 0xE0);
static const gfx_color_t C_YELLOW  = GFX_RGB(0xE8, 0xC1, 0x5A);
static const gfx_color_t C_RED     = GFX_RGB(0xE0, 0x56, 0x4F);
static const gfx_color_t C_WIN_BG  = GFX_RGB(0x1A, 0x1E, 0x2A);
static const gfx_color_t C_WIN_BAR = GFX_RGB(0x23, 0x28, 0x38);
static const gfx_color_t C_WIN_BR  = GFX_RGB(0x0B, 0x0D, 0x14);
static const gfx_color_t C_ROW_ALT = GFX_RGB(0x1F, 0x24, 0x33);
static const gfx_color_t C_BAR_BG  = GFX_RGB(0x26, 0x2B, 0x3D);
static const gfx_color_t C_TERM_BG = GFX_RGB(0x0B, 0x0F, 0x16);
static const gfx_color_t C_TERM_LN = GFX_RGB(0xAE, 0xB6, 0xC8);
static const gfx_color_t C_TERM_AL = GFX_RGB(0x0E, 0x13, 0x20);
static const gfx_color_t C_SHADOW  = GFX_RGB(0x05, 0x06, 0x0A);
static const gfx_color_t C_DOCK    = GFX_RGB(0x14, 0x17, 0x22);
static const gfx_color_t C_DOCK_BR = GFX_RGB(0x2E, 0x35, 0x50);

/* обои: трёхстопный градиент (кэш packed-строк ниже) */
static const gfx_color_t BG0 = GFX_RGB(0x0C, 0x10, 0x24);
static const gfx_color_t BG1 = GFX_RGB(0x13, 0x1A, 0x3C);
static const gfx_color_t BG2 = GFX_RGB(0x0E, 0x2A, 0x38);

static uint32_t *g_bg;            /* packed цвет каждой строки экрана */
static uint32_t  g_scr_w, g_scr_h;

/* ---------------- утилиты ---------------- */
static void strcpy_small(char *dst, const char *src) { while ((*dst++ = *src++)) {} }
static void u32dec(uint32_t v, char *out) {
    char tmp[16]; int i = 0, j = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
}
static void pcat(char **p, const char *s) { while (*s) *(*p)++ = *s++; **p = 0; }
static int  has_prefix(const char *s, const char *pfx) {
    while (*pfx) if (*s++ != *pfx++) return 0;
    return 1;
}
static int  contains(const char *s, const char *needle) {
    for (; *s; s++) if (has_prefix(s, needle)) return 1;
    return 0;
}

/* ---------------- приложения/окна ---------------- */
enum { APP_ABOUT = 0, APP_TASKMAN, APP_LOGS, APP_N };
typedef struct { int open; int32_t x, y, w, h; } win_t;
static win_t g_w[APP_N];
static int   g_z[APP_N] = { 0, 1, 2 };     /* порядок отрисовки: [N-1] — верх */

static void app_raise(int a) {
    int pos = 0;
    for (int i = 0; i < APP_N; i++) if (g_z[i] == a) pos = i;
    for (int i = pos; i < APP_N - 1; i++) g_z[i] = g_z[i + 1];
    g_z[APP_N - 1] = a;
}
static int app_top(void) {
    for (int i = APP_N - 1; i >= 0; i--)
        if (g_w[g_z[i]].open) return g_z[i];
    return -1;
}
static void app_toggle(int a) {
    g_w[a].open = !g_w[a].open;
    if (g_w[a].open) app_raise(a);
}

/* ---------------- обои ---------------- */
static void bg_cache_build(void) {
    /* вертикальный градиент BG0 → BG1 → BG2 */
    for (uint32_t y = 0; y < g_scr_h; y++) {
        uint32_t half = g_scr_h / 2;
        gfx_color_t c;
        if (y < half) {
            int t = half ? (int)(y * 255 / half) : 0;
            c.r = (uint8_t)(BG0.r + ((int)BG1.r - BG0.r) * t / 255);
            c.g = (uint8_t)(BG0.g + ((int)BG1.g - BG0.g) * t / 255);
            c.b = (uint8_t)(BG0.b + ((int)BG1.b - BG0.b) * t / 255);
        } else {
            int t = half ? (int)((y - half) * 255 / (g_scr_h - half)) : 0;
            c.r = (uint8_t)(BG1.r + ((int)BG2.r - BG1.r) * t / 255);
            c.g = (uint8_t)(BG1.g + ((int)BG2.g - BG1.g) * t / 255);
            c.b = (uint8_t)(BG1.b + ((int)BG2.b - BG1.b) * t / 255);
        }
        g_bg[y] = gfx_pack(c);
    }
}

/* ---------------- панель ---------------- */
static uint64_t g_last_sec = 0xFFFFFFFFFFFFFFFFULL;

static void clock_draw(void) {
    uint32_t s = (uint32_t)g_last_sec;
    char buf[16];
    char *p = buf;
    *p++ = (char)('0' + (s / 36000) % 10);
    *p++ = (char)('0' + (s / 3600) % 10);
    *p++ = ':';
    *p++ = (char)('0' + (s / 600) % 6);
    *p++ = (char)('0' + (s / 60) % 10);
    *p++ = ':';
    *p++ = (char)('0' + (s / 10) % 6);
    *p++ = (char)('0' + s % 10);
    *p = 0;
    uint32_t cx = g_scr_w / 2 - 44;
    gfx_fill_round_rect(cx, 8, 88, 18, 6, GFX_RGB(0x17, 0x1B, 0x29));
    gfx_text(cx + 12, 13, buf, C_TXT);
}

static void panel_draw(void) {
    gfx_fill_rect(0, 0, g_scr_w, PANEL_H, C_PANEL);
    gfx_fill_rect(0, PANEL_H - 1, g_scr_w, 1, C_PLINE);
    gfx_text_bold(14, 13, "AresOS", C_ACCENT);
    gfx_fill_round_rect(86, 8, 62, 18, 6, GFX_RGB(0x1B, 0x20, 0x30));
    gfx_text(94, 13, "v0.5.1", C_TXT2);
    gfx_text(g_scr_w - 226, 13, "F1 About   F2 Tasks   F3 Logs", C_TXT2);
    clock_draw();
}

/* ---------------- док ---------------- */
static uint32_t dock_x(void) { return (g_scr_w - (APP_N * DOCK_STEP + 12)) / 2; }
static uint32_t dock_y(void) { return g_scr_h - DOCK_H - 12; }

typedef struct { const char *glyph; gfx_color_t col; } icon_t;
static const icon_t ICONS[APP_N] = {
    { "i",  GFX_RGB(0x4A, 0x9D, 0xFF) },
    { "TM", GFX_RGB(0xFF, 0x9E, 0x49) },
    { ">_", GFX_RGB(0x4F, 0xC3, 0x7B) },
};

static int dock_hit(int32_t mx, int32_t my) {   /* индекс значка или -1 */
    uint32_t dx = dock_x(), dy = dock_y();
    if (my < (int32_t)dy || my >= (int32_t)(dy + DOCK_H)) return -1;
    for (int i = 0; i < APP_N; i++) {
        int32_t ix = (int32_t)(dx + 12 + i * DOCK_STEP);
        if (mx >= ix && mx < ix + DOCK_ICON) return i;
    }
    return -1;
}

static void dock_draw(void) {
    uint32_t dx = dock_x(), dy = dock_y();
    uint32_t dw = APP_N * DOCK_STEP + 12;
    gfx_fill_round_rect(dx - 1, dy - 1, dw + 2, DOCK_H + 2, 11, C_DOCK_BR);
    gfx_fill_round_rect(dx, dy, dw, DOCK_H, 10, C_DOCK);
    int32_t mx = mouse_x(), my = mouse_y();
    int hov = dock_hit(mx, my);
    for (int i = 0; i < APP_N; i++) {
        uint32_t ix = dx + 12 + (uint32_t)i * DOCK_STEP;
        uint32_t iy = dy + 7;
        gfx_color_t c = ICONS[i].col;
        if (i == hov) {   /* лёгкое «подсвечивание» при наведении */
            c.r = (uint8_t)(c.r < 220 ? c.r + 35 : 255);
            c.g = (uint8_t)(c.g < 220 ? c.g + 35 : 255);
            c.b = (uint8_t)(c.b < 220 ? c.b + 35 : 255);
        }
        gfx_fill_round_rect(ix, iy, DOCK_ICON, DOCK_ICON, 8, c);
        gfx_text_bold(ix + (DOCK_ICON - 8 * 2) / 2 + (ICONS[i].glyph[1] ? 0 : 4),
                      iy + DOCK_ICON / 2 - 4, ICONS[i].glyph, GFX_RGB(0xFF, 0xFF, 0xFF));
        if (g_w[i].open)  /* точка «приложение открыто» */
            gfx_fill_round_rect(ix + DOCK_ICON / 2 - 3, iy + DOCK_ICON + 2, 6, 3, 1, C_TXT2);
    }
}

/* ---------------- нижний правый индикатор ---------------- */
static int g_last_key = -1;
static void key_name(char *out) {
    char *p = out;
    int k = g_last_key;
    if (k < 0) { strcpy_small(out, "-"); return; }
    if (k >= KEY_F1 && k <= KEY_F10) {
        char num[8];
        *p++ = 'F';
        u32dec((uint32_t)(k - KEY_F1 + 1), num);
        strcpy_small(p, num);
        return;
    }
    if (k >= 32 && k < 127) { *p++ = '\''; *p++ = (char)k; *p++ = '\''; *p = 0; return; }
    if (k == 27) { strcpy_small(out, "Esc"); return; }
    strcpy_small(out, "(sp)");
}

static void pill_draw(void) {
    char buf[40], num[16];
    char *p = buf;
    *p++ = 'x'; *p++ = '=';
    u32dec((uint32_t)mouse_x(), num); pcat(&p, num);
    *p++ = ' '; *p++ = 'y'; *p++ = '=';
    u32dec((uint32_t)mouse_y(), num); pcat(&p, num);
    *p++ = ' '; *p++ = 'k'; *p++ = '=';
    key_name(num); pcat(&p, num);
    gfx_fill_round_rect(g_scr_w - 240, g_scr_h - 30, 206, 20, 6, C_DOCK);
    gfx_text(g_scr_w - 232, g_scr_h - 24, buf, C_TXT2);
}

/* ---------------- окна: каркас ---------------- */
static void draw_content(int app, int32_t x, int32_t y, int32_t w, int32_t h);

static const struct { const char *t; gfx_color_t c; } APP_META[APP_N] = {
    { "About AresOS",  GFX_RGB(0x4A, 0x9D, 0xFF) },
    { "Task Manager",  GFX_RGB(0xFF, 0x9E, 0x49) },
    { "System logs — terminal", GFX_RGB(0x4F, 0xC3, 0x7B) },
};

static void draw_window(int app) {
    win_t *W = &g_w[app];
    int32_t x = W->x, y = W->y, w = W->w, h = W->h;
    /* тень + рамка + тело */
    gfx_fill_round_rect(x + 4, y + 5, w, h, 8, C_SHADOW);
    gfx_fill_round_rect(x - 1, y - 1, w + 2, h + 2, 8, C_WIN_BR);
    gfx_fill_round_rect(x, y, w, h, 6, C_WIN_BG);
    /* заголовочная полоса (верх скруглён, низ прямой) */
    gfx_fill_round_rect(x, y, w, WIN_BAR_H, 6, C_WIN_BAR);
    gfx_fill_rect(x, y + WIN_BAR_H / 2, w, WIN_BAR_H / 2, C_WIN_BAR);
    gfx_fill_rect(x, y + WIN_BAR_H, w, 1, C_WIN_BR);
    /* цветная точка приложения + заголовок */
    gfx_fill_round_rect(x + 12, y + 9, 12, 12, 3, APP_META[app].c);
    gfx_text_bold(x + 32, y + 11, APP_META[app].t, C_TXT);
    /* «traffic lights»: красная = закрыть */
    gfx_fill_round_rect(x + w - 24, y + 10, 10, 10, 3, C_RED);
    gfx_fill_round_rect(x + w - 40, y + 10, 10, 10, 3, GFX_RGB(0x3A, 0x41, 0x56));
    gfx_fill_round_rect(x + w - 56, y + 10, 10, 10, 3, GFX_RGB(0x3A, 0x41, 0x56));
    draw_content(app, x, y + WIN_BAR_H, w, h - WIN_BAR_H);
}

/* ---- приложение About ---- */
static char g_ram_line[48];
static char g_pe_line[40];
static void about_draw(int32_t x, int32_t y, int32_t w) {
    (void)w;
    int32_t cx = x + 16;
    int32_t yy = y + 14;
    gfx_text_bold(cx, yy, "AresOS kernel 0.5.1 (x86-64)", C_TXT); yy += 18;
    gfx_text(cx, yy, g_ram_line, C_TXT2); yy += 14;
    gfx_text(cx, yy, "IRQ model: IOAPIC+LAPIC 100 Hz (PIC fallback)", C_TXT2); yy += 14;
    gfx_text(cx, yy, "Heap/VMM alive, PE32+ loader inside kernel", C_TXT2); yy += 14;
    if (g_pe_line[0]) { gfx_text(cx, yy, g_pe_line, C_GREEN); yy += 14; }
    yy += 4;
    gfx_fill_rect(cx, yy, 240, 1, C_PLINE); yy += 8;
    gfx_text(cx, yy, "VM tip: click inside the window to", C_ACCENT); yy += 12;
    gfx_text(cx, yy, "capture the mouse! Ctrl+Arrows =", C_ACCENT); yy += 12;
    gfx_text(cx, yy, "move cursor by keyboard.", C_ACCENT); yy += 14;
    gfx_text(cx, yy, "Drag windows by title, Esc closes top.", C_TXT2);
}

/* ---- приложение Task Manager ---- */
static uint64_t g_tm_prev_total;
static uint64_t g_tm_prev[32];
static uint64_t g_tm_stamp;

static void tm_draw(int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t cx = x + 12;
    int32_t hy = y + 8;
    /* шапка */
    gfx_fill_round_rect(cx, hy, w - 24, 16, 4, C_ROW_ALT);
    const gfx_color_t HC = C_CYAN;
    gfx_text(cx + 10,        hy + 4, "id",    HC);
    gfx_text(cx + 10 + 4*8,  hy + 4, "name",  HC);
    gfx_text(cx + 10 + 18*8, hy + 4, "state", HC);
    gfx_text(cx + 10 + 25*8, hy + 4, "ticks", HC);
    gfx_text(cx + 10 + 34*8, hy + 4, "cpu",   HC);
    gfx_text(cx + 10 + 50*8, hy + 4, "type",  HC);

    uint64_t now = sched_ticks();
    uint64_t span = now - g_tm_prev_total;
    g_tm_prev_total = now;

    proc_info_t pi[18];
    int n = proc_list(pi, 18);
    int maxrows = (int)((h - 30 - 34) / 13);
    if (n > maxrows) n = maxrows;

    uint64_t busy = 0;
    for (int i = 0; i < n; i++) {
        int32_t ry = hy + 20 + i * 13;
        if (pi[i].state == PROC_RUNNING)
            gfx_fill_round_rect(cx, ry - 2, w - 24, 13, 3, GFX_RGB(0x2A, 0x31, 0x20));
        else if (i & 1)
            gfx_fill_round_rect(cx, ry - 2, w - 24, 13, 3, C_ROW_ALT);

        char num[16], row[40];
        char *p = row;
        /* id */
        u32dec((uint32_t)pi[i].id, num);
        if (num[1] == 0) { *p++ = ' '; }
        pcat(&p, num); *p++ = ' '; *p++ = ' ';
        /* name (13) */
        int j;
        for (j = 0; j < 13 && pi[i].name[j]; j++) *p++ = pi[i].name[j];
        while (j++ < 13) *p++ = ' ';
        *p++ = ' ';
        *p = 0;
        gfx_text(cx + 10, ry, row, C_TXT);

        /* state (цветной) */
        gfx_color_t sc = C_TXT2;
        if (pi[i].state == PROC_RUNNING) sc = C_GREEN;
        else if (pi[i].state == PROC_READY) sc = C_YELLOW;
        else if (pi[i].state == PROC_DEAD) sc = C_RED;
        gfx_text(cx + 10 + 18*8, ry, proc_state_name(pi[i].state), sc);

        /* ticks */
        char tk[16]; u32dec((uint32_t)pi[i].ticks, tk);
        gfx_text(cx + 10 + 25*8, ry, tk, C_TERM_LN);

        /* cpu bar (10 символов = 80px) + % */
        uint64_t dt = pi[i].ticks - g_tm_prev[pi[i].id & 31];
        g_tm_prev[pi[i].id & 31] = pi[i].ticks;
        uint32_t pct = span ? (uint32_t)(dt * 100 / span) : 0;
        if (pct > 100) pct = 100;
        if (pi[i].id != 0) busy += dt;   /* нагрузка = всё, кроме idle */
        uint32_t bw = 72;
        gfx_fill_round_rect(cx + 10 + 34*8, ry + 1, bw, 7, 3, C_BAR_BG);
        uint32_t fw = bw * pct / 100;
        if (fw) gfx_fill_round_rect(cx + 10 + 34*8, ry + 1, fw, 7, 3, C_ACCENT);
        char pc[8]; char *q = pc;
        u32dec(pct, num); pcat(&q, num); *q++ = '%'; *q = 0;
        gfx_text(cx + 10 + 34*8 + bw + 8, ry, pc, C_TXT2);

        /* type pill */
        int bg = pi[i].flags & PROC_F_BACKGROUND;
        gfx_fill_round_rect(cx + 10 + 50*8, ry - 1, 40, 12, 3, bg ? GFX_RGB(0x1E, 0x33, 0x28) : GFX_RGB(0x1E, 0x29, 0x3D));
        gfx_text(cx + 10 + 50*8 + 10, ry + 1, bg ? "bg" : "app", bg ? C_GREEN : C_BLUE);
    }
    /* нижняя строка: нагрузка */
    char num[16], line[54];
    char *p = line;
    pcat(&p, "procs: "); u32dec((uint32_t)n, num); pcat(&p, num);
    pcat(&p, "  load: ");
    uint32_t lp = span ? (uint32_t)(busy * 100 / span) : 0;
    if (lp > 100) lp = 100;
    u32dec(lp, num); pcat(&p, num); *p++ = '%'; *p = 0;
    gfx_text(cx, y + h - 20, line, C_TXT2);
}

/* ---- приложение Логи ---- */
static int g_log_scroll;             /* 0 = следим за хвостом */
static uint64_t g_log_stamp;

static gfx_color_t log_color(const char *s) {
    if (contains(s, "!!!") || contains(s, "PANIC") || contains(s, "FAIL") ||
        contains(s, "EXCEPTION")) return C_RED;
    if (has_prefix(s, "[acpi]") || has_prefix(s, "[lapic]") || has_prefix(s, "[ioapic]")) return C_CYAN;
    if (has_prefix(s, "[irq]") || has_prefix(s, "[kbd]")  || has_prefix(s, "[mouse]") ||
        has_prefix(s, "[pic]") || has_prefix(s, "[pit]"))  return C_GREEN;
    if (has_prefix(s, "[heap]") || has_prefix(s, "[pmm]")  || has_prefix(s, "[vmm]") ||
        has_prefix(s, "[mem]"))                              return C_PURPLE;
    if (has_prefix(s, "[proc]") || has_prefix(s, "[m5]")   || has_prefix(s, "[sched]")) return C_ACCENT;
    if (has_prefix(s, "[taskman]") || has_prefix(s, "[desktop]") || has_prefix(s, "[sysmon]"))
        return GFX_RGB(0x8F, 0xA3, 0xC8);
    return C_TERM_LN;
}

static void logs_draw(int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t cx = x + 10;
    int32_t cy = y + 6;
    int32_t th = h - 12;
    gfx_fill_round_rect(cx, cy, w - 20, th, 5, C_TERM_BG);
    int vis = (th - 8) / TERM_LINE_H;
    int total = logbuf_count();

    /* кламп скролла */
    int max_scroll = total - vis;
    if (max_scroll < 0) max_scroll = 0;
    if (g_log_scroll > max_scroll) g_log_scroll = max_scroll;
    int start = total - vis - g_log_scroll;
    if (start < 0) start = 0;

    for (int i = 0; i < vis; i++) {
        int li = start + i;
        if (li >= total) break;
        const char *s = logbuf_line(li);
        if (i & 1) gfx_fill_rect(cx + 4, cy + 4 + i * TERM_LINE_H, w - 28, TERM_LINE_H, C_TERM_AL);
        /* обрезка по ширине окна: макс символов */
        int maxc = (w - 40) / 8;
        char clip[LOG_COLS > 128 ? 128 : LOG_COLS + 1];
        int j = 0;
        while (s[j] && j < maxc && j < (int)sizeof(clip) - 1) { clip[j] = s[j]; j++; }
        clip[j] = 0;
        gfx_text(cx + 8, cy + 5 + i * TERM_LINE_H, clip, log_color(s));
    }

    /* скроллбар */
    if (total > vis) {
        int32_t sbx = cx + w - 26;
        int32_t sbh = th - 8;
        gfx_fill_round_rect(sbx, cy + 4, 4, sbh, 2, GFX_RGB(0x1C, 0x22, 0x32));
        int32_t thumb_h = sbh * vis / total;
        if (thumb_h < 8) thumb_h = 8;
        int32_t thumb_y = cy + 4 + (sbh - thumb_h) * (total - vis - g_log_scroll) / (total - vis);
        gfx_fill_round_rect(sbx, thumb_y, 4, thumb_h, 2, C_CYAN);
    }
    /* подсказка */
    char info[48], num[16];
    char *p = info;
    pcat(&p, "lines: ");
    u32dec((uint32_t)total, num); pcat(&p, num);
    pcat(&p, g_log_scroll ? "  (Up/Down/PgUp/PgDn scroll)" : "  (live tail)");
    gfx_text(cx + 4, y + 2, info, C_TXT2);
}

/* контент-диспетчер */
static void draw_content(int app, int32_t x, int32_t y, int32_t w, int32_t h) {
    switch (app) {
    case APP_ABOUT:   about_draw(x, y, w); break;
    case APP_TASKMAN: tm_draw(x, y, w, h); break;
    case APP_LOGS:    logs_draw(x, y, w, h); break;
    }
}

/* ---------------- курсор (save/restore, двухцветный) ---------------- */
static const uint16_t ARROW[17] = {
    0b000000000001, 0b000000000011, 0b000000000101, 0b000000001001,
    0b000000010001, 0b000000100001, 0b000001000001, 0b000010000001,
    0b000100000001, 0b001000000001, 0b011111100001, 0b000100100101,
    0b000010101001, 0b000010100011, 0b000010100001, 0b000010100000,
    0b000011000000,
};
#define CURW 14
#define CURH 19
static uint32_t g_cur_save[CURW * CURH];    /* packed-пиксели под курсором */
static int      g_cur_on;
static int32_t  g_cur_x, g_cur_y;

static void cursor_hide(void) {
    if (!g_cur_on) return;
    for (int32_t r = 0; r < CURH; r++)
        for (int32_t c = 0; c < CURW; c++) {
            int32_t x = g_cur_x + c, y = g_cur_y + r;
            if (x >= 0 && y >= 0 && (uint32_t)x < g_scr_w && (uint32_t)y < g_scr_h)
                gfx_poke((uint32_t)x, (uint32_t)y, g_cur_save[r * CURW + c]);
        }
    g_cur_on = 0;
}
static void cursor_show(int32_t x, int32_t y) {
    if (g_cur_on) cursor_hide();
    /* сохранить фон под стрелкой */
    for (int32_t r = 0; r < CURH; r++)
        for (int32_t c = 0; c < CURW; c++) {
            int32_t px = x + c, py = y + r;
            g_cur_save[r * CURW + c] =
                (px >= 0 && py >= 0 && (uint32_t)px < g_scr_w && (uint32_t)py < g_scr_h)
                ? gfx_peek((uint32_t)px, (uint32_t)py) : 0;
        }
    g_cur_x = x; g_cur_y = y; g_cur_on = 1;
    /* чёрный контур */
    for (int32_t r = 0; r < 17; r++)
        for (int32_t c = 0; c < 12; c++)
            if ((ARROW[r] >> c) & 1)
                gfx_pixel((uint32_t)(x + c), (uint32_t)(y + r), GFX_RGB(0x00, 0x00, 0x00));
    /* белое тело (та же маска со сдвигом +1,+1 → классическая заливка) */
    for (int32_t r = 0; r < 16; r++)
        for (int32_t c = 0; c < 11; c++)
            if ((ARROW[r] >> c) & 1)
                gfx_pixel((uint32_t)(x + c + 1), (uint32_t)(y + r + 1), GFX_RGB(0xFF, 0xFF, 0xFF));
}

/* ---------------- PE-призрак: квадрат, который нарисовал TESTPE.EXE ----------------
 * EXE рисует однажды при старте; после перерисовок мира рисуем за него
 * тот же узор (персистентные поверхности приложений — это уже этап M6+). */
static int g_pe_ok;
static void pe_ghost(void) {
    if (!g_pe_ok) return;
    uint32_t x0 = (g_scr_w * 3) / 4 - 32;
    uint32_t y0 = (g_scr_h * 2) / 3 - 32;
    for (uint32_t dy = 0; dy < 96; dy += 12)
        for (uint32_t dx = 0; dx < 96; dx += 12) {
            gfx_color_t c = (((dx / 12) + (dy / 12)) & 1)
                          ? GFX_RGB(0x28, 0x2C, 0x48) : GFX_RGB(0xFF, 0x9E, 0x49);
            gfx_fill_rect(x0 + dx, y0 + dy, 12, 12, c);
        }
}

/* ---------------- мир ---------------- */
static void world_draw(void) {
    gfx_blit_rows(g_bg);
    pe_ghost();
    panel_draw();
    dock_draw();
    for (int i = 0; i < APP_N; i++)
        if (g_w[g_z[i]].open) draw_window(g_z[i]);
    pill_draw();
    clock_draw();
}

/* v0.5.1: частичный ремонт при перетаскивании — обновляем ТОЛЬКО область
 * (старое∪новое положение окна + тень), а не весь мир → никакого мерцания */
static void world_repair(int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx + rw > (int32_t)g_scr_w) rw = (int32_t)g_scr_w - rx;
    if (ry + rh > (int32_t)g_scr_h) rh = (int32_t)g_scr_h - ry;
    if (rw <= 0 || rh <= 0) return;

    gfx_blit_rows_region((uint32_t)rx, (uint32_t)ry, (uint32_t)rw, (uint32_t)rh, g_bg);

    if (g_pe_ok) {   /* квадрат TESTPE.EXE */
        int32_t gx = (int32_t)((g_scr_w * 3) / 4) - 32;
        int32_t gy = (int32_t)((g_scr_h * 2) / 3) - 32;
        if (rx < gx + 96 && rx + rw > gx && ry < gy + 96 && ry + rh > gy)
            pe_ghost();
    }
    for (int i = 0; i < APP_N; i++) {           /* окна снизу вверх */
        int a = g_z[i];
        if (!g_w[a].open) continue;
        if (g_w[a].x < rx + rw && g_w[a].x + g_w[a].w + 8 > rx &&
            g_w[a].y < ry + rh && g_w[a].y + g_w[a].h + 8 > ry)
            draw_window(a);
    }
    if (ry < PANEL_H) panel_draw();
    if (ry + rh > (int32_t)(g_scr_h - DOCK_H - 16)) dock_draw();
    if (ry + rh > (int32_t)(g_scr_h - 34) && rx + rw > (int32_t)(g_scr_w - 244))
        pill_draw();
}

/* ---------------- вход и главный цикл ---------------- */
__attribute__((noreturn)) void desktop_enter(const bootinfo_t *bi,
                                             uint64_t total_mib,
                                             uint64_t free_mib) {
    gfx_init(&bi->fb);
    if (!gfx_ready())
        kpanic("desktop: no framebuffer");
    g_scr_w = gfx_width();
    g_scr_h = gfx_height();
    mouse_set_limits(g_scr_w, g_scr_h);

    /* кэш строк обоев + геометрия окон */
    g_bg = (uint32_t *)kmalloc(g_scr_h * sizeof(uint32_t));
    if (!g_bg) kpanic("desktop: no mem for wallpaper cache");
    bg_cache_build();

    g_w[APP_ABOUT].x = 48;  g_w[APP_ABOUT].y = 92;  g_w[APP_ABOUT].w = 448; g_w[APP_ABOUT].h = 236;
    g_w[APP_TASKMAN].x = 262; g_w[APP_TASKMAN].y = 64; g_w[APP_TASKMAN].w = 560; g_w[APP_TASKMAN].h = 372;
    g_w[APP_LOGS].x = 190;  g_w[APP_LOGS].y = 78;   g_w[APP_LOGS].w = 640; g_w[APP_LOGS].h = 392;

    /* статичные строки About */
    {
        char num[16]; char *p = g_ram_line;
        pcat(&p, "RAM total ");
        u32dec((uint32_t)total_mib, num); pcat(&p, num);
        pcat(&p, " MiB · free ");
        u32dec((uint32_t)free_mib, num); pcat(&p, num);
        pcat(&p, " MiB");
    }
    g_pe_line[0] = 0;

    /* стартовое состояние: открыт About */
    g_w[APP_ABOUT].open = 1;
    app_raise(APP_ABOUT);
    world_draw();
    kprintf("[desktop] %lux%lu modern UI up; F1/F2/F3 or dock clicks\n", g_scr_w, g_scr_h);

    /* PE-демо (M3): запускаем ПОСЛЕ первой отрисовки — квадрат рисует сам EXE */
    {
        int ret = pe_demo_run();
        char *p = g_pe_line;
        g_pe_ok = ((uint32_t)ret == ARES_PE_TEST_OK);
        if (g_pe_ok) {
            strcpy_small(g_pe_line, "PE test: OK (ret=0xA2E5)");
        } else {
            pcat(&p, "PE test: FAIL ret=");
            char num[16]; u32dec((uint32_t)(-(int32_t)ret), num);
            if (ret < 0) *p++ = '-';
            pcat(&p, num);
        }
        world_draw();   /* перерисуем: строка About + квадрат TESTPE.EXE виден */
    }

    /* включить курсор */
    g_cur_x = mouse_x(); g_cur_y = mouse_y();
    cursor_show(g_cur_x, g_cur_y);

    int btn_old = mouse_left();
    int drag_app = -1;
    int32_t drag_dx = 0, drag_dy = 0;
    int rep_on = 0;
    int32_t rep_rx = 0, rep_ry = 0, rep_rw = 0, rep_rh = 0;

    for (;;) {
        int need_world = 0, tm_dirty = 0, log_dirty = 0, aux_dirty = 0;
        rep_on = 0;

        /* ===== клавиатура ===== */
        int k;
        while ((k = keyboard_getch()) >= 0) {
            g_last_key = k;
            aux_dirty = 1;
            switch (k) {
            case KEY_F1: app_toggle(APP_ABOUT);   need_world = 1; break;
            case KEY_F2: app_toggle(APP_TASKMAN); need_world = 1; break;
            case KEY_F3: app_toggle(APP_LOGS);    need_world = 1; break;
            case 27: { int t = app_top();         /* Esc — закрыть верхнее */
                if (t >= 0) { g_w[t].open = 0; need_world = 1; }
                break; }
            case KEY_UP:
                if (g_w[APP_LOGS].open) { g_log_scroll += 1; log_dirty = 1; }
                break;
            case KEY_DOWN:
                if (g_w[APP_LOGS].open) { if (g_log_scroll > 0) g_log_scroll--; log_dirty = 1; }
                break;
            case KEY_PGUP:
                if (g_w[APP_LOGS].open) { g_log_scroll += 20; log_dirty = 1; }
                break;
            case KEY_PGDN:
                if (g_w[APP_LOGS].open) {
                    g_log_scroll -= 20;
                    if (g_log_scroll < 0) g_log_scroll = 0;
                    log_dirty = 1;
                }
                break;
            case KEY_MLEFT:  mouse_nudge(-14, 0); break;
            case KEY_MRIGHT: mouse_nudge(14, 0);  break;
            case KEY_MUP:    mouse_nudge(0, -14); break;
            case KEY_MDOWN:  mouse_nudge(0, 14);  break;
            default: break;
            }
        }

        /* ===== мышь ===== */
        int moved = mouse_moved();
        int left = mouse_left();
        int pressed = left && !btn_old;

        if (pressed) {
            int32_t mx = mouse_x(), my = mouse_y();
            int hit = -1, topmost = -1;
            /* окна сверху вниз */
            for (int i = APP_N - 1; i >= 0; i--) {
                int a = g_z[i];
                if (!g_w[a].open) continue;
                win_t *W = &g_w[a];
                if (mx >= W->x && mx < W->x + W->w && my >= W->y && my < W->y + W->h) {
                    topmost = a;
                    break;
                }
            }
            if (topmost >= 0) {
                win_t *W = &g_w[topmost];
                app_raise(topmost);
                need_world = 1;
                /* красная точка → закрыть */
                if (mx >= W->x + W->w - 28 && mx < W->x + W->w - 8 &&
                    my >= W->y + 6 && my < W->y + 26) {
                    W->open = 0;
                    kprintf("[desktop] window %d closed\n", topmost);
                } else if (my < W->y + WIN_BAR_H) {
                    drag_app = topmost;
                    drag_dx = mx - W->x;
                    drag_dy = my - W->y;
                }
            } else if ((hit = dock_hit(mx, my)) >= 0) {
                app_toggle(hit);
                need_world = 1;
                kprintf("[desktop] dock toggle app %d\n", hit);
            }
        }

        if (drag_app >= 0) {
            if (!left) {
                drag_app = -1;
            } else if (moved) {
                win_t *W = &g_w[drag_app];
                int32_t nx = mouse_x() - drag_dx;
                int32_t ny = mouse_y() - drag_dy;
                if (ny < PANEL_H + 2) ny = PANEL_H + 2;
                if (ny > (int32_t)g_scr_h - 40) ny = (int32_t)g_scr_h - 40;
                if (nx < -W->w + 80) nx = -W->w + 80;
                if (nx > (int32_t)g_scr_w - 80) nx = (int32_t)g_scr_w - 80;
                if (nx != W->x || ny != W->y) {
                    /* частичный ремонт: объединение старого/нового + тень */
                    int32_t ox = W->x, oy = W->y;
                    W->x = nx; W->y = ny;
                    rep_rx = ox < nx ? ox : nx;
                    rep_ry = oy < ny ? oy : ny;
                    rep_rw = (ox > nx ? ox : nx) + W->w - rep_rx + 9;
                    rep_rh = (oy > ny ? oy : ny) + W->h - rep_ry + 9;
                    rep_on = 1;
                }
            }
        }

        /* ===== периодика ===== */
        {
            uint64_t t = sched_ticks();
            uint64_t sec = t / 100;
            if (sec != g_last_sec) {
                g_last_sec = sec;
                aux_dirty = 1;                                     /* часы */
                if (g_w[APP_LOGS].open && g_log_scroll == 0) log_dirty = 1;
            }
            if (g_w[APP_TASKMAN].open && t - g_tm_stamp >= TM_REFRESH) {
                g_tm_stamp = t;
                tm_dirty = 1;
            }
            if (g_w[APP_LOGS].open && t - g_log_stamp >= 50 && g_log_scroll == 0) {
                g_log_stamp = t;
                log_dirty = 1;
            }
        }

        /* ===== отрисовка одним батчем (курсор прячем на время) ===== */
        /* окно можно перерисовать прямо только если оно ВЕРХНЕЕ —
         * иначе зальёт чужие окна → тогда перерисовываем весь мир */
        if (tm_dirty && app_top() != APP_TASKMAN) need_world = 1;
        if (log_dirty && app_top() != APP_LOGS)   need_world = 1;
        /* пилюля в углу может быть под окном → тоже через мир */
        if ((moved || aux_dirty) && !need_world) {
            for (int i = 0; i < APP_N; i++)
                if (g_w[i].open &&
                    g_w[i].x + g_w[i].w > (int32_t)g_scr_w - 240 &&
                    g_w[i].y + g_w[i].h > (int32_t)g_scr_h - 34)
                    need_world = 1;
        }

        if (need_world || rep_on || tm_dirty || log_dirty || aux_dirty || moved) {
            cursor_hide();
            if (need_world) {
                world_draw();
            } else {
                if (rep_on)    world_repair(rep_rx, rep_ry, rep_rw, rep_rh);
                if (tm_dirty)  draw_window(APP_TASKMAN);
                if (log_dirty) draw_window(APP_LOGS);
                if (aux_dirty) { clock_draw(); pill_draw(); }
                if (moved && !rep_on) pill_draw();
            }
            cursor_show(mouse_x(), mouse_y());
        }

        btn_old = left;
        __asm__ volatile ("hlt");   /* разбудит таймер 100 Гц / IRQ1 / IRQ12 */
    }
}
