/* AresOS - прототип рабочего стола (M2.5-bonus): обои, панель, окно, мышь */
#ifndef ARES_DESKTOP_H
#define ARES_DESKTOP_H

#include <stdint.h>
#include "bootinfo.h"

/* сколько шагов в "установке" (сид файловой системы). Общий для
 * установщика в загрузочном меню (session.c) и приложения F6. */
#define SETUP_STEPS 9

/* Инициализация графики/мыши и главный цикл десктопа. Не возвращается. */
__attribute__((noreturn)) void desktop_enter(const bootinfo_t *bi,
                                             uint64_t total_mib,
                                             uint64_t free_mib);

/* ---- v0.8.0: общие сервисы экрана (использует и session.c) ---- */
/* Графика + мышь + NVRAM-настройки + бэкбуфер/кэш обоев.
 * Вызывается session_run() ДО стола; в desktop_enter() - холостой повтор. */
void desktop_boot_graphics(const bootinfo_t *bi);

int  desktop_scale(void);                 /* масштаб интерфейса: 100/150/200 */
void desktop_bg_fill(void);               /* обои в бэкбуфер (весь экран)   */
void desktop_present_full(void);          /* кадр целиком + курсор -> экран */
void desktop_present_cursor(void);        /* двинулась только мышь: микро-сброс */

/* шаги установки: создают папки/файлы в ramfs (0..SETUP_STEPS-1) */
void setup_steps_run(int step);

#endif
