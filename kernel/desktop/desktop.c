/* AresOS - рабочий стол v0.7.0.
 *  НОВОЕ в этой версии:
 *  - УСТАНОВЩИК при запуске: создаёт системные файлы в ramfs с прогрессом,
 *    плюс "Центр обновления" (честные подписи: сеть - этап M7)
 *  - НАСТРОЙКИ: разрешение экрана (через UEFI NVRAM + перезагрузка),
 *    масштаб интерфейса 100/150/200%, обои (5 вариантов), скорость мыши
 *  - VFS/ramfs: НАСТОЯЩИЕ папки и файлы в памяти: создать папку/файл,
 *    открыть, ПРОЧИТАТЬ и даже ДОПИСАТЬ (мини-редактор, F2 = сохранить)
 *  - 60 FPS: таймер 1000 Гц (тик 1 мс), пэйсинг кадров 16 мс, курсор
 *    рисуется мгновенно при пробуждении (IRQ12/IRQ1/PIT)
 *  - мышь: скорость 50..250% с накоплением дробной части + ускорение x2
 *  - русская раскладка клавиатуры (Alt+Shift) - можно писать имена файлов
 *  Проверенная механика v0.6.x сохранена: двойная буферизация,
 *  окна-процессы, Alt+F4/Esc, колесо IntelliMouse, стекло-UI. */
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
#include "vfs.h"
#include "efi_rt.h"
#include "fb_console.h"
#include <stdint.h>
#include <string.h>

/* ---------------- масштаб интерфейса (Настройки -> Интерфейс) ---------------- */
static int g_ui_scale = 100;              /* 100 / 150 / 200 % */
static int g_mag10    = 10;               /* шрифт в десятых долях (= scale/10) */
#define SC(v)  ((v) * g_ui_scale / 100)   /* любой размер интерфейса */
#define ADV    SC(8)                      /* ширина знакоместа шрифта, пикс */
#define FNT    SC(8)                      /* высота шрифта, пикс */

#define PANEL_H     SC(34)
#define DOCK_H      SC(58)
#define DOCK_ICON   SC(44)
#define DOCK_STEP   SC(56)
#define WIN_BAR_H   SC(30)
#define TERM_LINE_H SC(10)
#define FILE_ROW_H  SC(15)
#define TM_REFRESH  333            /* ~3 раза/сек (тик = 1 мс) */
#define FRAME_MIN   16             /* 60 fps: мин. мс между "мягкими" кадрами */
#define LOGS_REFRESH 500

static void T(int32_t x, int32_t y, const char *s, gfx_color_t c) {
    gfx_text_mag((uint32_t)x, (uint32_t)y, s, c, g_mag10);
}
static void TB(int32_t x, int32_t y, const char *s, gfx_color_t c) {
    gfx_text_bold_mag((uint32_t)x, (uint32_t)y, s, c, g_mag10);
}
/* вертикальное центрирование текста в строке высотой h */
static int32_t TY(int32_t y, int32_t h) { return y + (h - FNT) / 2; }

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
static const gfx_color_t C_BTN_H   = GFX_RGB(0x2C, 0x34, 0x4C);

/* обои: 5 вариантов трёхстопного градиента (Настройки -> Обои) */
typedef struct { gfx_color_t a, b, c; const char *name; } wall_t;
static const wall_t WALLS[] = {
    { GFX_RGB(0x0B, 0x0F, 0x26), GFX_RGB(0x14, 0x1B, 0x42), GFX_RGB(0x0C, 0x2C, 0x3C), "Аврора" },
    { GFX_RGB(0x14, 0x0A, 0x24), GFX_RGB(0x3A, 0x12, 0x3C), GFX_RGB(0x5E, 0x24, 0x28), "Закат" },
    { GFX_RGB(0x04, 0x0C, 0x1C), GFX_RGB(0x08, 0x22, 0x3A), GFX_RGB(0x06, 0x30, 0x44), "Океан" },
    { GFX_RGB(0x08, 0x16, 0x0E), GFX_RGB(0x0E, 0x2A, 0x18), GFX_RGB(0x14, 0x30, 0x22), "Лес" },
    { GFX_RGB(0x16, 0x0C, 0x20), GFX_RGB(0x3A, 0x14, 0x38), GFX_RGB(0x58, 0x20, 0x40), "Рассвет" },
};
#define WALL_N 5
static int g_wall = 0;

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
/* стереть ПОСЛЕДНИЙ символ (с хвостом UTF-8 continuation-байтов) */
static void edit_backspace(char *buf, int *len) {
    int n = *len;
    if (n <= 0) return;
    n--;
    while (n > 0 && ((uint8_t)buf[n] & 0xC0) == 0x80) n--;
    buf[n] = 0;
    *len = n;
}

/* ---------------- окна: экземпляры = процессы ---------------- */
enum { APP_ABOUT = 0, APP_TASKMAN, APP_LOGS, APP_FILES, APP_VIEW,
       APP_SETTINGS, APP_SETUP, APP_TN };
#define LAUNCH_N 6                    /* пускатели в доке/панели (без просмотрщика) */
#define MAX_WIN 10

typedef struct {
    int  open;
    int  app;                         /* тип приложения */
    int  pid;                         /* слот процесса-владельца в планировщике */
    int32_t x, y, w, h;
    int  aux;                         /* logs/view: scroll; files: dir; set: секция; setup: страница */
    int  aux2;                        /* view: edit-scroll; settings: выбор */
    int  vnode;                       /* view: узел vfs файла (VFS_NONE = статика) */
    char name[24];                    /* view: имя открытого файла */
    const char *text;                 /* view: статический текст (если vnode < 0) */
    /* --- текстовый ввод --- */
    int      edit_mode;               /* 0 = нет; files: 1/2 имя папки/файла; view: 1 = редактор */
    int      edit_len;
    char     edit_name[32];           /* files: имя создаваемого объекта */
    char    *edit_buf;                /* view: буфер редактора (kmalloc) */
} win_t;

static win_t g_w[MAX_WIN];
static int   g_z[MAX_WIN];            /* z-порядок: g_z[0] - нижнее, [zn-1] - верх */
static int   g_zn;
static int   g_pending_view_node = VFS_NONE;   /* файлы -> просмотр (узел vfs) */

static const struct { const char *t; const char *pname; gfx_color_t c; } APP_META[APP_TN] = {
    { "О системе AresOS",        "w:Инфо",   GFX_RGB(0x4A, 0x9D, 0xFF) },
    { "Диспетчер задач",         "w:Задачи", GFX_RGB(0xFF, 0x9E, 0x49) },
    { "Логи системы - терминал", "w:Логи",   GFX_RGB(0x4F, 0xC3, 0x7B) },
    { "Проводник - Мои файлы",   "w:Файлы",  GFX_RGB(0xE8, 0xC1, 0x5A) },
    { "Просмотр файла",          "w:Текст",  GFX_RGB(0xB0, 0x8A, 0xE0) },
    { "Настройки",               "w:Опции",  GFX_RGB(0x56, 0xC2, 0xE8) },
    { "Установка AresOS",        "w:Устан",  GFX_RGB(0x28, 0xC8, 0x40) },
};

/* процесс-оболочка окна: живёт, пока окно открыто; тики идут ему в диспетчер */
static void app_win_proc(void *arg) { (void)arg; for (;;) proc_sleep(3600000); }

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
    W->vnode = VFS_NONE;
    static const int32_t GEO[APP_TN][4] = {
        { 120, 84, 448, 240 },   /* О системе */
        { 262, 64, 560, 372 },   /* Диспетчер */
        { 190, 78, 640, 392 },   /* Логи */
        { 150, 96, 470, 330 },   /* Файлы */
        { 230, 130, 460, 300 },  /* Просмотр */
        { 300, 100, 570, 400 },  /* Настройки */
        { 330, 150, 520, 380 },  /* Установка */
    };
    int off = (g_zn % 5) * 26;
    W->x = SC(GEO[app][0]) + off; W->y = SC(GEO[app][1]) + off;
    W->w = SC(GEO[app][2]);       W->h = SC(GEO[app][3]);
    if (W->x + W->w > (int32_t)g_scr_w - 8) W->x = (int32_t)g_scr_w - W->w - 8;
    if (W->y + W->h > (int32_t)g_scr_h - 70) W->y = PANEL_H + 4;
    if (app == APP_VIEW) {
        if (vfile_name) {
            int i = 0;
            for (const char *s = vfile_name; *s && i < 23; s++) W->name[i++] = *s;
            W->name[i] = 0;
        }
        W->text  = vfile_text;
        W->vnode = g_pending_view_node;
        g_pending_view_node = VFS_NONE;
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
    if (W->edit_buf) { kfree(W->edit_buf); W->edit_buf = 0; }
    if (W->pid >= 4) proc_kill_slot(W->pid);   /* процесс окна умирает */
    W->open = 0;
    z_remove(idx);
    kprintf("[desktop] окно '%s' закрыто (осталось %d)\n",
            APP_META[W->app].pname, g_zn);
}

