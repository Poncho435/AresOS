#!/usr/bin/env python3
"""
AresOS elf2efi — конвертер static-PIE ELF64 в PE32+ EFI-приложение (BOOTX64.EFI).

Почему существует: UEFI понимает только PE/COFF, а наш toolchain (gcc+ld)
выдаёт ELF. Собираем загрузчик как static-PIE и перепаковываем в PE:

  ELF PT_LOAD-сегменты  -> PE-секции (.text/.rdata/.data)
  ELF .rela.dyn (только R_X86_64_RELATIVE) -> PE .reloc (базовые релокации DIR64)

Заодно это наш учебный парсер/сериализатор PE — к M8 (запуск .exe)
знание формата пригодится напрямую.

Usage:
  elf2efi.py input.elf output.efi     # конвертация
  elf2efi.py --verify file.efi        # разбор и проверка готового PE
"""
import struct
import sys

SALIGN = 0x1000   # SectionAlignment
FALIGN = 0x200    # FileAlignment
IMAGE_BASE = 0x400000

RELA_TYPES = {0: "NONE", 1: "64", 2: "PC32", 6: "GLOB_DAT", 7: "JUMP_SLOT", 8: "RELATIVE"}


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]

def ceildiv(a, b): return (a + b - 1) // b
def align(v, a): return (v + a - 1) & ~(a - 1)


class ElfError(Exception):
    pass


