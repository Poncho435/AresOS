/* AresOS - PE32+ loader (M3-пробник -> M8 WinAPI-слой).
 * Разбирает PE/COFF по спецификации: MZ, PE, COFF, Optional Header PE32+,
 * секции, каталог импортов (пока отклоняем), каталог базовых релоков (DIR64),
 * маппинг страниц через VMM с правами из IMAGE_SCN_*, вызов точки входа. */
#ifndef ARES_PE_H
#define ARES_PE_H

#include <stdint.h>
#include "bootinfo.h"

/* контекст, который ядро передаёт тестовым приложениям (prototype ABI: SysV) */
typedef struct {
    uint64_t fb_base;
    uint32_t width, height, pitch, format;
    uint32_t magic;      /* 'ARES' = 0x41524553 */
    uint32_t draw_x, draw_y;
} ares_api_t;

#define ARES_PE_MAGIC   0x41524553u
#define ARES_PE_TEST_OK 0x0000A2E5

void pe_demo_init(const bootinfo_fb_t *fb);
int  pe_demo_run(void);   /* ret приложения или отрицательный код ошибки lo-адера */

/* низкоуровневый API (пригодится M6+ при загрузке из файла) */
int  pe_check(const uint8_t *file, uint64_t size);        /* 0 = ок, иначе -код */

#endif
