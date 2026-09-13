/* AresOS - GDT: минимум, необходимый long mode + TSS (v0.8.1).
 * [0] null, [1] kernel code (0x08), [2] kernel data (0x10),
 * [3..4] TSS (селектор 0x18 - дескриптор системного типа занимает 16 байт).
 *
 * ВАЖНО (урок v0.3.3): lgdt НЕ меняет CS! Прошивочный селектор (на VBox это
 * 0x38 - индекс 7, ВНЕ нашей таблицы из 3 записей) остаётся в CS и отдельно
 * валидируется процессором при КАЖДОМ возврате iretq из прерывания:
 * CS загружается из кадра -> валидация против НАШЕЙ GDT -> #GP(0x38).
 * Проявлялось как "мышь мертва": система умирала на первом же IRQ12.
 * Лечится единственно - дальним переходом (push cs; push rip; lretq).
 *
 * ВАЖНО (урок v0.8.1, VirtualBox Guru Meditation): без TSS процессор не может
 * переключить стек на входе в обработчик исключения. Если стек потока уехал
 * в неотображённую страницу, #PF пытается положить свой кадр на ТОТ ЖЕ битый
 * стек -> #DF -> тоже на битый стек -> triple fault (VM просто умирает, ни
 * одной строчки диагностики). TSS.IST даёт критичным векторам (#DF, #PF, #SS,
 * NMI, #MC) отдельные, заведомо валидные стеки - и мы печатаем нормальную
 * панику вместо тройной ошибки. */
#include <stdint.h>

#define GDT_CODE 0x08
#define GDT_DATA 0x10
#define GDT_TSS  0x18

/* ---- TSS (long mode, Intel SDM vol.3 7.7) ---- */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];          /* ist[0] = IST1 ... ist[6] = IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

static tss_t tss;

/* Аварийные стеки. Лежат в .bss ядра (зона 0x200000..0x400000 замаплена
 * постранично RW+NX) - они валидны всегда, даже когда стек потока разрушен. */
#define IST_STACK_SIZE 16384
static uint8_t ist_df[IST_STACK_SIZE] __attribute__((aligned(16)));  /* IST1: #DF */
static uint8_t ist_pf[IST_STACK_SIZE] __attribute__((aligned(16)));  /* IST2: #PF/#SS */
static uint8_t ist_nmi[IST_STACK_SIZE] __attribute__((aligned(16))); /* IST3: NMI/#MC */

static uint64_t gdt[5];       /* 3 обычных + 2 слова под 16-байтный TSS-дескриптор */

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

    /* ---- TSS ---- */
    for (unsigned i = 0; i < sizeof(tss); i++) ((uint8_t *)&tss)[i] = 0;
    /* стеки растут ВНИЗ: в IST кладём вершину, выровненную по 16 */
    tss.ist[0] = ((uint64_t)(uintptr_t)ist_df  + IST_STACK_SIZE) & ~0xFULL;  /* IST1 */
    tss.ist[1] = ((uint64_t)(uintptr_t)ist_pf  + IST_STACK_SIZE) & ~0xFULL;  /* IST2 */
    tss.ist[2] = ((uint64_t)(uintptr_t)ist_nmi + IST_STACK_SIZE) & ~0xFULL;  /* IST3 */
    /* rsp0 понадобится на M7 (ring3 -> ring0); пока — стек #PF */
    tss.rsp0 = tss.ist[1];
    tss.iomap_base = sizeof(tss);    /* I/O-карты нет */

    uint64_t base  = (uint64_t)(uintptr_t)&tss;
    uint32_t limit = (uint32_t)(sizeof(tss) - 1);
    /* системный дескриптор TSS: type=9 (64-bit TSS available), P=1, DPL=0 */
    gdt[3] = (uint64_t)(limit & 0xFFFF)
           | ((base & 0xFFFFFFULL) << 16)
           | (0x89ULL << 40)
           | ((uint64_t)((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFFULL) << 56);
    gdt[4] = (base >> 32) & 0xFFFFFFFFULL;

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

    /* TR: только ПОСЛЕ lgdt. Без ltr поля IST в IDT игнорируются. */
    uint16_t tr = GDT_TSS;
    __asm__ volatile ("ltr %w0" :: "rm"(tr) : "memory");
}

/* границы аварийных стеков - для диагностики в exceptions.c */
void gdt_ist_range(int n, uint64_t *lo, uint64_t *hi) {
    const uint8_t *b = (n == 1) ? ist_df : (n == 2) ? ist_pf : ist_nmi;
    *lo = (uint64_t)(uintptr_t)b;
    *hi = *lo + IST_STACK_SIZE;
}
