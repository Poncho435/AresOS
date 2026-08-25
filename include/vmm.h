/* AresOS — VMM: свои таблицы страниц (M3).
 * • identity-маппинг 0..4 ГиБ (ядро пока low-half)
 * • HHDM: вся физпамять продублирована на 0xFFFF800000000000
 * • 2-МиБ страницы везде, где можно; зона ядра — постранично 4 КиБ:
 *   .text=RX, .rodata=R, остальное=RW+NX; страница 0 НЕ замаплена (null-guard)
 * • EFER.NXE=1, CR0.WP=1 (RO реально защищает даже в ring-0) */
#ifndef ARES_VMM_H
#define ARES_VMM_H

#include <stdint.h>
#include "bootinfo.h"

#define VMM_HHDM_BASE 0xFFFF800000000000ULL
#define HHDM(phys)  ((void *)(uintptr_t)(VMM_HHDM_BASE + (uint64_t)(phys)))

#define VMM_P  (1ULL << 0)
#define VMM_W  (1ULL << 1)
#define VMM_U  (1ULL << 2)
#define VMM_NX (1ULL << 63)

void vmm_init(const bootinfo_t *bi);

/* замапить одну страницу 4 КиБ: таблицы достраиваются через PMM.
 * Поддерживает любые адреса (в т.ч. вне готовых 2-МиБ зон — для модулей). */
void vmm_map_4k(uint64_t virt, uint64_t phys, uint64_t flags);

/* убрать страницу из таблиц (invlpg включён) */
void vmm_unmap_4k(uint64_t virt);

/* поменять флаги УЖЕ замапленной страницы (физ. адрес сохраняется) */
void vmm_protect_4k(uint64_t virt, uint64_t flags);

/* физ. адрес по виртуальному (0, если не замаплен) */
uint64_t vmm_get_phys(uint64_t virt);

#endif
