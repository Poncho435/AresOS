/*
 * bootinfo - структура, которую UEFI-загрузчик передаёт ядру.
 * Общий заголовок: используется и в boot/, и в kernel/.
 * Изменил поле - пересобери И загрузчик, И ядро.
 */
#ifndef ARES_BOOTINFO_H
#define ARES_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC 0x415245534F533031ULL  /* "ARESOS01" */

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

typedef struct {
    uint64_t magic;
    bootinfo_fb_t fb;
    uint64_t mmap_phys;        /* физ. адрес массива дескрипторов памяти UEFI */
    uint64_t mmap_size;        /* размер всей карты, байт */
    uint64_t mmap_desc_size;   /* размер одного дескриптора, байт */
    uint32_t mmap_desc_version;
    uint32_t _reserved;
    uint64_t rsdp_phys;        /* ACPI RSDP из EFI ConfigurationTable (0 = искать сканом) */
} bootinfo_t;

#endif
