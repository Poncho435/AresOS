/*
 * AresOS - UEFI-загрузчик (BOOTX64.EFI).
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
 * в PE32+ (EFI Application) - см. Makefile.
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
static const EFI_GUID gFileInfoGuid =
    { 0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

/* diag-полосы (определение - ниже, перед efi_main) */
static void diag_band(uint32_t y0, uint32_t y1, uint8_t r, uint8_t g, uint8_t b);

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

/* ===================== ядро, вшитое прямо в BOOTX64.EFI (v0.2.4) =====================
 * Makefile влинковывает build/kernel.elf в загрузчик как бинарный объект
 * (ld -r -b binary) -> символы ниже. Так мы получаем файл ядра БЕЗ единого
 * обращения к FAT/SimpleFileSystem: прошивка VBox подала признаки нестабиль-
 * ности ровно в fs->OpenVolume() на El Torito FAT12 - обходим этот путь совсем. */
extern const uint8_t _binary_kernel_elf_start[];
extern const uint8_t _binary_kernel_elf_end[];

static EFI_STATUS read_kernel_embedded(uint8_t **out_buf, UINTN *out_size) {
    const uint8_t *s = _binary_kernel_elf_start;
    const uint8_t *e = _binary_kernel_elf_end;
    if (e <= s)
        return EFI_ERROR_BIT | 10;   /* payload не влинкован (старая сборка) */

    diag_band(8, 16, 0xFF, 0xA0, 0x00);           /* R2: payload найден внутри себя */
    serial_str("[boot] embedded kernel payload present\n");

    UINTN size = (UINTN)(e - s);
    if (size < 64 || !(s[0] == 0x7F && s[1] == 'E' && s[2] == 'L' && s[3] == 'F')) {
        log_line("[boot] FAIL: embedded payload is not an ELF");
        return EFI_ERROR_BIT | 1;
    }
    diag_band(16, 24, 0xE0, 0xE0, 0x00);          /* R3: ELF-магия верна */
    serial_str("[boot] embedded payload ELF magic ok\n");

    log_hex("[boot] embedded KERNEL size = ", size);
    diag_band(24, 32, 0x80, 0xE0, 0x00);          /* R4: размер адекватный */

    *out_buf = (uint8_t *)s;
    *out_size = size;
    return EFI_SUCCESS;
}

