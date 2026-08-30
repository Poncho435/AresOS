/* AresOS - рабочий стол v0.6.0 "glass aurora".
 *  - ДВОЙНАЯ БУФЕРИЗАЦИЯ: мир рисуется в RAM-буфер, на экран выливается
 *    только изменившаяся область -> мерцание исключено физически
 *  - стекло: настоящий alpha-блендинг (тени, док, панель, плашки)
 *  - КАЖДОЕ окно = настоящий процесс ядра (виден в диспетчере!),
 *    приложений одного типа можно открыть сколько угодно (каскад окон)
 *  - Alt+F4 / Esc / красная x = закрыть верхнее окно (процесс убивается)
 *  - колесо мыши IntelliMouse: прокрутка логов и файлов
 *  - frame pacing: рендер не чаще ~50 fps, hover - по факту изменений
 *  - приложения: О системе (F1), Задачи (F2), Логи (F3), Файлы (F4), Просмотр
 *  - Ctrl+стрелки = клавиатурная мышь (спасение в виртуалках) */
#include "desktop.h"
#include "gfx.h"
#include "mouse.h"
#include "keyboard.h"
#include "proc.h"
#include "logbuf.h"
#include "kprintf.h"
#include "pe.h"
#include "heap.h"
#include "pmm.h"
#include "fb_console.h"
#include <stdint.h>
#include <string.h>

#define PANEL_H     34
#define DOCK_H      58
#define DOCK_ICON   44
#define DOCK_STEP   56
#define WIN_BAR_H   30
#define TERM_LINE_H 10
#define TM_REFRESH  33            /* 3 раза/сек */
#define FRAME_MIN   2             /* минимум тиков между "мягкими" кадрами */
#define LOGS_REFRESH 50

/* ---------- палитра "glass aurora" ---------- */
static const gfx_color_t C_PANEL   = GFX_RGB(0x0B, 0x0E, 0x17);
static const gfx_color_t C_PLINE   = GFX_RGB(0x2A, 0x30, 0x48);
static const gfx_color_t C_TXT     = GFX_RGB(0xE7, 0xEA, 0xF3);
static const gfx_color_t C_TXT2    = GFX_RGB(0x8B, 0x92, 0xA9);
static const gfx_color_t C_ACCENT  = GFX_RGB(0xFF, 0x9E, 0x49);
static const gfx_color_t C_GREEN   = GFX_RGB(0x4F, 0xC3, 0x7B);
static const gfx_color_t C_BLUE    = GFX_RGB(0x4A, 0x9D, 0xFF);
static const gfx_color_t C_CYAN    = GFX_RGB(0x56, 0xC2, 0xE8);
static const gfx_color_t C_PURPLE  = GFX_RGB(0xB0, 0x8A, 0xE0);
static const gfx_color_t C_YELLOW  = GFX_RGB(0xE8, 0xC1, 0x5A);
static const gfx_color_t C_RED     = GFX_RGB(0xFF, 0x5F, 0x57);
static const gfx_color_t C_AMBER   = GFX_RGB(0xFE, 0xBC, 0x2E);
static const gfx_color_t C_LIME    = GFX_RGB(0x28, 0xC8, 0x40);
static const gfx_color_t C_WIN_BG  = GFX_RGB(0x19, 0x1D, 0x29);
static const gfx_color_t C_WIN_BAR = GFX_RGB(0x22, 0x27, 0x37);
static const gfx_color_t C_WIN_BR  = GFX_RGB(0x0B, 0x0D, 0x14);
static const gfx_color_t C_ROW_ALT = GFX_RGB(0x1F, 0x24, 0x33);
static const gfx_color_t C_BAR_BG  = GFX_RGB(0x26, 0x2B, 0x3D);
static const gfx_color_t C_TERM_BG = GFX_RGB(0x0B, 0x0F, 0x16);
static const gfx_color_t C_TERM_LN = GFX_RGB(0xAE, 0xB6, 0xC8);
static const gfx_color_t C_TERM_AL = GFX_RGB(0x0E, 0x13, 0x20);
static const gfx_color_t C_SHADOW  = GFX_RGB(0x03, 0x04, 0x08);
static const gfx_color_t C_DOCK    = GFX_RGB(0x10, 0x13, 0x1F);
static const gfx_color_t C_DOCK_BR = GFX_RGB(0x3A, 0x42, 0x60);
static const gfx_color_t C_GLASS   = GFX_RGB(0x1E, 0x25, 0x38);

/* обои: трёхстопный градиент (кэш packed-строк ниже) */
static const gfx_color_t BG0 = GFX_RGB(0x0B, 0x0F, 0x26);
static const gfx_color_t BG1 = GFX_RGB(0x14, 0x1B, 0x42);
static const gfx_color_t BG2 = GFX_RGB(0x0C, 0x2C, 0x3C);

static uint32_t *g_bg;            /* packed цвет каждой строки экрана */
static uint32_t *g_back;          /* RAM-буфер кадра (двойная буферизация) */
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
static int  utf_glyphs(const char *s) {          /* глифов в строке UTF-8 */
    int n = 0;
    for (; *s; s++) if (((uint8_t)*s & 0xC0) != 0x80) n++;
    return n;
}

/* ---------------- окна: экземпляры = процессы ---------------- */
enum { APP_ABOUT = 0, APP_TASKMAN, APP_LOGS, APP_FILES, APP_VIEW, APP_TN };
#define LAUNCH_N 4                    /* пускатели в доке/панели (без просмотрщика) */
#define MAX_WIN 10

typedef struct {
    int  open;
    int  app;                         /* тип приложения */
    int  pid;                         /* слот процесса-владельца в планировщике */
    int32_t x, y, w, h;
    int  aux;                         /* logs/view: scroll; files: каталог */
    char name[24];                    /* view: имя открытого файла */
    const char *text;                 /* view: текст */
} win_t;

static win_t g_w[MAX_WIN];
static int   g_z[MAX_WIN];            /* z-порядок: g_z[0] - нижнее, [zn-1] - верх */
static int   g_zn;

static const struct { const char *t; const char *pname; gfx_color_t c; } APP_META[APP_TN] = {
    { "О системе AresOS",        "w:Инфо",   GFX_RGB(0x4A, 0x9D, 0xFF) },
    { "Диспетчер задач",         "w:Задачи", GFX_RGB(0xFF, 0x9E, 0x49) },
    { "Логи системы - терминал", "w:Логи",   GFX_RGB(0x4F, 0xC3, 0x7B) },
    { "Проводник - Мои файлы",   "w:Файлы",  GFX_RGB(0xE8, 0xC1, 0x5A) },
    { "Просмотр файла",          "w:Текст",  GFX_RGB(0xB0, 0x8A, 0xE0) },
};

/* процесс-оболочка окна: живёт, пока окно открыто; тики идут ему в диспетчер */
static void app_win_proc(void *arg) {
    (void)arg;
    for (;;) proc_sleep(3600000);
}

static int app_top(void) { return g_zn ? g_z[g_zn - 1] : -1; }

static void z_push(int idx)  { g_z[g_zn++] = idx; }
static void z_remove(int idx) {
    for (int i = 0; i < g_zn; i++)
        if (g_z[i] == idx) {
            for (; i < g_zn - 1; i++) g_z[i] = g_z[i + 1];
            g_zn--;
            return;
        }
}
static void win_raise(int idx) { z_remove(idx); z_push(idx); }

static int win_open_count(int app) {
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++) if (g_w[i].open && g_w[i].app == app) n++;
    return n;
}

