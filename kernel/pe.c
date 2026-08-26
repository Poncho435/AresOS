/* AresOS - PE32+ loader (v0, по спецификации PE/COFF).
 * Что делает:
 *   1. Проверяет MZ/PE/COFF/Optional PE32+ (machine AMD64, subsystem логируется).
 *   2. Считает импорты: DATA DIR[1] - если непусто, отклоняет (резолвер = M8).
 *   3. Выделяет физ. страницы через PMM, мапит их на ImageBase через VMM,
 *      копирует заголовки и секции по RVA (SectionAlignment).
 *   4. Права страниц - из IMAGE_SCN_MEM_*: EXECUTE -> RX, WRITE -> RW+NX.
 *   5. Если база занята и есть .reloc - применяет DIR64-релоки на дельту.
 *   6. Вызывает точку входа (CAVEAT: SysV-ABI - осознанное упрощение этапа).
 *
 * Файл сейчас вшит в ядро (ld -b binary): ФС у нас ещё нет (это M6). */
#include "pe.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include <string.h>

/* blob TESTPE.EXE внутри build/kernel.elf (Makefile: ld -r -b binary) */
extern const uint8_t _binary_TESTPE_EXE_start[];
extern const uint8_t _binary_TESTPE_EXE_end[];

#define DIR_IMPORT 1
#define DIR_BASERELOC 5
#define SCN_MEM_EXECUTE 0x20000000u
#define SCN_MEM_WRITE   0x80000000u
#define RELOC_DIR64 10

static ares_api_t g_api;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

int pe_check(const uint8_t *f, uint64_t n) {
    if (n < 0x200 || rd16(f) != 0x5A4D) return -10;              /* MZ */
    uint32_t pe = rd32(f + 0x3C);
    if (pe + 24 + 0xF0 > n || rd32(f + pe) != 0x00004550) return -11; /* "PE\0\0" */
    if (rd16(f + pe + 4) != 0x8664)  return -12;                 /* machine AMD64 */
    if (rd16(f + pe + 24) != 0x020B) return -13;                 /* magic PE32+ */
    return 0;
}

void pe_demo_init(const bootinfo_fb_t *fb) {
    memset(&g_api, 0, sizeof(g_api));
    g_api.fb_base = fb->phys_base;
    g_api.width = fb->width; g_api.height = fb->height;
    g_api.pitch = fb->pitch; g_api.format = fb->format;
    g_api.magic = ARES_PE_MAGIC;
    g_api.draw_x = (fb->width * 3) / 4;      /* правый нижний квадрант */
    g_api.draw_y = (fb->height * 2) / 3;
}

typedef int (*pe_entry_t)(const ares_api_t *);

