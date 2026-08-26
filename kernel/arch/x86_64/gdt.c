/* AresOS - GDT: минимум, необходимый long mode.
 * [0] null, [1] kernel code (0x08), [2] kernel data (0x10).
 * На M7 сюда добавятся user-сегменты и TSS.
 *
 * ВАЖНО (урок v0.3.3): lgdt НЕ меняет CS! Прошивочный селектор (на VBox это
 * 0x38 - индекс 7, ВНЕ нашей таблицы из 3 записей) остаётся в CS и ОТРАЛЬНО
 * валидируется процессором при КАЖДОМ возврате iretq из прерывания:
 * CS загружается из кадра -> валидация против НАШЕЙ GDT -> #GP(0x38).
 * Проявлялось как "мышь мертва": система умирала на первом же IRQ12.
 * Лечится единственно - дальним переходом (push cs; push rip; lretq). */
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
        "movw %w0, %%fs\n\t"
        "movw %w0, %%gs\n\t"
        :: "rm"(ds) : "memory");

    /* перезагрузка CS дальним возвратом: кладём целевой CS и адрес возврата,
       lretq снимает их и прыгает - CS теперь НАШ (0x08, внутри GDT). */
    __asm__ volatile (
        "pushq %0\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "i"(GDT_CODE)
        : "rax", "memory");
}
