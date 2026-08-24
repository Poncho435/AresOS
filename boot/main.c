/*
 * AresOS — UEFI-загрузчик (BOOTX64.EFI).
 *
 * Что делает:
 *   1. Инициализирует debug-вывод (ConOut на экран + COM1 в терминал).
 *   2. Выключает сторожевой таймер UEFI.
 *   3. Читает KERNEL.ELF с ESP-раздела (FAT) через Simple File System.
 *   4. Парсит ELF64 и загружает PT_LOAD-сегменты по физическим адресам.
 *   5. Настраивает видеорежим через GOP (предпочтительно 1024x768, 32bpp).
 *   6. Получает карту памяти, выходит из Boot Services
 *      и прыгает на точку входа ядра, передавая bootinfo_t*.
 *
 * Собирается как static-PIE ELF, затем tools/elf2efi.py конвертирует
 * в PE32+ (EFI Application) — см. Makefile.
 */
#include "uefi.h"
#include "bootinfo.h"

/* ===================== локальные функции libc (freestanding) ===================== */
static void *lmemcpy(void *d, const void *s, UINTN n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    for (UINTN i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}
static void lmemset(void *d, uint8_t v, UINTN n) {
    uint8_t *dd = d;
    for (UINTN i = 0; i < n; i++) dd[i] = v;
}


/* ===================== глобалы ===================== */
static EFI_SYSTEM_TABLE *gST;
static EFI_BOOT_SERVICES *gBS;
static EFI_HANDLE g_loaded_image;
static bootinfo_t g_bootinfo;   /* живёт в нашей памяти до прыжка в ядро */

static const EFI_GUID gGopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static const EFI_GUID gLoadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const EFI_GUID gSimpleFsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

/* ===================== debug-вывод: COM1 + экран ===================== */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("out %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("in %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
#define COM1 0x3F8

static void serial_init(void) {
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01); outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7); outb(COM1 + 4, 0x0B);
}
static void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20)) {}
    outb(COM1, (uint8_t)c);
}
static void serial_str(const char *s) {
    while (*s) { if (*s == '\n') serial_putc('\r'); serial_putc(*s++); }
}
static void serial_hex(uint64_t v) {
    serial_str("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc("0123456789ABCDEF"[(v >> i) & 0xF]);
}
static void serial_dec(uint64_t v) {
    char buf[24]; int i = 0;
    if (!v) { serial_putc('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i) serial_putc(buf[--i]);
}

static void screen_print(const CHAR16 *s) {
    if (gST->ConOut) gST->ConOut->OutputString(gST->ConOut, s);
}

/* лог одной строкой на оба канала (serial + экран) */
static void log_line(const char *ascii) {
    CHAR16 buf[256];
    UINTN i = 0;
    serial_str(ascii); serial_str("\n");
    for (; ascii[i] && i < 250; i++) buf[i] = (CHAR16)ascii[i];
    buf[i++] = u'\r'; buf[i++] = u'\n'; buf[i] = 0;
    screen_print(buf);
}
static void log_hex(const char *prefix, uint64_t v) {
    serial_str(prefix); serial_hex(v); serial_str("\n");
}

/* ===================== ELF64 (только loader-часть) ===================== */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} elf64_phdr_t;

#define PT_LOAD 1

/* ===================== чтение KERNEL.ELF ===================== */
static EFI_STATUS read_kernel_file(uint8_t **out_buf, UINTN *out_size) {
    EFI_STATUS st;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *file;

    st = gBS->HandleProtocol(g_loaded_image, &gLoadedImageGuid, (void **)&li);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: HandleProtocol(LoadedImage)"); return st; }

    st = gBS->HandleProtocol(li->DeviceHandle, &gSimpleFsGuid, (void **)&fs);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: HandleProtocol(SimpleFS)"); return st; }

    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: OpenVolume"); return st; }

    st = root->Open(root, &file, u"KERNEL.ELF", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: open KERNEL.ELF — файла нет на ESP"); return st; }

    /* размер файла — трюком SetPosition(0xFF..FF) + GetPosition */
    uint64_t size = 0;
    st = file->SetPosition(file, 0xFFFFFFFFFFFFFFFFULL);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: SetPosition(end)"); return st; }
    st = file->GetPosition(file, &size);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: GetPosition"); return st; }
    file->SetPosition(file, 0);

    EFI_PHYSICAL_ADDRESS buf = 0;
    UINTN pages = EFI_SIZE_TO_PAGES(size);
    st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &buf);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: AllocatePages(kernel file)"); return st; }

    UINTN read_size = size;
    st = file->Read(file, &read_size, (void *)buf);
    if (EFI_ERROR(st) || read_size != size) {
        log_line("[boot] FAIL: Read(kernel)");
        return EFI_ERROR_BIT | 1;
    }
    file->Close(file);
    root->Close(root);

    log_hex("[boot] KERNEL.ELF read, size = ", size);
    *out_buf = (uint8_t *)buf;
    *out_size = size;
    return EFI_SUCCESS;
}

/* ===================== загрузка сегментов ядра ===================== */
static EFI_STATUS load_kernel_segments(uint8_t *buf, uint64_t *out_entry) {
    elf64_ehdr_t *eh = (elf64_ehdr_t *)buf;

    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
          eh->e_ident[2] == 'L'  && eh->e_ident[3] == 'F')) {
        log_line("[boot] FAIL: not an ELF file");
        return EFI_ERROR_BIT | 1;
    }
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1 || eh->e_machine != 0x3E || eh->e_type != 2) {
        log_line("[boot] FAIL: ELF is not x86-64 EXEC LSB");
        return EFI_ERROR_BIT | 1;
    }

    elf64_phdr_t *ph = (elf64_phdr_t *)(buf + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;

        EFI_PHYSICAL_ADDRESS base = ph[i].p_paddr & ~0xFFFULL;
        UINTN extra = (UINTN)(ph[i].p_paddr - base);
        UINTN pages = EFI_SIZE_TO_PAGES(extra + ph[i].p_memsz);

        EFI_STATUS st = gBS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &base);
        if (EFI_ERROR(st)) {
            log_hex("[boot] FAIL: AllocatePages @ ", base);
            return st;
        }
        lmemset((void *)(base + extra), 0, ph[i].p_memsz);
        lmemcpy((void *)(base + extra), buf + ph[i].p_offset, ph[i].p_filesz);

        log_hex("[boot] PT_LOAD dest=", base + extra);
        log_hex("       filesz/memsz=", ph[i].p_filesz);
        log_hex("                  =", ph[i].p_memsz);
    }

    *out_entry = eh->e_entry;
    return EFI_SUCCESS;
}

