/* AresOS — прототип рабочего стола (M2.5-bonus): обои, панель, окно, мышь */
#ifndef ARES_DESKTOP_H
#define ARES_DESKTOP_H

#include "bootinfo.h"

/* Инициализация графики/мыши и главный цикл десктопа. Не возвращается. */
__attribute__((noreturn)) void desktop_enter(const bootinfo_t *bi,
                                             uint64_t total_mib,
                                             uint64_t free_mib);

#endif