def parse_elf(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise ElfError("not an ELF")
    if data[4] != 2 or data[5] != 1:
        raise ElfError("need 64-bit LSB ELF")
    if u16(data, 16) not in (2, 3):   # ET_EXEC или ET_DYN
        raise ElfError("need ET_EXEC/ET_DYN")
    if u16(data, 18) != 0x3E:
        raise ElfError("need EM_X86_64")

    e_entry = u64(data, 24)
    e_phoff = u64(data, 32)
    e_shoff = u64(data, 40)
    e_phentsize = u16(data, 54)
    e_phnum = u16(data, 56)
    e_shentsize = u16(data, 58)
    e_shnum = u16(data, 60)
    e_shstrndx = u16(data, 62)

    # --- program headers: PT_LOAD ---
    loads = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type = u32(data, o)
        if p_type != 1:   # PT_LOAD
            continue
        p_flags = u32(data, o + 4)
        p_offset = u64(data, o + 8)
        p_vaddr = u64(data, o + 16)
        p_filesz = u64(data, o + 32)
        p_memsz = u64(data, o + 40)
        if p_memsz == 0:
            continue
        loads.append({
            "flags": p_flags, "offset": p_offset, "vaddr": p_vaddr,
            "filesz": p_filesz, "memsz": p_memsz,
        })

    if not loads:
        raise ElfError("no PT_LOAD segments")

    # --- секции: имена + .rela.dyn/.rela.plt ---
    sections = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        sections.append({
            "name_off": u32(data, o), "type": u32(data, o + 4),
            "addr": u64(data, o + 16), "offset": u64(data, o + 24),
            "size": u64(data, o + 32), "link": u32(data, o + 40),
            "entsize": u64(data, o + 56),
        })
    shstr = b""
    if 0 < e_shstrndx < e_shnum:
        s = sections[e_shstrndx]
        shstr = data[s["offset"]:s["offset"] + s["size"]]
    for s in sections:
        no = s["name_off"]
        end = shstr.find(b"\0", no)
        s["name"] = shstr[no:end].decode() if shstr and 0 <= end else ""

    relas = []
    for s in sections:
        if s["name"] not in (".rela.dyn", ".rela.plt") or s["entsize"] == 0:
            continue
        n = s["size"] // s["entsize"]
        for i in range(n):
            o = s["offset"] + i * s["entsize"]
            r_offset = u64(data, o)
            r_info = u64(data, o + 8)
            r_type = r_info & 0xFFFFFFFF
            relas.append((r_offset, r_type))

    return data, e_entry, loads, relas


def build_relocations(relas, delta, rva_low, rva_high):
    """R_X86_64_RELATIVE -> PE base relocs. Возвращает список байтов блоков."""
    by_page = {}
    for r_offset, r_type in relas:
        if r_type == 0:   # R_X86_64_NONE
            continue
        if r_type != 8:   # только RELATIVE
            t = RELA_TYPES.get(r_type, str(r_type))
            raise ElfError(f"unsupported reloc type {t} at 0x{r_offset:x} — "
                           "загрузчик должен быть чистым static-PIE (только RELATIVE)")
        rva = r_offset + delta
        if not (rva_low <= rva < rva_high):
            raise ElfError(f"reloc RVA 0x{rva:x} вне секций")
        page = rva & ~0xFFF
        by_page.setdefault(page, []).append(rva & 0xFFF)

    blocks = b""
    for page in sorted(by_page):
        offs = by_page[page]
        n = len(offs)
        if n % 2:  # выравнивание записей до 4 байт
            offs = offs + [None]
        block = struct.pack("<II", page, 8 + 2 * len(offs))
        for o in offs:
            block += struct.pack("<H", 0 if o is None else 0xA000 | o)  # DIR64
        blocks += block
    return blocks


def convert(inp, outp):
    data, e_entry, loads, relas = parse_elf(inp)

    # --- макет PE-секций ---
    nsec_prelim = len(loads) + (1 if relas else 0)
    headers_size = 0x80 + 4 + 20 + 0xF0 + nsec_prelim * 40
    delta = align(headers_size, SALIGN)   # все vaddr сдвигаются на delta

    sections_out = []
    for idx, seg in enumerate(loads):
        exec_, write_ = bool(seg["flags"] & 1), bool(seg["flags"] & 2)
        name = ".text" if exec_ else (".data" if write_ else ".rdata")
        if any(s["name"] == name for s in sections_out):
            name += str(idx)
        rva = seg["vaddr"] + delta
        if exec_:
            chars = 0x60000020          # CODE | EXECUTE | READ
        elif write_:
            chars = 0xC0000040          # INITIALIZED_DATA | READ | WRITE
        else:
            chars = 0x40000040          # INITIALIZED_DATA | READ
        sections_out.append({
            "name": name, "rva": rva, "vsize": seg["memsz"],
            "raw": data[seg["offset"]:seg["offset"] + seg["filesz"]],
            "chars": chars,
        })

    # vaddr-диапазон для валидации релокаций
    rva_low = min(s["rva"] for s in sections_out)
    rva_high = max(s["rva"] + max(s["vsize"], 1) for s in sections_out)

    reloc_blob = b""
    reloc_sec = None
    if relas:
        reloc_blob = build_relocations(relas, delta, rva_low, rva_high)
        reloc_rva = max(s["rva"] + s["vsize"] for s in sections_out)
        reloc_rva = align(reloc_rva, SALIGN)
        if reloc_blob:
            reloc_sec = {
                "name": ".reloc", "rva": reloc_rva, "vsize": len(reloc_blob),
                "raw": reloc_blob, "chars": 0x42000040,  # INIT_DATA | DISCARDABLE | READ
            }
            sections_out.append(reloc_sec)

    sections_out.sort(key=lambda s: s["rva"])

    # --- заголовки ---
    nsec = len(sections_out)
    headers_size = align(0x80 + 4 + 20 + 0xF0 + nsec * 40, FALIGN)

    file_off = headers_size
    for s in sections_out:
        s["raw_ptr"] = file_off
        s["raw_size"] = align(len(s["raw"]), FALIGN)
        file_off += s["raw_size"]

    size_of_image = align(max(s["rva"] + max(s["vsize"], s["raw_size"]) for s in sections_out), SALIGN)
    size_of_code = sum(s["raw_size"] for s in sections_out if s["chars"] & 0x20)
    size_of_idata = sum(s["raw_size"] for s in sections_out if s["chars"] & 0x40)
    entry_rva = e_entry + delta

    out = bytearray()
    # DOS header
    out += b"MZ" + b"\0" * 0x3A
    out += struct.pack("<I", 0x80)          # e_lfanew
    out += b"\0" * (0x80 - len(out))
    # PE signature
    out += b"PE\0\0"
    # COFF header
    out += struct.pack("<HHIIIHH",
                       0x8664,              # Machine: AMD64
                       nsec,
                       0,                   # TimeDateStamp
                       0, 0,                # символы отсутствуют
                       0xF0,                # SizeOfOptionalHeader (PE32+)
                       0x2022)              # EXECUTABLE | LARGE_ADDRESS_AWARE | (DEBUG_STRIPPED)
    # Optional header (PE32+)
    opt = struct.pack("<HBBIIII", 0x20B, 6, 0, size_of_code, size_of_idata, 0,
                      entry_rva)
    opt += struct.pack("<I", sections_out[0]["rva"])          # BaseOfCode
    opt += struct.pack("<Q", IMAGE_BASE)                       # ImageBase
    opt += struct.pack("<II", SALIGN, FALIGN)
    opt += struct.pack("<HHHHHH", 0, 0, 0, 0, 6, 0)            # версии ОС/подсистемы
    opt += struct.pack("<I", 0)                                # Win32VersionValue
    opt += struct.pack("<I", size_of_image)
    opt += struct.pack("<I", headers_size)
    opt += struct.pack("<I", 0)                                # CheckSum
    out += opt
    dirs_pre = struct.pack("<HH", 10, 0)                        # Subsystem=EFI_APPLICATION, DllChars
    dirs_pre += struct.pack("<QQQQ", 0x100000, 0x1000, 0x100000, 0x1000)  # stack/heap
    dirs_pre += struct.pack("<II", 0, 16)                       # LoaderFlags, NumberOfRvaAndSizes
    out += dirs_pre
    # 16 data directories
    dirs = [(0, 0)] * 16
    if reloc_sec:
        dirs[5] = (reloc_sec["rva"], len(reloc_blob))           # Base Relocation Table
    for rva, sz in dirs:
        out += struct.pack("<II", rva, sz)
    # section headers
    for s in sections_out:
        name = s["name"].encode()[:8].ljust(8, b"\0")
        out += struct.pack("<8sIIIIIIHHI", name, s["vsize"], s["rva"],
                           s["raw_size"], s["raw_ptr"], 0, 0, 0, 0, s["chars"])
    out += b"\0" * (headers_size - len(out))
    # section bodies
    for s in sections_out:
        assert len(out) == s["raw_ptr"]
        out += s["raw"]
        out += b"\0" * (s["raw_size"] - len(s["raw"]))

    open(outp, "wb").write(bytes(out))
    print(f"[elf2efi] {inp} -> {outp}")
    for s in sections_out:
        print(f"  section {s['name']:8s} RVA=0x{s['rva']:05x} vsize=0x{s['vsize']:05x} "
              f"raw={s['raw_size']}b")
    print(f"  entry RVA=0x{entry_rva:x}  image size=0x{size_of_image:x}  relocs={len(relas)}")


def verify(path):
    data = open(path, "rb").read()
    assert data[:2] == b"MZ", "нет MZ"
    pe_off = u32(data, 0x3C)
    assert data[pe_off:pe_off + 4] == b"PE\0\0", "нет PE-сигнатуры"
    machine = u16(data, pe_off + 4)
    nsec = u16(data, pe_off + 6)
    opt_size = u16(data, pe_off + 20)
    chars = u16(data, pe_off + 22)
    o = pe_off + 24
    magic = u16(data, o)
    entry = u32(data, o + 16)
    image_base = u64(data, o + 24)
    salign = u32(data, o + 32)
    falign = u32(data, o + 36)
    size_img = u32(data, o + 56)
    subsystem = u16(data, o + 68)
    n_dirs = u32(data, o + 108)
    reloc_rva, reloc_sz = u32(data, o + 112 + 5 * 8), u32(data, o + 112 + 5 * 8 + 4)

    print(f"[verify] machine=0x{machine:04x} (AMD64: {'OK' if machine == 0x8664 else 'FAIL'})")
    print(f"[verify] PE32+ magic=0x{magic:03x} {'OK' if magic == 0x20B else 'FAIL'}  chars=0x{chars:04x}")
    print(f"[verify] subsystem={subsystem} (10=EFI_APPLICATION: {'OK' if subsystem == 10 else 'FAIL'})")
    print(f"[verify] imagebase=0x{image_base:x} salign=0x{salign:x} falign=0x{falign:x} image=0x{size_img:x}")
    print(f"[verify] entry RVA=0x{entry:x}")

    shdr = o + opt_size
    min_text, max_text = None, None
    def rva2off(rva):
        for i in range(nsec):
            p = shdr + i * 40
            va = u32(data, p + 12)
            vsz = u32(data, p + 8)
            rpo = u32(data, p + 20)
            rpsz = u32(data, p + 16)
            if va <= rva < va + max(vsz, rpsz):
                return rpo + (rva - va)
        return None
    for i in range(nsec):
        p = shdr + i * 40
        name = data[p:p + 8].split(b"\0")[0].decode(errors="replace")
        vsz, va, rpsz, rpo = u32(data, p + 8), u32(data, p + 12), u32(data, p + 16), u32(data, p + 20)
        ch = u32(data, p + 36)
        print(f"[verify] section {name:8s} RVA=0x{va:05x} vsize={vsz:5d} raw={rpsz:5d}B chars=0x{ch:08x}")
        if name.startswith(".text"):
            min_text, max_text = va, va + max(vsz, rpsz)
    if min_text is not None:
        ok = min_text <= entry < max_text
        print(f"[verify] entry внутри .text: {'OK' if ok else 'FAIL'}")
    # разбор .reloc
    if reloc_rva and n_dirs > 5:
        off = rva2off(reloc_rva)
        end = off + reloc_sz
        nblocks = nentries = 0
        while off and off < end:
            page, size = struct.unpack_from("<II", data, off)
            cnt = (size - 8) // 2
            types = {u16(data, off + 8 + 2 * j) >> 12 for j in range(cnt)}
            bad = types - {0, 10}
            assert not bad, f"странные типы релокаций: {bad}"
            nblocks += 1
            nentries += cnt
            off += size
        print(f"[verify] .reloc: {nblocks} блок(ов), {nentries} записей (DIR64) — OK")
    print("[verify] структура PE валидна — образ готов к загрузке UEFI")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--verify":
        verify(sys.argv[2])
    elif len(sys.argv) == 3:
        convert(sys.argv[1], sys.argv[2])
    else:
        print(__doc__)
        sys.exit(1)
