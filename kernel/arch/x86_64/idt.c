/* AresOS — IDT: таблица дескрипторов прерываний (Intel SDM vol.3, гл. 6).
 * Пока только исключения CPU (0..31): IRQ/APIC подключим на M4. */
#include <stdint.h>

typedef struct {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

#define IDT_ENTRIES 32
#define GATE_INTERRUPT 0x8E   /* P=1, DPL=0, type=1110 (64-bit interrupt gate) */
#define SEL_KCODE      0x08

static idt_entry_t idt[IDT_ENTRIES];

/* стабы из idt_stubs.S */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

static void (*const stub_table[IDT_ENTRIES])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

static void idt_set(int n, void (*handler)(void)) {
    uint64_t addr = (uint64_t)handler;
    idt[n].offset_lo  = (uint16_t)(addr & 0xFFFF);
    idt[n].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[n].offset_hi  = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[n].selector   = SEL_KCODE;
    idt[n].ist        = 0;
    idt[n].type_attr  = GATE_INTERRUPT;
    idt[n].zero       = 0;
}

void idt_init(void) {
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set(i, stub_table[i]);

    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)&idt,
    };
    __asm__ volatile ("lidt %0" :: "m"(idtr) : "memory");
    /* sti НЕ делаем: пока нет обработчиков IRQ — включим на M4 */
}