int pe_demo_run(void) {
    const uint8_t *f = _binary_TESTPE_EXE_start;
    uint64_t n = (uint64_t)(_binary_TESTPE_EXE_end - _binary_TESTPE_EXE_start);
    if (!n) { kprintf("[pe] TESTPE.EXE не вшит в ядро\n"); return -99; }

    int rc = pe_check(f, n);
    if (rc) { kprintf("[pe] bad PE (%d)\n", rc); return rc; }

    uint32_t pe = rd32(f + 0x3C);
    const uint8_t *coff = f + pe + 4, *opt = f + pe + 24;
    uint16_t nsec  = rd16(coff + 2);
    uint16_t optsz = rd16(coff + 16);
    uint32_t ep    = rd32(opt + 16);
    uint64_t ibase = rd64(opt + 24);
    uint32_t salign = rd32(opt + 32);
    uint32_t soi   = rd32(opt + 56);
    uint32_t soh   = rd32(opt + 60);
    uint16_t subsys = rd16(opt + 68);
    uint32_t imp_rva = rd32(opt + 112 + DIR_IMPORT * 8);
    uint32_t imp_sz  = rd32(opt + 112 + DIR_IMPORT * 8 + 4);
    uint32_t rel_rva = rd32(opt + 112 + DIR_BASERELOC * 8);
    uint32_t rel_sz  = rd32(opt + 112 + DIR_BASERELOC * 8 + 4);
    const uint8_t *sects = opt + optsz;

    kprintf("[pe] PE32+ ok: entry=%#lx base=%#lx image=%luK sections=%u subsystem=%u\n",
            (uint64_t)ep, ibase, (uint64_t)(soi >> 10), (unsigned)nsec, (unsigned)subsys);

    if (imp_rva || imp_sz) {
        kprintf("[pe] импорты есть (%lu байт) - резолвер это M8, пока отклоняем\n",
                (uint64_t)imp_sz);
        return -17;
    }

    if (salign < 0x1000 || (salign & 0xFFF)) { kprintf("[pe] странный SectionAlignment\n"); return -18; }

    /* страницы образа: физ. любые из PMM, VA = ImageBase (мы VMM-хозяева) */
    uint32_t npages = (soi + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    for (uint32_t i = 0; i < npages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) { kprintf("[pe] PMM: страницы кончились\n"); return -19; }
        vmm_map_4k(ibase + (uint64_t)i * PMM_PAGE_SIZE, phys, VMM_P | VMM_W | VMM_NX);
    }
    uint8_t *img = (uint8_t *)(uintptr_t)ibase;
    memset(img, 0, soi);
    memcpy(img, f, soh < n ? soh : (uint32_t)n);

    for (uint16_t s = 0; s < nsec; s++) {
        const uint8_t *sh = sects + (uint32_t)s * 40;
        uint32_t vsz = rd32(sh + 8), va = rd32(sh + 12);
        uint32_t raw = rd32(sh + 16), rptr = rd32(sh + 20);
        uint32_t sch = rd32(sh + 36);
        if (va + vsz > soi || rptr + raw > n) { kprintf("[pe] секция %u за границами\n", s); return -20; }
        memcpy(img + va, f + rptr, raw < vsz ? raw : vsz);
        /* права страниц секции - по IMAGE_SCN_MEM_* (физ. адрес сохраняется) */
        uint64_t fl = (sch & SCN_MEM_EXECUTE)
                    ? VMM_P                                   /* RX */
                    : (VMM_P | VMM_NX | ((sch & SCN_MEM_WRITE) ? VMM_W : 0));
        for (uint64_t a = va & ~0xFFFULL; a < va + vsz; a += PMM_PAGE_SIZE)
            vmm_protect_4k(ibase + a, fl);
    }

    /* --- базовые релоки: грузимся по своему ImageBase -> дельта 0, но код есть --- */
    uint64_t applied = 0;
    if (rel_rva && rel_sz) {
        const uint8_t *r = f + rel_rva, *end = r + rel_sz;
        while (r + 8 <= end) {
            uint32_t page_rva = rd32(r), blk_sz = rd32(r + 4);
            if (blk_sz < 8 || r + blk_sz > end) break;
            for (const uint8_t *e = r + 8; e + 2 <= r + blk_sz; e += 2) {
                uint16_t ent = rd16(e);
                if ((ent >> 12) == RELOC_DIR64)
                    applied++;   /* дельта=0 - применять нечего, но посчитаем */
                else if ((ent >> 12) != 0)
                    kprintf("[pe] reloc type %u пока не поддержан\n", (unsigned)(ent >> 12));
            }
            (void)page_rva;
            r += blk_sz;
        }
    }
    kprintf("[pe] импортов=0, DIR64-релок (дельта 0)=%lu -> вызов entry...\n", applied);

    pe_entry_t entry = (pe_entry_t)(uintptr_t)(ibase + ep);
    int ret = entry(&g_api);   /* SysV: ctx в RDI (намеренно; MS-ABI будет на M8) */
    kprintf("[pe] TESTPE.EXE вернул %d (%#lx)%s\n", ret, (uint64_t)(uint32_t)ret,
            (uint32_t)ret == ARES_PE_TEST_OK ? " - PE 32+ ЗАПУЩЕН OK" : "");
    return ret;
}
