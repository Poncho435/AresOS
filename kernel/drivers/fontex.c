/* AresOS — fontex: UTF-8 → слот глифа 8x8.
 * Кириллица в UTF-8: А..я = D0 90 .. D0 BF (U+0410..044F),
 *                    Ё = D0 81 (U+0401),  ё = D1 91 (U+0451). */
#include "fontex.h"
#include "../drivers/font8x8.h"
#include "../drivers/font8x8_cyr.h"

int fontex_slot(int *state, uint8_t b) {
    int hi = *state;
    if (!hi) {
        if (b < 0x80) return b;                 /* ASCII */
        if (b == 0xD0 || b == 0xD1) { *state = b; return -1; }
        return '?';                             /* прочий мусор — вопросик */
    }
    *state = 0;
    if (hi == 0xD0) {
        if (b >= 0x90 && b <= 0xBF) return 128 + (b - 0x90);  /* А..я → 128..191 */
        if (b == 0x81) return 128 + 64;                       /* Ё */
    } else {                                  /* 0xD1 */
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
