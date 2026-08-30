/* AresOS - клавиатура PS/2 (M4). Сканкоды set 1 -> ASCII (EN раскладка,
 * Shift/CapsLock/NumPad-стрелки E0). Ring-буфер на 64 события. */
#include "keyboard.h"
#include "io.h"
#include "kprintf.h"
#include <stdint.h>

#define PS2_DATA 0x60

static const char SC_LO[128] = {
    [0x01] = 27,  [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5',
    [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0', [0x0C]='-',
    [0x0D]='=', [0x0E]=8,   [0x0F]=9,   [0x10]='q', [0x11]='w', [0x12]='e',
    [0x13]='r', [0x14]='t', [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o',
    [0x19]='p', [0x1A]='[', [0x1B]=']', [0x1C]=13,  [0x1E]='a', [0x1F]='s',
    [0x20]='d', [0x21]='f', [0x22]='g', [0x23]='h', [0x24]='j', [0x25]='k',
    [0x26]='l', [0x27]=';', [0x28]='\'',[0x29]='`', [0x2B]='\\',[0x2C]='z',
    [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b', [0x31]='n', [0x32]='m',
    [0x33]=',', [0x34]='.', [0x35]='/', [0x39]=' ',
};
static const char SC_HI[128] = {
    [0x01] = 27,  [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
    [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')', [0x0C]='_',
    [0x0D]='+', [0x0E]=8,   [0x0F]=9,   [0x10]='Q', [0x11]='W', [0x12]='E',
    [0x13]='R', [0x14]='T', [0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O',
    [0x19]='P', [0x1A]='{', [0x1B]='}', [0x1C]=13,  [0x1E]='A', [0x1F]='S',
    [0x20]='D', [0x21]='F', [0x22]='G', [0x23]='H', [0x24]='J', [0x25]='K',
    [0x26]='L', [0x27]=':', [0x28]='"', [0x29]='~', [0x2B]='|', [0x2C]='Z',
    [0x2D]='X', [0x2E]='C', [0x2F]='V', [0x30]='B', [0x31]='N', [0x32]='M',
    [0x33]='<', [0x34]='>', [0x35]='?', [0x39]=' ',
};

/* v0.7.0: русская раскладка "йцукен" (Alt+Shift = переключить EN<->RU).
 * Таблица = номер буквы 0..31 (а=0..я=31) и 32 = ё; 0 - клавиша не буква. */
static const uint8_t SC_RU[128] = {
    [0x10]=9,  [0x11]=22, [0x12]=19, [0x13]=10, [0x14]=5,  [0x15]=13,
    [0x16]=3,  [0x17]=24, [0x18]=25, [0x19]=7,  [0x1A]=21, [0x1B]=26,
    [0x1E]=20, [0x1F]=27, [0x20]=2,  [0x21]=0,  [0x22]=15, [0x23]=16,
    [0x24]=14, [0x25]=11, [0x26]=4,  [0x27]=6,  [0x28]=29,
    [0x29]=32,
    [0x2C]=31, [0x2D]=23, [0x2E]=17, [0x2F]=12, [0x30]=8,  [0x31]=18,
    [0x32]=28, [0x33]=1,  [0x34]=30,
};

static volatile uint16_t g_buf[64];
static volatile uint8_t  g_head, g_tail;
static int g_shift, g_caps, g_e0, g_ctrl, g_alt;
static int g_ru;

static void push(uint16_t code) {
    uint8_t n = (uint8_t)((g_head + 1) & 63);
    if (n != g_tail) { g_buf[g_head] = code; g_head = n; }
}

void keyboard_init(void) {
    g_head = g_tail = 0; g_shift = g_caps = 0; g_e0 = 0;
    kprintf("[kbd] PS/2 keyboard: IRQ1, set-1 -> ASCII ring[64]\n");
}

void keyboard_irq_handler(void) {
    uint8_t sc = inb(PS2_DATA);
    if (sc == 0xE0) { g_e0 = 1; return; }

    int released = sc & 0x80;
    uint8_t code = sc & 0x7F;

    if (g_e0) {
        g_e0 = 0;
        if (code == 0x1D) { g_ctrl = !released; return; }   /* правый Ctrl */
        if (code == 0x38) { g_alt  = !released; return; }   /* правый Alt */
        if (!released) {
            int kc = 0;
            switch (code) {
            case 0x48: kc = g_ctrl ? KEY_MUP    : KEY_UP;    break;
            case 0x50: kc = g_ctrl ? KEY_MDOWN  : KEY_DOWN;  break;
            case 0x4B: kc = g_ctrl ? KEY_MLEFT  : KEY_LEFT;  break;
            case 0x4D: kc = g_ctrl ? KEY_MRIGHT : KEY_RIGHT; break;
            case 0x49: kc = KEY_PGUP; break;
            case 0x51: kc = KEY_PGDN; break;
            default: break;
            }
            if (kc) push((uint16_t)kc);
        }
        return;
    }

    switch (code) {
    case 0x2A: case 0x36:
        if (!released && g_alt) g_ru = !g_ru;    /* Alt+Shift: раскладка EN<->RU */
        g_shift = !released;
        return;
    case 0x3A: if (!released) g_caps = !g_caps; return;
    case 0x1D: g_ctrl = !released; return;               /* левый Ctrl */
    case 0x38: g_alt  = !released; return;               /* левый Alt */
    }
    if (released) return;

    /* --- русская буква: кладём ДВА байта UTF-8 в очередь --- */
    if (g_ru) {
        uint8_t idx = SC_RU[code];
        if (idx) {
            int upper = (g_caps != g_shift);
            uint8_t b1, b2;
            if (idx == 32) { b1 = 0xD0 + (upper ? 0 : 1); b2 = 0x81 + (upper ? 0 : 0x10); /* Ёё */ }
            else if (idx < 16)  /* а..п / А..П */
                { b1 = 0xD0; b2 = (uint8_t)(0xB0 + idx - (upper ? 0x20 : 0)); }
            else                /* р..я / Р..Я */
                { b1 = upper ? 0xD0 : 0xD1; b2 = (uint8_t)((upper ? 0xA0 : 0x80) + (idx - 16)); }
            push((uint16_t)b1); push((uint16_t)b2);
            return;
        }
        /* не буква: падаем в EN-таблицы как обычно */
    }

    /* F1..F10: 0x3B..0x44; Alt+F4 - отдельный код (закрыть верхнее окно) */
    if (code >= 0x3B && code <= 0x44) {
        if (code == 0x3E && g_alt) push(KEY_ALTF4);
        else push((uint16_t)(KEY_F1 + (code - 0x3B)));
        return;
    }

    int shift = g_shift;
    char c = shift ? SC_HI[code] : SC_LO[code];
    if (c >= 'a' && c <= 'z' && g_caps != shift)
        c = (char)(c - 32);
    else if (c >= 'A' && c <= 'Z' && g_caps != shift)
        c = (char)(c + 32);
    if (c) push((uint16_t)(uint8_t)c);
}

int keyboard_getch(void) {
    if (g_tail == g_head) return -1;
    int c = g_buf[g_tail];
    g_tail = (uint8_t)((g_tail + 1) & 63);
    return c;
}

int keyboard_ru(void) { return g_ru; }   /* v0.7.0: индикатор раскладки RU/EN */