/* открыть НОВОЕ окно приложения (мультизапуск; viewer - через параметры) */
static int launch_app(int app, const char *vfile_name, const char *vfile_text) {
    int idx = -1;
    for (int i = 0; i < MAX_WIN; i++)
        if (!g_w[i].open) { idx = i; break; }
    if (idx < 0) {
        kprintf("[desktop] все %d окон заняты - закрой что-нибудь\n", MAX_WIN);
        return -1;
    }
    win_t *W = &g_w[idx];
    memset(W, 0, sizeof(*W));
    W->open = 1;
    W->app  = app;
    static const int32_t GEO[APP_TN][4] = {
        { 120, 84, 448, 236 },   /* О системе */
        { 262, 64, 560, 372 },   /* Диспетчер */
        { 190, 78, 640, 392 },   /* Логи */
        { 150, 96, 420, 280 },   /* Файлы */
        { 230, 130, 446, 270 },  /* Просмотр */
    };
    int off = (g_zn % 5) * 26;
    W->x = GEO[app][0] + off; W->y = GEO[app][1] + off;
    W->w = GEO[app][2];       W->h = GEO[app][3];
    if (W->x + W->w > (int32_t)g_scr_w - 8) W->x = (int32_t)g_scr_w - W->w - 8;
    if (W->y + W->h > (int32_t)g_scr_h - 70) W->y = PANEL_H + 4;
    if (app == APP_VIEW && vfile_text) {
        int i = 0;
        for (const char *s = vfile_name; *s && i < 23; s++) W->name[i++] = *s;
        W->name[i] = 0;
        W->text = vfile_text;
    }
    W->pid = proc_spawn(app_win_proc, 0, APP_META[app].pname, 0);
    if (W->pid < 0) { W->open = 0; return -1; }
    z_push(idx);
    kprintf("[desktop] окно '%s' открыто (pid #%d, всего %d)\n",
            APP_META[app].pname, W->pid, g_zn);
    return idx;
}

static void close_window(int idx) {
    win_t *W = &g_w[idx];
    if (!W->open) return;
    if (W->pid >= 4) proc_kill_slot(W->pid);   /* процесс окна умирает */
    W->open = 0;
    z_remove(idx);
    kprintf("[desktop] окно '%s' закрыто (осталось %d)\n",
            APP_META[W->app].pname, g_zn);
}

/* ---------------- обои ---------------- */
static void bg_cache_build(void) {
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

/* ---------------- панель (стекло) ---------------- */
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
    uint32_t cx = g_scr_w - 100;
    gfx_blend_round_rect(cx, 7, 88, 19, 8, C_GLASS, 200);
    gfx_text(cx + 12, 13, buf, C_TXT);
}

static void panel_draw(void) {
    gfx_blend_rect(0, 0, g_scr_w, PANEL_H, C_PANEL, 225);       /* стекло */
    gfx_blend_rect(0, 0, g_scr_w, 1, GFX_RGB(0xFF, 0xFF, 0xFF), 18);
    gfx_fill_rect(0, PANEL_H - 1, g_scr_w, 1, C_PLINE);
    gfx_text_bold(14, 13, "AresOS", C_ACCENT);
    gfx_blend_round_rect(86, 8, 62, 18, 6, C_GLASS, 170);
    gfx_text(94, 13, "v0.6.3", C_TXT2);
    static const char *BTN[LAUNCH_N] = { "О системе", "Задачи", "Логи", "Файлы" };
    static const gfx_color_t BC[LAUNCH_N] = { C_BLUE, C_ACCENT, C_GREEN, C_YELLOW };
    int32_t mx = mouse_x(), my = mouse_y();
    for (int i = 0; i < LAUNCH_N; i++) {
        int32_t bx = 160 + i * 98;
        int hov = (mx >= bx && mx < bx + 90 && my < PANEL_H);
        if (hov) gfx_blend_round_rect(bx, 5, 90, 24, 7, C_GLASS, 220);
        gfx_fill_round_rect(bx + 9, 14, 8, 8, 3, BC[i]);
        gfx_text(bx + 24, 13, BTN[i], hov ? C_TXT : C_TXT2);
        if (win_open_count(i))
            gfx_fill_round_rect(bx + 10, 26, 70, 2, 1, BC[i]);
    }
    clock_draw();
}

static int panel_hit(int32_t mx, int32_t my) {
    if (my >= PANEL_H) return -1;
    for (int i = 0; i < LAUNCH_N; i++) {
        int32_t bx = 160 + i * 98;
        if (mx >= bx && mx < bx + 90) return i;
    }
    return -1;
}

/* ---------------- док (стекло, отскок при hover) ---------------- */
static uint32_t dock_x(void) { return (g_scr_w - (LAUNCH_N * DOCK_STEP + 12)) / 2; }
static uint32_t dock_y(void) { return g_scr_h - DOCK_H - 12; }

typedef struct { const char *glyph; gfx_color_t col; } icon_t;
static const icon_t ICONS[LAUNCH_N] = {
    { "i",  GFX_RGB(0x4A, 0x9D, 0xFF) },
    { "TM", GFX_RGB(0xFF, 0x9E, 0x49) },
    { ">_", GFX_RGB(0x4F, 0xC3, 0x7B) },
    { "FM", GFX_RGB(0xE8, 0xC1, 0x5A) },
};

static int dock_hit(int32_t mx, int32_t my) {
    uint32_t dx = dock_x(), dy = dock_y();
    if (my < (int32_t)dy || my >= (int32_t)(dy + DOCK_H)) return -1;
    for (int i = 0; i < LAUNCH_N; i++) {
        int32_t ix = (int32_t)(dx + 12 + i * DOCK_STEP);
        if (mx >= ix && mx < ix + DOCK_ICON) return i;
    }
    return -1;
}

static void dock_draw(void) {
    uint32_t dx = dock_x(), dy = dock_y();
    uint32_t dw = LAUNCH_N * DOCK_STEP + 12;
    gfx_blend_round_rect(dx - 1, dy - 1, dw + 2, DOCK_H + 2, 14, C_DOCK_BR, 140);
    gfx_blend_round_rect(dx, dy, dw, DOCK_H, 13, C_DOCK, 215);        /* стекло */
    gfx_blend_round_rect(dx + 4, dy + 3, dw - 8, 12, 9,
                         GFX_RGB(0xFF, 0xFF, 0xFF), 14);             /* блик */
    int32_t mx = mouse_x(), my = mouse_y();
    int hov = dock_hit(mx, my);
    for (int i = 0; i < LAUNCH_N; i++) {
        uint32_t ix = dx + 12 + (uint32_t)i * DOCK_STEP;
        uint32_t iy = dy + 7;
        gfx_color_t c = ICONS[i].col;
        if (i == hov) {
            iy -= 3;                                                  /* подскок */
            c.r = (uint8_t)(c.r < 220 ? c.r + 35 : 255);
            c.g = (uint8_t)(c.g < 220 ? c.g + 35 : 255);
            c.b = (uint8_t)(c.b < 220 ? c.b + 35 : 255);
        }
        gfx_blend_round_rect(ix + 2, iy + 4, DOCK_ICON, DOCK_ICON, 10, C_SHADOW, 90);
        gfx_fill_round_rect(ix, iy, DOCK_ICON, DOCK_ICON, 10, c);
        gfx_fill_round_rect(ix + 5, iy + 4, 34, 9, 5, GFX_RGB(0xFF, 0xFF, 0xFF));
        gfx_fill_round_rect(ix + 5, iy + 4, 34, 7, 4, c);
        gfx_text_bold(ix + (DOCK_ICON - 8 * 2) / 2 + (ICONS[i].glyph[1] ? 0 : 4),
                      iy + DOCK_ICON / 2 - 4, ICONS[i].glyph, GFX_RGB(0xFF, 0xFF, 0xFF));
        if (win_open_count(i))
            gfx_fill_round_rect(ix + DOCK_ICON / 2 - 3, dy + DOCK_H - 6, 6, 3, 1, GFX_RGB(0xCF, 0xD6, 0xEA));
    }
}

