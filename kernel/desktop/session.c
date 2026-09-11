/* AresOS - session.c v0.8.0: загрузочный экран, мастер установки,
 * сплеш, экран блокировки и вход. Всё рисуется в тот же бэкбуфер, что и
 * рабочий стол, а кадры выводятся через desktop_present_full/_cursor -
 * поэтому курсор здесь такой же живой и без мерцаний.
 *
 * Поток, если AresOS ещё НЕ установлена (AresInstalled != 1 в NVRAM):
 *   сплеш -> загрузочное меню (Установить/Обновить/Диагностика/Live)
 *   Установить: прогресс -> имя учётки -> пароль (можно пропустить) ->
 *   готово (перезагрузка или на стол).
 * Если установлена:
 *   сплеш -> экран блокировки (часы RTC, "нажми клавишу / потяни вверх")
 *   -> экран входа (аватар, имя, пароль-звёздочки) -> рабочий стол.
 * F8 на блокировке и входе = вернуться в загрузочное меню (восстановление).
 *
 * Правило перерисовки: экран перерисовывается ТОЛЬКО когда что-то сменилось
 * (редко), а простое движение мыши обходится микро-сбросом курсора - так
 * 200-герцовая мышь не гоняет 8-МиБ сбросы. */
#include "session.h"
#include "desktop.h"
#include "gfx.h"
#include "mouse.h"
#include "keyboard.h"
#include "proc.h"
#include "efi_rt.h"
#include "vfs.h"
#include "rtc.h"
#include "logbuf.h"
#include "kprintf.h"
#include "fb_console.h"
#include <stdint.h>
#include <string.h>

/* ---------------- масштаб/шрифт ---------------- */
#define SSC(v) ((v) * desktop_scale() / 100)
static int MAG(void) { return desktop_scale() / 10; }

/* ---------------- палитра (та же "glass aurora") ---------------- */
static const gfx_color_t C_TXT    = GFX_RGB(0xE7, 0xEA, 0xF3);
static const gfx_color_t C_TXT2   = GFX_RGB(0x8B, 0x92, 0xA9);
static const gfx_color_t C_ACCENT = GFX_RGB(0xFF, 0x9E, 0x49);
static const gfx_color_t C_GREEN  = GFX_RGB(0x4F, 0xC3, 0x7B);
static const gfx_color_t C_LIME   = GFX_RGB(0x28, 0xC8, 0x40);
static const gfx_color_t C_BLUE   = GFX_RGB(0x4A, 0x9D, 0xFF);
static const gfx_color_t C_CYAN   = GFX_RGB(0x56, 0xC2, 0xE8);
static const gfx_color_t C_YELLOW = GFX_RGB(0xE8, 0xC1, 0x5A);
static const gfx_color_t C_RED    = GFX_RGB(0xFF, 0x5F, 0x57);
static const gfx_color_t C_BLACK  = GFX_RGB(0x02, 0x04, 0x0A);
static const gfx_color_t C_CARD   = GFX_RGB(0x19, 0x1D, 0x2A);
static const gfx_color_t C_CARD_B = GFX_RGB(0x33, 0x3B, 0x55);
static const gfx_color_t C_FIELD  = GFX_RGB(0x0D, 0x11, 0x1C);
static const gfx_color_t C_ROWALT = GFX_RGB(0x1F, 0x24, 0x33);
static const gfx_color_t C_BAR_BG = GFX_RGB(0x26, 0x2B, 0x3D);
static const gfx_color_t C_INK    = GFX_RGB(0x0A, 0x0C, 0x12);

/* ---------------- мелкие помощники ---------------- */
static void cpu_wait(void) { __asm__ volatile ("hlt"); }

static int glyph_n(const char *s) {
    int n = 0;
    for (; *s; s++) if (((uint8_t)*s & 0xC0) != 0x80) n++;
    return n;
}
static int twn(const char *s, int mag) { return glyph_n(s) * 8 * mag / 10; }

static void STM(int32_t x, int32_t y, const char *s, gfx_color_t c, int mag) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    gfx_text_mag((uint32_t)x, (uint32_t)y, s, c, mag);
}
static void STB(int32_t x, int32_t y, const char *s, gfx_color_t c, int mag) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    gfx_text_bold_mag((uint32_t)x, (uint32_t)y, s, c, mag);
}
static void STC(int32_t cx, int32_t y, const char *s, gfx_color_t c, int mag) {
    STM(cx - twn(s, mag) / 2, y, s, c, mag);
}
static void STBC(int32_t cx, int32_t y, const char *s, gfx_color_t c, int mag) {
    STB(cx - twn(s, mag) / 2, y, s, c, mag);
}
static void u32dec(uint32_t v, char *out) {
    char tmp[16]; int i = 0, j = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
}
static void pcat(char **p, const char *s) { while (*s) *(*p)++ = *s++; **p = 0; }

static void kb_drain(void) { while (keyboard_getch() >= 0) {} }

/* фон: обои + затемнение a (0..255) */
static void bg_dim(uint8_t a) {
    desktop_bg_fill();
    gfx_blend_rect(0, 0, gfx_width(), gfx_height(), C_BLACK, a);
}

/* карточка по центру */
static void card_draw(int32_t x, int32_t y, int32_t w, int32_t h) {
    gfx_blend_round_rect(x + SSC(6), y + SSC(10), w, h, SSC(18), C_BLACK, 110);
    gfx_fill_round_rect(x - 1, y - 1, w + 2, h + 2, SSC(19), C_CARD_B);
    gfx_fill_round_rect(x, y, w, h, SSC(18), C_CARD);
    gfx_blend_round_rect(x + SSC(2), y + SSC(2), w - SSC(4), SSC(26), SSC(8),
                         GFX_RGB(0xFF, 0xFF, 0xFF), 14);
}

/* кнопка */
typedef struct {
    int32_t x, y, w, h;
    const char *label;
    gfx_color_t col;
    int primary;     /* 1 = яркая заливка, 0 = призрак */
    int disabled;
} sbtn_t;

