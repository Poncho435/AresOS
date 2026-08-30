/* AresOS - UEFI Runtime Services из ядра (v0.7.0).
 * Прошивка ушла, но RT-сервисы ЖИВЫ: через них ядро пишет настройки в
 * NVRAM (SetVariable) и делает холодную перезагрузку (ResetSystem).
 * Мы НЕ вызывали SetVirtualAddressMap -> все указатели для RT - ФИЗИЧЕСКИЕ
 * адреса. Подходят только данные из identity-зоны ядра (0x200000..0x400000),
 * поэтому тут - static-буферы, а НЕ kmalloc (куча на 0x500000000!). */
#ifndef ARES_EFI_RT_H
#define ARES_EFI_RT_H

#include <stdint.h>
#include "bootinfo.h"

void efi_rt_init(const bootinfo_t *bi);  /* запомнить указатель RT */
int  efi_rt_ok(void);                    /* RT-таблица есть и замаплена? */

/* NVRAM-переменные AresOS (vendor GUID из bootinfo.h). Возвращают 1 = ок. */
int  efi_var_get_u32(const char *name, uint32_t *out);
int  efi_var_set_u32(const char *name, uint32_t v);
int  efi_var_get_str(const char *name, char *out, uint32_t out_cap);
int  efi_var_set_str(const char *name, const char *s);
int  efi_var_delete(const char *name);   /* стереть (SetVariable size=0) */

/* холодная перезагрузка (EfiResetCold); не возвращается при успехе. */
void efi_reset_cold(void);

#endif