/* ---------------- иконки на рабочем столе ---------------- */
#define DESK_X   24
#define DESK_Y   64
#define DESK_STEP 86
#define DESK_COL_W 80

static const struct { const char *glyph; const char *label; int app; gfx_color_t col; }
DICON[LAUNCH_N] = {
    { "i",  "Компьютер", APP_ABOUT,   GFX_RGB(0x4A, 0x9D, 0xFF) },
    { "FM", "Файлы",     APP_FILES,   GFX_RGB(0xE8, 0xC1, 0x5A) },
    { "TM", "Задачи",    APP_TASKMAN, GFX_RGB(0xFF, 0x9E, 0x49) },
    { ">_", "Логи",      APP_LOGS,    GFX_RGB(0x4F, 0xC3, 0x7B) },
};

static int desk_hit(int32_t mx, int32_t my) {
    for (int i = 0; i < LAUNCH_N; i++) {
        int32_t iy = DESK_Y + i * DESK_STEP;
        if (mx >= DESK_X - 6 && mx < DESK_X - 6 + DESK_COL_W &&
            my >= iy - 4 && my < iy + 66) return i;
    }
    return -1;
}

static void deskicons_draw(void) {
    int32_t mx = mouse_x(), my = mouse_y();
    int hov = desk_hit(mx, my);
    for (int i = 0; i < LAUNCH_N; i++) {
        int32_t ix = DESK_X + (DESK_COL_W - 46) / 2 - 6;
        int32_t iy = DESK_Y + i * DESK_STEP;
        gfx_color_t c = DICON[i].col;
        if (i == hov) {
            gfx_blend_round_rect(DESK_X - 6, iy - 4, DESK_COL_W, 68, 10, C_GLASS, 170);
            c.r = (uint8_t)(c.r < 220 ? c.r + 35 : 255);
            c.g = (uint8_t)(c.g < 220 ? c.g + 35 : 255);
            c.b = (uint8_t)(c.b < 220 ? c.b + 35 : 255);
        }
        gfx_blend_round_rect(ix + 2, iy + 3, 46, 46, 11, C_SHADOW, 80);
        gfx_fill_round_rect(ix, iy, 46, 46, 11, c);
        gfx_fill_round_rect(ix + 5, iy + 4, 36, 9, 5, GFX_RGB(0xFF, 0xFF, 0xFF));
        gfx_fill_round_rect(ix + 5, iy + 4, 36, 7, 4, c);
        gfx_text_bold(ix + (46 - 16) / 2 + (DICON[i].glyph[1] ? 0 : 6),
                      iy + 46 / 2 - 4, DICON[i].glyph, GFX_RGB(0xFF, 0xFF, 0xFF));
        int lpx = utf_glyphs(DICON[i].label) * 8;
        int lx = DESK_X - 6 + (DESK_COL_W - lpx) / 2;
        if (lx < DESK_X - 6) lx = DESK_X - 6;
        gfx_text(lx + 1, iy + 51, DICON[i].label, GFX_RGB(0x04, 0x05, 0x0A));
        gfx_text(lx, iy + 50, DICON[i].label, C_TXT);
    }
}

/* ---------------- окна: каркас (стекло + трафик-лампы) ---------------- */
static void draw_content(int idx, int32_t x, int32_t y, int32_t w, int32_t h);

static void draw_window(int idx) {
    win_t *W = &g_w[idx];
    int32_t x = W->x, y = W->y, w = W->w, h = W->h;
    int top = (idx == app_top());
    /* двойная мягкая тень */
    gfx_blend_round_rect(x + 8, y + 10, w, h, 10, C_SHADOW, 40);
    gfx_blend_round_rect(x + 3, y + 5,  w, h, 9,  C_SHADOW, 70);
    /* рамка + тело */
    gfx_fill_round_rect(x - 1, y - 1, w + 2, h + 2, 9, C_WIN_BR);
    gfx_fill_round_rect(x, y, w, h, 8, C_WIN_BG);
    /* заголовок: стеклянная полоса */
    gfx_fill_round_rect(x, y, w, WIN_BAR_H, 8, top ? C_WIN_BAR : GFX_RGB(0x1C, 0x20, 0x2D));
    gfx_fill_rect(x, y + WIN_BAR_H / 2, w, WIN_BAR_H / 2, top ? C_WIN_BAR : GFX_RGB(0x1C, 0x20, 0x2D));
    gfx_blend_rect(x + 4, y + 2, w - 8, 10, GFX_RGB(0xFF, 0xFF, 0xFF), 10);
    gfx_fill_rect(x, y + WIN_BAR_H, w, 1, C_WIN_BR);
    /* цветная точка приложения + заголовок */
    gfx_fill_round_rect(x + 12, y + 9, 12, 12, 4, APP_META[W->app].c);
    gfx_text_bold(x + 32, y + 11, APP_META[W->app].t, top ? C_TXT : C_TXT2);
    /* traffic lights: желтая/зелёная - декор, красная со x = ЗАКРЫТЬ */
    gfx_fill_round_rect(x + w - 59, y + 10, 10, 10, 5, C_LIME);
    gfx_fill_round_rect(x + w - 43, y + 10, 10, 10, 5, C_AMBER);
    int hovx = (mouse_x() >= x + w - 28 && mouse_x() < x + w - 8 &&
                mouse_y() >= y + 6 && mouse_y() < y + 26);
    gfx_fill_round_rect(x + w - 26, y + 9, 12, 12, 5,
                        hovx ? GFX_RGB(0xFF, 0x77, 0x70) : C_RED);
    for (int i = 0; i < 6; i++) {
        gfx_pixel((uint32_t)(x + w - 23 + i),     (uint32_t)(y + 12 + i), GFX_RGB(0xFF, 0xEE, 0xEE));
        gfx_pixel((uint32_t)(x + w - 23 + 5 - i), (uint32_t)(y + 12 + i), GFX_RGB(0xFF, 0xEE, 0xEE));
    }
    draw_content(idx, x, y + WIN_BAR_H, w, h - WIN_BAR_H);
}

/* ---- приложение О системе ---- */
static char g_ram_line[96];   /* v0.6.2: было 48 - УЕЗЖАЛО за границу при
                                 RAM >= 10 ГиБ (4 цифры + кириллица UTF-8!) */
static char g_pe_line[40];
static void about_draw(int32_t x, int32_t y, int32_t w) {
    (void)w;
    int32_t cx = x + 16;
    int32_t yy = y + 14;
    gfx_text_bold(cx, yy, "Ядро AresOS 0.6.3 (x86-64)", C_TXT); yy += 18;
    gfx_text(cx, yy, g_ram_line, C_TXT2); yy += 14;
    gfx_text(cx, yy, "Окна = процессы. Куча/VMM живы, PE32+ в ядре", C_TXT2); yy += 14;
    if (g_pe_line[0]) { gfx_text(cx, yy, g_pe_line, C_GREEN); yy += 14; }
    yy += 4;
    gfx_fill_rect(cx, yy, 250, 1, C_PLINE); yy += 8;
    gfx_text(cx, yy, "Запуск: иконки стола, док, панель, F1-F4.", C_ACCENT); yy += 12;
    gfx_text(cx, yy, "Окон много: каждый клик открывает новое!", C_ACCENT); yy += 12;
    gfx_text(cx, yy, "Закрыть: красная x, Esc или Alt+F4.", C_TXT2); yy += 12;
    gfx_text(cx, yy, "Колесо мыши крутит логи и файлы.", C_TXT2); yy += 12;
    gfx_text(cx, yy, "Ctrl+стрелки - курсор без мыши.", C_TXT2);
}

