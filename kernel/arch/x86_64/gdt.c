/* AresOS — GDT: минимум, необходимый long mode.
 * [0] null, [1] kernel code (0x08), [2] kernel data (0x10).
 * CS не перезагружаем far-прыжком: в long mode база CS игнорируется,
 * а текущий CS от UEFI остаётся валидным исполняемым сегментом.
 * На M7 сюда добавятся user-сегменты и TSS. */
#include <stdint.h>

#define GDT_CODE 0x08
#define GDT_DATA 0x10

static uint64_t gdt[3];

static inline uint64_t gdt_entry(uint8_t access, uint8_t gran) {
    /* base=0, limit=0xFFFFF (в long mode limit игнорируется, но поля заполнены) */
    return 0xFFFFULL
         | ((uint64_t)access << 40)
         | ((uint64_t)(gran & 0x0F) << 52)
         | (0xFULL << 48);
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = gdt_entry(0x9A, 0x0A);  /* present, ring0, code, exec/read  | G=1, L=1 */
    gdt[2] = gdt_entry(0x92, 0x0C);  /* present, ring0, data, read/write | G=1, D/B=1 */

    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr = {
        .limit = sizeof(gdt) - 1,
        .base  = (uint64_t)&gdt,
    };
    __asm__ volatile ("lgdt %0" :: "m"(gdtr) : "memory");

    uint16_t ds = GDT_DATA;
    __asm__ volatile (
        "movw %w0, %%ds\n\t"   /* AT&T-порядок: src, dst */
        "movw %w0, %%es\n\t"
        "movw %w0, %%ss\n\t"
        :: "rm"(ds) : "memory");
}