/* ===================== чтение KERNEL.ELF (запасной путь, ESP FAT) ===================== */
static EFI_STATUS read_kernel_file(uint8_t **out_buf, UINTN *out_size) {
    EFI_STATUS st;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *file;

    st = gBS->HandleProtocol(g_loaded_image, &gLoadedImageGuid, (void **)&li);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: HandleProtocol(LoadedImage)"); return st; }
    serial_str("[fs] loaded-image ok\n");
    diag_band(0, 8, 0xFF, 0x60, 0x00);            /* R1 */

    st = gBS->HandleProtocol(li->DeviceHandle, &gSimpleFsGuid, (void **)&fs);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: HandleProtocol(SimpleFS)"); return st; }
    serial_str("[fs] simple-fs ok\n");
    diag_band(8, 16, 0xFF, 0xA0, 0x00);           /* R2 */

    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: OpenVolume"); return st; }
    serial_str("[fs] volume open ok\n");
    diag_band(16, 24, 0xE0, 0xE0, 0x00);          /* R3 */

    st = root->Open(root, &file, u"KERNEL.ELF", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: open KERNEL.ELF - файла нет на ESP"); return st; }
    serial_str("[fs] KERNEL.ELF open ok\n");
    diag_band(24, 32, 0x80, 0xE0, 0x00);          /* R4 */

    /* размер файла: СНАЧАЛА канонический GetInfo (EFI_FILE_INFO),
       а трюк SetPosition(end) - только как запасной вариант */
    uint64_t size = 0;
    UINTN info_sz = 0;
    st = file->GetInfo(file, &gFileInfoGuid, &info_sz, (void *)0);
    if (st == EFI_BUFFER_TOO_SMALL && info_sz) {
        void *info_buf = (void *)0;
        st = gBS->AllocatePool(2 /*EfiLoaderData*/, info_sz, &info_buf);
        if (!EFI_ERROR(st)) {
            UINTN got = info_sz;
            st = file->GetInfo(file, &gFileInfoGuid, &got, info_buf);
            if (!EFI_ERROR(st) && got >= 16) {
                /* EFI_FILE_INFO: Size(u64)@0, FileSize(u64)@8 */
                size = *(uint64_t *)((uint8_t *)info_buf + 8);
            }
            gBS->FreePool(info_buf);
        }
    }
    if (!size) {   /* fallback: старый трюк с позицией */
        st = file->SetPosition(file, 0xFFFFFFFFFFFFFFFFULL);
        if (EFI_ERROR(st)) { log_line("[boot] FAIL: SetPosition(end)"); return st; }
        st = file->GetPosition(file, &size);
        if (EFI_ERROR(st)) { log_line("[boot] FAIL: GetPosition"); return st; }
        file->SetPosition(file, 0);
    }
    serial_str("[fs] size ok\n");
    diag_band(32, 40, 0x40, 0xC0, 0x40);          /* R5 */

    if (!size) { log_line("[boot] FAIL: KERNEL.ELF size == 0"); return EFI_ERROR_BIT | 1; }

    EFI_PHYSICAL_ADDRESS buf = 0;
    UINTN pages = EFI_SIZE_TO_PAGES(size);
    st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &buf);
    if (EFI_ERROR(st)) { log_line("[boot] FAIL: AllocatePages(kernel file)"); return st; }
    serial_str("[fs] pages alloc ok\n");
    diag_band(40, 48, 0x20, 0xC0, 0x80);          /* R6 */

    UINTN read_size = size;
    st = file->Read(file, &read_size, (void *)buf);
    if (EFI_ERROR(st) || read_size != size) {
        log_line("[boot] FAIL: Read(kernel)");
        return EFI_ERROR_BIT | 1;
    }
    serial_str("[fs] read ok\n");
    diag_band(48, 56, 0x00, 0xC0, 0xC0);          /* R7 */
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
    g_bootinfo.fb.phys_base = 0;   /* по умолчанию - нет графики */

    EFI_STATUS st = gBS->LocateProtocol(&gGopGuid, (void *)0, (void **)&gop);
    if (EFI_ERROR(st)) {
        log_line("[boot] GOP not found - kernel will use serial only");
        return;
    }

    /* ищем 1024x768; если нет - оставляем текущий режим */
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
            log_line("[boot] ExitBootServices OK - bye, firmware!");
            return EFI_SUCCESS;
        }
        log_line("[boot] ExitBootServices retry...");
        gBS->FreePages(map_buf, pages);
    }
    log_line("[boot] FAIL: ExitBootServices (5 attempts)");
    return EFI_ERROR_BIT | 1;
}

/* ===================== diag-маркеры: полосы в fb напрямую =====================
 * Рисуем в GOP framebuffer БЕЗ прошивки и консоли. По фото экрана видно,
 * до какого этапа дошла загрузка (даже если падает текст/serial).
 * Легенда v0.2.4 (ядро вшито в BOOTX64.EFI - файловая система не нужна):
 *   M1 синий весь экран   - загрузчик жив, GOP найден, fb пишется
 *   R1..R4 оранжевые слои - (fallback: шаги чтения с ESP) /
 *                           встроенный payload: найден / ELF-магия / размер ок
 *   M2 зелёная полоса     - образ ядра получен и проверен
 *   M3 бирюзовая (y56-64) - PT_LOAD ядра размещены в памяти
 *   M4 пурпурная (y64-96) - ExitBootServices прошёл (прошивка отпустила)
 *   M5 циановая (y96-128) - ставит ядро: kmain отработал инициализацию IDT
 */