/* ---- приложение Диспетчер задач ---- */
static uint64_t g_tm_prev_total;
static uint64_t g_tm_prev[32];
static uint64_t g_tm_stamp;

static void tm_draw(int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t cx = x + 12;
    int32_t hy = y + 8;
    gfx_fill_round_rect(cx, hy, w - 24, 16, 5, C_ROW_ALT);
    const gfx_color_t HC = C_CYAN;
    gfx_text(cx + 10,        hy + 4, "id",    HC);
    gfx_text(cx + 10 + 4*8,  hy + 4, "имя",   HC);
    gfx_text(cx + 10 + 18*8, hy + 4, "статус", HC);
    gfx_text(cx + 10 + 25*8, hy + 4, "тики",  HC);
    gfx_text(cx + 10 + 34*8, hy + 4, "cpu",   HC);
    gfx_text(cx + 10 + 50*8, hy + 4, "тип",   HC);

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
            gfx_fill_round_rect(cx, ry - 2, w - 24, 13, 4, GFX_RGB(0x2A, 0x31, 0x20));
        else if (i & 1)
            gfx_fill_round_rect(cx, ry - 2, w - 24, 13, 4, C_ROW_ALT);
        if (pi[i].state == PROC_DEAD)
            gfx_fill_round_rect(cx, ry - 2, w - 24, 13, 4, GFX_RGB(0x23, 0x1E, 0x22));

        char num[16], row[40];
        char *p = row;
        u32dec((uint32_t)pi[i].id, num);
        if (num[1] == 0) { *p++ = ' '; }
        pcat(&p, num); *p++ = ' '; *p++ = ' ';
        int j;
        for (j = 0; j < 13 && pi[i].name[j]; j++) *p++ = pi[i].name[j];
        while (j++ < 13) *p++ = ' ';
        *p++ = ' ';
        *p = 0;
        gfx_text(cx + 10, ry, row, pi[i].state == PROC_DEAD ? C_TXT2 : C_TXT);

        gfx_color_t sc = C_TXT2;
        if (pi[i].state == PROC_RUNNING) sc = C_GREEN;
        else if (pi[i].state == PROC_READY) sc = C_YELLOW;
        else if (pi[i].state == PROC_DEAD) sc = C_RED;
        gfx_text(cx + 10 + 18*8, ry, proc_state_name(pi[i].state), sc);

        char tk[16]; u32dec((uint32_t)pi[i].ticks, tk);
        gfx_text(cx + 10 + 25*8, ry, tk, C_TERM_LN);

        uint64_t dt = pi[i].ticks - g_tm_prev[pi[i].id & 31];
        g_tm_prev[pi[i].id & 31] = pi[i].ticks;
        uint32_t pct = span ? (uint32_t)(dt * 100 / span) : 0;
        if (pct > 100) pct = 100;
        if (pi[i].id != 0) busy += dt;
        uint32_t bw = 72;
        gfx_fill_round_rect(cx + 10 + 34*8, ry + 1, bw, 7, 3, C_BAR_BG);
        uint32_t fw = bw * pct / 100;
        if (fw) gfx_fill_round_rect(cx + 10 + 34*8, ry + 1, fw, 7, 3, C_ACCENT);
        char pc[8]; char *q = pc;
        u32dec(pct, num); pcat(&q, num); *q++ = '%'; *q = 0;
        gfx_text(cx + 10 + 34*8 + bw + 8, ry, pc, C_TXT2);

        int bg = pi[i].flags & PROC_F_BACKGROUND;
        int isw = pi[i].name[0] == 'w' && pi[i].name[1] == ':';
        gfx_fill_round_rect(cx + 10 + 50*8 - 1, ry - 2, 42, 13, 4,
                            bg ? GFX_RGB(0x1E, 0x33, 0x28)
                               : (isw ? GFX_RGB(0x2A, 0x21, 0x38) : GFX_RGB(0x1E, 0x29, 0x3D)));
        gfx_text(cx + 10 + 50*8 + 5, ry + 1,
                 bg ? "фон" : (isw ? "окно" : "обыч"),
                 bg ? C_GREEN : (isw ? C_PURPLE : C_BLUE));
    }
    char num[16], line[54];
    char *p = line;
    pcat(&p, "процессов: "); u32dec((uint32_t)n, num); pcat(&p, num);
    pcat(&p, "  нагрузка: ");
    uint32_t lp = span ? (uint32_t)(busy * 100 / span) : 0;
    if (lp > 100) lp = 100;
    u32dec(lp, num); pcat(&p, num); *p++ = '%'; *p = 0;
    gfx_text(cx, y + h - 20, line, C_TXT2);
}

/* ---- приложение Логи ---- */
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

static void logs_draw(int idx, int32_t x, int32_t y, int32_t w, int32_t h) {
    win_t *W = &g_w[idx];
    int32_t cx = x + 10;
    int32_t cy = y + 6;
    int32_t th = h - 12;
    gfx_fill_round_rect(cx, cy, w - 20, th, 6, C_TERM_BG);
    int vis = (th - 8) / TERM_LINE_H;
    int total = logbuf_count();

    int max_scroll = total - vis;
    if (max_scroll < 0) max_scroll = 0;
    if (W->aux > max_scroll) W->aux = max_scroll;
    if (W->aux < 0) W->aux = 0;
    int start = total - vis - W->aux;
    if (start < 0) start = 0;

    for (int i = 0; i < vis; i++) {
        int li = start + i;
        if (li >= total) break;
        const char *s = logbuf_line(li);
        if (i & 1) gfx_fill_round_rect(cx + 4, cy + 4 + i * TERM_LINE_H, w - 28, TERM_LINE_H, 3, C_TERM_AL);
        int maxc = (w - 40) / 8;
        char clip[LOG_COLS > 128 ? 128 : LOG_COLS + 1];
        int j = 0;
        while (s[j] && j < maxc && j < (int)sizeof(clip) - 1) { clip[j] = s[j]; j++; }
        clip[j] = 0;
        gfx_text(cx + 8, cy + 5 + i * TERM_LINE_H, clip, log_color(s));
    }

    if (total > vis) {
        int32_t sbx = cx + w - 26;
        int32_t sbh = th - 8;
        gfx_fill_round_rect(sbx, cy + 4, 4, sbh, 2, GFX_RGB(0x1C, 0x22, 0x32));
        int32_t thumb_h = sbh * vis / total;
        if (thumb_h < 8) thumb_h = 8;
        int32_t thumb_y = cy + 4 + (sbh - thumb_h) * (total - vis - W->aux) / (total - vis);
        gfx_fill_round_rect(sbx, thumb_y, 4, thumb_h, 2, C_CYAN);
    }
    char info[56], num[16];
    char *p = info;
    pcat(&p, "строк: ");
    u32dec((uint32_t)total, num); pcat(&p, num);
    pcat(&p, W->aux ? "  (стрелки/колесо - листать)" : "  (прямой хвост)");
    gfx_text(cx + 4, y + 2, info, C_TXT2);
}

/* ---- Проводник (демо-ФС в памяти; драйвер диска - этап M6) ---- */
typedef struct { const char *name; int is_dir; int dir; const char *text; } fitem_t;

