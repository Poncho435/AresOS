/*
 * bootinfo - структура, которую UEFI-загрузчик передаёт ядру.
 * Общий заголовок: используется и в boot/, и в kernel/.
 * Изменил поле - пересобери И загрузчик, И ядро.
 */
#ifndef ARES_BOOTINFO_H
#define ARES_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC 0x415245534F533032ULL  /* "ARESOS02" (v0.7.0: +RT, +modes) */

/* Пиксельные форматы - совпадают с EFI_GRAPHICS_PIXEL_FORMAT */
#define FB_FORMAT_RGB 0   /* PixelRedGreenBlueReserved8BitPerColor */
#define FB_FORMAT_BGR 1   /* PixelBlueGreenRedReserved8BitPerColor  */

typedef struct {
    uint64_t phys_base; /* физ. адрес framebuffer (0 = графики нет) */
    uint32_t width;     /* пикселей в строке (видимых) */
    uint32_t height;
    uint32_t pitch;     /* пикселей на scanline, включая выравнивание */
    uint32_t format;    /* FB_FORMAT_* */
} bootinfo_fb_t;

/* один доступный видеорежим GOP (для Настройки -> Экран) */
typedef struct {
    uint32_t w, h;      /* разрешение */
    uint32_t num;       /* номер режима GOP (для загрузчика) */
    uint32_t _pad;
} bootinfo_mode_t;

#define BOOTINFO_MAX_MODES 24

typedef struct {
    uint64_t magic;
    bootinfo_fb_t fb;
    uint64_t mmap_phys;        /* физ. адрес массива дескрипторов памяти UEFI */
    uint64_t mmap_size;        /* размер всей карты, байт */
    uint64_t mmap_desc_size;   /* размер одного дескриптора, байт */
    uint32_t mmap_desc_version;
    uint32_t _reserved;
    uint64_t rsdp_phys;        /* ACPI RSDP из EFI ConfigurationTable (0 = искать сканом) */
    /* ---- v0.7.0 ---- */
    uint64_t rt_phys;          /* EFI_RUNTIME_SERVICES* (физ. адрес, 0 = нет) */
    uint32_t modes_n;                          /* сколько режимов записано ниже */
    uint32_t _reserved2;
    bootinfo_mode_t modes[BOOTINFO_MAX_MODES]; /* все RGB/BGR режимы GOP */
} bootinfo_t;

/* ---- UEFI-переменные AresOS (NVRAM): настройки, переживающие перезагрузку ----
 * Один vendor-GUID на все. Значения храним как сырые байты:
 *   AresVideoMode  = ASCII "1920x1080" (читает ЗАГРУЗЧИК, ставит GOP)
 *   AresScale      = u32 100/150/200   (масштаб интерфейса, %)
 *   AresWallpaper  = u32 0..4          (вариант обоев)
 *   AresMouse      = u32: idx скорости (0..8 -> 50..250%) | accel<<16 */
#define ARES_OS_VAR_GUID \
    { 0xA35E0517, 0x7B4D, 0x4A11, { 0x9C, 0xE2, 0x51, 0xA3, 0x05, 0xBE, 0xEF, 0x42 } }
#define ARES_VAR_ATTRS 7 /* NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS */

#endif