/* ===================== видеорежим (GOP) ===================== */
static void setup_graphics(void) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    g_bootinfo.fb.phys_base = 0;   /* по умолчанию — нет графики */

    EFI_STATUS st = gBS->LocateProtocol(&gGopGuid, (void *)0, (void **)&gop);
    if (EFI_ERROR(st)) {
        log_line("[boot] GOP not found — kernel will use serial only");
        return;
    }

    /* ищем 1024x768; если нет — оставляем текущий режим */
    int chosen = -1;
    for (uint32_t m = 0; m < gop->Mode->MaxMode; m++) {
        UINTN info_size;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        st = gop->QueryMode(gop, m, &info_size, &info);
        if (EFI_ERROR(st)) continue;
        if (info->HorizontalResolution == 1024 &&
            info->VerticalResolution == 768 &&
            (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
             info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)) {
            chosen = (int)m;
            break;
        }
    }
    if (chosen >= 0) {
        st = gop->SetMode(gop, (uint32_t)chosen);
        if (EFI_ERROR(st)) log_line("[boot] SetMode failed, keeping current");
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *cur = gop->Mode->Info;
    g_bootinfo.fb.phys_base = gop->Mode->FrameBufferBase;
    g_bootinfo.fb.width     = cur->HorizontalResolution;
    g_bootinfo.fb.height    = cur->VerticalResolution;
    g_bootinfo.fb.pitch     = cur->PixelsPerScanLine;
    g_bootinfo.fb.format    = (cur->PixelFormat == PixelRedGreenBlueReserved8BitPerColor)
                              ? FB_FORMAT_RGB : FB_FORMAT_BGR;

    log_hex("[boot] framebuffer base = ", g_bootinfo.fb.phys_base);
    serial_str("[boot] mode "); serial_dec(g_bootinfo.fb.width);
    serial_str("x"); serial_dec(g_bootinfo.fb.height);
    serial_str(" fmt="); serial_dec(g_bootinfo.fb.format); serial_str("\n");
}

/* ===================== выход из Boot Services ===================== */
static EFI_STATUS exit_boot_services(EFI_HANDLE image) {
    for (int attempt = 0; attempt < 5; attempt++) {
        UINTN map_size = 0, map_key = 0, desc_size = 0;
        uint32_t desc_ver = 0;

        EFI_STATUS st = gBS->GetMemoryMap(&map_size, (void *)0, &map_key, &desc_size, &desc_ver);
        if (st != EFI_BUFFER_TOO_SMALL) { log_line("[boot] FAIL: GetMemoryMap(size)"); return st; }

        map_size += 2 * desc_size;   /* запас: сама AllocatePages меняет карту */
        UINTN pages = EFI_SIZE_TO_PAGES(map_size);
        EFI_PHYSICAL_ADDRESS map_buf = 0;
        st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &map_buf);
        if (EFI_ERROR(st)) { log_line("[boot] FAIL: AllocatePages(mmap)"); return st; }

        st = gBS->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)map_buf,
                               &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(st)) { log_line("[boot] FAIL: GetMemoryMap(fill)"); continue; }

        st = gBS->ExitBootServices(image, map_key);
        if (st == EFI_SUCCESS) {
            g_bootinfo.mmap_phys         = map_buf;
            g_bootinfo.mmap_size         = map_size;
            g_bootinfo.mmap_desc_size    = desc_size;
            g_bootinfo.mmap_desc_version = desc_ver;
            log_line("[boot] ExitBootServices OK — bye, firmware!");
            return EFI_SUCCESS;
        }
        log_line("[boot] ExitBootServices retry...");
        gBS->FreePages(map_buf, pages);
    }
    log_line("[boot] FAIL: ExitBootServices (5 attempts)");
    return EFI_ERROR_BIT | 1;
}

