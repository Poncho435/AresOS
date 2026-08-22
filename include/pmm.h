/* AresOS — PMM: менеджер физических страниц 4 КиБ (bitmap) */
#ifndef ARES_PMM_H
#define ARES_PMM_H

#include <stdint.h>
#include "bootinfo.h"

#define PMM_PAGE_SIZE 4096ULL

void     pmm_init(const bootinfo_t *bi);
uint64_t pmm_alloc_page(void);          /* физ. адрес или 0 при нехватке */
void     pmm_free_page(uint64_t phys);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);

#endif