static const fitem_t FS_ROOT[] = {
    { "Документы", 1, 1, 0 },
    { "Система",   1, 2, 0 },
    { "README.TXT", 0, 0,
      "AresOS v0.6.3\n64-битная ОС голого железа: свой загрузчик UEFI,\nсвоё ядро, свой графический рабочий стол.\nПроводник показывает демо-файлы ИЗ ПАМЯТИ -\nдрайвер диска и настоящая ФС придут на этапе M6.\n" },
    { "version.txt", 0, 0,
      "kernel 0.6.3 (x86-64)\nAPIC/PIC 100 Гц, PMM+VMM+heap, PE32+ loader\nокна-процессы, стекло-UI, двойная буферизация\n" },
    { "testpe.exe", 0, 0,
      "PE32+ тестовая программа (проверка M3):\nрисует шахматку 96x96 прямо на рабочем столе\nи возвращает 0xA2E5. Загружается строго по\nспецификации PE/COFF (секции, релоки, импорты).\n" },
};
static const fitem_t FS_DOC[] = {
    { "привет.txt", 0, 0,
      "Привет из Проводника AresOS!\nЭтот файл пока живёт в памяти ядра.\nСкоро здесь будут настоящие файлы с диска.\n" },
    { "план.txt", 0, 0,
      "План этапа M6:\n1. драйвер диска (AHCI / initrd)\n2. VFS - единый интерфейс ФС\n3. FAT32 чтение+запись\n4. и тогда этот Проводник станет настоящим\n" },
};
static const fitem_t FS_SYS[] = {
    { "kernel.elf", 0, 0,
      "Ядро AresOS - этот файл настоящий!\nGDT/IDT, PMM/VMM/heap, LAPIC+PIC, планировщик,\nPS/2 клавиатура+мышь и весь этот рабочий стол.\n" },
    { "bootx64.efi", 0, 0,
      "Наш UEFI-загрузчик: ELF-парсер, графика GOP,\nпоиск ACPI RSDP, ExitBootServices.\nПишется без gnu-efi - только свои структуры.\n" },
    { "config.ini", 0, 0,
      "theme=glass-aurora\nfont=8x8-cyrillic\nmouse=ps2+wheel\ntimer=PIT-100Hz\n" },
};
static const struct { const fitem_t *it; int n; const char *path; } FS_DIRS[] = {
    { FS_ROOT, 5, "Мой компьютер" },
    { FS_DOC,  2, "Мой компьютер / Документы" },
    { FS_SYS,  3, "Мой компьютер / Система" },
};
#define FILE_ROW_H 15

static void files_draw(int idx, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)h;
    win_t *W = &g_w[idx];
    int dir = W->aux;
    if (dir < 0 || dir > 2) dir = W->aux = 0;
    int32_t cx = x + 12;
    gfx_text_bold(cx, y + 6, FS_DIRS[dir].path, C_YELLOW);
    gfx_blend_round_rect(x + w - 128, y + 2, 116, 16, 6, C_ROW_ALT, 220);
    char num[16], sz[34]; char *p = sz;
    pcat(&p, "RAM, своб. ");
    u32dec((uint32_t)(pmm_free_pages() * 4 / 1024), num); pcat(&p, num);
    pcat(&p, " МиБ");
    gfx_text(x + w - 122, y + 6, sz, C_TXT2);
    gfx_fill_rect(cx, y + 18, w - 24, 1, C_PLINE);

    const fitem_t *items = FS_DIRS[dir].it;
    int n = FS_DIRS[dir].n;
    int rows = n + (dir ? 1 : 0);
    int32_t mx = mouse_x(), my = mouse_y();
    for (int r = 0; r < rows; r++) {
        int32_t ry = y + 24 + r * FILE_ROW_H;
        int up = (dir && r == 0);
        const fitem_t *it = up ? 0 : &items[r - (dir ? 1 : 0)];
        int hov = (mx >= x + 4 && mx < x + w - 4 && my >= ry - 1 && my < ry + FILE_ROW_H - 1);
        if (hov)
            gfx_blend_round_rect(x + 6, ry - 1, w - 12, FILE_ROW_H, 5, C_GLASS, 200);
        else if (r & 1)
            gfx_fill_round_rect(x + 6, ry - 1, w - 12, FILE_ROW_H, 5, C_ROW_ALT);
        int is_dir = up || it->is_dir;
        gfx_fill_round_rect(cx, ry + 1, 11, 11, 4, is_dir ? C_YELLOW : C_BLUE);
        gfx_text(cx + 18, ry + 3, up ? ".." : it->name, hov ? C_TXT : (up ? C_YELLOW : C_TXT));
        gfx_text(x + w - 64, ry + 3, is_dir ? "папка" : "файл", C_TXT2);
    }
    gfx_text(cx, y + 24 + rows * FILE_ROW_H + 6,
             "клик: открыть / зайти. Это окно - тоже процесс!", C_TXT2);
}

static int files_click(int idx, int32_t mx, int32_t my) {
    win_t *W = &g_w[idx];
    (void)mx;
    int32_t wy = W->y + WIN_BAR_H;
    if (my < wy + 24 || my >= wy + 24 + 16 * FILE_ROW_H) return 0;
    int r = (my - (wy + 24)) / FILE_ROW_H;
    int rows = FS_DIRS[W->aux].n + (W->aux ? 1 : 0);
    if (r >= rows) return 0;
    if (W->aux && r == 0) { W->aux = 0; return 1; }
    const fitem_t *it = &FS_DIRS[W->aux].it[r - (W->aux ? 1 : 0)];
    if (it->is_dir) { W->aux = it->dir; return 1; }
    launch_app(APP_VIEW, it->name, it->text);   /* открыть файл новым окном-процессом */
    return 1;
}

/* ---- Просмотр файла ---- */
static int count_lines(const char *s) {
    int n = 1;
    for (; s && *s; s++) if (*s == '\n') n++;
    return n;
}

static void view_draw(int idx, int32_t x, int32_t y, int32_t w, int32_t h) {
    win_t *W = &g_w[idx];
    int32_t cx = x + 12;
    gfx_text_bold(cx, y + 6, W->name[0] ? W->name : "(файл)", C_PURPLE);
    gfx_text(x + w - 150, y + 6, "Alt+F4 - закрыть", C_TXT2);
    gfx_fill_rect(cx, y + 18, w - 24, 1, C_PLINE);
    gfx_fill_round_rect(cx, y + 22, w - 24, h - 34, 6, C_TERM_BG);

    int vis = (h - 42) / 11;
    int total = count_lines(W->text);
    int maxs = total - vis; if (maxs < 0) maxs = 0;
    if (W->aux > maxs) W->aux = maxs;
    if (W->aux < 0) W->aux = 0;

    int skip = W->aux, row = 0;
    char line[64]; int li = 0;
    for (const char *s = W->text; s && *s; s++) {
        if (*s == '\n') {
            line[li] = 0;
            if (skip > 0) skip--;
            else if (row < vis) {
                int maxc = (w - 44) / 8;
                line[maxc < li ? maxc : li] = 0;
                gfx_text(cx + 8, y + 27 + row * 11, line, C_TERM_LN);
                row++;
            }
            li = 0;
        } else if (li < 63) line[li++] = *s;
    }
    line[li] = 0;
    if (skip == 0 && row < vis)
        gfx_text(cx + 8, y + 27 + row * 11, line, C_TERM_LN);

    if (total > vis) {
        char info[40], num[16]; char *p = info;
        pcat(&p, "строка "); u32dec((uint32_t)(W->aux + 1), num); pcat(&p, num);
        pcat(&p, "/"); u32dec((uint32_t)total, num); pcat(&p, num);
        pcat(&p, " (стрелки/колесо)");
        gfx_text(cx, y + h - 12, info, C_TXT2);
    }
}

