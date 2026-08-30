/* AresOS - UEFI Runtime Services (v0.7.0): NVRAM-настройки + Reset.
 * Вызов идёт в прошивочный код, замапленный vmm как identity+исполняемый
 * (см. vmm.c: регионы с атрибутом EFI_MEMORY_RUNTIME).
 * Осторожно соглашение вызовов: UEFI = Microsoft x64 ABI (ms_abi), а не
 * наш привычный System V - объявления ниже это учитывают. */
#include "efi_rt.h"
#include "kprintf.h"
#include "io.h"
#include <stdint.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t EFI_STATUS;
#define EFI_ERROR_BIT (1ULL << 63)
#define EFI_ERROR(s)  (((int64_t)(s)) < 0)

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} efi_hdr_t;

typedef struct {
    efi_hdr_t Hdr;
    void *GetTime; void *SetTime; void *GetWakeupTime; void *SetWakeupTime;
    void *SetVirtualAddressMap; void *ConvertPointer; void *GetVariable;
    void *GetNextVariableName; void *SetVariable; void *GetNextHighMonotonicCount;
    void *ResetSystem; void *UpdateCapsule; void *QueryCapsuleCapabilities;
    void *QueryVariableInfo;
} efi_rt_t;

typedef EFI_STATUS (EFIAPI *get_var_t)(const uint16_t *name, const void *guid,
                                       uint32_t *attrs, uint64_t *size, void *data);
typedef EFI_STATUS (EFIAPI *set_var_t)(const uint16_t *name, const void *guid,
                                       uint32_t attrs, uint64_t size, const void *data);
typedef void (EFIAPI *reset_t)(uint32_t type, EFI_STATUS status,
                               uint64_t size, const void *data);

static efi_rt_t *g_rt;
static int      g_ok;
static const struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; }
g_guid = ARES_OS_VAR_GUID;

void efi_rt_init(const bootinfo_t *bi) {
    g_rt = 0; g_ok = 0;
    if (!bi || !bi->rt_phys) {
        kprintf("[efi] Runtime Services не переданы загрузчиком - настройки без NVRAM\n");
        return;
    }
    g_rt = (efi_rt_t *)(uintptr_t)bi->rt_phys;
    g_ok = 1;   /* регионы RT уже замаплены vmm_init по карте UEFI */
    kprintf("[efi] Runtime Services @ %#lx: NVRAM-настройки и Reset доступны\n",
            (uint64_t)(uintptr_t)g_rt);
}

int efi_rt_ok(void) { return g_ok; }

/* имя переменной ASCII -> CHAR16 в static-буфере (физ. адрес = вирт.) */
static uint16_t g_name[32];
static uint8_t  g_data[64];

static int name_to_u16(const char *n) {
    int i = 0;
    while (n[i] && i < 30) { g_name[i] = (uint16_t)(uint8_t)n[i]; i++; }
    g_name[i] = 0;
    return i;
}

static uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static void irq_restore(uint64_t f) {
    if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
}

static int var_set(const char *name, const void *data, uint64_t size) {
    if (!g_ok || !g_rt->SetVariable) return 0;
    name_to_u16(name);
    /* static-буферы лежат в identity-зоне: их VA == PA - RT доволен */
    uint64_t f = irq_save();
    EFI_STATUS st = ((set_var_t)g_rt->SetVariable)(
        g_name, &g_guid, ARES_VAR_ATTRS, size, data);
    irq_restore(f);
    if (EFI_ERROR(st)) {
        kprintf("[efi] SetVariable(%s) -> ошибка %#lx\n", name, st);
        return 0;
    }
    return 1;
}

int efi_var_set_u32(const char *name, uint32_t v) {
    *(uint32_t *)g_data = v;
    int ok = var_set(name, g_data, 4);
    kprintf("[efi] NVRAM: %s <- %lu (%s)\n", name, (uint64_t)v, ok ? "ок" : "НЕ СОХРАНЕНО");
    return ok;
}

int efi_var_set_str(const char *name, const char *s) {
    uint64_t n = 0;
    while (s[n] && n < sizeof(g_data) - 1) { g_data[n] = (uint8_t)s[n]; n++; }
    int ok = var_set(name, g_data, n);
    kprintf("[efi] NVRAM: %s <- '%s' (%s)\n", name, s, ok ? "ок" : "НЕ СОХРАНЕНО");
    return ok;
}

int efi_var_delete(const char *name) {
    int ok = var_set(name, g_data, 0);      /* size=0 -> удалить переменную */
    kprintf("[efi] NVRAM: %s удалена (%s)\n", name, ok ? "ок" : "не было/ошибка");
    return ok;
}

static int var_get(const char *name, void *data, uint64_t *size) {
    if (!g_ok || !g_rt->GetVariable) return 0;
    name_to_u16(name);
    uint64_t f = irq_save();
    EFI_STATUS st = ((get_var_t)g_rt->GetVariable)(
        g_name, &g_guid, (uint32_t *)0, size, data);
    irq_restore(f);
    return !EFI_ERROR(st);
}

int efi_var_get_u32(const char *name, uint32_t *out) {
    uint64_t sz = 4;
    if (!var_get(name, g_data, &sz) || sz != 4) return 0;
    *out = *(uint32_t *)g_data;
    return 1;
}

int efi_var_get_str(const char *name, char *out, uint32_t out_cap) {
    uint64_t sz = sizeof(g_data) - 1;
    if (!var_get(name, g_data, &sz) || !sz || sz >= sizeof(g_data) || out_cap < 2)
        return 0;
    uint32_t n = (uint32_t)sz;
    if (n >= out_cap) n = out_cap - 1;
    for (uint32_t i = 0; i < n; i++) out[i] = (char)g_data[i];
    out[n] = 0;
    return 1;
}

void efi_reset_cold(void) {
    kprintf("[efi] ПЕРЕЗАГРУЗКА через ResetSystem(EfiResetCold)...\n");
    if (g_ok && g_rt->ResetSystem) {
        uint64_t f = irq_save();
        ((reset_t)g_rt->ResetSystem)(0 /*EfiResetCold*/, 0, 0, (const void *)0);
        irq_restore(f);   /* не должен вернуться, но вдруг */
    }
    /* запасной путь: контроллер клавиатуры 8042, линия RESET */
    for (int i = 0; i < 20; i++) { outb(0x64, 0xFE); io_wait(); }
    /* последний шанс: triple fault через пустой IDT */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int $3" :: "m"(null_idt));
    __builtin_unreachable();
}
