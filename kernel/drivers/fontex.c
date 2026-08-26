/* AresOS - fontex: UTF-8 -> слот глифа 8x8.
 * Кириллица в UTF-8: А..п = D0 90 .. D0 BF (U+0410..043F),
 *                    р..я = D1 80 .. D1 8F (U+0440..044F),
 *                    Ё = D0 81 (U+0401),  ё = D1 91 (U+0451).
 * Любой другой многобайтный символ проглатывается целиком и печатается
 * как ОДИН '?'; одиночные continuation-байты (мусор) молча пропускаются. */
#include "fontex.h"
#include "../drivers/font8x8.h"
#include "../drivers/font8x8_cyr.h"

int fontex_slot(int *state, uint8_t b) {
    int st = *state;
    if (st == 0) {
        if (b < 0x80) return b;                             /* ASCII */
        if (b == 0xD0 || b == 0xD1) { *state = b; return -1; }
        if (b >= 0xC2 && b <= 0xDF) { *state = -1; return -1; }  /* ещё 1 байт */
        if (b >= 0xE0 && b <= 0xEF) { *state = -2; return -1; }  /* ещё 2 байта */
        if (b >= 0xF0 && b <= 0xF4) { *state = -3; return -1; }  /* ещё 3 байта */
        return -1;                            /* мусорный одиночный байт - молчим */
    }
    if (st < 0) {                             /* глотаем "чужой" символ */
        if (b >= 0x80 && b <= 0xBF) {
            *state = st + 1;
            return (st + 1 == 0) ? '?' : -1;  /* доели целиком -> один '?' */
        }
        *state = 0;                           /* последовательность оборвалась */
        return fontex_slot(state, b);         /* байт - начало нового символа */
    }
    /* st == 0xD0 / 0xD1 - второй байт кириллицы */
    *state = 0;
    if (st == 0xD0) {
        if (b >= 0x90 && b <= 0xBF) return 128 + (b - 0x90);  /* А..Я, а..п -> 128..175 */
        if (b == 0x81) return 128 + 64;                       /* Ё */
    } else {                                  /* 0xD1 */
        if (b >= 0x80 && b <= 0x8F) return 176 + (b - 0x80);  /* р..я -> 176..191 */
        if (b == 0x91) return 128 + 65;                       /* ё */
    }
    return '?';
}

const uint8_t *fontex_glyph(int slot) {
    if (slot < 0) slot = 0;
    if (slot < 128) return font8x8_basic[slot];
    slot -= 128;
    if (slot > 65) slot = 65;
    return font8x8_cyr[slot];
}
