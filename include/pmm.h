/* AresOS - PMM: менеджер физических страниц 4 КиБ (bitmap) */
#ifndef ARES_PMM_H
#define ARES_PMM_H

#include <stdint.h>
#include "bootinfo.h"

#define PMM_PAGE_SIZE 4096ULL
#define PMM_IDENTITY_TOP 0x100000000ULL   /* 4 ГиБ - предел identity-карты VMM */

void     pmm_init(const bootinfo_t *bi);
uint64_t pmm_alloc_page(void);          /* физ. адрес или 0 при нехватке */
uint64_t pmm_alloc_page_low(void);      /* то же, но строго < 4 ГиБ (таблицы страниц) */
void     pmm_free_page(uint64_t phys);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);

#endif