/* контент-диспетчер */
static void draw_content(int idx, int32_t x, int32_t y, int32_t w, int32_t h) {
    switch (g_w[idx].app) {
    case APP_ABOUT:   about_draw(x, y, w); break;
    case APP_TASKMAN: tm_draw(x, y, w, h); break;
    case APP_LOGS:    logs_draw(idx, x, y, w, h); break;
    case APP_FILES:   files_draw(idx, x, y, w, h); break;
    case APP_VIEW:    view_draw(idx, x, y, w, h); break;
    }
}

/* ---------------- курсор: РИСУЕТСЯ НА РЕАЛЬНЫЙ ЭКРАН поверх буфера ---------------- */
static const uint16_t ARROW[17] = {
    0b000000000001, 0b000000000011, 0b000000000101, 0b000000001001,
    0b000000010001, 0b000000100001, 0b000001000001, 0b000010000001,
    0b000100000001, 0b001000000001, 0b011111100001, 0b000100100101,
    0b000010101001, 0b000010100011, 0b000010100001, 0b000010100000,
    0b000011000000,
};
#define CURW 14
#define CURH 19
static uint32_t g_cur_save[CURW * CURH];
static int      g_cur_on;
static int32_t  g_cur_x, g_cur_y;

static void cursor_hide(void) {
    if (!g_cur_on) return;
    for (int32_t r = 0; r < CURH; r++)
        for (int32_t c = 0; c < CURW; c++) {
            int32_t x = g_cur_x + c, y = g_cur_y + r;
            if (x >= 0 && y >= 0 && (uint32_t)x < g_scr_w && (uint32_t)y < g_scr_h)
                gfx_poke_fb((uint32_t)x, (uint32_t)y, g_cur_save[r * CURW + c]);
        }
    g_cur_on = 0;
}
static void cursor_show(int32_t x, int32_t y) {
    if (g_cur_on) cursor_hide();
    for (int32_t r = 0; r < CURH; r++)
        for (int32_t c = 0; c < CURW; c++) {
            int32_t px = x + c, py = y + r;
            g_cur_save[r * CURW + c] =
                (px >= 0 && py >= 0 && (uint32_t)px < g_scr_w && (uint32_t)py < g_scr_h)
                ? gfx_peek_fb((uint32_t)px, (uint32_t)py) : 0;
        }
    g_cur_x = x; g_cur_y = y; g_cur_on = 1;
    for (int32_t r = 0; r < 17; r++)
        for (int32_t c = 0; c < 12; c++)
            if ((ARROW[r] >> c) & 1)
                gfx_poke_fb((uint32_t)(x + c), (uint32_t)(y + r), gfx_pack(GFX_RGB(0x00, 0x00, 0x00)));
    for (int32_t r = 0; r < 16; r++)
        for (int32_t c = 0; c < 11; c++)
            if ((ARROW[r] >> c) & 1)
                gfx_poke_fb((uint32_t)(x + c + 1), (uint32_t)(y + r + 1), gfx_pack(GFX_RGB(0xFF, 0xFF, 0xFF)));
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
    if (k == KEY_ALTF4) { strcpy_small(out, "A+F4"); return; }
    if (k >= 32 && k < 127) { *p++ = '\''; *p++ = (char)k; *p++ = '\''; *p = 0; return; }
    if (k == 27) { strcpy_small(out, "Esc"); return; }
    strcpy_small(out, "(sp)");
}

static void pill_draw(void) {
    char buf[44], num[16];
    char *p = buf;
    *p++ = 'x'; *p++ = '=';
    u32dec((uint32_t)mouse_x(), num); pcat(&p, num);
    *p++ = ' '; *p++ = 'y'; *p++ = '=';
    u32dec((uint32_t)mouse_y(), num); pcat(&p, num);
    *p++ = ' '; *p++ = 'k'; *p++ = '=';
    key_name(num); pcat(&p, num);
    gfx_blend_round_rect(g_scr_w - 240, g_scr_h - 30, 206, 20, 7, C_DOCK, 200);
    gfx_text(g_scr_w - 232, g_scr_h - 24, buf, C_TXT2);
}

/* ---------------- PE-призрак ---------------- */
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

/* ---------------- мир: рисуется в RAM-буфер ---------------- */
static void world_draw(void) {
    gfx_blit_rows(g_bg);
    pe_ghost();
    deskicons_draw();
    panel_draw();
    dock_draw();
    for (int i = 0; i < g_zn; i++)
        draw_window(g_z[i]);
    pill_draw();
}

/* частичный ремонт (область УЖЕ имеет запас на рамку/тень со всех сторон) */
static void world_repair(int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx + rw > (int32_t)g_scr_w) rw = (int32_t)g_scr_w - rx;
    if (ry + rh > (int32_t)g_scr_h) rh = (int32_t)g_scr_h - ry;
    if (rw <= 0 || rh <= 0) return;

    gfx_blit_rows_region((uint32_t)rx, (uint32_t)ry, (uint32_t)rw, (uint32_t)rh, g_bg);

    if (g_pe_ok) {
        int32_t gx = (int32_t)((g_scr_w * 3) / 4) - 32;
        int32_t gy = (int32_t)((g_scr_h * 2) / 3) - 32;
        if (rx < gx + 96 && rx + rw > gx && ry < gy + 96 && ry + rh > gy)
            pe_ghost();
    }
    if (rx < DESK_X - 6 + DESK_COL_W && ry < DESK_Y + LAUNCH_N * DESK_STEP &&
        rx + rw > DESK_X - 6 && ry + rh > DESK_Y - 4)
        deskicons_draw();
    if (ry < PANEL_H) panel_draw();
    if (ry + rh > (int32_t)(g_scr_h - DOCK_H - 16)) dock_draw();
    for (int i = 0; i < g_zn; i++) {
        int a = g_z[i];
        if (g_w[a].x < rx + rw && g_w[a].x + g_w[a].w + 16 > rx &&
            g_w[a].y < ry + rh && g_w[a].y + g_w[a].h + 16 > ry)
            draw_window(a);
    }
    if (ry + rh > (int32_t)(g_scr_h - 34) && rx + rw > (int32_t)(g_scr_w - 244))
        pill_draw();
}

/* ---------------- dirty-регион (что вылить на экран) ---------------- */
static int32_t g_dx0, g_dy0, g_dx1, g_dy1;
static int     g_dirty_on;

static void dirty_reset(void) { g_dirty_on = 0; }
static void dirty_add(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)g_scr_w) w = (int32_t)g_scr_w - x;
    if (y + h > (int32_t)g_scr_h) h = (int32_t)g_scr_h - y;
    if (w <= 0 || h <= 0) return;
    if (!g_dirty_on) { g_dx0 = x; g_dy0 = y; g_dx1 = x + w; g_dy1 = y + h; g_dirty_on = 1; return; }
    if (x < g_dx0) g_dx0 = x;
    if (y < g_dy0) g_dy0 = y;
    if (x + w > g_dx1) g_dx1 = x + w;
    if (y + h > g_dy1) g_dy1 = y + h;
}
static void dirty_flush(void) {
    if (!g_dirty_on) return;
    gfx_flush((uint32_t)g_dx0, (uint32_t)g_dy0,
              (uint32_t)(g_dx1 - g_dx0), (uint32_t)(g_dy1 - g_dy0));
    g_dirty_on = 0;
}

static void dirty_add_win(int idx) {
    win_t *W = &g_w[idx];
    dirty_add(W->x - 12, W->y - 12, W->w + 24, W->h + 24);   /* +тень/рамка */
}

/* ---------------- накапливаемый ущерб от перетаскивания/подъёма ----------------
 * Между кадрами (пэйсинг 50 fps) прямоугольники объединяются - так на экран
 * всегда уходит ОДИН регион и не остаётся "хвостов". */
