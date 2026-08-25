#!/usr/bin/env python3
"""AresOS mkpe — упаковывает плоский бинарник в настоящий PE32+ (.exe).

Это «обратный» инструмент к tools/elf2efi.py и заготовка PE-парсера для M8:
здесь мы _создаём_ все структуры по спецификации PE/COFF (MZ-заглушка,
COFF-заголовок, Optional Header PE32+, таблица секций), а kernel/pe.c их потом
_разбирает_ по той же спецификации — стороны не подглядывают друг у друга.

Ограничения v0 (все осознанные):
  • одна секция .text (код+rodata приложений пишется flat, без globals)
  • без импортов/экспорта/ресурсов → kernel/pe.c обязан отвергать импорты
    (резолвер импортов = M8, WinAPI-слой)
  • .reloc генерируется пустым: наши приложения PIC (RIP-relative), но
    kernel/pe.c обязан УМЕТЬ применять DIR64-релоки, если база занята

Usage:
  mkpe.py input.bin output.exe [--imagebase 0x400000000] [--subsystem 3]
  mkpe.py --verify file.exe
"""
import struct
import sys

SALIGN = 0x1000
FALIGN = 0x200
MACHINE_AMD64 = 0x8664

# IMAGE_SCN_*
SCN_CNT_CODE = 0x20
SCN_MEM_EXECUTE = 0x20000000
SCN_MEM_READ = 0x40000000


def build(bin_path: str, out_path: str, imagebase: int, subsystem: int) -> None:
    with open(bin_path, "rb") as f:
        code = f.read()
    assert code, "пустой input.bin"

    dos = bytearray(0x80)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x80)          # e_lfanew
    stub = b"This program cannot be run in DOS mode.\r\n$"
    dos[0x40:0x40 + len(stub)] = stub

    text_rva = SALIGN
    raw_size = (len(code) + FALIGN - 1) // FALIGN * FALIGN
    size_of_image = text_rva + (len(code) + SALIGN - 1) // SALIGN * SALIGN

    coff = struct.pack(
        "<HHIIIHH",
        MACHINE_AMD64,          # Machine
        1,                      # NumberOfSections
        0x66A2E500,             # TimeDateStamp (наш, декоративный)
        0, 0,                   # symtab (нет)
        0xF0,                   # SizeOfOptionalHeader
        0x0022,                 # Characteristics: EXECUTABLE | LARGE_ADDRESS_AWARE
    )

    opt = struct.struct = struct.pack(
        "<HBBIIIIIQIIHHHHHHIIIIHHQQQQII",
        0x020B,                 # Magic PE32+
        14, 0,                  # Linker version
        raw_size,               # SizeOfCode
        0, 0,                   # SizeOfInitialized/UninitializedData
        text_rva,               # AddressOfEntryPoint
        text_rva,               # BaseOfCode
        imagebase,              # ImageBase
        SALIGN,                 # SectionAlignment
        FALIGN,                 # FileAlignment
        6, 0, 0, 0, 6, 0,       # OS/Image/Subsystem versions
        0,                      # Win32VersionValue
        size_of_image,          # SizeOfImage
        FALIGN,                 # SizeOfHeaders
        0,                      # CheckSum
        subsystem,              # 3 = CUI (консольное)
        0x0020,                 # DllCharacteristics: NX_COMPAT
        0x100000, 0x1000,       # Stack reserve/commit
        0x100000, 0x1000,       # Heap reserve/commit
        0,                      # LoaderFlags
        16,                     # NumberOfRvaAndSizes
    )
    opt += b"\x00" * (16 * 8)                        # 16 DataDirectory — все нули

    sec = bytearray(40)
    sec[0:8] = b".text\x00\x00\x00"
    struct.pack_into("<IIIIIIHHI", sec, 8,
                     len(code),                       # VirtualSize
                     text_rva,                        # VirtualAddress
                     raw_size, FALIGN,                # SizeOfRawData, PtrToRawData
                     0, 0, 0, 0,
                     SCN_CNT_CODE | SCN_MEM_EXECUTE | SCN_MEM_READ)

    headers = bytes(dos) + b"PE\x00\x00" + coff + opt + bytes(sec)
    headers += b"\x00" * (FALIGN - len(headers) % FALIGN) if len(headers) % FALIGN else b""
    pe = headers + code + b"\x00" * (raw_size - len(code))

    with open(out_path, "wb") as f:
        f.write(pe)
    print(f"[mkpe] {out_path}: PE32+ AMD64, entry RVA={text_rva:#x}, "
          f"ImageBase={imagebase:#x}, image={size_of_image:#x} байт")


def verify(path: str) -> None:
    d = open(path, "rb").read()
    assert d[0:2] == b"MZ", "нет MZ"
    (lfanew,) = struct.unpack_from("<I", d, 0x3C)
    assert d[lfanew:lfanew + 4] == b"PE\x00\x00", "нет PE-сигнатуры"
    mach, nsec, _, _, _, optsz, chars = struct.unpack_from("<HHIIIHH", d, lfanew + 4)
    assert mach == MACHINE_AMD64, f"machine {mach:#x} != AMD64"
    (magic,) = struct.unpack_from("<H", d, lfanew + 24)
    assert magic == 0x020B, f"optional magic {magic:#x} != PE32+"
    ep, base_of_code = struct.unpack_from("<II", d, lfanew + 24 + 16)
    (imagebase,) = struct.unpack_from("<Q", d, lfanew + 24 + 24)
    salign, falign = struct.unpack_from("<II", d, lfanew + 24 + 32)
    (soi, soh, subsys, ndirs) = struct.unpack_from("<IIH", d, lfanew + 24 + 56)[0], \
        struct.unpack_from("<I", d, lfanew + 24 + 60)[0], \
        struct.unpack_from("<H", d, lfanew + 24 + 68)[0], \
        struct.unpack_from("<I", d, lfanew + 24 + 108)[0]
    sec_off = lfanew + 24 + optsz
    name = d[sec_off:sec_off + 8].rstrip(b"\x00").decode()
    vsz, va, raw, rawptr = struct.unpack_from("<IIII", d, sec_off + 8)
    schars, = struct.unpack_from("<I", d, sec_off + 36)
    print(f"[mkpe-verify] PE32+ AMD64 OK: sections={nsec} subsystem={subsys} "
          f"dirs={ndirs} chars={chars:#06x}")
    print(f"[mkpe-verify] ImageBase={imagebase:#x} EntryRVA={ep:#x} "
          f"Image={soi:#x} Headers={soh:#x}")
    print(f"[mkpe-verify] секция {name}: RVA={va:#x} vsize={vsz:#x} "
          f"raw={raw}B @{rawptr:#x} chars={schars:#010x}")
    assert va % salign == 0 and rawptr % falign == 0, "выравнивания нарушены"
    print("[mkpe-verify] структура валидна ✔")


def main() -> None:
    args = sys.argv[1:]
    if args and args[0] == "--verify":
        assert len(args) == 2, "usage: mkpe.py --verify file.exe"
        verify(args[1])
        return
    imagebase, subsystem = 0x400000000, 3
    pos = []
    i = 0
    while i < len(args):
        if args[i] == "--imagebase":
            imagebase = int(args[i + 1], 0); i += 2
        elif args[i] == "--subsystem":
            subsystem = int(args[i + 1], 0); i += 2
        else:
            pos.append(args[i]); i += 1
    assert len(pos) == 2, __doc__
    build(pos[0], pos[1], imagebase, subsystem)


if __name__ == "__main__":
    main()
