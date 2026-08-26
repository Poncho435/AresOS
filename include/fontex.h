/* AresOS — fontex: доступ к глифам 8x8 с декодированием UTF-8 (кириллица).
 * Слоты: 0..127 = ASCII font8x8_basic; 128..193 = font8x8_cyr (А..я, Ёё). */
#ifndef ARES_FONTEX_H
#define ARES_FONTEX_H

#include <stdint.h>

/* Конечный автомат UTF-8. *state — старший байт (0xD0/0xD1) или 0.
 * Возвращает слот глифа ≥0, либо -1 «жду второй байт». */
int fontex_slot(int *state, uint8_t b);

/* 8 байт изображения глифа по слоту (slot приводится к диапазону) */
const uint8_t *fontex_glyph(int slot);

#endif