static int btn_hit(const sbtn_t *b, int32_t mx, int32_t my) {
    return mx >= b->x && mx < b->x + b->w && my >= b->y && my < b->y + b->h;
}
static int32_t btn_ty(const sbtn_t *b) {         /* y текста в кнопке */
    return b->y + (b->h - MAG() * 8 / 10) / 2 - 1;
}
static void btn_paint(const sbtn_t *b, int hover) {
    if (b->disabled) {
        gfx_fill_round_rect(b->x, b->y, b->w, b->h, SSC(9), C_ROWALT);
        STBC(b->x + b->w / 2, btn_ty(b), b->label, C_TXT2, MAG());
        return;
    }
    if (b->primary) {
        gfx_fill_round_rect(b->x, b->y, b->w, b->h, SSC(9), b->col);
        if (hover) gfx_blend_round_rect(b->x, b->y, b->w, b->h, SSC(9),
                                        GFX_RGB(0xFF, 0xFF, 0xFF), 36);
        STBC(b->x + b->w / 2, btn_ty(b), b->label, C_INK, MAG());
    } else {
        gfx_fill_round_rect(b->x, b->y, b->w, b->h, SSC(9), C_ROWALT);
        if (hover) gfx_blend_round_rect(b->x, b->y, b->w, b->h, SSC(9),
                                        GFX_RGB(0xFF, 0xFF, 0xFF), 22);
        gfx_fill_round_rect(b->x + SSC(10), b->y + b->h / 2 - SSC(4),
                            SSC(8), SSC(8), SSC(3), b->col);
        STB(b->x + SSC(26), btn_ty(b), b->label, hover ? C_TXT : C_TXT2, MAG());
    }
}

/* ---------------- текстовое поле (UTF-8, каретка, звёздочки) ---------------- */
typedef struct { char buf[64]; int len; } field_t;

static int field_key(field_t *f, int k, int max_glyph) {
    if (k == 8) {                                  /* Backspace (UTF-8-aware) */
        if (f->len > 0) {
            f->len--;
            while (f->len > 0 && ((uint8_t)f->buf[f->len] & 0xC0) == 0x80) f->len--;
            f->buf[f->len] = 0;
        }
        return 1;
    }
    if (k >= 0x20 && k < 0x100) {                  /* печатный байт (в т.ч. UTF-8) */
        if (f->len < 60) {
            f->buf[f->len++] = (char)k;
            f->buf[f->len] = 0;
            if (glyph_n(f->buf) > max_glyph) {     /* откат, если длиннее лимита */
                f->len--;
                while (f->len > 0 && ((uint8_t)f->buf[f->len] & 0xC0) == 0x80) f->len--;
                f->buf[f->len] = 0;
            }
        }
        return 1;
    }
    return 0;
}

static void field_paint(int32_t x, int32_t y, int32_t w, int32_t h,
                        const field_t *f, int masked, int caret_on) {
    gfx_fill_round_rect(x - 1, y - 1, w + 2, h + 2, SSC(10), C_CARD_B);
    gfx_fill_round_rect(x, y, w, h, SSC(9), C_FIELD);
    char show[80];
    if (masked) {
        int g = glyph_n(f->buf), i;
        for (i = 0; i < g && i < 30; i++) show[i] = '*';
        show[i] = 0;
    } else {
        int i;
        for (i = 0; i < f->len && i < 76; i++) show[i] = f->buf[i];
        show[i] = 0;
    }
    int mag = MAG() * 2;
    STM(x + SSC(12), y + (h - mag * 8 / 10) / 2 - 1, show,
        masked ? C_YELLOW : C_TXT, mag);
    if (caret_on) {
        int32_t cw2 = SSC(2); if (cw2 < 2) cw2 = 2;
        gfx_fill_rect(x + SSC(12) + twn(show, mag) + 1,
                      y + h / 2 - (mag * 8 / 10) / 2 - 2,
                      cw2, mag * 8 / 10 + 4, C_ACCENT);
    }
}

/* ---------------- учётная запись: хэш пароля (демо-уровень) ----------------
 * Пароль хранится НЕ открытым текстом, а как FNV-1a64("ares:"+пароль) в hex.
 * Честно: это защита "от любопытных глаз", не настоящая криптография - у нас
 * пока нет и диска. Но открытый текст в NVRAM мы не пишем принципиально. */