/* ---------------- обои ---------------- */
static void bg_cache_build(void) {
    const wall_t *W = &WALLS[g_wall];
    for (uint32_t y = 0; y < g_scr_h; y++) {
        uint32_t half = g_scr_h / 2;
        gfx_color_t c;
        if (y < half) {
            int t = half ? (int)(y * 255 / half) : 0;
            c.r = (uint8_t)(W->a.r + ((int)W->b.r - W->a.r) * t / 255);
            c.g = (uint8_t)(W->a.g + ((int)W->b.g - W->a.g) * t / 255);
            c.b = (uint8_t)(W->a.b + ((int)W->b.b - W->a.b) * t / 255);
        } else {
            int t = half ? (int)((y - half) * 255 / (g_scr_h - half)) : 0;
            c.r = (uint8_t)(W->b.r + ((int)W->c.r - W->b.r) * t / 255);
            c.g = (uint8_t)(W->b.g + ((int)W->c.g - W->b.g) * t / 255);
            c.b = (uint8_t)(W->b.b + ((int)W->c.b - W->b.b) * t / 255);
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
    int32_t cx = (int32_t)g_scr_w - SC(100);
    gfx_blend_round_rect(cx, SC(7), SC(88), SC(19), SC(8), C_GLASS, 200);
    T(cx + SC(12), TY(SC(7), SC(19)), buf, C_TXT);
}

static int panel_btns(void) { return g_scr_w >= (uint32_t)SC(1050) ? 6 : 4; }

static void panel_draw(void) {
    gfx_blend_rect(0, 0, g_scr_w, PANEL_H, C_PANEL, 225);       /* стекло */
    gfx_blend_rect(0, 0, g_scr_w, 1, GFX_RGB(0xFF, 0xFF, 0xFF), 18);
    gfx_fill_rect(0, PANEL_H - 1, g_scr_w, 1, C_PLINE);
    TB(SC(14), TY(0, PANEL_H), "AresOS", C_ACCENT);
    gfx_blend_round_rect(SC(86), SC(8), SC(66), SC(18), SC(6), C_GLASS, 170);
    T(SC(94), TY(SC(8), SC(18)), "v0.7.0", C_TXT2);
    static const char *BTN[LAUNCH_N] = { "Инфо", "Задачи", "Логи", "Файлы", "Опции", "Устан." };
    static const gfx_color_t BC[LAUNCH_N] = { C_BLUE, C_ACCENT, C_GREEN, C_YELLOW, C_CYAN, C_LIME };
    static const int A2L[LAUNCH_N] = { APP_ABOUT, APP_TASKMAN, APP_LOGS, APP_FILES, APP_SETTINGS, APP_SETUP };
    int32_t mx = mouse_x(), my = mouse_y();
    int nb = panel_btns();
    for (int i = 0; i < nb; i++) {
        int32_t bx = SC(160) + i * SC(96);
        int hov = (mx >= bx && mx < bx + SC(90) && my < PANEL_H);
        if (hov) gfx_blend_round_rect(bx, SC(5), SC(90), SC(24), SC(7), C_GLASS, 220);
        gfx_fill_round_rect(bx + SC(9), TY(0, PANEL_H) + 1, SC(8), SC(8), SC(3), BC[i]);
        T(bx + SC(24), TY(0, PANEL_H), BTN[i], hov ? C_TXT : C_TXT2);
        if (win_open_count(A2L[i]))
            gfx_fill_round_rect(bx + SC(10), SC(26), SC(70), SC(2), 1, BC[i]);
    }
    /* индикатор раскладки RU/EN слева от часов */
    int32_t lx = (int32_t)g_scr_w - SC(100) - SC(42);
    gfx_blend_round_rect(lx, SC(7), SC(34), SC(19), SC(8), C_GLASS, 200);
    T(lx + SC(9), TY(SC(7), SC(19)), keyboard_ru() ? "RU" : "EN", keyboard_ru() ? C_ACCENT : C_TXT2);
    clock_draw();
}

static int panel_hit(int32_t mx, int32_t my) {
    if (my >= PANEL_H) return -1;
    int nb = panel_btns();
    for (int i = 0; i < nb; i++) {
        int32_t bx = SC(160) + i * SC(96);
        if (mx >= bx && mx < bx + SC(90)) return i;
    }
    return -1;
}

/* ---------------- док (стекло, отскок при hover) ---------------- */
static uint32_t dock_x(void) { return (g_scr_w - (uint32_t)(LAUNCH_N * DOCK_STEP + 12)) / 2; }
static uint32_t dock_y(void) { return g_scr_h - (uint32_t)DOCK_H - 12; }

typedef struct { const char *glyph; gfx_color_t col; } icon_t;
static const icon_t ICONS[LAUNCH_N] = {
    { "i",  GFX_RGB(0x4A, 0x9D, 0xFF) },
    { "TM", GFX_RGB(0xFF, 0x9E, 0x49) },
    { ">_", GFX_RGB(0x4F, 0xC3, 0x7B) },
    { "FM", GFX_RGB(0xE8, 0xC1, 0x5A) },
    { "*",  GFX_RGB(0x56, 0xC2, 0xE8) },   /* Настройки ("шестерёнка") */
    { "U",  GFX_RGB(0x28, 0xC8, 0x40) },   /* Установка */
};
static const int LAUNCH_APP[LAUNCH_N] = { APP_ABOUT, APP_TASKMAN, APP_LOGS, APP_FILES,
                                          APP_SETTINGS, APP_SETUP };

static int dock_hit(int32_t mx, int32_t my) {
    uint32_t dx = dock_x(), dy = dock_y();
    if (my < (int32_t)dy || my >= (int32_t)(dy + (uint32_t)DOCK_H)) return -1;
    for (int i = 0; i < LAUNCH_N; i++) {
        int32_t ix = (int32_t)(dx + 12 + (uint32_t)i * (uint32_t)DOCK_STEP);
        if (mx >= ix && mx < ix + DOCK_ICON) return i;
    }
    return -1;
}

static void dock_draw(void) {
    uint32_t dx = dock_x(), dy = dock_y();
    int32_t dw = LAUNCH_N * DOCK_STEP + 12;
    gfx_blend_round_rect((int32_t)dx - 1, (int32_t)dy - 1, dw + 2, DOCK_H + 2, SC(14), C_DOCK_BR, 140);
    gfx_blend_round_rect((int32_t)dx, (int32_t)dy, dw, DOCK_H, SC(13), C_DOCK, 215);
    gfx_blend_round_rect((int32_t)dx + 4, (int32_t)dy + 3, dw - 8, SC(12), SC(9),
                         GFX_RGB(0xFF, 0xFF, 0xFF), 14);             /* блик */
    int32_t mx = mouse_x(), my = mouse_y();
    int hov = dock_hit(mx, my);
    for (int i = 0; i < LAUNCH_N; i++) {
        int32_t ix = (int32_t)dx + 12 + i * DOCK_STEP;
        int32_t iy = (int32_t)dy + SC(7);
        gfx_color_t c = ICONS[i].col;
        if (i == hov) {
            iy -= SC(3);                                              /* подскок */
            c.r = (uint8_t)(c.r < 220 ? c.r + 35 : 255);
            c.g = (uint8_t)(c.g < 220 ? c.g + 35 : 255);
            c.b = (uint8_t)(c.b < 220 ? c.b + 35 : 255);
        }
        gfx_blend_round_rect(ix + 2, iy + 4, DOCK_ICON, DOCK_ICON, SC(10), C_SHADOW, 90);
        gfx_fill_round_rect(ix, iy, DOCK_ICON, DOCK_ICON, SC(10), c);
        gfx_fill_round_rect(ix + SC(5), iy + SC(4), SC(34), SC(9), SC(5), GFX_RGB(0xFF, 0xFF, 0xFF));
        gfx_fill_round_rect(ix + SC(5), iy + SC(4), SC(34), SC(7), SC(4), c);
        int gw = utf_glyphs(ICONS[i].glyph) * ADV;
        TB(ix + (DOCK_ICON - gw) / 2, iy + (DOCK_ICON - FNT) / 2,
           ICONS[i].glyph, GFX_RGB(0xFF, 0xFF, 0xFF));
        if (win_open_count(LAUNCH_APP[i]))
            gfx_fill_round_rect(ix + DOCK_ICON / 2 - SC(3), (int32_t)dy + DOCK_H - SC(6),
                                SC(6), SC(3), 1, GFX_RGB(0xCF, 0xD6, 0xEA));
    }
}

/* ---------------- иконки на рабочем столе ---------------- */
#define DESK_X   SC(24)
#define DESK_Y   SC(64)
#define DESK_STEP SC(86)
#define DESK_COL_W SC(80)

static const struct { const char *glyph; const char *label; int app; gfx_color_t col; }
DICON[LAUNCH_N] = {
    { "i",  "Компьютер",  APP_ABOUT,    GFX_RGB(0x4A, 0x9D, 0xFF) },
    { "FM", "Файлы",      APP_FILES,    GFX_RGB(0xE8, 0xC1, 0x5A) },
    { "TM", "Задачи",     APP_TASKMAN,  GFX_RGB(0xFF, 0x9E, 0x49) },
    { ">_", "Логи",       APP_LOGS,     GFX_RGB(0x4F, 0xC3, 0x7B) },
    { "*",  "Настройки",  APP_SETTINGS, GFX_RGB(0x56, 0xC2, 0xE8) },
    { "U",  "Установка",  APP_SETUP,    GFX_RGB(0x28, 0xC8, 0x40) },
};

static int desk_hit(int32_t mx, int32_t my) {
    for (int i = 0; i < LAUNCH_N; i++) {
        int32_t iy = DESK_Y + i * DESK_STEP;
        if (mx >= DESK_X - 6 && mx < DESK_X - 6 + DESK_COL_W &&
            my >= iy - 4 && my < iy + SC(66)) return i;
    }
    return -1;
}

static void deskicons_draw(void) {
    int32_t mx = mouse_x(), my = mouse_y();
    int hov = desk_hit(mx, my);
    for (int i = 0; i < LAUNCH_N; i++) {
        int32_t ix = DESK_X + (DESK_COL_W - SC(46)) / 2 - 6;
        int32_t iy = DESK_Y + i * DESK_STEP;
        gfx_color_t c = DICON[i].col;
        if (i == hov) {
            gfx_blend_round_rect(DESK_X - 6, iy - 4, DESK_COL_W, SC(68), SC(10), C_GLASS, 170);
            c.r = (uint8_t)(c.r < 220 ? c.r + 35 : 255);
            c.g = (uint8_t)(c.g < 220 ? c.g + 35 : 255);
            c.b = (uint8_t)(c.b < 220 ? c.b + 35 : 255);
        }
        gfx_blend_round_rect(ix + 2, iy + 3, SC(46), SC(46), SC(11), C_SHADOW, 80);
        gfx_fill_round_rect(ix, iy, SC(46), SC(46), SC(11), c);
        gfx_fill_round_rect(ix + SC(5), iy + SC(4), SC(36), SC(9), SC(5), GFX_RGB(0xFF, 0xFF, 0xFF));
        gfx_fill_round_rect(ix + SC(5), iy + SC(4), SC(36), SC(7), SC(4), c);
        int gw = utf_glyphs(DICON[i].glyph) * ADV;
        TB(ix + (SC(46) - gw) / 2, iy + (SC(46) - FNT) / 2,
           DICON[i].glyph, GFX_RGB(0xFF, 0xFF, 0xFF));
        int lpx = utf_glyphs(DICON[i].label) * ADV;
        int lx = DESK_X - 6 + (DESK_COL_W - lpx) / 2;
        if (lx < DESK_X - 6) lx = DESK_X - 6;
        T(lx + 1, iy + SC(51), DICON[i].label, GFX_RGB(0x04, 0x05, 0x0A));
        T(lx, iy + SC(50), DICON[i].label, C_TXT);
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
    gfx_blend_rect(x + 4, y + 2, w - 8, SC(10), GFX_RGB(0xFF, 0xFF, 0xFF), 10);
    gfx_fill_rect(x, y + WIN_BAR_H, w, 1, C_WIN_BR);
    /* цветная точка приложения + заголовок */
    gfx_fill_round_rect(x + SC(12), TY(y, WIN_BAR_H) - 1, SC(12), SC(12), SC(4), APP_META[W->app].c);
    TB(x + SC(32), TY(y, WIN_BAR_H), APP_META[W->app].t, top ? C_TXT : C_TXT2);
    /* traffic lights: жёлтая/зелёная - декор, красная со x = ЗАКРЫТЬ */
    gfx_fill_round_rect(x + w - SC(59), y + SC(10), SC(10), SC(10), SC(5), C_LIME);
    gfx_fill_round_rect(x + w - SC(43), y + SC(10), SC(10), SC(10), SC(5), C_AMBER);
    int hovx = (mouse_x() >= x + w - SC(28) && mouse_x() < x + w - SC(8) &&
                mouse_y() >= y + SC(6) && mouse_y() < y + SC(26));
    gfx_fill_round_rect(x + w - SC(26), y + SC(9), SC(12), SC(12), SC(5),
                        hovx ? GFX_RGB(0xFF, 0x77, 0x70) : C_RED);
    for (int i = 0; i < SC(6); i++) {
        gfx_pixel((uint32_t)(x + w - SC(23) + i),          (uint32_t)(y + SC(12) + i), GFX_RGB(0xFF, 0xEE, 0xEE));
        gfx_pixel((uint32_t)(x + w - SC(23) + SC(5) - i),  (uint32_t)(y + SC(12) + i), GFX_RGB(0xFF, 0xEE, 0xEE));
    }
    draw_content(idx, x, y + WIN_BAR_H, w, h - WIN_BAR_H);
}

/* кнопка: вернуть 1 если мышь над ней; нарисовать её */
static int btn_draw(int32_t x, int32_t y, int32_t w, int32_t h,
                    const char *label, gfx_color_t acc, int active) {
    int32_t mx = mouse_x(), my = mouse_y();
    int hov = (mx >= x && mx < x + w && my >= y && my < y + h);
    if (active)      gfx_fill_round_rect(x, y, w, h, SC(6), acc);
    else if (hov)    gfx_fill_round_rect(x, y, w, h, SC(6), C_BTN_H);
    else             gfx_fill_round_rect(x, y, w, h, SC(6), C_ROW_ALT);
    if (!active) gfx_frame_rect(x, y, w, h, hov ? acc : C_PLINE);
    int lw = utf_glyphs(label) * ADV;
    T(x + (w - lw) / 2, TY(y, h), label,
      active ? GFX_RGB(0x10, 0x12, 0x18) : (hov ? C_TXT : C_TXT2));
    return hov;
}

/* ---- приложение О системе ---- */
static char g_ram_line[96];   /* v0.6.2: было 48 - УЕЗЖАЛО за границу */
static char g_pe_line[40];
static void about_draw(int32_t x, int32_t y, int32_t w) {
    (void)w;
    int32_t cx = x + SC(16);
    int32_t yy = y + SC(12);
    int LH = FNT + SC(6);
    TB(cx, yy, "Ядро AresOS 0.7.0 (x86-64)", C_TXT); yy += LH + SC(4);
    T(cx, yy, g_ram_line, C_TXT2); yy += LH;
    T(cx, yy, "Окна = процессы. Куча/VMM живы, PE32+ в ядре", C_TXT2); yy += LH;
    if (g_pe_line[0]) { T(cx, yy, g_pe_line, C_GREEN); yy += LH; }
    yy += SC(4);
    gfx_fill_rect(cx, yy, SC(300), 1, C_PLINE); yy += SC(8);
    TB(cx, yy, "Новое в v0.7.0:", C_ACCENT); yy += LH;
    T(cx, yy, "Установщик при запуске + Центр обновления.", C_TXT2); yy += LH;
    T(cx, yy, "Настройки: экран, масштаб, обои, мышь (F5).", C_TXT2); yy += LH;
    T(cx, yy, "Проводник: свои папки и файлы, чтение/запись.", C_TXT2); yy += LH;
    T(cx, yy, "60 FPS и плавная мышь. Раскладка RU - Alt+Shift.", C_TXT2); yy += LH;
    yy += SC(2);
    T(cx, yy, "Закрыть окно: красная x, Esc или Alt+F4.", C_TXT2);
}

/* ---- приложение Диспетчер задач ---- */
static uint64_t g_tm_prev_total;
static uint64_t g_tm_prev[32];
static uint64_t g_tm_stamp;

static void tm_draw(int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t cx = x + SC(12);
    int32_t hy = y + SC(8);
#define COC(c) ((c) * ADV)
    gfx_fill_round_rect(cx, hy, w - SC(24), SC(16), SC(5), C_ROW_ALT);
    const gfx_color_t HC = C_CYAN;
    T(cx + SC(10),          TY(hy, SC(16)), "id",     HC);
    T(cx + SC(10) + COC(4), TY(hy, SC(16)), "имя",    HC);
    T(cx + SC(10) + COC(18),TY(hy, SC(16)), "статус", HC);
    T(cx + SC(10) + COC(25),TY(hy, SC(16)), "тики",   HC);
    T(cx + SC(10) + COC(34),TY(hy, SC(16)), "cpu",    HC);
    T(cx + SC(10) + COC(50),TY(hy, SC(16)), "тип",    HC);

    uint64_t now = sched_ticks();
    uint64_t span = now - g_tm_prev_total;
    g_tm_prev_total = now;

    proc_info_t pi[18];
    int n = proc_list(pi, 18);
    int rowh = FNT + SC(5);
    int maxrows = (int)((h - SC(30) - SC(34)) / rowh);
    if (n > maxrows) n = maxrows;

    uint64_t busy = 0;
    for (int i = 0; i < n; i++) {
        int32_t ry = hy + SC(20) + i * rowh;
        if (pi[i].state == PROC_RUNNING)
            gfx_fill_round_rect(cx, ry - 2, w - SC(24), rowh, 4, GFX_RGB(0x2A, 0x31, 0x20));
        else if (i & 1)
            gfx_fill_round_rect(cx, ry - 2, w - SC(24), rowh, 4, C_ROW_ALT);
        if (pi[i].state == PROC_DEAD)
            gfx_fill_round_rect(cx, ry - 2, w - SC(24), rowh, 4, GFX_RGB(0x23, 0x1E, 0x22));

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
        T(cx + SC(10), TY(ry - 2, rowh), row, pi[i].state == PROC_DEAD ? C_TXT2 : C_TXT);

        gfx_color_t sc = C_TXT2;
        if (pi[i].state == PROC_RUNNING) sc = C_GREEN;
        else if (pi[i].state == PROC_READY) sc = C_YELLOW;
        else if (pi[i].state == PROC_DEAD) sc = C_RED;
        T(cx + SC(10) + COC(18), TY(ry - 2, rowh), proc_state_name(pi[i].state), sc);

        char tk[16]; u32dec((uint32_t)pi[i].ticks, tk);
        T(cx + SC(10) + COC(25), TY(ry - 2, rowh), tk, C_TERM_LN);

        uint64_t dt = pi[i].ticks - g_tm_prev[pi[i].id & 31];
        g_tm_prev[pi[i].id & 31] = pi[i].ticks;
        uint32_t pct = span ? (uint32_t)(dt * 100 / span) : 0;
        if (pct > 100) pct = 100;
        if (pi[i].id != 0) busy += dt;
        int32_t bw = SC(72);
        gfx_fill_round_rect(cx + SC(10) + COC(34), ry + 1, bw, SC(7), SC(3), C_BAR_BG);
        int32_t fw = bw * (int32_t)pct / 100;
        if (fw) gfx_fill_round_rect(cx + SC(10) + COC(34), ry + 1, fw, SC(7), SC(3), C_ACCENT);
        char pc[8]; char *q = pc;
        u32dec(pct, num); pcat(&q, num); *q++ = '%'; *q = 0;
        T(cx + SC(10) + COC(34) + bw + SC(8), TY(ry - 2, rowh), pc, C_TXT2);

        int bg = pi[i].flags & PROC_F_BACKGROUND;
        int isw = pi[i].name[0] == 'w' && pi[i].name[1] == ':';
        gfx_fill_round_rect(cx + SC(10) + COC(50) - 1, ry - 2, SC(42), rowh, 4,
                            bg ? GFX_RGB(0x1E, 0x33, 0x28)
                               : (isw ? GFX_RGB(0x2A, 0x21, 0x38) : GFX_RGB(0x1E, 0x29, 0x3D)));
        T(cx + SC(10) + COC(50) + SC(5), TY(ry - 2, rowh),
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
    T(cx, TY(y + h - SC(20), SC(16)), line, C_TXT2);
#undef COC
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
        has_prefix(s, "[mem]") || has_prefix(s, "[vfs]"))  return C_PURPLE;
    if (has_prefix(s, "[efi]") || has_prefix(s, "[boot]")) return C_BLUE;
    if (has_prefix(s, "[proc]") || has_prefix(s, "[m5]")   || has_prefix(s, "[sched]")) return C_ACCENT;
    if (has_prefix(s, "[taskman]") || has_prefix(s, "[desktop]") || has_prefix(s, "[sysmon]") ||
        has_prefix(s, "[setup]"))
        return GFX_RGB(0x8F, 0xA3, 0xC8);
    return C_TERM_LN;
}

static void logs_draw(int idx, int32_t x, int32_t y, int32_t w, int32_t h) {
    win_t *W = &g_w[idx];
    int32_t cx = x + SC(10);
    int32_t cy = y + SC(6);
    int32_t th = h - SC(12);
    gfx_fill_round_rect(cx, cy, w - SC(20), th, 6, C_TERM_BG);
    int vis = (th - SC(8)) / TERM_LINE_H;
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
        if (i & 1) gfx_fill_round_rect(cx + SC(4), cy + SC(4) + i * TERM_LINE_H,
                                       w - SC(28), TERM_LINE_H, 3, C_TERM_AL);
        int maxc = (w - SC(40)) / ADV;
        char clip[LOG_COLS > 128 ? 128 : LOG_COLS + 1];
        int j = 0;
        while (s[j] && j < maxc && j < (int)sizeof(clip) - 1) { clip[j] = s[j]; j++; }
        clip[j] = 0;
        T(cx + SC(8), cy + SC(5) + i * TERM_LINE_H, clip, log_color(s));
    }

    if (total > vis) {
        int32_t sbx = cx + w - SC(26);
        int32_t sbh = th - SC(8);
        gfx_fill_round_rect(sbx, cy + SC(4), SC(4), sbh, 2, GFX_RGB(0x1C, 0x22, 0x32));
        int32_t thumb_h = sbh * vis / total;
        if (thumb_h < SC(8)) thumb_h = SC(8);
        int32_t thumb_y = cy + SC(4) + (sbh - thumb_h) * (total - vis - W->aux) / (total - vis);
        gfx_fill_round_rect(sbx, thumb_y, SC(4), thumb_h, 2, C_CYAN);
    }
    char info[80], num[16];
    char *p = info;
    pcat(&p, "строк: ");
    u32dec((uint32_t)total, num); pcat(&p, num);
    pcat(&p, W->aux ? "  (стрелки/колесо - листать)" : "  (прямой хвост)");
    T(cx + SC(4), TY(y, SC(6) + 4), info, C_TXT2);
}

/* ---- Проводник: НАСТОЯЩАЯ ФС (ramfs); создавай папки/файлы мышкой ---- */
static void files_path(char *out, int dir) {
    /* пусть простой: "Мой компьютер" + имена родителей (в обратном порядке) */
    char tmp[224]; tmp[0] = 0;
    char *tp = tmp;
    int chain[8], cn = 0;
    for (int d = dir; d > 0 && cn < 8; d = vfs_parent(d)) chain[cn++] = d;
    for (int i = cn - 1; i >= 0; i--) {
        pcat(&tp, " / ");
        pcat(&tp, vfs_name(chain[i]));
    }
    char *p = out;
    pcat(&p, "Мой компьютер");
    pcat(&p, tmp);
}

/* кнопки тулбара Проводника */
#define FBW SC(74)
#define FBH SC(18)
static int files_btn_hit(win_t *W, int which, int32_t mx, int32_t my) {
    int32_t bx = W->x + W->w - SC(12) - (2 - which) * (FBW + SC(6));
    int32_t by = W->y + WIN_BAR_H + SC(2);
    return mx >= bx && mx < bx + FBW && my >= by && my < by + FBH;
}

static void files_draw(int idx, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)h;
    win_t *W = &g_w[idx];
    int dir = W->aux;
    if (dir < 0 || dir >= VFS_MAX_NODES) dir = W->aux = VFS_ROOT;
    int32_t cx = x + SC(12);
    /* путь + пилюля свободной памяти */
    char path[280];
    files_path(path, dir);
    TB(cx, TY(y + SC(4), SC(18)), path, C_YELLOW);
    gfx_blend_round_rect(x + w - SC(150), y + SC(24), SC(138), SC(16), SC(6), C_ROW_ALT, 220);
    char num[16], sz[56]; char *p = sz;
    pcat(&p, "диск RAM, занято ");
    u32dec(vfs_used_bytes() / 1024, num); pcat(&p, num);
    pcat(&p, " КиБ");
    T(x + w - SC(146), TY(y + SC(24), SC(16)), sz, C_TXT2);
    gfx_fill_rect(cx, y + SC(42), w - SC(24), 1, C_PLINE);

    /* тулбар: две кнопки создания */
    btn_draw(x + w - SC(12) - 2 * (FBW + SC(6)), y + SC(2), FBW, FBH,
             "+ Папка", C_YELLOW, W->edit_mode == 1);
    btn_draw(x + w - SC(12) - (FBW + SC(6)), y + SC(2), FBW, FBH,
             "+ Файл", C_BLUE, W->edit_mode == 2);

    int32_t ry0 = y + SC(48);
    int rows_cap = (h - SC(48) - SC(26)) / FILE_ROW_H;
    int32_t mx = mouse_x(), my = mouse_y();
    int r = 0;
    if (dir != VFS_ROOT) {          /* ".." - наверх */
        int hov = (mx >= x + 4 && mx < x + w - 4 && my >= ry0 - 1 && my < ry0 + FILE_ROW_H - 1);
        if (hov) gfx_blend_round_rect(x + 6, ry0 - 1, w - 12, FILE_ROW_H, 5, C_GLASS, 200);
        else gfx_fill_round_rect(x + 6, ry0 - 1, w - 12, FILE_ROW_H, 5, C_ROW_ALT);
        gfx_fill_round_rect(cx, TY(ry0, FILE_ROW_H) - 1, SC(11), SC(11), SC(4), C_YELLOW);
        T(cx + SC(18), TY(ry0, FILE_ROW_H), ".. (наверх)", hov ? C_TXT : C_YELLOW);
        r = 1;
    }
    for (int c = vfs_first_child(dir); c != VFS_NONE && r < rows_cap;
         c = vfs_next_sibling(c)) {
        int32_t ry = ry0 + r * FILE_ROW_H;
        int hov = (mx >= x + 4 && mx < x + w - 4 && my >= ry - 1 && my < ry + FILE_ROW_H - 1);
        if (hov) gfx_blend_round_rect(x + 6, ry - 1, w - 12, FILE_ROW_H, 5, C_GLASS, 200);
        else if (r & 1) gfx_fill_round_rect(x + 6, ry - 1, w - 12, FILE_ROW_H, 5, C_ROW_ALT);
        gfx_fill_round_rect(cx, TY(ry, FILE_ROW_H) - 1, SC(11), SC(11), SC(4),
                            vfs_is_dir(c) ? C_YELLOW : C_BLUE);
        T(cx + SC(18), TY(ry, FILE_ROW_H), vfs_name(c), hov ? C_TXT : C_TXT);
        if (vfs_is_dir(c)) {
            T(x + w - SC(64), TY(ry, FILE_ROW_H), "папка", C_TXT2);
        } else {
            char sb[24], nb[12]; char *q = sb;
            u32dec(vfs_size(c), nb); pcat(&q, nb); pcat(&q, " б");
            T(x + w - SC(64), TY(ry, FILE_ROW_H), sb, C_TXT2);
        }
        r++;
    }
    /* строка ввода имени (при создании) */
    int32_t iy = y + h - SC(22);
    if (W->edit_mode) {
        gfx_fill_round_rect(cx, iy - 2, w - SC(24), SC(20), SC(6), C_TERM_BG);
        gfx_frame_rect(cx, iy - 2, w - SC(24), SC(20), C_ACCENT);
        char *p = path;                    /* переиспользуем буфер */
        pcat(&p, W->edit_mode == 1 ? "Имя папки: " : "Имя файла: ");
        pcat(&p, W->edit_name);
        pcat(&p, (sched_ticks() / 500) & 1 ? "|" : " ");
        TB(cx + SC(8), TY(iy - 2, SC(20)), path, C_TXT);
        T(cx + w - SC(200), TY(iy - 2, SC(20)), "Enter - создать, Esc - отмена", C_TXT2);
    } else {
        T(cx, TY(iy, SC(18)),
          "клик: открыть/зайти. Кнопки справа вверху - создать!", C_TXT2);
    }
}

static int files_click(int idx, int32_t mx, int32_t my) {
    win_t *W = &g_w[idx];
    if (files_btn_hit(W, 0, mx, my)) {      /* + Папка */
        W->edit_mode = 1; W->edit_name[0] = 0; W->edit_len = 0;
        return 1;
    }
    if (files_btn_hit(W, 1, mx, my)) {      /* + Файл */
        W->edit_mode = 2; W->edit_name[0] = 0; W->edit_len = 0;
        return 1;
    }
    int dir = W->aux;
    int32_t ry0 = W->y + WIN_BAR_H + SC(48);
    int r = (my - ry0) / FILE_ROW_H;
    if (r < 0) return 0;
    int seen = 0;
    if (dir != VFS_ROOT) {
        if (r == 0) { W->aux = vfs_parent(dir); return 1; }
        seen = 1;
    }
    for (int c = vfs_first_child(dir); c != VFS_NONE; c = vfs_next_sibling(c)) {
        if (seen == r) {
            if (vfs_is_dir(c)) { W->aux = c; return 1; }
            g_pending_view_node = c;
            launch_app(APP_VIEW, vfs_name(c), 0);
            return 1;
        }
        seen++;
    }
    return 0;
}

/* ввод имени файла/папки (также русские буквы из RU-раскладки) */
static int files_text_key(int idx, int k) {
    win_t *W = &g_w[idx];
    if (k == 27) { W->edit_mode = 0; return 1; }                    /* Esc */
    if (k == 8)  { edit_backspace(W->edit_name, &W->edit_len); return 1; }
    if (k == 13) {                                                  /* Enter */
        const char *nm = W->edit_name[0] ? W->edit_name
                          : (W->edit_mode == 1 ? "Новая папка" : "файл.txt");
        if (W->edit_mode == 1) {
            if (vfs_mkdir(W->aux, nm) != VFS_NONE)
                kprintf("[vfs] папка '%s' создана\n", nm);
            else
                kprintf("[vfs] не вышло создать папку '%s' (имя занято?)\n", nm);
        } else {
            if (vfs_create(W->aux, nm) != VFS_NONE)
                kprintf("[vfs] файл '%s' создан (0 байт, открой его!)\n", nm);
            else
                kprintf("[vfs] не вышло создать файл '%s' (имя занято?)\n", nm);
        }
        W->edit_mode = 0;
        return 1;
    }
    if (k >= 32 && k < 256 && W->edit_len < (int)sizeof(W->edit_name) - 1) {
        W->edit_name[W->edit_len++] = (char)k;
        W->edit_name[W->edit_len] = 0;
        return 1;
    }
    return 1;      /* остальные клавиши в режиме ввода глотаем молча */
}

/* ---- Просмотр файла + мини-редактор (F2 = сохранить) ---- */
static int count_lines(const char *s) {
    int n = 1;
    for (; s && *s; s++) if (*s == '\n') n++;
    return n;
}

static const char *view_source(win_t *W, uint32_t *len) {
    if (W->vnode != VFS_NONE) return vfs_read(W->vnode, len);
    if (len) *len = 0;
    return W->text ? W->text : "(пусто)\n";
}

/* кнопка "Изменить"/"Сохранить" в правом верхнем углу контента */
#define VBW SC(86)
#define VBH SC(18)
static int view_btn_hit(win_t *W, int32_t mx, int32_t my) {
    int32_t bx = W->x + W->w - SC(12) - VBW;
    int32_t by = W->y + WIN_BAR_H + SC(2);
    return mx >= bx && mx < bx + VBW && my >= by && my < by + VBH;
}

static void view_draw(int idx, int32_t x, int32_t y, int32_t w, int32_t h) {
    win_t *W = &g_w[idx];
    int32_t cx = x + SC(12);
    TB(cx, TY(y + SC(4), SC(18)), W->name[0] ? W->name : "(файл)", C_PURPLE);
    btn_draw(x + w - SC(12) - VBW, y + SC(2), VBW, VBH,
             W->edit_mode ? "Сохранить" : "Изменить",
             W->edit_mode ? C_LIME : C_PURPLE, 0);
    if (W->edit_mode)
        T(x + w - SC(12) - VBW - SC(150), TY(y + SC(2), VBH), "F2/Esc, ввод - в конец", C_TXT2);
    gfx_fill_rect(cx, y + SC(24), w - SC(24), 1, C_PLINE);

    int32_t ty = y + SC(28);
    gfx_fill_round_rect(cx, ty, w - SC(24), h - SC(28) - SC(16), 6,
                        W->edit_mode ? GFX_RGB(0x10, 0x14, 0x0E) : C_TERM_BG);
    int vis = (h - SC(28) - SC(16) - SC(8)) / (FNT + SC(3));
    const char *src;
    uint32_t slen = 0;
    if (W->edit_mode) src = W->edit_buf ? W->edit_buf : "";
    else src = view_source(W, &slen);
    (void)slen;
    int total = count_lines(src);

    /* скролл: aux - в чтении, aux2 - в редакторе (каретка всегда видна) */
    int sc0 = W->edit_mode ? W->aux2 : W->aux;
    int maxs = total - vis; if (maxs < 0) maxs = 0;
    if (W->edit_mode) {
        int caret_row = 1;                      /* строка конца буфера */
        for (const char *s = src; *s; s++) if (*s == '\n') caret_row++;
        if (caret_row - sc0 + 1 > vis) sc0 = caret_row - vis + 1;
    }
    if (sc0 > maxs) sc0 = maxs;
    if (sc0 < 0) sc0 = 0;
    if (W->edit_mode) W->aux2 = sc0; else W->aux = sc0;

    int skip = sc0, row = 0;
    char line[128]; int li = 0;
    int maxc = (w - SC(44)) / ADV;
    for (const char *s = src; ; s++) {
        int end = !*s || *s == '\n';
        if (end) {
            line[li] = 0;
            if (skip > 0) skip--;
            else if (row < vis) {
                int cc = li > maxc ? maxc : li;
                line[cc] = 0;
                T(cx + SC(8), ty + SC(5) + row * (FNT + SC(3)), line, C_TERM_LN);
                row++;
            }
            li = 0;
            if (!*s) break;
        } else if (li < 127) line[li++] = *s;
    }
    if (W->edit_mode && (sched_ticks() / 500) & 1) {
        /* мигающий курсор у конца текста */
        int crow = 0, ccol = 0;
        for (const char *s = src; *s; s++) {
            if (*s == '\n') { crow++; ccol = 0; }
            else ccol++;
        }
        crow -= sc0;
        if (crow >= 0 && crow < vis && ccol < maxc)
            gfx_fill_rect((uint32_t)(cx + SC(8) + ccol * ADV),
                          (uint32_t)(ty + SC(5) + crow * (FNT + SC(3))),
                          (uint32_t)ADV, (uint32_t)FNT, C_LIME);
    }

    char info[64], num[16]; char *p = info;
    if (W->edit_mode) {
        pcat(&p, "знаков: ");
        u32dec((uint32_t)W->edit_len, num); pcat(&p, num);
        pcat(&p, "/2048");
    } else {
        pcat(&p, "строка "); u32dec((uint32_t)(W->aux + 1), num); pcat(&p, num);
        pcat(&p, "/"); u32dec((uint32_t)total, num); pcat(&p, num);
        pcat(&p, " (стрелки/колесо)");
    }
    T(cx, TY(ty + h - SC(28) - SC(16) + SC(2), SC(14)), info, C_TXT2);
}

static int view_click(int idx, int32_t mx, int32_t my) {
    win_t *W = &g_w[idx];
    if (!view_btn_hit(W, mx, my)) return 0;
    if (!W->edit_mode) {
        /* входим в редактор: копия содержимого в буфер */
        if (!W->edit_buf)
            W->edit_buf = (char *)kmalloc(2048);
        if (!W->edit_buf) return 1;
        uint32_t len = 0;
        const char *src = view_source(W, &len);
        W->edit_len = 0;
        for (uint32_t i = 0; i < len && W->edit_len < 2040; i++)
            W->edit_buf[W->edit_len++] = src[i];
        W->edit_buf[W->edit_len] = 0;
        W->edit_mode = 1;
        W->aux2 = 0;
        kprintf("[vfs] редактор: '%s' открыт для правки\n", W->name);
    } else {
        /* СОХРАНИТЬ */
        if (W->vnode != VFS_NONE) {
            vfs_write(W->vnode, W->edit_buf ? W->edit_buf : "", (uint32_t)W->edit_len);
            kprintf("[vfs] файл '%s' сохранён (%lu байт)\n", W->name, (uint64_t)W->edit_len);
            W->edit_mode = 0;
        } else {
            kprintf("[vfs] '%s' - статический демо-текст, не перезаписать\n", W->name);
            W->edit_mode = 0;
        }
    }
    return 1;
}

static int view_text_key(int idx, int k) {
    win_t *W = &g_w[idx];
    if (k == 27 || k == KEY_F2) {            /* Esc = выйти, F2 = сохранить */
        if (k == KEY_F2 && W->vnode != VFS_NONE) {
            vfs_write(W->vnode, W->edit_buf ? W->edit_buf : "", (uint32_t)W->edit_len);
            kprintf("[vfs] файл '%s' сохранён (%lu байт)\n", W->name, (uint64_t)W->edit_len);
        }
        W->edit_mode = 0;
        return 1;
    }
    if (k == 8) { edit_backspace(W->edit_buf, &W->edit_len); return 1; }
    if (k == 13) {
        if (W->edit_len < 2040) { W->edit_buf[W->edit_len++] = '\n'; W->edit_buf[W->edit_len] = 0; }
        return 1;
    }
    if (k >= 32 && k < 256 && W->edit_len < 2040) {
        W->edit_buf[W->edit_len++] = (char)k;
        W->edit_buf[W->edit_len] = 0;
        return 1;
    }
    return 1;
}

/* ================= НАСТРОЙКИ (F5) =================
 * Секции левым списком. Выбор экрана сохраняется в UEFI NVRAM и
 * применяется ПОСЛЕ ПЕРЕЗАГРУЗКИ (GOP ставит загрузчик), остальное - сразу. */
#define SET_SECS 5
static const char *SET_SEC[SET_SECS] = {
    "Экран", "Интерфейс", "Обои", "Мышь", "Система"
};
static int  g_set_mode   = -1;   /* выбранный режим в списке (idx bootinfo) */
static char g_set_status[120];
static int  g_set_status_ok = 1;
static int  g_reboot_arm = 0;
static bootinfo_mode_t g_modes[BOOTINFO_MAX_MODES];
static uint32_t        g_modes_n;
static int             g_mouse_idx = 2;   /* 0..8 -> 50..250% */

static void set_status(const char *s, int ok) {
    int i = 0;
    for (; s[i] && i < 119; i++) g_set_status[i] = s[i];
    g_set_status[i] = 0;
    g_set_status_ok = ok;
}

/* построить "WxH" и сохранить как желаемый режим загрузки */
static void settings_apply_mode(void) {
    if (g_set_mode < 0 || (uint32_t)g_set_mode >= g_modes_n)
        { set_status("Сначала выбери режим в списке.", 0); return; }
    char s[20], num[12]; char *p = s;
    u32dec(g_modes[g_set_mode].w, num); pcat(&p, num);
    *p++ = 'x';
    u32dec(g_modes[g_set_mode].h, num); pcat(&p, num);
    *p = 0;
    if (efi_var_set_str("AresVideoMode", s))
        set_status("Сохранено в NVRAM! Режим применится после перезагрузки.", 1);
    else
        set_status("UEFI NVRAM недоступна - настройка НЕ переживёт перезагрузку.", 0);
    g_reboot_arm = 0;
}

static void settings_apply_scale(int pct) {
    char s[80], num[8]; char *p = s;
    g_ui_scale = pct;
    g_mag10 = pct / 10;
    mouse_set_limits(g_scr_w, g_scr_h);   /* на всякий случай */
    pcat(&p, "Масштаб "); u32dec((uint32_t)pct, num); pcat(&p, num);
    pcat(&p, "% применён сразу! Сохранён: ");
    pcat(&p, efi_var_set_u32("AresScale", (uint32_t)pct) ? "да" : "нет NVRAM");
    set_status(s, 1);
}

static void settings_apply_wall(int i) {
    g_wall = i;
    bg_cache_build();
    char s[96]; char *p = s;
    pcat(&p, "Обои '"); pcat(&p, WALLS[i].name);
    pcat(&p, "' применены. Сохранено: ");
    pcat(&p, efi_var_set_u32("AresWallpaper", (uint32_t)i) ? "да" : "нет NVRAM");
    set_status(s, 1);
}

static void settings_apply_mouse(void) {
    int pct = 50 + g_mouse_idx * 25;
    mouse_set_speed(pct);
    efi_var_set_u32("AresMouse",
                    (uint32_t)g_mouse_idx | ((uint32_t)mouse_get_accel() << 16));
    char s[96], num[8]; char *p = s;
    pcat(&p, "Скорость мыши: "); u32dec((uint32_t)pct, num); pcat(&p, num);
    pcat(&p, "% - действует сразу."); *p = 0;
    set_status(s, 1);
}

static void settings_draw(int idx, int32_t x, int32_t y, int32_t w, int32_t h) {
    win_t *W = &g_w[idx];
    (void)h;
    int32_t mx = mouse_x(), my = mouse_y();
    /* левый столбец секций */
    for (int i = 0; i < SET_SECS; i++) {
        int32_t ry = y + SC(8) + i * SC(26);
        int hov = (mx >= x + SC(10) && mx < x + SC(124) && my >= ry && my < ry + SC(22));
        if (i == W->aux) gfx_fill_round_rect(x + SC(10), ry, SC(114), SC(22), SC(6), C_CYAN);
        else if (hov)  gfx_fill_round_rect(x + SC(10), ry, SC(114), SC(22), SC(6), C_BTN_H);
        T(x + SC(20), TY(ry, SC(22)), SET_SEC[i],
          i == W->aux ? GFX_RGB(0x08, 0x0C, 0x12) : (hov ? C_TXT : C_TXT2));
    }
    gfx_fill_rect(x + SC(132), y + SC(8), 1, h - SC(40), C_PLINE);
    int32_t cx = x + SC(146);
    (void)w;
    int32_t yy = y + SC(10);

    switch (W->aux) {
    case 0: { /* ------- Экран ------- */
        TB(cx, yy, "Разрешение экрана", C_TXT); yy += FNT + SC(8);
        for (uint32_t i = 0; i < g_modes_n && i < 12; i++) {
            int32_t ry = yy + (int32_t)i * SC(17);
            int cur = (g_modes[i].w == g_scr_w && g_modes[i].h == g_scr_h);
            int hov = (mx >= cx && mx < cx + SC(220) && my >= ry && my < ry + SC(16));
            if ((int32_t)i == (int32_t)(W->aux2 & 0xFF))
                gfx_fill_round_rect(cx - 4, ry, SC(228), SC(16), SC(5), GFX_RGB(0x24, 0x30, 0x4A));
            else if (hov)
                gfx_fill_round_rect(cx - 4, ry, SC(228), SC(16), SC(5), C_ROW_ALT);
            char rb[20], n1[10], n2[10]; char *p = rb;
            u32dec(g_modes[i].w, n1); pcat(&p, n1); *p++ = 'x';
            u32dec(g_modes[i].h, n2); pcat(&p, n2); *p = 0;
            T(cx + SC(14), TY(ry, SC(16)), rb, cur ? C_GREEN : C_TXT);
            if (cur) T(cx + SC(100), TY(ry, SC(16)), "- сейчас", C_GREEN);
        }
        yy += (int32_t)g_modes_n * SC(17) + SC(8);
        if (g_modes_n > 12) { T(cx, TY(yy, SC(16)), "(показано 12 режимов)", C_TXT2); yy += SC(17); }
        T(cx, TY(yy, SC(16)), "Выбор запоминается и применяется ПОСЛЕ перезагрузки!", C_ACCENT);
        yy += SC(20);
        btn_draw(cx, yy, SC(120), SC(24), "Применить", C_BLUE, 0);
        btn_draw(cx + SC(130), yy, SC(140), SC(24),
                 g_reboot_arm == 1 ? "Точно перезагрузить?" : "Перезагрузить", C_RED, g_reboot_arm == 1);
        break; }
    case 1: { /* ------- Интерфейс (масштаб) ------- */
        TB(cx, yy, "Масштаб интерфейса и шрифта", C_TXT); yy += FNT + SC(10);
        static const int SCV[3] = { 100, 150, 200 };
        for (int i = 0; i < 3; i++) {
            char lb[12], nb[8]; char *p = lb;
            u32dec((uint32_t)SCV[i], nb); pcat(&p, nb); *p++ = '%'; *p = 0;
            btn_draw(cx, yy + i * (SC(34)), SC(150), SC(28), lb,
                     C_CYAN, g_ui_scale == SCV[i]);
        }
        yy += 3 * SC(34) + SC(8);
        T(cx, TY(yy, SC(18)), "Меняется МГНОВЕННО: окна, панель, док и шрифт.", C_TXT2);
        yy += SC(18);
        T(cx, TY(yy, SC(18)), "150% = крупный шрифт 12px, 200% = огромный 16px.", C_TXT2);
        break; }
    case 2: { /* ------- Обои ------- */
        TB(cx, yy, "Фон рабочего стола", C_TXT); yy += FNT + SC(10);
        for (int i = 0; i < WALL_N; i++) {
            int32_t sx = cx + (i % 2) * (SC(150) + SC(12));
            int32_t sy = yy + (i / 2) * (SC(56) + SC(10));
            gfx_gradient_v_rect((uint32_t)sx, (uint32_t)sy, SC(150), SC(56),
                                WALLS[i].a, WALLS[i].c);
            gfx_frame_rect((uint32_t)sx, (uint32_t)sy, SC(150), SC(56),
                           i == g_wall ? C_LIME : C_PLINE);
            T(sx + SC(6), TY(sy, SC(56)), WALLS[i].name,
              i == g_wall ? C_LIME : C_TXT);
        }
        break; }
    case 3: { /* ------- Мышь ------- */
        TB(cx, yy, "Указатель мыши", C_TXT); yy += FNT + SC(12);
        T(cx, TY(yy, SC(18)), "Скорость:", C_TXT2);
        btn_draw(cx + SC(90), yy - 2, SC(28), SC(22), "-", C_CYAN, 0);
        /* полоса значений 50..250 */
        int32_t bx = cx + SC(124);
        gfx_fill_round_rect(bx, yy + SC(2), SC(150), SC(16), SC(8), C_BAR_BG);
        gfx_fill_round_rect(bx, yy + SC(2), (int32_t)((g_mouse_idx + 1) * SC(150) / 9),
                            SC(16), SC(8), C_CYAN);
        char nb[8]; u32dec((uint32_t)(50 + g_mouse_idx * 25), nb);
        T(bx + SC(156), TY(yy, SC(18)), nb, C_TXT);
        btn_draw(cx + SC(292), yy - 2, SC(28), SC(22), "+", C_CYAN, 0);
        yy += SC(34);
        int32_t cbx = cx + SC(90);
        gfx_frame_rect((uint32_t)cbx, (uint32_t)yy, SC(16), SC(16), C_PLINE);
        if (mouse_get_accel())
            gfx_fill_round_rect(cbx + 2, yy + 2, SC(16) - 4, SC(16) - 4, SC(4), C_LIME);
        T(cbx + SC(24), TY(yy, SC(16)), "Ускорение при резком движении", C_TXT);
        yy += SC(28);
        T(cx, TY(yy, SC(18)), "Всё действует сразу и сохраняется в NVRAM.", C_TXT2);
        break; }
    case 4: { /* ------- Система ------- */
        TB(cx, yy, "Система", C_TXT); yy += FNT + SC(8);
        T(cx, TY(yy, SC(16)), "AresOS v0.7.0 - ядро 0.7.0 (x86-64)", C_TXT); yy += SC(16);
        T(cx, TY(yy, SC(16)), g_ram_line, C_TXT2); yy += SC(16);
        {
            char up[64], n1[12], n2[12]; char *p = up;
            uint64_t secs = sched_ticks() / 1000;
            pcat(&p, "Аптайм: ");
            u32dec((uint32_t)(secs / 60), n1); pcat(&p, n1); pcat(&p, " мин ");
            u32dec((uint32_t)(secs % 60), n2); pcat(&p, n2); pcat(&p, " сек");
            T(cx, TY(yy, SC(16)), up, C_TXT2); yy += SC(16);
        }
        {
            char la[64]; char *p = la;
            pcat(&p, "Раскладка: "); pcat(&p, keyboard_ru() ? "RU (русская)" : "EN (англ.)");
            T(cx, TY(yy, SC(16)), la, C_TXT2); yy += SC(20);
        }
        btn_draw(cx, yy, SC(160), SC(26),
                 g_reboot_arm == 2 ? "Точно перезагрузить?" : "Перезагрузить", C_RED, g_reboot_arm == 2);
        btn_draw(cx, yy + SC(34), SC(160), SC(26), "Сбросить настройки", C_AMBER, 0);
        yy += SC(34) + SC(32);
        T(cx, TY(yy, SC(16)), "Перезагрузка нужна только для смены разрешения.", C_TXT2);
        break; }
    }
    /* строка статуса */
    if (g_set_status[0])
        T(x + SC(146), TY(y + h - SC(22), SC(18)), g_set_status,
          g_set_status_ok ? C_GREEN : C_RED);
}

static int settings_click(int idx, int32_t mx, int32_t my) {
    win_t *W = &g_w[idx];
    int32_t x = W->x, y = W->y + WIN_BAR_H, h = W->h - WIN_BAR_H;
    /* секции */
    for (int i = 0; i < SET_SECS; i++) {
        int32_t ry = y + SC(8) + i * SC(26);
        if (mx >= x + SC(10) && mx < x + SC(124) && my >= ry && my < ry + SC(22)) {
            W->aux = i; g_reboot_arm = 0;
            return 1;
        }
    }
    int32_t cx = x + SC(146);
    int32_t yy = y + SC(10);
    switch (W->aux) {
    case 0: { /* Экран */
        yy += FNT + SC(8);
        for (uint32_t i = 0; i < g_modes_n && i < 12; i++) {
            int32_t ry = yy + (int32_t)i * SC(17);
            if (mx >= cx - 4 && mx < cx + SC(224) && my >= ry && my < ry + SC(16)) {
                W->aux2 = (W->aux2 & ~0xFF) | (int)i;
                g_set_mode = (int)i;
                g_reboot_arm = 0;
                return 1;
            }
        }
        yy += (int32_t)g_modes_n * SC(17) + SC(8);
        if (g_modes_n > 12) yy += SC(17);
        yy += SC(20);
        if (mx >= cx && mx < cx + SC(120) && my >= yy && my < yy + SC(24)) {
            settings_apply_mode();
            return 1;
        }
        if (mx >= cx + SC(130) && mx < cx + SC(270) && my >= yy && my < yy + SC(24)) {
            if (g_reboot_arm == 1) efi_reset_cold();
            g_reboot_arm = 1;
            return 1;
        }
        break; }
    case 1: { /* Интерфейс */
        yy += FNT + SC(10);
        static const int SCV[3] = { 100, 150, 200 };
        for (int i = 0; i < 3; i++) {
            int32_t byy = yy + i * SC(34);
            if (mx >= cx && mx < cx + SC(150) && my >= byy && my < byy + SC(28)) {
                g_reboot_arm = 0;
                settings_apply_scale(SCV[i]);
                return 1;
            }
        }
        break; }
    case 2: { /* Обои */
        yy += FNT + SC(10);
        for (int i = 0; i < WALL_N; i++) {
            int32_t sx = cx + (i % 2) * (SC(150) + SC(12));
            int32_t sy = yy + (i / 2) * (SC(56) + SC(10));
            if (mx >= sx && mx < sx + SC(150) && my >= sy && my < sy + SC(56)) {
                g_reboot_arm = 0;
                settings_apply_wall(i);
                return 1;
            }
        }
        break; }
    case 3: { /* Мышь */
        yy += FNT + SC(12);
        if (mx >= cx + SC(90) && mx < cx + SC(118) && my >= yy - 2 && my < yy + SC(20)) {
            if (g_mouse_idx > 0) { g_mouse_idx--; settings_apply_mouse(); }
            return 1;
        }
        int32_t bx = cx + SC(124);
        if (mx >= bx && mx < bx + SC(150) && my >= yy - 2 && my < yy + SC(20)) {
            g_mouse_idx = (mx - bx) * 9 / SC(150);
            if (g_mouse_idx > 8) g_mouse_idx = 8;
            if (g_mouse_idx < 0) g_mouse_idx = 0;
            settings_apply_mouse();
            return 1;
        }
        if (mx >= cx + SC(292) && mx < cx + SC(320) && my >= yy - 2 && my < yy + SC(20)) {
            if (g_mouse_idx < 8) { g_mouse_idx++; settings_apply_mouse(); }
            return 1;
        }
        yy += SC(34);
        if (mx >= cx + SC(90) && mx < cx + SC(130) + (int32_t)(utf_glyphs("Ускорение при резком движении") * ADV) &&
            my >= yy && my < yy + SC(16)) {
            mouse_set_accel(!mouse_get_accel());
            settings_apply_mouse();
            return 1;
        }
        break; }
    case 4: { /* Система */
        int32_t byy = y + SC(10) + FNT + SC(8) + 4 * SC(16) + SC(4);
        if (mx >= cx && mx < cx + SC(160) && my >= byy && my < byy + SC(26)) {
            if (g_reboot_arm == 2) efi_reset_cold();
            g_reboot_arm = 2;
            return 1;
        }
        if (mx >= cx && mx < cx + SC(160) && my >= byy + SC(34) && my < byy + SC(34) + SC(26)) {
            efi_var_delete("AresVideoMode"); efi_var_delete("AresScale");
            efi_var_delete("AresWallpaper"); efi_var_delete("AresMouse");
            set_status("Сохранённые настройки стёрты из NVRAM.", 1);
            g_reboot_arm = 0;
            return 1;
        }
        break; }
    }
    g_reboot_arm = 0;
    (void)h;
    return 0;
}

/* ================= УСТАНОВКА (автозапуск при старте) =================
 * Пишет системные файлы в ramfs - НАСТОЯЩИЕ действия, прогресс виден.
 * Честная подпись: диск (M6) ещё впереди, всё живёт в оперативной памяти. */
#define SETUP_STEPS 9
static int      g_inst_win  = -1;     /* окно, идёт установка (или -1) */
static int      g_inst_step;
static uint64_t g_inst_t0;
static int      g_upd_win   = -1;     /* окно, идёт "проверка обновлений" */
static uint64_t g_upd_t0;
#define UPD_TICKS 1400                /* мс "поиска обновлений" */

static void setup_do_step(int s) {
    int r = VFS_ROOT;
    switch (s) {
    case 0: if (vfs_find(r, "Система") == VFS_NONE) vfs_mkdir(r, "Система");
            kprintf("[setup] папка /Система\n"); break;
    case 1: { int d = vfs_find(r, "Система");
        if (d != VFS_NONE && vfs_find(d, "kernel.elf") == VFS_NONE) {
            int f = vfs_create(d, "kernel.elf");
            if (f != VFS_NONE)
                vfs_write(f, "Ядро AresOS - этот файл настоящий!\nGDT/IDT, PMM/VMM/heap, LAPIC+PIC, планировщик,\nPS/2 клавиатура+мышь и весь этот рабочий стол.\n", 138);
        }
        kprintf("[setup] /Система/kernel.elf\n"); break; }
    case 2: { int d = vfs_find(r, "Система");
        if (d != VFS_NONE && vfs_find(d, "bootx64.efi") == VFS_NONE) {
            int f = vfs_create(d, "bootx64.efi");
            if (f != VFS_NONE)
                vfs_write(f, "Наш UEFI-загрузчик: ELF-парсер, графика GOP,\nпоиск ACPI RSDP, NVRAM-настройки, ExitBootServices.\nПишется без gnu-efi - только свои структуры.\n", 132);
        }
        kprintf("[setup] /Система/bootx64.efi\n"); break; }
    case 3: { int d = vfs_find(r, "Система");
        if (d != VFS_NONE && vfs_find(d, "config.ini") == VFS_NONE) {
            int f = vfs_create(d, "config.ini");
            if (f != VFS_NONE)
                vfs_write(f, "theme=glass-aurora\nfont=8x8-cyrillic\nmouse=ps2+wheel\ntimer=1000Hz\nversion=0.7.0\n", 80);
        }
        kprintf("[setup] /Система/config.ini\n"); break; }
    case 4: if (vfs_find(r, "Документы") == VFS_NONE) vfs_mkdir(r, "Документы");
            kprintf("[setup] папка /Документы\n"); break;
    case 5: { int d = vfs_find(r, "Документы");
        if (d != VFS_NONE && vfs_find(d, "привет.txt") == VFS_NONE) {
            int f = vfs_create(d, "привет.txt");
            if (f != VFS_NONE)
                vfs_write(f, "Привет из Проводника AresOS!\nЭтот файл живёт в оперативной памяти (ramfs).\nОткрой меня кнопкой 'Изменить', допиши строку\nи нажми F2 или 'Сохранить' - я настоящий!\n", 180);
        }
        kprintf("[setup] /Документы/привет.txt\n"); break; }
    case 6: { int d = vfs_find(r, "Документы");
        if (d != VFS_NONE && vfs_find(d, "план.txt") == VFS_NONE) {
            int f = vfs_create(d, "план.txt");
            if (f != VFS_NONE)
                vfs_write(f, "План этапа M6:\n1. драйвер диска (AHCI / ramdisk)\n2. сохранение ramfs на диск при выходе\n3. FAT32 чтение+запись\n4. и тогда файлы переживут перезагрузку!\n", 146);
        }
        kprintf("[setup] /Документы/план.txt\n"); break; }
    case 7:
        if (vfs_find(r, "README.TXT") == VFS_NONE) {
            int f = vfs_create(r, "README.TXT");
            if (f != VFS_NONE)
                vfs_write(f, "AresOS v0.7.0\n64-битная ОС голого железа: свой загрузчик UEFI,\nсвоё ядро, свой графический рабочий стол.\nЭти файлы - настоящие, в памяти (ramfs):\nсоздавай свои папки и файлы прямо в Проводнике!\n", 182);
        }
        kprintf("[setup] /README.TXT\n"); break;
    case 8:
        if (vfs_find(r, "version.txt") == VFS_NONE) {
            int f = vfs_create(r, "version.txt");
            if (f != VFS_NONE)
                vfs_write(f, "kernel 0.7.0 (x86-64)\nAPIC/PIC 1000 Гц, PMM+VMM+heap, PE32+ loader\nокна-процессы, стекло-UI, ramfs-ФС, NVRAM-настройки\n", 122);
        }
        kprintf("[setup] /version.txt\n"); break;
    }
}

/* кнопки установщика: координаты по страницам */
static void setup_draw(int idx, int32_t x, int32_t y, int32_t w, int32_t h) {
    win_t *W = &g_w[idx];
    int32_t cx = x + SC(20);
    int LH = FNT + SC(7);
    int32_t yy;

    if (W->aux == 0) {          /* ---------- Приветствие ---------- */
        yy = y + SC(16);
        TB(cx, yy, "Добро пожаловать в AresOS v0.7.0!", C_TXT); yy += LH + SC(6);
        T(cx, yy, "Мастер установит систему: создаст папки и системные", C_TXT2); yy += LH;
        T(cx, yy, "файлы, настроит рабочий стол. Это займёт пару секунд.", C_TXT2); yy += LH;
        yy += SC(4);
        T(cx, yy, "Честно: файлы пока живут в оперативной памяти (ramfs) -", C_ACCENT); yy += LH;
        T(cx, yy, "драйвер диска и сохранение на диск придут на этапе M6.", C_ACCENT); yy += LH;
        yy += SC(6);
        if (vfs_find(VFS_ROOT, "Система") != VFS_NONE) {
            T(cx, yy, "Система УЖЕ установлена - можно переустановить.", C_GREEN); yy += LH;
        }
        int32_t by = y + h - SC(66);
        btn_draw(cx, by, SC(170), SC(30), "Установить", C_LIME, 0);
        btn_draw(cx, by + SC(38), SC(170), SC(26), "Пропустить", C_ROW_ALT, 0);
        btn_draw(cx + SC(184), by, SC(170), SC(30), "Центр обновления", C_CYAN, 0);
        T(cx + SC(184), TY(by + SC(38), SC(26)), "Установщик открывается при", C_TXT2);
        T(cx + SC(184), TY(by + SC(38), SC(26)) + FNT + SC(2), "каждой загрузке.", C_TXT2);
    } else if (W->aux == 1) {   /* ---------- Прогресс ---------- */
        yy = y + SC(16);
        TB(cx, yy, "Установка системы...", C_TXT); yy += LH + SC(10);
        int32_t bx = cx, bwd = w - SC(40);
        gfx_fill_round_rect(bx, yy, bwd, SC(22), SC(8), C_BAR_BG);
        int pct = g_inst_step * 100 / SETUP_STEPS;
        int32_t fw = bwd * pct / 100;
        if (fw > SC(8)) gfx_fill_round_rect(bx, yy, fw, SC(22), SC(8), C_LIME);
        char pb[8], nb[8]; char *p = pb;
        u32dec((uint32_t)(pct > 100 ? 100 : pct), nb); pcat(&p, nb); *p++ = '%'; *p = 0;
        TB(bx + SC(10), TY(yy, SC(22)), pb, C_TXT);
        yy += SC(22) + SC(12);
        /* последние строки журнала установки */
        static const char *STEP_TXT[SETUP_STEPS] = {
            "Создание папки Система",
            "Запись /Система/kernel.elf",
            "Запись /Система/bootx64.efi",
            "Запись /Система/config.ini",
            "Создание папки Документы",
            "Запись /Документы/привет.txt",
            "Запись /Документы/план.txt",
            "Запись README.TXT",
            "Запись version.txt",
        };
        int first = g_inst_step - 6; if (first < 0) first = 0;
        for (int i = first; i < g_inst_step && i < SETUP_STEPS; i++) {
            T(cx + SC(8), yy, STEP_TXT[i], i == g_inst_step - 1 ? C_GREEN : C_TXT2);
            T(cx, yy, ">", C_GREEN);
            yy += LH;
        }
        if (g_inst_step >= SETUP_STEPS) {
            T(cx + SC(8), yy, "Готово! Система установлена.", C_GREEN);
        } else {
            /* бегущая точка-спиннер */
            int ph = (int)((sched_ticks() / 90) % 4);
            char sp[8]; char *p = sp;
            for (int i = 0; i < 3; i++) *p++ = (i < ph) ? '.' : ' ';
            *p = 0;
            TB(cx + SC(8), yy, sp, C_LIME);
        }
        (void)h;
    } else if (W->aux == 2) {   /* ---------- Готово ---------- */
        yy = y + SC(18);
        TB(cx, yy, "AresOS установлена!", C_GREEN); yy += LH + SC(8);
        T(cx, yy, "Созданы папки и файлы: открой Проводник (жёлтую иконку)", C_TXT2); yy += LH;
        T(cx, yy, "и загляни в /Система и /Документы - всё по-настоящему.", C_TXT2); yy += LH;
        yy += SC(8);
        T(cx, yy, "Дальше по желанию: Настройки (F5) - разрешение экрана,", C_TXT2); yy += LH;
        T(cx, yy, "масштаб интерфейса, обои и скорость мыши.", C_TXT2); yy += LH;
        int32_t by = y + h - SC(66);
        btn_draw(cx, by, SC(170), SC(30), "Центр обновления", C_CYAN, 0);
        btn_draw(cx + SC(184), by, SC(170), SC(30), "На рабочий стол", C_LIME, 0);
    } else {                    /* ---------- Центр обновления ---------- */
        yy = y + SC(16);
        TB(cx, yy, "Центр обновления AresOS", C_TXT); yy += LH + SC(6);
        {
            char vl[64]; char *p = vl;
            pcat(&p, "Текущая версия: v0.7.0 (ядро 0.7.0)");
            T(cx, yy, vl, C_TXT2); yy += LH;
        }
        yy += SC(4);
        if (g_upd_win < 0) {
            if (W->aux2 == 2) {
                T(cx, yy, "Обновлений нет - у тебя самая свежая версия!", C_GREEN); yy += LH;
                T(cx, yy, "(проверено только что)", C_TXT2); yy += LH;
            } else {
                int32_t by = yy + SC(4);
                btn_draw(cx, by, SC(200), SC(28), "Проверить обновления", C_CYAN, 0);
                yy = by + SC(36);
            }
            yy += SC(6);
            T(cx, yy, "Честно: сетевой стек появится только на этапе M7, поэтому", C_ACCENT); yy += LH;
            T(cx, yy, "реальное обновление = собрать новый ISO и заменить CD в VM.", C_ACCENT); yy += LH;
            yy += SC(6);
            btn_draw(cx, y + h - SC(40), SC(140), SC(26), "Назад", C_ROW_ALT, 0);
        } else {
            /* "поиск": прогресс-полоска и честная пометка */
            uint64_t dt = sched_ticks() - g_upd_t0;
            if (dt > UPD_TICKS) dt = UPD_TICKS;
            int pct = (int)(dt * 100 / UPD_TICKS);
            int32_t bwd = w - SC(40);
            gfx_fill_round_rect(cx, yy, bwd, SC(18), SC(8), C_BAR_BG);
            int32_t fw = bwd * pct / 100;
            if (fw > SC(8)) gfx_fill_round_rect(cx, yy, fw, SC(18), SC(8), C_CYAN);
            yy += SC(18) + SC(10);
            T(cx, yy, "Проверяю наличие новых версий...", C_TXT2); yy += LH;
            T(cx, yy, "(демо-режим: интернета в ядре пока нет - этап M7)", C_ACCENT); yy += LH;
        }
    }
}

static int setup_click(int idx, int32_t mx, int32_t my) {
    win_t *W = &g_w[idx];
    int32_t x = W->x, y = W->y + WIN_BAR_H, h = W->h - WIN_BAR_H;
    int32_t cx = x + SC(20);
    if (W->aux == 0) {
        int32_t by = y + h - SC(66);
        if (mx >= cx && mx < cx + SC(170) && my >= by + SC(38) && my < by + SC(38) + SC(26)) {
            close_window(idx);                 /* Пропустить - просто закрыть окно */
            return 1;
        }
        if (mx >= cx && mx < cx + SC(170) && my >= by && my < by + SC(30)) {
            /* поехали: копим минимальные сиды, включаем прогресс */
            W->aux = 1;
            g_inst_win = idx; g_inst_step = 0; g_inst_t0 = sched_ticks();
            kprintf("[setup] === установка AresOS v0.7.0 началась ===\n");
            return 1;
        }
        if (mx >= cx + SC(184) && mx < cx + SC(354) && my >= by && my < by + SC(30)) {
            W->aux = 3; W->aux2 = 0;
            return 1;
        }
    } else if (W->aux == 2) {
        int32_t by = y + h - SC(66);
        if (mx >= cx && mx < cx + SC(170) && my >= by && my < by + SC(30)) {
            W->aux = 3; W->aux2 = 0;
            return 1;
        }
        if (mx >= cx + SC(184) && mx < cx + SC(354) && my >= by && my < by + SC(30)) {
            close_window(idx);
            return 1;
        }
    } else if (W->aux == 3) {
        if (g_upd_win >= 0) return 0;
        int32_t yy = y + SC(16) + FNT + SC(7) + FNT + SC(7) + SC(4);
        if (W->aux2 != 2 &&
            mx >= cx && mx < cx + SC(200) && my >= yy + SC(4) && my < yy + SC(4) + SC(28)) {
            g_upd_win = idx; g_upd_t0 = sched_ticks();
            kprintf("[setup] центр обновления: проверяю...\n");
            return 1;
        }
        int32_t by = y + h - SC(40);
        if (mx >= cx && mx < cx + SC(140) && my >= by && my < by + SC(26)) {
            W->aux2 = 0;
            W->aux = (vfs_find(VFS_ROOT, "Система") != VFS_NONE) ? 2 : 0;
            return 1;
        }
    }
    return 0;
}

/* периодика установщика и центра обновления (вызывается из главного цикла) */
static int setup_periodic(void) {
    int changed = 0;
    if (g_inst_win >= 0) {
        win_t *W = &g_w[g_inst_win];
        if (!W->open || W->app != APP_SETUP || W->aux != 1) {
            g_inst_win = -1;
        } else {
            uint64_t t = sched_ticks();
            if (g_inst_step < SETUP_STEPS && t - g_inst_t0 >= 260) {
                setup_do_step(g_inst_step);
                g_inst_step++;
                g_inst_t0 = t;
                changed = 1;
                if (g_inst_step >= SETUP_STEPS) {
                    kprintf("[setup] === установка завершена, файлы на месте ===\n");
                    W->aux = 2;                    /* страница "Готово" */
                    g_inst_win = -1;
                }
            }
        }
    }
    if (g_upd_win >= 0) {
        win_t *W = &g_w[g_upd_win];
        if (!W->open || W->app != APP_SETUP) {
            g_upd_win = -1;
        } else if (sched_ticks() - g_upd_t0 >= UPD_TICKS) {
            W->aux2 = 2;                           /* результат "обновлений нет" */
            g_upd_win = -1;
            kprintf("[setup] обновлений не найдено - v0.7.0 последняя\n");
            changed = 1;
        } else changed = 1;                        /* полоска ползёт - перерисовка */
    }
    return changed;
}

/* контент-диспетчер */
static void draw_content(int idx, int32_t x, int32_t y, int32_t w, int32_t h) {
    switch (g_w[idx].app) {
    case APP_ABOUT:    about_draw(x, y, w); break;
    case APP_TASKMAN:  tm_draw(x, y, w, h); break;
    case APP_LOGS:     logs_draw(idx, x, y, w, h); break;
    case APP_FILES:    files_draw(idx, x, y, w, h); break;
    case APP_VIEW:     view_draw(idx, x, y, w, h); break;
    case APP_SETTINGS: settings_draw(idx, x, y, w, h); break;
    case APP_SETUP:    setup_draw(idx, x, y, w, h); break;
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
    gfx_blend_round_rect((int32_t)g_scr_w - SC(240), (int32_t)g_scr_h - SC(30),
                         SC(206), SC(20), SC(7), C_DOCK, 200);
    T((int32_t)g_scr_w - SC(232), TY((int32_t)g_scr_h - SC(30), SC(20)), buf, C_TXT2);
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
    if (ry + rh > (int32_t)(g_scr_h - (uint32_t)DOCK_H - 16)) dock_draw();
    for (int i = 0; i < g_zn; i++) {
        int a = g_z[i];
        if (g_w[a].x < rx + rw && g_w[a].x + g_w[a].w + 16 > rx &&
            g_w[a].y < ry + rh && g_w[a].y + g_w[a].h + 16 > ry)
            draw_window(a);
    }
    if (ry + rh > (int32_t)(g_scr_h - (uint32_t)SC(34)) &&
        rx + rw > (int32_t)(g_scr_w - (uint32_t)SC(244)))
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

/* ---------------- накапливаемый ущерб от перетаскивания/подъёма --------------
 * Между кадрами (пэйсинг 60 fps) прямоугольники объединяются - так на экран
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

    /* список видеорежимов для Настроек (передал загрузчик) */
    g_modes_n = bi->modes_n;
    if (g_modes_n > BOOTINFO_MAX_MODES) g_modes_n = BOOTINFO_MAX_MODES;
    for (uint32_t i = 0; i < g_modes_n; i++) g_modes[i] = bi->modes[i];
    g_set_status[0] = 0;

    /* NVRAM-настройки: масштаб, обои, мышь (переживают перезагрузку!) */
    {
        uint32_t v;
        if (efi_var_get_u32("AresScale", &v) && (v == 100 || v == 150 || v == 200)) {
            g_ui_scale = (int)v; g_mag10 = g_ui_scale / 10;
            kprintf("[desktop] NVRAM: масштаб %lu%%\n", (uint64_t)v);
        }
        if (efi_var_get_u32("AresWallpaper", &v) && v < WALL_N) {
            g_wall = (int)v;
            kprintf("[desktop] NVRAM: обои #%lu '%s'\n", (uint64_t)v, WALLS[v].name);
        }
        if (efi_var_get_u32("AresMouse", &v)) {
            int mi = (int)(v & 0xFFFF);
            if (mi >= 0 && mi <= 8) {
                g_mouse_idx = mi;
                mouse_set_speed(50 + mi * 25);
            }
            mouse_set_accel((int)((v >> 16) & 1));
            kprintf("[desktop] NVRAM: мышь %d%%, ускорение %s\n",
                    (uint64_t)mouse_get_speed(), mouse_get_accel() ? "вкл" : "выкл");
        }
    }

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
    g_bg = (uint32_t *)kmalloc((uint64_t)g_scr_h * sizeof(uint32_t));
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

    kprintf("[desktop] рисую стол: буфер %lux%lu, масштаб %d%%, обои '%s'...\n",
            g_scr_w, g_scr_h, g_ui_scale, WALLS[g_wall].name);
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

    /* стартовое окно: УСТАНОВЩИК (v0.7.0) - приветствие и кнопки */
    launch_app(APP_SETUP, 0, 0);
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
    uint64_t caret_stamp = 0;

    for (;;) {
        int struct_dirty = 0, tm_dirty = 0, log_dirty = 0, aux_dirty = 0;
        int scroll_win = -1;

        /* ===== клавиатура ===== */
        int k;
        while ((k = keyboard_getch()) >= 0) {
            g_last_key = k;
            aux_dirty = 1;
            /* текстовый ввод - в АКТИВНОЕ окно, если оно в режиме ввода */
            {
                int tw = app_top();
                if (tw >= 0 && g_w[tw].edit_mode) {
                    if (g_w[tw].app == APP_FILES) { files_text_key(tw, k); struct_dirty = 1; continue; }
                    if (g_w[tw].app == APP_VIEW)  { view_text_key(tw, k);  scroll_win = tw; continue; }
                }
            }
            switch (k) {
            case KEY_F1: launch_app(APP_ABOUT, 0, 0);    struct_dirty = 1; break;
            case KEY_F2: launch_app(APP_TASKMAN, 0, 0);  struct_dirty = 1; break;
            case KEY_F3: launch_app(APP_LOGS, 0, 0);     struct_dirty = 1; break;
            case KEY_F4: launch_app(APP_FILES, 0, 0);    struct_dirty = 1; break;
            case KEY_F5: launch_app(APP_SETTINGS, 0, 0); struct_dirty = 1; break;
            case KEY_F6: launch_app(APP_SETUP, 0, 0);    struct_dirty = 1; break;
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
            if (t >= 0 && (g_w[t].app == APP_LOGS ||
                (g_w[t].app == APP_VIEW && !g_w[t].edit_mode))) {
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
                launch_app(LAUNCH_APP[hit], 0, 0);
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
                if (mx >= W->x + W->w - SC(28) && mx < W->x + W->w - SC(8) &&
                    my >= W->y + SC(6) && my < W->y + SC(26)) {
                    close_window(topmost);    /* структура изменилась */
                    rep_reset();
                } else if (my < W->y + WIN_BAR_H) {
                    drag_win = topmost;
                    drag_dx = mx - W->x;
                    drag_dy = my - W->y;
                    struct_dirty = 0;         /* драг - это repair, не мир */
                } else if (W->app == APP_FILES) {
                    if (!files_click(topmost, mx, my)) struct_dirty = 0;
                } else if (W->app == APP_VIEW) {
                    if (!view_click(topmost, mx, my)) struct_dirty = 0;
                } else if (W->app == APP_SETTINGS) {
                    if (!settings_click(topmost, mx, my)) struct_dirty = 0;
                } else if (W->app == APP_SETUP) {
                    if (!setup_click(topmost, mx, my)) struct_dirty = 0;
                } else struct_dirty = 0;      /* просто поднять: хватит repair */
            } else if ((hit = dock_hit(mx, my)) >= 0) {
                launch_app(LAUNCH_APP[hit], 0, 0);
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
            uint64_t sec = t / 1000;                     /* тик = 1 мс (1000 Гц) */
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
            /* анимации установщика и центра обновления */
            if (setup_periodic()) struct_dirty = 1;
            /* мигание каретки редактора / поля ввода */
            int tw = app_top();
            if (tw >= 0 && g_w[tw].edit_mode && t - caret_stamp >= 500) {
                caret_stamp = t;
                dirty_add_win(tw);
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
                    if (mx >= W->x + W->w - SC(28) && mx < W->x + W->w - SC(8) &&
                        my >= W->y + SC(6) && my < W->y + SC(26)) { zone = 400 + g_z[zi]; break; }
                }
            }
        }
        int hover_dirty = (zone != hover_zone_old);
        int zone_from = hover_zone_old;
        hover_zone_old = zone;

        /* hover внутри окон с кнопками - тоже мягкая перерисовка окна */
        {
            static int32_t omx = -1, omy = -1;
            int32_t mx = mouse_x(), my = mouse_y();
            int t = app_top();
            if (t >= 0 && (g_w[t].app == APP_SETTINGS || g_w[t].app == APP_SETUP ||
                           g_w[t].app == APP_FILES || g_w[t].app == APP_VIEW) &&
                (mx != omx || my != omy))
                dirty_add_win(t);
            omx = mx; omy = my;
        }

        /* ===== пакетный рендер =====
         * Философия: экран НЕ дёргаем по мелочи. Всё рисуется в RAM-буфер,
         * на видеопамять выливается ОДИН объединённый регион и не чаще ~60 fps
         * для "мягких" событий (движение мыши/драг). Курсор живёт отдельно -
         * тычками прямо в видеопамять, поэтому он летает всегда. */
        uint64_t now_t = sched_ticks();
        int pace_ok = (int64_t)(now_t - last_frame) >= FRAME_MIN;
        int render = struct_dirty || hover_dirty || tm_dirty || log_dirty ||
                     aux_dirty || (scroll_win >= 0) || g_dirty_on ||
                     ((g_rep_on || moved) && pace_ok);

        if (render) {
            cursor_hide();
            int caret_mark = g_dirty_on;          /* каретка пометила окно? */
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
                if (caret_mark) {
                    int tw = app_top();
                    if (tw >= 0) dirty_add_win(tw);
                }
                if (tm_dirty)
                    for (int i = 0; i < g_zn; i++)
                        if (g_w[g_z[i]].app == APP_TASKMAN) dirty_add_win(g_z[i]);
                if (log_dirty)
                    for (int i = 0; i < g_zn; i++)
                        if (g_w[g_z[i]].app == APP_LOGS) dirty_add_win(g_z[i]);
                if (scroll_win >= 0) dirty_add_win(scroll_win);
                if (aux_dirty) {
                    dirty_add((int32_t)g_scr_w - SC(104), 4, SC(104), SC(26));  /* часы */
                    dirty_add((int32_t)g_scr_w - SC(246), (int32_t)g_scr_h - SC(32),
                              SC(246), SC(32));                                  /* пилюля */
                } else if (moved) {
                    dirty_add((int32_t)g_scr_w - SC(246), (int32_t)g_scr_h - SC(32),
                              SC(246), SC(32));                                  /* x=..., y=... */
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
        __asm__ volatile ("hlt");   /* разбудит таймер 1000 Гц / IRQ1 / IRQ12 */
    }
}