static void diag_band(uint32_t y0, uint32_t y1, uint8_t r, uint8_t g, uint8_t b) {
    if (!g_bootinfo.fb.phys_base || g_bootinfo.fb.format > FB_FORMAT_BGR) return;
    if (!g_bootinfo.fb.pitch || !g_bootinfo.fb.width || !g_bootinfo.fb.height) return;
    /* UEFI-семантика байтов в памяти (little-endian u32):
     *   FB_FORMAT_RGB = PixelRedGreenBlue: байт0=R -> u32 = R | G<<8 | B<<16
     *   FB_FORMAT_BGR = PixelBlueGreenRed: байт0=B -> u32 = B | G<<8 | R<<16 */
    uint32_t v = (g_bootinfo.fb.format == FB_FORMAT_RGB)
               ? ((uint32_t)b << 16) | ((uint32_t)g << 8) | r
               : ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
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
        screen_print(u"AresOS loader (BOOTX64.EFI) v0.6.2\r\n");
    }

    /* графику поднимаем ПЕРВОЙ (SetMode сам очищает экран) - нужна для маркеров */
    setup_graphics();
    diag_band(0, 0xFFFFFFFF, 0x00, 0x00, 0xC0);      /* M1: весь экран синий */
    serial_str("[diag] M1 loader-alive GOP-ok\n");
    log_line("[boot] stage 1: console up");

    /* сторожевой таймер - выключить (иначе ребут через 5 минут) */
    gBS->SetWatchdogTimer(0, 0, 0, (const CHAR16 *)0);

    uint8_t *kbuf = (uint8_t *)0;
    UINTN ksize = 0;
    EFI_STATUS st = read_kernel_embedded(&kbuf, &ksize);
    if (EFI_ERROR(st)) {
        serial_str("[boot] no embedded payload - fallback to KERNEL.ELF on ESP\n");
        st = read_kernel_file(&kbuf, &ksize);
        if (EFI_ERROR(st)) goto hang;
    }
    diag_band(0, 32, 0x00, 0xC0, 0x00);              /* M2: ядро у нас */
    serial_str("[diag] M2 kernel-file-read-ok\n");

    uint64_t entry;
    st = load_kernel_segments(kbuf, &entry);
    if (EFI_ERROR(st)) goto hang;
    diag_band(56, 64, 0x00, 0xA8, 0xA8);             /* M3: PT_LOAD размещены */
    serial_str("[diag] M3 segments-loaded-ok\n");
    log_hex("[boot] kernel entry = ", entry);

    g_bootinfo.magic = BOOTINFO_MAGIC;

    /* M4: RSDP из EFI ConfigurationTable (GUID ACPI 2.0/1.0).
       На UEFI-загрузке скан 0xE0000 может НИЧЕГО не найти - это единственный
       надёжный путь. Делать ДО ExitBootServices. */
    {
        g_bootinfo.rsdp_phys = 0;
        static const EFI_GUID ACPI20 = { 0x8868e871, 0xe4f1, 0x11d3,
            { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
        static const EFI_GUID ACPI10 = { 0xeb9d2d30, 0x2d88, 0x11d3,
            { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
        typedef struct { EFI_GUID guid; void *table; } cfg_ent_t;
        cfg_ent_t *ct = (cfg_ent_t *)gST->ConfigurationTable;
        for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
            const uint8_t *g = (const uint8_t *)&ct[i].guid;
            int is20 = 1, is10 = 1;
            for (int b = 0; b < 16; b++) {
                if (g[b] != ((const uint8_t *)&ACPI20)[b]) is20 = 0;
                if (g[b] != ((const uint8_t *)&ACPI10)[b]) is10 = 0;
            }
            if (is20 || is10) {
                g_bootinfo.rsdp_phys = (uint64_t)(uintptr_t)ct[i].table;
                serial_str("[boot] ACPI RSDP from EFI cfg table ok\n");
                break;
            }
        }
        if (!g_bootinfo.rsdp_phys)
            serial_str("[boot] no RSDP in EFI cfg table - kernel scans BIOS area\n");
    }

    /* ВАЖНО: после этого вызова никакие Boot Services больше нельзя трогать */
    st = exit_boot_services(image_handle);
    if (EFI_ERROR(st)) goto hang;
    /* прошивка отдала управление; fb - просто память, писать всё ещё можно */
    diag_band(64, 96, 0xC0, 0x00, 0xC0);             /* M4: ExitBootServices OK */
    serial_str("[diag] M4 exit-boot-services-ok\n");

    __asm__ volatile ("cli");
    ((void (*)(bootinfo_t *))entry)(&g_bootinfo);

    __builtin_unreachable();

hang:
    log_line("[boot] FATAL - system halted");
    /* аварийное состояние: полосы-"усы" ниже маркеров, чтобы было видно на фото */
    for (uint32_t yy = 128; yy < 768; yy += 16)
        diag_band(yy, yy + 8, 0xC0, 0x20, 0x00);
    for (;;) __asm__ volatile ("hlt");
}