static int     g_rep_on;
static int32_t g_rep_x, g_rep_y, g_rep_w, g_rep_h;

static void rep_reset(void) { g_rep_on = 0; }
static void rep_add(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)g_scr_w) w = (int32_t)g_scr_w - x;
    if (y + h > (int32_t)g_scr_h) h = (int32_t)g_scr_h - y;
    if (w <= 0 || h <= 0) return;
    if (!g_rep_on) { g_rep_x = x; g_rep_y = y; g_rep_w = w; g_rep_h = h; g_rep_on = 1; return; }
    int32_t x1 = g_rep_x + g_rep_w, y1 = g_rep_y + g_rep_h;
    if (x < g_rep_x) g_rep_x = x;
    if (y < g_rep_y) g_rep_y = y;
    if (x + w > x1) x1 = x + w;
    if (y + h > y1) y1 = y + h;
    g_rep_w = x1 - g_rep_x;
    g_rep_h = y1 - g_rep_y;
}

/* hover-зона -> прямоугольник, который надо починить при входе/уходе курсора */
static void zone_rect_dirty(int zone) {
    if (zone >= 400) { dirty_add_win(zone - 400); return; }        /* крестик окна */
    if (zone >= 300) { dirty_add(0, 0, (int32_t)g_scr_w, PANEL_H); return; }
    if (zone >= 200) {
        dirty_add((int32_t)(dock_x() - 2), (int32_t)(dock_y() - 2),
                  LAUNCH_N * DOCK_STEP + 16, DOCK_H + 4);
        return;
    }
    if (zone >= 100) {
        dirty_add(DESK_X - 6, DESK_Y - 4, DESK_COL_W, LAUNCH_N * DESK_STEP);
        return;
    }
    /* zone <= 0: курсор на пустом столе - подсветок нет */
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

    /* RAM-буфер кадра (двойная буферизация) + кэш обоев */
    g_back = (uint32_t *)kmalloc((uint64_t)g_scr_w * g_scr_h * sizeof(uint32_t));
    if (g_back) {
        gfx_set_target(g_back, g_scr_w, g_scr_h);  /* весь UI рисуется в RAM */
    } else {
        /* v0.6.1: деградация вместо паники - рисуем прямо в видеопамять
         * (gfx_flush станет холостым, курсор работает как раньше) */
        kprintf("[desktop] ВНИМАНИЕ: нет %lu МиБ под бэкбуфер - прямая отрисовка\n",
                ((uint64_t)g_scr_w * g_scr_h * 4) >> 20);
    }
    g_bg = (uint32_t *)kmalloc(g_scr_h * sizeof(uint32_t));
    if (!g_bg) kpanic("desktop: no mem for wallpaper cache");
    bg_cache_build();

    {
        char num[16]; char *p = g_ram_line;
        pcat(&p, "Память: всего ");
        u32dec((uint32_t)total_mib, num); pcat(&p, num);
        pcat(&p, " МиБ, свободно ");
        u32dec((uint32_t)free_mib, num); pcat(&p, num);
        pcat(&p, " МиБ");
    }
    g_pe_line[0] = 0;

    kprintf("[desktop] рисую стол: буфер %lux%lu, стекло-UI...\n", g_scr_w, g_scr_h);
    world_draw();
    dirty_add(0, 0, (int32_t)g_scr_w, (int32_t)g_scr_h);
    dirty_flush();
    kprintf("[desktop] стол нарисован OK (буфер->экран); PE-тест дальше\n");

    /* PE-демо (M3): запускаем ПОСЛЕ первой отрисовки - квадрат рисует сам EXE */
    {
        int ret = pe_demo_run();
        char *p = g_pe_line;
        g_pe_ok = ((uint32_t)ret == ARES_PE_TEST_OK);
        if (g_pe_ok) {
            strcpy_small(g_pe_line, "PE-тест: OK (ret=0xA2E5)");
        } else {
            pcat(&p, "PE-тест: ОШИБКА ret=");
            char num[16]; u32dec((uint32_t)(-(int32_t)ret), num);
            if (ret < 0) *p++ = '-';
            pcat(&p, num);
        }
        world_draw();            /* строка About обновлена + призрак в буфере */
        dirty_add(0, 0, (int32_t)g_scr_w, (int32_t)g_scr_h);
        dirty_flush();
    }

    fb_console_detach();         /* консоль больше НЕ рисуется - экран наш */

    /* стартовое окно */
    launch_app(APP_ABOUT, 0, 0);
    world_draw();
    dirty_add(0, 0, (int32_t)g_scr_w, (int32_t)g_scr_h);
    dirty_flush();

    g_cur_x = mouse_x(); g_cur_y = mouse_y();
    cursor_show(g_cur_x, g_cur_y);

    int btn_old = mouse_left();
    int drag_win = -1;
    int32_t drag_dx = 0, drag_dy = 0;
    int hover_zone_old = -999;
    uint64_t last_frame = 0;

    for (;;) {
        int struct_dirty = 0, tm_dirty = 0, log_dirty = 0, aux_dirty = 0;
        int scroll_win = -1;

        /* ===== клавиатура ===== */
        int k;
        while ((k = keyboard_getch()) >= 0) {
            g_last_key = k;
            aux_dirty = 1;
            switch (k) {
            case KEY_F1: launch_app(APP_ABOUT, 0, 0);   struct_dirty = 1; break;
            case KEY_F2: launch_app(APP_TASKMAN, 0, 0); struct_dirty = 1; break;
            case KEY_F3: launch_app(APP_LOGS, 0, 0);    struct_dirty = 1; break;
            case KEY_F4: launch_app(APP_FILES, 0, 0);   struct_dirty = 1; break;
            case 27:                                    /* Esc = закрыть верхнее */
            case KEY_ALTF4: {                           /* Alt+F4 = то же */
                int t = app_top();
                if (t >= 0) { close_window(t); struct_dirty = 1; }
                break; }
            case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
                int t = app_top();
                if (t < 0) break;
                int app = g_w[t].app;
                int d = (k == KEY_UP) ? 1 : (k == KEY_DOWN) ? -1 : (k == KEY_PGUP) ? 12 : -12;
                if (app == APP_LOGS) { g_w[t].aux += d; scroll_win = t; }
                else if (app == APP_VIEW) { g_w[t].aux -= d; scroll_win = t; }
                break; }
            case KEY_MLEFT:  mouse_nudge(-14, 0); break;
            case KEY_MRIGHT: mouse_nudge(14, 0);  break;
            case KEY_MUP:    mouse_nudge(0, -14); break;
            case KEY_MDOWN:  mouse_nudge(0, 14);  break;
            default: break;
            }
        }

        /* ===== колесо мыши: крутим ВЕРХНЕЕ окно логов/просмотра ===== */
        int wheel = mouse_wheel();
        if (wheel) {
            int t = app_top();
            if (t >= 0 && (g_w[t].app == APP_LOGS || g_w[t].app == APP_VIEW)) {
                int d = wheel * 2;
                if (d > 12) d = 12;
                if (d < -12) d = -12;
                if (g_w[t].app == APP_LOGS) g_w[t].aux += d;
                else g_w[t].aux -= d;
                scroll_win = t;
            }
        }

        /* ===== мышь ===== */
        int moved = mouse_moved();
        int left = mouse_left();
        int pressed = left && !btn_old;

        if (pressed) {
            int32_t mx = mouse_x(), my = mouse_y();
            int hit;
            struct_dirty = 1;   /* открытия/закрытия: мир целиком в RAM (быстро) */
            if ((hit = panel_hit(mx, my)) >= 0) {
                launch_app(hit, 0, 0);
                goto click_done;
            }
            int topmost = -1;
            for (int i = g_zn - 1; i >= 0; i--) {
                win_t *W = &g_w[g_z[i]];
                if (mx >= W->x && mx < W->x + W->w && my >= W->y && my < W->y + W->h) {
                    topmost = g_z[i];
                    break;
                }
            }
            if (topmost >= 0) {
                win_t *W = &g_w[topmost];
                int was_top = (app_top() == topmost);
                win_raise(topmost);
                if (!was_top)                 /* подняли окно над соседями: */
                    rep_add(W->x - 12, W->y - 12, W->w + 24, W->h + 24);
                if (mx >= W->x + W->w - 28 && mx < W->x + W->w - 8 &&
                    my >= W->y + 6 && my < W->y + 26) {
                    close_window(topmost);    /* структура изменилась */
                    rep_reset();
                } else if (my < W->y + WIN_BAR_H) {
                    drag_win = topmost;
                    drag_dx = mx - W->x;
                    drag_dy = my - W->y;
                    struct_dirty = 0;         /* драг - это repair, не мир */
                } else if (W->app == APP_FILES) {
                    if (!files_click(topmost, mx, my)) struct_dirty = 0;
                } else struct_dirty = 0;      /* просто поднять: хватит repair */
            } else if ((hit = dock_hit(mx, my)) >= 0) {
                launch_app(hit, 0, 0);
            } else if ((hit = desk_hit(mx, my)) >= 0) {
                launch_app(DICON[hit].app, 0, 0);
            } else struct_dirty = 0;          /* клик в пустой стол */
        }
    click_done:;

        if (drag_win >= 0) {
            if (!left) drag_win = -1;
            else if (moved) {
                win_t *W = &g_w[drag_win];
                int32_t nx = mouse_x() - drag_dx;
                int32_t ny = mouse_y() - drag_dy;
                if (ny < PANEL_H + 2) ny = PANEL_H + 2;
                if (ny > (int32_t)g_scr_h - 40) ny = (int32_t)g_scr_h - 40;
                if (nx < -W->w + 80) nx = -W->w + 80;
                if (nx > (int32_t)g_scr_w - 80) nx = (int32_t)g_scr_w - 80;
                if (nx != W->x || ny != W->y) {
                    int32_t ox = W->x, oy = W->y;
                    W->x = nx; W->y = ny;
                    /* запас на рамку/тень со ВСЕХ сторон - иначе "хвосты" */
                    int32_t rx = (ox < nx ? ox : nx) - 14;
                    int32_t ry = (oy < ny ? oy : ny) - 14;
                    rep_add(rx, ry,
                            (ox > nx ? ox : nx) + W->w - rx + 24,
                            (oy > ny ? oy : ny) + W->h - ry + 24);
                }
            }
        }

        /* ===== периодика: только помечаем, ЧТО изменилось (без перерисовки мира) ===== */
        {
            uint64_t t = sched_ticks();
            uint64_t sec = t / 100;
            if (sec != g_last_sec) {
                g_last_sec = sec;
                aux_dirty = 1;                                /* часы тикают */
                if (win_open_count(APP_LOGS)) {
                    int t2 = app_top();
                    if (t2 < 0 || g_w[t2].app == APP_LOGS) log_dirty = 1;
                }
            }
            if (win_open_count(APP_TASKMAN) && t - g_tm_stamp >= TM_REFRESH) {
                g_tm_stamp = t;
                tm_dirty = 1;
            }
            if (win_open_count(APP_LOGS) && t - g_log_stamp >= LOGS_REFRESH) {
                g_log_stamp = t;
                int t2 = app_top();
                if (t2 < 0 || g_w[t2].app == APP_LOGS) log_dirty = 1;
            }
        }

        /* ===== hover: подсветки меняются ТОЛЬКО при смене цели ===== */
        int zone;
        {
            int32_t mx = mouse_x(), my = mouse_y();
            int i = panel_hit(mx, my);
            if (i >= 0) zone = 300 + i;
            else if ((i = dock_hit(mx, my)) >= 0) zone = 200 + i;
            else if ((i = desk_hit(mx, my)) >= 0) zone = 100 + i;
            else {
                zone = 0;
                for (int zi = g_zn - 1; zi >= 0; zi--) {
                    win_t *W = &g_w[g_z[zi]];
                    if (mx >= W->x + W->w - 28 && mx < W->x + W->w - 8 &&
                        my >= W->y + 6 && my < W->y + 26) { zone = 400 + g_z[zi]; break; }
                }
            }
        }
        int hover_dirty = (zone != hover_zone_old);
        int zone_from = hover_zone_old;
        hover_zone_old = zone;

        /* ===== пакетный рендер =====
         * Философия v0.6.0: экран НЕ дёргаем по мелочи. Всё рисуется в RAM-буфер,
         * на видеопамять выливается ОДИН объединённый регион и не чаще ~50 fps
         * для "мягких" событий (движение мыши/драг). Курсор живёт отдельно -
         * тычками прямо в видеопамять, поэтому он летает всегда. */
        uint64_t now_t = sched_ticks();
        int pace_ok = (int64_t)(now_t - last_frame) >= FRAME_MIN;
        int render = struct_dirty || hover_dirty || tm_dirty || log_dirty ||
                     aux_dirty || (scroll_win >= 0) ||
                     ((g_rep_on || moved) && pace_ok);

        if (render) {
            cursor_hide();
            dirty_reset();
            if (struct_dirty) {
                /* открытие/закрытие/навигация: мир целиком в RAM - это дёшево,
                 * на экран всё равно уйдёт один регион */
                world_draw();
                dirty_add(0, 0, (int32_t)g_scr_w, (int32_t)g_scr_h);
            } else {
                /* точечные правки: собираем ущерб в один прямоугольник;
                 * repair восстановит подложку и перерисует всё в z-порядке,
                 * а подсветки draw-функции возьмут из текущей позиции мыши */
                if (g_rep_on) dirty_add(g_rep_x, g_rep_y, g_rep_w, g_rep_h);
                if (hover_dirty) {
                    zone_rect_dirty(zone_from);     /* погасить старую цель */
                    zone_rect_dirty(zone);          /* подсветить новую */
                }
                if (tm_dirty)
                    for (int i = 0; i < g_zn; i++)
                        if (g_w[g_z[i]].app == APP_TASKMAN) dirty_add_win(g_z[i]);
                if (log_dirty)
                    for (int i = 0; i < g_zn; i++)
                        if (g_w[g_z[i]].app == APP_LOGS) dirty_add_win(g_z[i]);
                if (scroll_win >= 0) dirty_add_win(scroll_win);
                if (aux_dirty) {
                    dirty_add((int32_t)g_scr_w - 104, 4, 104, 26);            /* часы */
                    dirty_add((int32_t)g_scr_w - 246, (int32_t)g_scr_h - 32,
                              246, 32);                                       /* пилюля */
                } else if (moved) {
                    dirty_add((int32_t)g_scr_w - 246, (int32_t)g_scr_h - 32,
                              246, 32);                                       /* x=..., y=... */
                }
                if (g_dirty_on)
                    world_repair(g_dx0, g_dy0, g_dx1 - g_dx0, g_dy1 - g_dy0);
            }
            dirty_flush();
            cursor_show(mouse_x(), mouse_y());
            last_frame = now_t;
            rep_reset();
        } else if (moved) {
            /* ничего на столе не изменилось - двигаем ТОЛЬКО курсор */
            cursor_hide();
            cursor_show(mouse_x(), mouse_y());
        }

        btn_old = left;
        __asm__ volatile ("hlt");   /* разбудит таймер 100 Гц / IRQ1 / IRQ12 */
    }
}