static uint64_t fnv1a64(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (const char *p = "ares:"; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    for (; *s; s++)                            { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    return h;
}
static void hash_hex(const char *pass, char out[17]) {
    uint64_t h = fnv1a64(pass);
    for (int i = 15; i >= 0; i--) { out[i] = "0123456789abcdef"[h & 15]; h >>= 4; }
    out[16] = 0;
}

static char g_user[48];
static int  g_has_user;
const char *session_user_name(void) { return g_user; }
int         session_user_present(void) { return g_has_user; }

/* =========================================================================
 *  СПЛЕШ-ЭКРАН (логотип + спиннер)
 * ========================================================================= */
static void splash(int ms, const char *caption) {
    kb_drain();
    uint64_t t0 = sched_ticks();
    int32_t W = (int32_t)gfx_width(), H = (int32_t)gfx_height();
    int lm = MAG() * 6;
    int last_ph = -1;
    for (;;) {
        uint64_t dt = sched_ticks() - t0;
        if (dt >= (uint64_t)ms) break;
        int ph = (int)((dt / 110) % 5);
        if (ph == last_ph) { cpu_wait(); continue; }
        last_ph = ph;
        bg_dim(175);
        const char *lg = "AresOS";
        int lw = twn(lg, lm);
        STB(W / 2 - lw / 2, H / 2 - SSC(96), "A", C_ACCENT, lm);
        STB(W / 2 - lw / 2 + twn("A", lm), H / 2 - SSC(96), "resOS", C_TXT, lm);
        int32_t uw = SSC(3); if (uw < 2) uw = 2;
        gfx_fill_round_rect(W / 2 - lw / 2, H / 2 - SSC(96) + lm * 8 / 10 + SSC(10),
                            lw, uw, SSC(2), C_ACCENT);
        STC(W / 2, H / 2 - SSC(96) + lm * 8 / 10 + SSC(26), "версия 0.8.0",
            C_TXT2, MAG() * 2);
        for (int i = 0; i < 5; i++) {
            gfx_color_t c = (i == ph) ? C_ACCENT
                          : ((i + 4) % 5 == ph ? C_BLUE : C_CARD_B);
            int32_t d = SSC(10);
            gfx_fill_round_rect(W / 2 + (i - 2) * SSC(22) - d / 2,
                                H / 2 + SSC(40), d, d, SSC(5), c);
        }
        STC(W / 2, H - SSC(84), caption, C_TXT2, MAG());
        desktop_present_full();
    }
    kb_drain();
}

/* =========================================================================
 *  ЗАГРУЗОЧНОЕ МЕНЮ (установить / обновить / диагностика / live)
 * ========================================================================= */
static const struct { const char *t; const char *d; gfx_color_t col; } MI[4] = {
    { "Установить AresOS", "Системные файлы и учётная запись", GFX_RGB(0x28, 0xC8, 0x40) },
    { "Обновить систему", "Проверить наличие новой версии", GFX_RGB(0x56, 0xC2, 0xE8) },
    { "Поиск неисправностей", "Диагностика, журнал, тест NVRAM", GFX_RGB(0xE8, 0xC1, 0x5A) },
    { "Рабочий стол (Live)", "Запустить без установки", GFX_RGB(0x4A, 0x9D, 0xFF) },
};

static void menu_geom(int32_t *px, int32_t *py, int32_t *pcw, int32_t *prh) {
    int32_t W = (int32_t)gfx_width(), H = (int32_t)gfx_height();
    int32_t cw = SSC(640), ch = SSC(520);
    if (cw > W - 40) cw = W - 40;
    if (ch > H - 40) ch = H - 40;
    *px = (W - cw) / 2; *py = (H - ch) / 2; *pcw = cw;
    *prh = (ch - SSC(96) - SSC(58) - 3 * SSC(10)) / 4;
}
static int menu_row_at(int32_t mx, int32_t my) {
    int32_t x, y, cw, rh;
    menu_geom(&x, &y, &cw, &rh);
    if (mx < x + SSC(24) || mx >= x + cw - SSC(24)) return -1;
    for (int i = 0; i < 4; i++) {
        int32_t ry = y + SSC(96) + i * (rh + SSC(10));
        if (my >= ry && my < ry + rh) return i;
    }
    return -1;
}

static void menu_draw(int sel, int32_t mx, int32_t my) {
    int32_t x, y, cw, rh;
    menu_geom(&x, &y, &cw, &rh);
    int32_t ch = rh * 4 + SSC(96) + SSC(58) + 3 * SSC(10);
    bg_dim(150);
    card_draw(x, y, cw, ch);
    int32_t cx = x + cw / 2;
    STBC(cx, y + SSC(24), "AresOS 0.8.0", C_TXT, MAG() * 3);
    STC(cx, y + SSC(24) + MAG() * 24 / 10 + SSC(8),
        "Загрузочный экран - выберите действие", C_TXT2, MAG());
    int32_t ry = y + SSC(96);
    for (int i = 0; i < 4; i++) {
        int32_t ix = x + SSC(24), iw = cw - SSC(48);
        int hov = (mx >= ix && mx < ix + iw && my >= ry && my < ry + rh);
        int on = hov || sel == i;
        gfx_fill_round_rect(ix, ry, iw, rh, SSC(10), on ? C_ROWALT : C_CARD);
        if (on) gfx_blend_round_rect(ix, ry, iw, rh, SSC(10),
                                     GFX_RGB(0xFF, 0xFF, 0xFF), 12);
        if (sel == i)
            gfx_fill_round_rect(ix, ry, SSC(4), rh, SSC(2), MI[i].col);
        gfx_fill_round_rect(ix + SSC(14), ry + rh / 2 - SSC(17), SSC(34), SSC(34),
                            SSC(9), MI[i].col);
        char num[2] = { (char)('1' + i), 0 };
        STBC(ix + SSC(14) + SSC(17), ry + rh / 2 - MAG() * 8 / 10, num, C_INK, MAG() * 2);
        STB(ix + SSC(62), ry + rh / 2 - MAG() * 12 / 10 - SSC(2), MI[i].t,
            on ? C_TXT : C_TXT2, MAG() * 2);
        STM(ix + SSC(62), ry + rh / 2 + SSC(6), MI[i].d, C_TXT2, MAG());
        ry += rh + SSC(10);
    }
    STC(cx, y + ch - SSC(36), "Стрелки/Tab - выбор    Enter - подтвердить    мышь тоже работает",
        C_TXT2, MAG());
}

static int boot_menu(void) {           /* 0..3 */
    kb_drain();
    int sel = 0, need = 1, hov_old = -2, sel_old = -1;
    int left_old = mouse_left();
    for (;;) {
        int k;
        while ((k = keyboard_getch()) >= 0) {
            if (k == KEY_UP)                    sel = (sel + 3) & 3;
            else if (k == KEY_DOWN || k == 9)   sel = (sel + 1) & 3;
            else if (k == 13)                   return sel;
            else if (k >= '1' && k <= '4')      return k - '1';
        }
        int moved = mouse_moved();
        int hov = menu_row_at(mouse_x(), mouse_y());
        if (moved && hov >= 0 && hov != sel) sel = hov;   /* мышь - только в движении */
        if (hov != hov_old) { hov_old = hov; need = 1; }
        if (sel != sel_old) { sel_old = sel; need = 1; }
        if (mouse_left() && !left_old && hov >= 0) return hov;
        left_old = mouse_left();
        if (need) {
            menu_draw(sel, mouse_x(), mouse_y());
            desktop_present_full();
            need = 0;
        } else if (moved) desktop_present_cursor();
        cpu_wait();
    }
}

/* =========================================================================
 *  МАСТЕР УСТАНОВКИ
 * ========================================================================= */
static const char *SETUP_TXT[SETUP_STEPS] = {
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

/* геометрия карточки мастера (ничего не рисует) */
static void wiz_geom(int32_t *px, int32_t *py, int32_t *pw, int32_t *ph) {
    int32_t W = (int32_t)gfx_width(), H = (int32_t)gfx_height();
    int32_t cw = SSC(620), ch = SSC(440);
    if (cw > W - 40) cw = W - 40;
    if (ch > H - 40) ch = H - 40;
    *px = (W - cw) / 2; *py = (H - ch) / 2; *pw = cw; *ph = ch;
}

/* рамка мастера: тёмный фон + карточка + заголовок (+ "Шаг N из M") */
static void wiz_frame(int32_t *px, int32_t *py, int32_t *pw, int32_t *ph,
                      const char *title, int page, int pages) {
    int32_t x, y, cw, ch;
    wiz_geom(&x, &y, &cw, &ch);
    bg_dim(150);
    card_draw(x, y, cw, ch);
    STBC(x + cw / 2, y + SSC(22), title, C_TXT, MAG() * 2);
    if (pages > 0) {
        char st[32], n1[8], n2[8]; char *p = st;
        u32dec((uint32_t)page, n1); u32dec((uint32_t)pages, n2);
        pcat(&p, "Шаг "); pcat(&p, n1); pcat(&p, " из "); pcat(&p, n2);
        STC(x + cw / 2, y + SSC(22) + MAG() * 16 / 10 + SSC(8), st, C_TXT2, MAG());
    }
    *px = x; *py = y; *pw = cw; *ph = ch;
}

/* ---- страница 1: прогресс установки файлов ---- */
static void wiz_progress(void) {
    kb_drain();
    kprintf("[session] === установка AresOS v0.8.0: поехали ===\n");
    int step = 0;
    uint64_t t0 = sched_ticks();
    for (;;) {
        int did = 0;
        if (step < SETUP_STEPS && sched_ticks() - t0 >= 240) {
            setup_steps_run(step);
            step++;
            t0 = sched_ticks();
            did = 1;
        }
        if (step >= SETUP_STEPS && sched_ticks() - t0 >= 600) break;
        /* перерисовка: раз 130 мс (спиннер) + каждое событие-шаг */
        static int ph_old = -1;
        int ph = (int)((sched_ticks() / 130) % 4);
        if (!did && ph == ph_old) { cpu_wait(); continue; }
        ph_old = ph;
        int32_t x, y, w, h;
        wiz_frame(&x, &y, &w, &h, "Установка AresOS", 1, 4);
        int32_t cx = x + SSC(40);
        int32_t bw = w - SSC(80);
        int32_t by = y + SSC(104);
        gfx_fill_round_rect(cx, by, bw, SSC(24), SSC(9), C_BAR_BG);
        int pct = step * 100 / SETUP_STEPS;
        int32_t fw = bw * pct / 100;
        if (fw > SSC(10)) gfx_fill_round_rect(cx, by, fw, SSC(24), SSC(9), C_LIME);
        char pb[8], nb[8]; char *p = pb;
        u32dec((uint32_t)pct, nb); pcat(&p, nb); *p++ = '%'; *p = 0;
        STBC(x + w / 2, by + SSC(12) - MAG() * 4 / 10, pb, C_TXT, MAG());
        int32_t yy = by + SSC(48);
        int first = step - 5; if (first < 0) first = 0;
        for (int i = first; i < step && i < SETUP_STEPS; i++) {
            STM(cx + SSC(10), yy, ">", C_LIME, MAG());
            STM(cx + SSC(26), yy, SETUP_TXT[i],
                i == step - 1 && step < SETUP_STEPS ? C_TXT : C_GREEN, MAG());
            yy += MAG() * 8 / 10 + SSC(8);
        }
        if (step >= SETUP_STEPS) {
            STB(cx + SSC(26), yy, "Файлы записаны! Дальше - ваша учётная запись.",
                C_GREEN, MAG());
        } else {
            char sp[8]; p = sp;
            for (int i = 0; i < 3; i++) *p++ = (i < ph) ? '.' : ' ';
            *p = 0;
            STB(cx + SSC(26), yy, sp, C_LIME, MAG());
        }
        desktop_present_full();
    }
}

/* ---- страницы 2/3: ввод (имя / пароль) ---- */
enum { PG_NEXT, PG_BACK, PG_SKIP };

typedef struct { sbtn_t back, skip, next; int32_t fld_x, fld_y, fld_w; } ipg_geo_t;
static void ipg_geom(int32_t x, int32_t y, int32_t w, int32_t h,
                     int allow_skip, ipg_geo_t *g) {
    int32_t bh = SSC(34);
    int32_t byy = y + h - SSC(58);
    int32_t bw = SSC(160);
    g->back = (sbtn_t){ x + SSC(24), byy, SSC(110), bh, "Назад", C_ACCENT, 0, 0 };
    if (allow_skip)
        g->skip = (sbtn_t){ x + w / 2 - bw - SSC(10), byy, bw, bh,
                            "Пропустить", C_YELLOW, 0, 0 };
    else
        g->skip = (sbtn_t){ 0, 0, 0, 0, "", C_YELLOW, 0, 1 };
    g->next = (sbtn_t){ x + w / 2 + SSC(10), byy, bw, bh, "Далее", C_LIME, 1, 0 };
    g->fld_x = x + SSC(40); g->fld_w = w - SSC(80);
    g->fld_y = y + SSC(150);
}

static int wiz_input_page(const char *title, const char *sub1, const char *sub2,
                          field_t *f, int max_glyph, int masked, int allow_skip,
                          int page) {
    kb_drain();
    f->len = 0; f->buf[0] = 0;
    int left_old = mouse_left();
    int need = 1, hov_old = -2, care_old = -1, dis_old = -1;
    int moved;
    for (;;) {
        moved = mouse_moved();
        int k;
        while ((k = keyboard_getch()) >= 0) {
            if (k == 13) {                       /* Enter = Далее */
                if (f->len > 0) return PG_NEXT;
                if (allow_skip) return PG_SKIP;
            } else if (k == 27) {                /* Esc = Назад */
                return PG_BACK;
            } else if (field_key(f, k, max_glyph)) {
                need = 1;
            }
        }
        int32_t mx = mouse_x(), my = mouse_y();
        int click = mouse_left() && !left_old;
        left_old = mouse_left();
        int32_t x, y, w, h;
        wiz_geom(&x, &y, &w, &h);                    /* только геометрия */
        ipg_geo_t g;
        ipg_geom(x, y, w, h, allow_skip, &g);
        g.next.disabled = (f->len == 0);
        if (allow_skip) g.next.disabled = 0;         /* у пароля Далее = тоже ок */
        int hov = btn_hit(&g.back, mx, my) ? 0 :
                  (allow_skip && btn_hit(&g.skip, mx, my)) ? 1 :
                  btn_hit(&g.next, mx, my) ? 2 : -1;
        if (hov != hov_old) { hov_old = hov; need = 1; }
        if (g.next.disabled != dis_old) { dis_old = g.next.disabled; need = 1; }
        int care = (int)((sched_ticks() / 500) & 1);
        if (care != care_old) { care_old = care; need = 1; }
        if (click) {
            if (btn_hit(&g.back, mx, my)) return PG_BACK;
            if (!g.next.disabled && btn_hit(&g.next, mx, my)) return PG_NEXT;
            if (allow_skip && btn_hit(&g.skip, mx, my)) return PG_SKIP;
            need = 1;
        }
        if (need) {
            int32_t x2, y2, w2, h2;
            wiz_frame(&x2, &y2, &w2, &h2, title, page, 4);   /* теперь рисуем */
            x = x2; y = y2; w = w2; h = h2;
            int32_t cx = x + SSC(40);
            int32_t yy = y + SSC(92);
            if (sub1) { STM(cx, yy, sub1, C_TXT2, MAG()); yy += MAG() * 8 / 10 + SSC(8); }
            if (sub2) { STM(cx, yy, sub2, C_TXT2, MAG()); yy += MAG() * 8 / 10 + SSC(8); }
            field_paint(g.fld_x, g.fld_y, g.fld_w, SSC(40), f, masked, care);
            yy = g.fld_y + SSC(52);
            STM(cx, yy, masked
                ? "Пароль хранится как хэш (демо), а не открытым текстом."
                : "Можно русскими буквами: Alt+Shift переключает RU/EN.",
                C_TXT2, MAG());
            btn_paint(&g.back, hov == 0);
            if (allow_skip) btn_paint(&g.skip, hov == 1);
            btn_paint(&g.next, hov == 2);
            desktop_present_full();
            need = 0;
        } else if (moved) desktop_present_cursor();
        cpu_wait();
    }
}

/* ---- страница 4: готово ---- */
static int wiz_done_page(const char *name) {  /* 0 = на стол */
    kb_drain();
    int left_old = mouse_left();
    int reboot_arm = 0, need = 1, hov_old = -2, arm_old = -1, moved;
    for (;;) {
        moved = mouse_moved();
        kb_drain();                              /* клавиши тут ни к чему */
        int32_t mx = mouse_x(), my = mouse_y();
        int click = mouse_left() && !left_old;
        left_old = mouse_left();
        int32_t x, y, w, h;
        int32_t W = (int32_t)gfx_width(), H = (int32_t)gfx_height();
        w = SSC(620); h = SSC(440);
        if (w > W - 40) w = W - 40;
        if (h > H - 40) h = H - 40;
        x = (W - w) / 2; y = (H - h) / 2;
        int32_t bw = SSC(200), bh = SSC(34);
        int32_t byy = y + h - SSC(58);
        sbtn_t reb = { x + w / 2 - bw - SSC(10), byy, bw, bh,
                       reboot_arm ? "Точно перезагрузить?" : "Перезагрузить",
                       C_ACCENT, 1, 0 };
        sbtn_t desk = { x + w / 2 + SSC(10), byy, bw, bh, "На рабочий стол",
                        C_LIME, 1, 0 };
        int hov = btn_hit(&reb, mx, my) ? 0 : btn_hit(&desk, mx, my) ? 1 : -1;
        if (hov != hov_old) { hov_old = hov; need = 1; }
        if (reboot_arm != arm_old) { arm_old = reboot_arm; need = 1; }
        if (click && btn_hit(&desk, mx, my)) return 0;
        if (click && btn_hit(&reb, mx, my)) {
            if (!reboot_arm) { reboot_arm = 1; need = 1; }
            else {
                kprintf("[session] перезагрузка после установки\n");
                efi_reset_cold();          /* не возвращается */
            }
        }
        if (need) {
            wiz_frame(&x, &y, &w, &h, "Установка завершена", 4, 4);
            int32_t d = SSC(64);
            gfx_fill_round_rect(x + w / 2 - d / 2, y + SSC(84), d, d, d / 2, C_LIME);
            STBC(x + w / 2, y + SSC(84) + d / 2 - MAG() * 8 / 10, "OK", C_INK, MAG() * 2);
            char line[128]; char *p = line;
            pcat(&p, "AresOS установлена, ");
            pcat(&p, name);
            *p = 0;
            STBC(x + w / 2, y + SSC(168), line, C_TXT, MAG() * 2);
            STC(x + w / 2, y + SSC(168) + MAG() * 20 / 10 + SSC(10),
                "Учётная запись сохранена в NVRAM - после перезагрузки", C_TXT2, MAG());
            STC(x + w / 2, y + SSC(168) + MAG() * 20 / 10 + SSC(10) + MAG() * 8 / 10 + SSC(6),
                "вас встретят экран блокировки и вход.", C_TXT2, MAG());
            STC(x + w / 2, y + h - SSC(100),
                "Рекомендуем перезагрузить - как настоящая ОС после установки!",
                C_ACCENT, MAG());
            btn_paint(&reb, hov == 0);
            btn_paint(&desk, hov == 1);
            desktop_present_full();
            need = 0;
        } else if (moved) desktop_present_cursor();
        cpu_wait();
    }
}

/* запись учётки: NVRAM + ramfs-паспорт */
static void account_commit(const char *name, const char *pass_or_null) {
    int ok1 = efi_var_set_u32("AresInstalled", 1);
    int ok2 = efi_var_set_str("AresUser", name);
    int ok3 = 1;
    if (pass_or_null && pass_or_null[0]) {
        char hh[17];
        hash_hex(pass_or_null, hh);
        ok3 = efi_var_set_str("AresHash", hh);
    } else {
        efi_var_delete("AresHash");
    }
    kprintf("[session] учётка '%s' -> NVRAM: installed=%d user=%d pass=%d\n",
            name, ok1, ok2, ok3);
    /* паспорт в ramfs (до рестарта; долговременная память учётки - NVRAM) */
    int d = vfs_find(VFS_ROOT, "Система");
    if (d != VFS_NONE) {
        int f = vfs_find(d, "accounts.txt");
        if (f == VFS_NONE) f = vfs_create(d, "accounts.txt");
        if (f != VFS_NONE) {
            char acc[160]; char *p = acc;
            pcat(&p, "user=");
            pcat(&p, name);
            pcat(&p, "\npassword=");
            if (pass_or_null && pass_or_null[0]) {
                char hh[17]; hash_hex(pass_or_null, hh);
                pcat(&p, "hash:"); pcat(&p, hh);
            } else pcat(&p, "none");
            *p++ = '\n'; *p = 0;
            vfs_write(f, acc, (uint32_t)strlen(acc));
        }
    }
    int i;
    for (i = 0; i < 47 && name[i]; i++) g_user[i] = name[i];
    g_user[i] = 0;
    g_has_user = 1;
}

static int install_flow(void) {  /* 0 = на стол, 2 = назад в меню */
    wiz_progress();
    field_t name, pass;
    int r = wiz_input_page("Ваша учётная запись",
                           "Как вас назвать? Это имя будет на экране входа",
                           "и в правом углу панели рабочего стола.",
                           &name, 12, 0, 0, 2);
    if (r == PG_BACK) return 2;
    r = wiz_input_page("Пароль (можно без него)",
                       "Придумайте пароль для входа - он понадобится после",
                       "перезагрузки. Не хотите пароль? Жмите [Пропустить].",
                       &pass, 30, 1, 1, 3);
    if (r == PG_BACK) return 2;
    account_commit(name.buf, r == PG_NEXT ? pass.buf : 0);
    return wiz_done_page(name.buf) == 0 ? 0 : 2;
}

/* =========================================================================
 *  ОБНОВЛЕНИЕ (честное: сети нет до M7)
 * ========================================================================= */
static void update_page(void) {
    kb_drain();
    uint64_t t0 = sched_ticks();
    int done = 0, left_old = mouse_left(), need = 1, hov_old = -2, moved;
    uint64_t bar_old = 0;
    kprintf("[session] центр обновления: проверяю...\n");
    for (;;) {
        moved = mouse_moved();
        int k;
        while ((k = keyboard_getch()) >= 0)
            if (k == 27 && done) return;         /* Esc = назад, когда готово */
        int32_t mx = mouse_x(), my = mouse_y();
        int click = mouse_left() && !left_old;
        left_old = mouse_left();
        if (!done && sched_ticks() - t0 >= 1400) { done = 1; need = 1; }
        uint64_t bar = sched_ticks() - t0; if (bar > 1400) bar = 1400;
        if (!done && bar / 40 != bar_old) { bar_old = bar / 40; need = 1; }
        int32_t W = (int32_t)gfx_width(), H = (int32_t)gfx_height();
        int32_t w = SSC(620), h = SSC(440);
        if (w > W - 40) w = W - 40;
        if (h > H - 40) h = H - 40;
        int32_t x = (W - w) / 2, y = (H - h) / 2;
        sbtn_t back = { x + SSC(24), y + h - SSC(58), SSC(130), SSC(34),
                        "Назад", C_ACCENT, 0, 0 };
        int hov = done && btn_hit(&back, mx, my);
        if (hov != hov_old) { hov_old = hov; if (done) need = 1; }
        if (click && done && btn_hit(&back, mx, my)) return;
        if (need) {
            int32_t x2, y2, w2, h2;
            wiz_frame(&x2, &y2, &w2, &h2, "Обновление системы", 0, 0);
            int32_t cx = x2 + SSC(40);
            int32_t yy = y2 + SSC(100);
            if (!done) {
                gfx_fill_round_rect(cx, yy, w2 - SSC(80), SSC(20), SSC(8), C_BAR_BG);
                int32_t fw = (w2 - SSC(80)) * (int32_t)bar / 1400;
                if (fw > SSC(10)) gfx_fill_round_rect(cx, yy, fw, SSC(20), SSC(8), C_CYAN);
                yy += SSC(40);
                STM(cx, yy, "Проверяю наличие новых версий...", C_TXT2, MAG());
            } else {
                STB(cx, yy, "Обновлений нет - v0.8.0 это самая свежая!", C_GREEN, MAG());
                yy += MAG() * 8 / 10 + SSC(10);
                STM(cx, yy, "Честно: сетевого стека в ядре пока нет (этап M7),", C_ACCENT, MAG());
                yy += MAG() * 8 / 10 + SSC(7);
                STM(cx, yy, "поэтому реальное обновление = новый ISO в VM.", C_ACCENT, MAG());
                yy += MAG() * 8 / 10 + SSC(14);
                STM(cx, yy, "Нажми [Назад] или Esc.", C_TXT2, MAG());
            }
            if (done) btn_paint(&back, hov);
            desktop_present_full();
            need = 0;
        } else if (moved) desktop_present_cursor();
        cpu_wait();
    }
}

/* =========================================================================
 *  ПОИСК НЕИСПРАВНОСТЕЙ (диагностика) - по фото этого экрана мы и чиним ОС
 * ========================================================================= */
static void diag_page(uint64_t total_mib, uint64_t free_mib) {
    kb_drain();
    int nv_ok = -1;
    if (efi_rt_ok()) {
        if (efi_var_set_u32("AresDiag", 0xA7E5u)) {
            uint32_t v = 0;
            nv_ok = (efi_var_get_u32("AresDiag", &v) && v == 0xA7E5u);
            efi_var_delete("AresDiag");
        } else nv_ok = 0;
        kprintf("[session] диагностика: NVRAM-тест %s\n", nv_ok == 1 ? "OK" : "ОШИБКА");
    }
    uint32_t keys = 0;
    uint32_t mirq0 = mouse_irq_count();
    int left_old = mouse_left();
    uint64_t redraw_t = 0;
    for (;;) {
        int k;
        while ((k = keyboard_getch()) >= 0) {
            keys++;
            if (k == 27) return;                 /* Esc = назад */
        }
        int32_t mx = mouse_x(), my = mouse_y();
        int click = mouse_left() && !left_old;
        left_old = mouse_left();
        if (sched_ticks() - redraw_t < 450 && !click) {
            if (mouse_moved()) desktop_present_cursor();
            cpu_wait();
            continue;
        }
        redraw_t = sched_ticks();

        int32_t W = (int32_t)gfx_width(), H = (int32_t)gfx_height();
        int32_t cw = SSC(760), ch = SSC(560);
        if (cw > W - 24) cw = W - 24;
        if (ch > H - 24) ch = H - 24;
        int32_t x = (W - cw) / 2, y = (H - ch) / 2;
        bg_dim(150);
        card_draw(x, y, cw, ch);
        STBC(W / 2, y + SSC(18), "Поиск неисправностей", C_TXT, MAG() * 2);
        STC(W / 2, y + SSC(18) + MAG() * 16 / 10 + SSC(6),
            "Если что-то глючит - сфотографируй этот экран целиком", C_TXT2, MAG());
        int32_t cx = x + SSC(28);
        int32_t yy = y + SSC(66);
        char row[128]; char *p = row;

        rtc_time_t rt; rtc_read(&rt);
        p = row; pcat(&p, "Время RTC: ");
        { char b[9];
          b[0] = (char)('0' + rt.hour / 10); b[1] = (char)('0' + rt.hour % 10);
          b[2] = ':'; b[3] = (char)('0' + rt.min / 10); b[4] = (char)('0' + rt.min % 10);
          b[5] = ':'; b[6] = (char)('0' + rt.sec / 10); b[7] = (char)('0' + rt.sec % 10);
          b[8] = 0;
          pcat(&p, b); }
        pcat(&p, "   Дата: ");
        { char n1[8], n2[8], n3[8];
          u32dec(rt.day, n1); u32dec(rt.mon, n2); u32dec(rt.year, n3);
          pcat(&p, n1); pcat(&p, "."); pcat(&p, n2); pcat(&p, "."); pcat(&p, n3); }
        STM(cx, yy, row, C_TXT2, MAG()); yy += MAG() * 8 / 10 + SSC(7);

        { char n1[12], n2[12], n3[12]; p = row;
          u32dec((uint32_t)gfx_width(), n1); u32dec((uint32_t)gfx_height(), n2);
          u32dec((uint32_t)desktop_scale(), n3);
          pcat(&p, "Видео: "); pcat(&p, n1); pcat(&p, "x"); pcat(&p, n2);
          pcat(&p, "   Масштаб: "); pcat(&p, n3); pcat(&p, "%"); }
        STM(cx, yy, row, C_TXT2, MAG()); yy += MAG() * 8 / 10 + SSC(7);

        { char n1[12], n2[12]; p = row;
          u32dec((uint32_t)total_mib, n1); u32dec((uint32_t)free_mib, n2);
          pcat(&p, "Память: всего "); pcat(&p, n1); pcat(&p, " МиБ, свободно ");
          pcat(&p, n2); pcat(&p, " МиБ"); }
        STM(cx, yy, row, C_TXT2, MAG()); yy += MAG() * 8 / 10 + SSC(7);

        p = row; pcat(&p, "NVRAM (UEFI): ");
        pcat(&p, nv_ok == 1 ? "запись/чтение ОК" :
                 nv_ok == 0 ? "ОШИБКА записи!" : "RT-сервисы недоступны");
        STM(cx, yy, row, nv_ok == 1 ? C_GREEN : C_RED, MAG()); yy += MAG() * 8 / 10 + SSC(7);

        { char n1[12], n2[12]; p = row;
          u32dec(keys, n1); u32dec(mouse_irq_count() - mirq0, n2);
          pcat(&p, "Клавиш нажато: "); pcat(&p, n1);
          pcat(&p, "   Пакетов мыши: "); pcat(&p, n2);
          pcat(&p, "  (подвигай мышь!)"); }
        STM(cx, yy, row, C_CYAN, MAG()); yy += MAG() * 8 / 10 + SSC(7);

        { char n1[12]; p = row;
          u32dec((uint32_t)(sched_ticks() / 1000), n1);
          pcat(&p, "Аптайм: "); pcat(&p, n1); pcat(&p, " сек   Таймер: 1000 Гц"); }
        STM(cx, yy, row, C_TXT2, MAG()); yy += MAG() * 8 / 10 + SSC(12);

        STB(cx, yy, "Последние строки журнала ядра:", C_TXT, MAG());
        yy += MAG() * 8 / 10 + SSC(8);
        int ln = logbuf_count();
        int first = ln - 8; if (first < 0) first = 0;
        gfx_fill_round_rect(cx - SSC(4), yy - SSC(2), cw - SSC(52),
                            (MAG() * 8 / 10 + SSC(4)) * 8 + SSC(6), SSC(6), C_FIELD);
        for (int i = first; i < ln; i++) {
            const char *t = logbuf_line(i);
            char crop[100];
            int ci = 0, gl = 0;
            for (; t[ci] && gl < 92 && ci < 96; ci++) { crop[ci] = t[ci];
                if (((uint8_t)t[ci] & 0xC0) != 0x80) gl++; }
            crop[ci] = 0;
            STM(cx, yy, crop, C_TXT2, MAG());
            yy += MAG() * 8 / 10 + SSC(4);
        }
        sbtn_t back = { x + SSC(24), y + ch - SSC(50), SSC(130), SSC(34),
                        "Назад", C_ACCENT, 0, 0 };
        if (click && btn_hit(&back, mx, my)) return;
        btn_paint(&back, btn_hit(&back, mx, my));
        STC(W / 2, y + ch - SSC(50) + SSC(40), "Esc - тоже назад", C_TXT2, MAG());
        desktop_present_full();
    }
}

/* =========================================================================
 *  ЭКРАН БЛОКИРОВКИ
 * ========================================================================= */
static const char *WDAY_RU[7] = {
    "воскресенье", "понедельник", "вторник", "среда",
    "четверг", "пятница", "суббота"
};
static const char *MON_RU[12] = {
    "января", "февраля", "марта", "апреля", "мая", "июня",
    "июля", "августа", "сентября", "октября", "ноября", "декабря"
};

static void lock_draw(int dim_a) {
    int32_t W = (int32_t)gfx_width(), H = (int32_t)gfx_height();
    bg_dim((uint8_t)dim_a);
    rtc_time_t rt; rtc_read(&rt);
    char clk[8];
    clk[0] = (char)('0' + rt.hour / 10); clk[1] = (char)('0' + rt.hour % 10);
    clk[2] = ':';
    clk[3] = (char)('0' + rt.min / 10); clk[4] = (char)('0' + rt.min % 10);
    clk[5] = 0;
    int bm = MAG() * 9;
    STBC(W / 2, H * 30 / 100, clk, C_TXT, bm);
    char date[64]; char *p = date;
    pcat(&p, WDAY_RU[rt.wday]); pcat(&p, ", ");
    char nb[8]; u32dec(rt.day, nb); pcat(&p, nb);
    *p++ = ' '; pcat(&p, MON_RU[rt.mon - 1]); *p = 0;
    STC(W / 2, H * 30 / 100 + bm * 8 / 10 + SSC(14), date, C_TXT2, MAG() * 2);
    const char *hint = "Нажмите любую клавишу, щёлкните мышью или потяните вверх";
    int hm = MAG();
    int32_t hw = twn(hint, hm) + SSC(40);
    int32_t hx = W / 2 - hw / 2, hy = H - SSC(120);
    gfx_blend_round_rect(hx, hy, hw, SSC(30), SSC(15), C_CARD, 215);
    STC(W / 2, hy + SSC(15) - hm * 8 / 20, hint, C_TXT, hm);
    STC(W / 2, H - SSC(56), "F8 - установка и восстановление системы", C_TXT2, MAG());
    if (session_user_present()) {
        char wl[80]; p = wl;
        pcat(&p, "Привет, "); pcat(&p, session_user_name()); *p = 0;
        STC(W / 2, H * 30 / 100 - MAG() * 28 / 10 - SSC(6), wl, C_CYAN, MAG() * 2);
    }
}

static int lock_screen(void) {   /* 0 = открыть, 1 = F8 (в меню) */
    kb_drain();
    int need = 1;
    int8_t last_min = -1;
    int press = 0;
    int32_t press_y = 0;
    for (;;) {
        int k;
        while ((k = keyboard_getch()) >= 0) {
            if (k == KEY_F8) return 1;
            goto unlocked;                        /* любая клавиша открывает */
        }
        {
            int l = mouse_left();
            if (l && !press) { press = 1; press_y = mouse_y(); }
            if (l && press && press_y - mouse_y() >= SSC(110)) goto unlocked;
            if (!l && press) {
                if (press_y - mouse_y() < SSC(8)) goto unlocked;  /* простой клик */
                press = 0;
                need = 1;
            }
        }
        {
            rtc_time_t rt; rtc_read(&rt);
            if ((int8_t)rt.min != last_min) { last_min = (int8_t)rt.min; need = 1; }
        }
        if (need) { lock_draw(70); desktop_present_full(); need = 0; }
        else if (mouse_moved()) desktop_present_cursor();
        cpu_wait();
    }
unlocked:
    /* микро-анимация: затемняем 3 кадра - экран "уезжает" */
    for (int a = 70; a <= 150; a += 40) {
        lock_draw(a);
        desktop_present_full();
        uint64_t t0 = sched_ticks();
        while (sched_ticks() - t0 < 35) cpu_wait();
    }
    return 0;
}

/* =========================================================================
 *  ЭКРАН ВХОДА
 * ========================================================================= */
static int login_screen(void) {  /* 0 = вошли, 1 = F8 */
    char hash[40];
    int haspass = efi_var_get_str("AresHash", hash, sizeof hash) && hash[0];
    field_t f = { "", 0 };
    int error = 0;
    int left_old = mouse_left();
    int need = 1, hov_old = -2, care_old = -1, err_old = -1, moved;
    kb_drain();
    kprintf("[session] экран входа: пользователь '%s', пароль %s\n",
            g_user, haspass ? "есть" : "не задан");
    for (;;) {
        moved = mouse_moved();
        int k;
        while ((k = keyboard_getch()) >= 0) {
            if (k == KEY_F8) return 1;
            if (haspass && k == 13) goto try_login;
            if (field_key(&f, k, 30)) { error = 0; need = 1; }
        }
        {
            int32_t mx = mouse_x(), my = mouse_y();
            int click = mouse_left() && !left_old;
            left_old = mouse_left();
            int32_t W = (int32_t)gfx_width(), H = (int32_t)gfx_height();
            int32_t cw = SSC(420), ch = haspass ? SSC(430) : SSC(360);
            if (cw > W - 40) cw = W - 40;
            if (ch > H - 40) ch = H - 40;
            int32_t x = (W - cw) / 2, y = (H - ch) / 2;
            sbtn_t login = { x + cw / 2 - SSC(80), y + ch - SSC(64),
                             SSC(160), SSC(36), "Войти", C_LIME, 1, 0 };
            int hov = btn_hit(&login, mx, my);
            if (hov != hov_old) { hov_old = hov; need = 1; }
            if (error != err_old) { err_old = error; need = 1; }
            {
                int care = haspass ? (int)((sched_ticks() / 500) & 1) : 0;
                if (care != care_old) { care_old = care; if (haspass) need = 1; }
            }
            if (click && btn_hit(&login, mx, my)) {
                if (!haspass) { g_has_user = 1; goto success; }
                goto try_login;
            }
            if (need) {
                bg_dim(95);
                card_draw(x, y, cw, ch);
                int32_t d = SSC(84);
                gfx_fill_round_rect(x + cw / 2 - d / 2, y + SSC(34), d, d, d / 2, C_BLUE);
                char ini[8];
                int nb = 1;
                while (nb < 4 && (g_user[nb] & 0xC0) == 0x80) nb++;  /* UTF-8 глиф */
                for (int i = 0; i < nb; i++) ini[i] = g_user[i];
                ini[nb] = 0;
                STBC(x + cw / 2, y + SSC(34) + d / 2 - MAG() * 20 / 10, ini, C_TXT, MAG() * 5);
                STBC(x + cw / 2, y + SSC(34) + d + SSC(16), g_user, C_TXT, MAG() * 2);
                if (haspass) {
                    int32_t fy = y + SSC(34) + d + SSC(56);
                    field_paint(x + SSC(40), fy, cw - SSC(80), SSC(40), &f, 1, care_old);
                    STC(x + cw / 2, fy + SSC(48),
                        error ? "Неверный пароль - попробуйте ещё раз"
                              : "Введите пароль и нажмите Enter",
                        error ? C_RED : C_TXT2, MAG());
                } else {
                    STC(x + cw / 2, y + SSC(34) + d + SSC(60),
                        "Пароль не задан - просто войдите", C_TXT2, MAG());
                }
                btn_paint(&login, hov);
                STC(x + cw / 2, y + ch - SSC(16) - MAG() * 8 / 10,
                    "F8 - установка и восстановление", C_TXT2, MAG());
                desktop_present_full();
                need = 0;
            } else if (moved) desktop_present_cursor();
        }
        cpu_wait();
        continue;

try_login:
        {
            char hh[17];
            hash_hex(f.buf, hh);
            int ok2 = 1;
            for (int i = 0; i < 17; i++)
                if (hh[i] != hash[i]) { ok2 = 0; break; }
            if (ok2) { g_has_user = 1; goto success; }
            error = 1;
            need = 1;
            f.len = 0; f.buf[0] = 0;
            kprintf("[session] неверный пароль\n");
            kb_drain();
        }
    }
success:
    kprintf("[session] вход выполнен: '%s'\n", g_user);
    return 0;
}

/* =========================================================================
 *  КОРНЕВОЙ СЦЕНАРИЙ
 * ========================================================================= */
static void menu_flow(uint64_t total_mib, uint64_t free_mib) {
    for (;;) {
        int c = boot_menu();
        if (c == 0) {
            int r = install_flow();
            if (r == 0) return;                /* установлена -> сразу на стол */
        } else if (c == 1) update_page();
        else if (c == 2) diag_page(total_mib, free_mib);
        else return;                           /* Live-режим */
    }
}

void session_run(const bootinfo_t *bi, uint64_t total_mib, uint64_t free_mib) {
    desktop_boot_graphics(bi);     /* графика + NVRAM-настройки + буферы */
    fb_console_detach();           /* дальше экран целиком наш */
    kprintf("[session] загрузочный экран: %lux%lu, масштаб %lu%%, NVRAM %s\n",
            (uint64_t)gfx_width(), (uint64_t)gfx_height(), (uint64_t)desktop_scale(),
            efi_rt_ok() ? "доступен" : "НЕДОСТУПЕН");

    uint32_t inst = 0;
    int installed = efi_rt_ok() && efi_var_get_u32("AresInstalled", &inst) && inst == 1;

    if (installed) {
        if (!efi_var_get_str("AresUser", g_user, sizeof g_user) || !g_user[0]) {
            const char *dflt = "Пользователь";
            int i; for (i = 0; i < 12 && dflt[i]; i++) g_user[i] = dflt[i];
            g_user[i] = 0;
        }
        g_has_user = 1;            /* имя известно уже на экране блокировки */
        splash(2400, "Загрузка AresOS...");
        for (;;) {
            int r = lock_screen();
            if (r == 1) { menu_flow(total_mib, free_mib); return; }
            r = login_screen();
            if (r == 1) { menu_flow(total_mib, free_mib); return; }
            break;                 /* вошли! */
        }
    } else {
        g_has_user = 0;
        splash(1600, "Подготовка к работе...");
        menu_flow(total_mib, free_mib);
        /* если установили прямо сейчас - имя уже в g_user (account_commit) */
    }
    kprintf("[session] готово: на рабочий стол (пользователь '%s', %s)\n",
            g_has_user ? g_user : "-", g_has_user ? "учётка" : "Live");
}