/* ===================== diag-маркеры (v0.2.2): полосы в fb напрямую =====================
 * Рисуем в GOP framebuffer БЕЗ прошивки и консоли. По фото экрана видно,
 * до какого этапа дошла загрузка (даже если падает текст/serial):
 *   M1 синий весь экран — загрузчик жив, GOP найден, fb пишется
 *   M2 зелёная полоса   — KERNEL.ELF прочитан с ESP
 *   M3 бирюзовая        — PT_LOAD ядра размещены в памяти
 *   M4 пурпурная        — ExitBootServices прошёл (прошивка отпустила)
 *   M5 циановая         — ставит ядро: точка входа выполняется (kmain)
 */
static void diag_band(uint32_t y0, uint32_t y1, uint8_t r, uint8_t g, uint8_t b) {
    if (!g_bootinfo.fb.phys_base || g_bootinfo.fb.format > FB_FORMAT_BGR) return;
    if (!g_bootinfo.fb.pitch || !g_bootinfo.fb.width || !g_bootinfo.fb.height) return;
    uint32_t v = (g_bootinfo.fb.format == FB_FORMAT_RGB)
               ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | b
               : ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    uint32_t *fb = (uint32_t *)(uintptr_t)g_bootinfo.fb.phys_base;
    if (y1 > g_bootinfo.fb.height) y1 = g_bootinfo.fb.height;
    for (uint32_t y = y0; y < y1; y++) {
        uint32_t *line = fb + (uint64_t)y * g_bootinfo.fb.pitch;
        for (uint32_t x = 0; x < g_bootinfo.fb.width; x++) line[x] = v;
    }
}

/* ===================== точка входа UEFI ===================== */
extern char __bss_start[], _end[];  /* символы link-скрипта PIE */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    /* 0. Первым делом: обнуляем СОБСТВЕННЫЙ bss.
       UEFI-загрузчик образов не гарантирует нулевой хвост секций. */
    for (char *p = __bss_start; p < _end; p++) *p = 0;

    gST = system_table;
    gBS = system_table->BootServices;
    g_loaded_image = image_handle;

    serial_init();
    serial_str("\n[boot] AresOS UEFI loader alive\n");

    if (gST->ConOut) {
        gST->ConOut->ClearScreen(gST->ConOut);
        screen_print(u"AresOS loader (BOOTX64.EFI) v0.2.2-diag\r\n");
    }

    /* графику поднимаем ПЕРВОЙ (SetMode сам очищает экран) — нужна для маркеров */
    setup_graphics();
    diag_band(0, 0xFFFFFFFF, 0x00, 0x00, 0xC0);      /* M1: весь экран синий */
    serial_str("[diag] M1 loader-alive GOP-ok\n");
    log_line("[boot] stage 1: console up");

    /* сторожевой таймер — выключить (иначе ребут через 5 минут) */
    gBS->SetWatchdogTimer(0, 0, 0, (const CHAR16 *)0);

    uint8_t *kbuf;
    UINTN ksize;
    EFI_STATUS st = read_kernel_file(&kbuf, &ksize);
    if (EFI_ERROR(st)) goto hang;
    diag_band(0, 32, 0x00, 0xC0, 0x00);              /* M2: ядро прочитано */
    serial_str("[diag] M2 kernel-file-read-ok\n");

    uint64_t entry;
    st = load_kernel_segments(kbuf, &entry);
    if (EFI_ERROR(st)) goto hang;
    diag_band(32, 64, 0x00, 0xA8, 0xA8);             /* M3: PT_LOAD размещены */
    serial_str("[diag] M3 segments-loaded-ok\n");
    log_hex("[boot] kernel entry = ", entry);

    g_bootinfo.magic = BOOTINFO_MAGIC;

    /* ВАЖНО: после этого вызова никакие Boot Services больше нельзя трогать */
    st = exit_boot_services(image_handle);
    if (EFI_ERROR(st)) goto hang;
    /* прошивка отдала управление; fb — просто память, писать всё ещё можно */
    diag_band(64, 96, 0xC0, 0x00, 0xC0);             /* M4: ExitBootServices OK */
    serial_str("[diag] M4 exit-boot-services-ok\n");

    __asm__ volatile ("cli");
    ((void (*)(bootinfo_t *))entry)(&g_bootinfo);

    __builtin_unreachable();

hang:
    log_line("[boot] FATAL — system halted");
    for (;;) __asm__ volatile ("hlt");
}
