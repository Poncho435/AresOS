/* AresOS — ACPI (M4): поиск RSDP, разбор RSDT/XSDT → MADT("APIC").
 * Нам нужны только: база LAPIC, база IOAPIC, BSP APIC ID, ремаппинги GSI. */
#ifndef ARES_ACPI_H
#define ARES_ACPI_H

#include <stdint.h>

int      acpi_init(uint64_t rsdp_hint); /* RSDP от загрузчика (0 = скан 0xE0000) */
uint64_t acpi_lapic_base(void);    /* физ. адрес регистров LAPIC */
uint64_t acpi_ioapic_base(void);   /* физ. адрес регистров IOAPIC (0 = нет) */
uint32_t acpi_bsp_apic_id(void);   /* id BSP из MADT (или из LAPIC ID регистра) */
uint32_t acpi_gsi_for_irq(uint32_t irq);  /* GSI с учётом overrides (по умолчанию = irq) */

#endif
