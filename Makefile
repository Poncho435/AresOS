# ============================================================================
# AresOS — единая точка сборки и запуска.
#
#   make            — собрать всё: ядро, UEFI-загрузчик, загрузочный образ
#   make run        — QEMU + OVMF (UEFI), serial-лог ядра в этом терминале
#   make debug      — QEMU с GDB-stub на :1234 (затем: make gdb)
#   make gdb        — подключить отладчик к make debug
#   make verify     — статические проверки бинарников (ELF/PE/FAT)
#   make clean
#
# Toolchain: если в PATH есть x86_64-elf-gcc — используется он,
# иначе host gcc (оба годятся: ядро freestanding, чужой libc не линкуется).
# ============================================================================

PYTHON   ?= python3
QEMU     ?= qemu-system-x86_64
BUILD     = build
OBJD      = $(BUILD)/obj

ifneq ($(shell command -v x86_64-elf-gcc 2>/dev/null),)
CC = x86_64-elf-gcc
else
CC = gcc
endif

# ---- флаги ----
COMMON_FLAGS := -std=gnu11 -O2 -g -Wall -Wextra \
    -ffreestanding -fno-stack-protector -fno-stack-check \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mgeneral-regs-only -MMD -MP

# ядро: статика, физические адреса (до M3), large-модель — ни на что не надеемся
KERNEL_CFLAGS := $(COMMON_FLAGS) -fno-pic -fno-pie -mcmodel=large -Iinclude -Ilibc/include

# загрузчик: позиционно-независимый PIE → elf2efi превратит в PE с релокациями
LOADER_CFLAGS := $(COMMON_FLAGS) -fPIC -Iinclude -Iboot

# ---- исходники ----
KERNEL_SRCS := $(shell find kernel libc -name '*.c' 2>/dev/null)
KERNEL_ASMS := $(shell find kernel -name '*.S' 2>/dev/null)
KERNEL_OBJS := $(addprefix $(OBJD)/,$(KERNEL_SRCS:.c=.o)) $(addprefix $(OBJD)/,$(KERNEL_ASMS:.S=.o))

LOADER_SRCS := $(shell find boot -name '*.c' 2>/dev/null)
LOADER_OBJS := $(addprefix $(OBJD)/,$(LOADER_SRCS:.c=.o))

KERNEL  := $(BUILD)/kernel.elf
BOOTELF := $(BUILD)/BOOTX64.ELF
BOOTEFI := $(BUILD)/BOOTX64.EFI
IMG     := $(BUILD)/aresos.img
ISO     := $(BUILD)/aresos.iso

# ---- OVMF (прошивка UEFI для QEMU) ----
OVMF_CODE ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/ovmf/OVMF.fd \
    /usr/share/qemu/OVMF.fd))
OVMF_VARS ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_VARS.fd \
    /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/ovmf/OVMF_VARS.fd))

QEMUFLAGS := -machine q35 -m 256M -net none \
    -drive format=raw,if=ide,file=$(IMG) \
    -serial stdio

.PHONY: all run debug gdb verify clean tools-check
all: $(IMG) $(ISO)

# ==================== компиляция ====================
$(OBJD)/boot/%.o: boot/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LOADER_CFLAGS) -c $< -o $@

$(OBJD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(OBJD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# ==================== линковка ====================
$(KERNEL): $(KERNEL_OBJS) kernel/linker.ld
	$(CC) $(KERNEL_CFLAGS) -nostdlib -static -T kernel/linker.ld \
	    -Wl,--build-id=none -o $@ $(KERNEL_OBJS)
	@echo "[make] ядро: $@"

# v0.2.4: ядро влинковывается ВНУТРЬ загрузчика (символы _binary_kernel_elf_*):
# загрузчику больше не нужен FAT/SimpleFileSystem, чтобы получить KERNEL.ELF.
KBLOB := $(BUILD)/kernel_blob.o
$(KBLOB): $(KERNEL)
	@mkdir -p $(BUILD)
	cd $(BUILD) && ld -r -b binary kernel.elf -o kernel_blob.o

$(BOOTELF): $(LOADER_OBJS) $(KBLOB) boot/loader.ld
	$(CC) $(LOADER_CFLAGS) -nostdlib -static-pie -T boot/loader.ld \
	    -Wl,-e,efi_main -Wl,-z,norelro -Wl,--build-id=none -o $@ $(LOADER_OBJS) $(KBLOB)
	@echo "[make] загрузчик (ELF PIE, ядро вшито, сегменты выровнены): $@"

$(BOOTEFI): $(BOOTELF)
	$(PYTHON) tools/elf2efi.py $< $@
	$(PYTHON) tools/elf2efi.py --verify $@

# ==================== загрузочный образ ====================
$(IMG): $(KERNEL) $(BOOTEFI)
	@printf 'AresOS boot disk (ESP).\r\nEFI/BOOT/BOOTX64.EFI loads KERNEL.ELF\r\n' > $(BUILD)/README.TXT
	$(PYTHON) tools/mkesp.py $@ \
	    --file KERNEL.ELF=$(KERNEL) \
	    --file EFI/BOOT/BOOTX64.EFI=$(BOOTEFI) \
	    --file README.TXT=$(BUILD)/README.TXT

# ISO для виртуальных машин (ISO9660 + El Torito UEFI)
$(ISO): $(KERNEL) $(BOOTEFI)
	$(PYTHON) tools/mkiso.py $@ \
	    --file KERNEL.ELF=$(KERNEL) \
	    --file EFI/BOOT/BOOTX64.EFI=$(BOOTEFI)

# ==================== запуск ====================
$(BUILD)/OVMF_VARS.fd:
ifeq ($(OVMF_VARS),)
	$(error OVMF_VARS.fd не найден — установи пакет ovmf: sudo apt install ovmf)
endif
	cp $(OVMF_VARS) $@

run: all $(BUILD)/OVMF_VARS.fd
ifeq ($(OVMF_CODE),)
	$(error OVMF_CODE.fd не найден — установи пакет ovmf: sudo apt install ovmf)
endif
	$(QEMU) $(QEMUFLAGS) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd

debug: QEMUFLAGS += -s -S -no-reboot
debug: run

gdb:
	gdb -ex 'target remote localhost:1234' -ex 'symbol-file $(KERNEL)' \
	    -ex 'layout split' -ex 'b kmain'

verify: $(KERNEL) $(BOOTEFI) $(IMG)
	$(PYTHON) tools/elf2efi.py --verify $(BOOTEFI)
	$(PYTHON) tools/mkesp.py $(IMG) --list
	-readelf -h $(KERNEL) | head -20

tools-check:
	@echo "CC      = $(CC)";      $(CC) --version | head -1
	@echo "PYTHON  = $(PYTHON)";  $(PYTHON) --version
	@echo "QEMU    = $(QEMU)";    $(QEMU) --version 2>/dev/null | head -1 || echo "  не установлен (apt install qemu-system-x86)"
	@echo "OVMF    = $(OVMF_CODE)"
	@echo "VARRS   = $(OVMF_VARS)"

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJS:.o=.d) $(LOADER_OBJS:.o=.d)
