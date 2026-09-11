/* AresOS - session: всё, что происходит ДО рабочего стола (v0.8.0):
 * загрузочный экран (установить/обновить/диагностика/live), мастер
 * установки с созданием учётной записи и пароля, сплеш, экран блокировки
 * и экран входа. Состояние "установлено" и учётка живут в UEFI NVRAM. */
#ifndef ARES_SESSION_H
#define ARES_SESSION_H

#include <stdint.h>
#include "bootinfo.h"

/* Запустить до-десктопную цепочку. Возвращается, когда пора рисовать стол. */
void session_run(const bootinfo_t *bi, uint64_t total_mib, uint64_t free_mib);

/* вошедший пользователь (для чипа на панели). present=0 в Live-режиме. */
const char *session_user_name(void);
int         session_user_present(void);

#endif
