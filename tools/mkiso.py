#!/usr/bin/env python3
"""
AresOS mkiso — генератор загрузочного ISO (ISO9660 + El Torito, UEFI).

Зачем: виртуальные машины (VirtualBox/VMware) в диалоге выбора CD-диска
понимают только .iso. Образ грузится прошивкой UEFI через El Torito
(no-emulation, platform 0xEF): внутри ISO лежит FAT-образ efiboot.img
с /EFI/BOOT/BOOTX64.EFI и KERNEL.ELF — точно такой же, как на флешке.

Usage:
  mkiso.py out.iso --file KERNEL.ELF=build/kernel.elf --file EFI/BOOT/BOOTX64.EFI=build/BOOTX64.EFI
  mkiso.py out.iso --list         # разобрать и проверить готовый ISO
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkesp import build_floppy_image          # noqa: E402

SECTOR = 2048
VOLDATE = b"2026082300000000"


def _both16(v):  return struct.pack("<H", v) + struct.pack(">H", v)
def _both32(v):  return struct.pack("<I", v) + struct.pack(">I", v)
def _pad(s, n):  return s.ljust(n, b" ")


def dir_record(lba, size, flags, name, vol_date=True):
    """Запись каталога ISO9660 (ECMA-119, 9.1). name: bytes (уже в ISO-виде)."""
    ln_fi = len(name)
    rec = bytearray()
    rec.append(0)                       # LEN_DR — заполним в конце
    rec.append(0)                       # ext attr length
    rec += _both32(lba)
    rec += _both32(size)
    rec += bytes([126, 8, 23, 0, 0, 0, 0]) if vol_date else bytes(7)   # дата
    rec.append(flags)                   # 0x02 = каталог
    rec.append(0)
    rec.append(0)
    rec += _both16(1)                   # volume sequence number
    rec.append(ln_fi)
    rec += name
    if ln_fi % 2 == 0:
        rec.append(0)                   # выравнивание до чётной длины
    rec[0] = len(rec)
    assert len(rec) % 2 == 0
    return bytes(rec)


class IsoBuilder:
    """Дерево: {"EFI": {"BOOT": {"BOOTX64.EFI": bytes, "EFIBOOT.IMG": bytes}},
                "KERNEL.ELF": bytes}  —-только нужная нам глубина."""

    def __init__(self):
        self.files = {}          # ISO-путь -> bytes (кроме каталогов)
        self.dirs = []           # ISO-пути каталогов (без корня)

    def add_file(self, path, data):
        self.files[path] = data
        parts = path.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            d = "/".join(parts[:i])
            if d not in self.dirs:
                self.dirs.append(d)

    # ---------- раскладка секторов ----------
    def layout(self):
        dirs_sorted = sorted(self.dirs, key=lambda d: d.count("/"))
        extents = {}
        sec = 21                                      # 0..20 — служебные
        # корень
        extents[""] = sec; sec += 1
        for d in dirs_sorted:
            body_size = self._dir_body_size(d)
            n = (body_size + SECTOR - 1) // SECTOR
            extents[d] = sec; sec += n
        for path in sorted(self.files):
            n = (len(self.files[path]) + SECTOR - 1) // SECTOR
            extents["F:" + path] = sec; sec += n
        self.catalog_sec = sec; sec += 1
        self.total_secs = sec
        return extents, dirs_sorted

    def _dir_body_size(self, d):
        size = 2 * 33                                 # "." и ".."
        prefix = (d + "/") if d else ""
        for child in self.dirs:
            if self._parent(child) == d:
                size += 33 + len(self._iso_name(child, is_dir=True)) + 1
        for path in self.files:
            if self._parent(path) == d:
                size += 33 + len(self._iso_name(path)) + 1
        return size

    @staticmethod
    def _parent(path):
        return "/".join(path.split("/")[:-1])

    @staticmethod
    def _iso_name(path, is_dir=False):
        n = path.split("/")[-1].upper()
        return n.encode() if is_dir else (n + ";1").encode()

    # ---------- path table ----------
    def path_tables(self, extents, dirs_sorted):
        def entry(lba, parent_idx, name):
            e = bytes([len(name), 0]) + struct.pack("<I", lba) + struct.pack("<H", parent_idx) + name
            return e + (b"\0" if len(name) % 2 else b"")
        entries = [entry(extents[""], 1, b"\x00")]
        for i, d in enumerate(dirs_sorted, start=2):
            parent = self._parent(d)
            pidx = 1 if parent == "" else dirs_sorted.index(parent) + 2
            entries.append(entry(extents[d], pidx, d.split("/")[-1].upper().encode()))
        ltab = b"".join(entries)
        # M-таблица — то же в big-endian
        def entry_m(lba, parent_idx, name):
            e = bytes([len(name), 0]) + struct.pack(">I", lba) + struct.pack(">H", parent_idx) + name
            return e + (b"\0" if len(name) % 2 else b"")
        mtab = entry_m(extents[""], 1, b"\x00")
        for i, d in enumerate(dirs_sorted, start=2):
            parent = self._parent(d)
            pidx = 1 if parent == "" else dirs_sorted.index(parent) + 2
            mtab += entry_m(extents[d], pidx, d.split("/")[-1].upper().encode())
        return ltab, mtab

    # ---------- тела каталогов ----------
    def dir_body(self, d, extents, dirs_sorted):
        self_lba = extents[d if d else ""]
        parent_d = self._parent(d) if d else ""
        parent_lba = extents[parent_d if parent_d else ""]
        body = dir_record(self_lba, SECTOR, 0x02, b"\x00")
        body += dir_record(parent_lba, SECTOR, 0x02, b"\x01")
        for child in dirs_sorted:
            if self._parent(child) == d:
                body += dir_record(extents[child], SECTOR, 0x02,
                                   self._iso_name(child, is_dir=True))
        for path in sorted(self.files):
            if self._parent(path) == d:
                body += dir_record(extents["F:" + path], len(self.files[path]), 0x00,
                                   self._iso_name(path))
        return body + bytes((-len(body)) % SECTOR)

    # ---------- том ----------
    def build(self, efiboot_lba, efiboot_size):
        extents, dirs_sorted = self.layout()
        img = bytearray(self.total_secs * SECTOR)

        ltab, mtab = self.path_tables(extents, dirs_sorted)
        img[19 * SECTOR:19 * SECTOR + len(ltab)] = ltab
        img[20 * SECTOR:20 * SECTOR + len(mtab)] = mtab

        # каталоги и файлы
        img[extents[""] * SECTOR:(extents[""] + 1) * SECTOR] = self.dir_body("", extents, dirs_sorted)
        for d in dirs_sorted:
            body = self.dir_body(d, extents, dirs_sorted)
            img[extents[d] * SECTOR:extents[d] * SECTOR + len(body)] = body
        for path, fsec in ((p, extents["F:" + p]) for p in self.files):
            data = self.files[path]
            img[fsec * SECTOR:fsec * SECTOR + len(data)] = data

        # ---- PVD ----
        pvd = bytearray(SECTOR)
        pvd[0] = 1
        pvd[1:6] = b"CD001"
        pvd[6] = 1
        pvd[8:40] = _pad(b"ARESOS", 32)                      # system id
        pvd[40:72] = _pad(b"ARESOS_BOOT", 32)                # volume id
        pvd[80:88] = _both32(self.total_secs)
        pvd[120:124] = _both16(1)
        pvd[124:128] = _both16(1)
        pvd[128:132] = _both16(SECTOR)
        pvd[132:140] = _both32(len(ltab))
        struct.pack_into("<I", pvd, 140, 19)                 # L path table LBA
        struct.pack_into("<I", pvd, 144, 0)
        struct.pack_into(">I", pvd, 148, 20)                 # M path table LBA
        struct.pack_into(">I", pvd, 152, 0)
        pvd[156:190] = dir_record(extents[""], SECTOR, 0x02, b"\x00")
        pvd[190:318] = _pad(b"AresOS", 128)
        pvd[318:446] = _pad(b"AresOS Project", 128)
        pvd[446:574] = _pad(b"mkiso.py", 128)
        pvd[813:830] = VOLDATE + b"\0"
        pvd[830:847] = VOLDATE + b"\0"
        pvd[847:864] = _pad(b"0", 16) + b"\0"
        pvd[864:881] = VOLDATE + b"\0"
        pvd[881] = 1
        img[16 * SECTOR:17 * SECTOR] = pvd

        # ---- Boot Record (El Torito) ----
        br = bytearray(SECTOR)
        br[0] = 0
        br[1:6] = b"CD001"
        br[6] = 1
        br[7:39] = _pad(b"EL TORITO SPECIFICATION", 32)
        struct.pack_into("<I", br, 71, self.catalog_sec)
        img[17 * SECTOR:18 * SECTOR] = br

        # ---- Terminator ----
        img[18 * SECTOR] = 255
        img[18 * SECTOR + 1:18 * SECTOR + 6] = b"CD001"
        img[18 * SECTOR + 6] = 1

        # ---- Boot Catalog ----
        cat = bytearray(SECTOR)
        # validation entry
        cat[0] = 1
        cat[1] = 0xEF                                        # platform: UEFI
        cat[4:28] = _pad(b"AresOS Boot CD", 24)
        cat[30] = 0x55
        cat[31] = 0xAA
        words = struct.unpack("<16H", bytes(cat[:32]))
        csum = (-sum(words)) & 0xFFFF
        struct.pack_into("<H", cat, 28, csum)
        # initial/default entry
        e = 32
        cat[e] = 0x88                                        # bootable
        cat[e + 1] = 0x00                                    # no emulation
        struct.pack_into("<H", cat, e + 2, 0)                # load segment (0 = default)
        cat[e + 4] = 0                                       # system type
        struct.pack_into("<H", cat, e + 6, (efiboot_size + 511) // 512)  # вирт. сектора
        struct.pack_into("<I", cat, e + 8, efiboot_lba)      # LBA boot-образа
        img[self.catalog_sec * SECTOR:(self.catalog_sec + 1) * SECTOR] = cat

        return bytes(img)


def build_iso(fs_files, efiboot_files):
    """fs_files: файлы в корне ISO (читаемость). efiboot_files: содержимое efiboot.img."""
    efiboot = build_floppy_image(efiboot_files)

    b = IsoBuilder()
    # efiboot.img обязателен в каталоге /EFI/BOOT (El Torito ссылается по LBA)
    efiboot_lba_holder = {}
    b.add_file("EFI/BOOT/EFIBOOT.IMG", efiboot)
    b.add_file("EFI/BOOT/BOOTX64.EFI", efiboot_files["EFI/BOOT/BOOTX64.EFI"])
    for path, data in fs_files.items():
        b.add_file(path, data)

    extents, _ = b.layout()
    efiboot_lba = extents["F:EFI/BOOT/EFIBOOT.IMG"]
    return b.build(efiboot_lba, len(efiboot)), b.total_secs


# ============================================================
#  Проверка: читаем ISO обратно
# ============================================================
def parse_dir(img, lba, prefix, out_depth=0):
    size = SECTOR
    body = img[lba * SECTOR:(lba + 1) * SECTOR]
    i = 0
    while i < size:
        ln = body[i]
        if ln == 0:
            break
        rec = body[i:i + ln]
        ext = struct.unpack_from("<I", rec, 2)[0]
        dlen = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name = rec[33:33 + rec[32]]
        if name not in (b"\x00", b"\x01"):
            disp = name.decode().rstrip(";1")
            if flags & 0x02:
                print(f"  {prefix}{disp}/ (LBA {ext})")
                parse_dir(img, ext, prefix + "  ")
            else:
                print(f"  {prefix}{disp}  LBA={ext} size={dlen}")
        i += ln


def list_iso(path):
    img = open(path, "rb").read()
    assert img[16 * SECTOR + 1:16 * SECTOR + 6] == b"CD001", "нет PVD"
    total = struct.unpack_from("<I", img, 16 * SECTOR + 80)[0]
    assert img[17 * SECTOR + 7:17 * SECTOR + 30].startswith(b"EL TORITO"), "нет El Torito"
    cat_lba = struct.unpack_from("<I", img, 17 * SECTOR + 71)[0]
    print(f"[mkiso] ISO9660: том {total} секторов ({total * SECTOR // 1024} КиБ)")

    cat = img[cat_lba * SECTOR:(cat_lba + 1) * SECTOR]
    assert cat[0] == 1 and cat[1] == 0xEF and cat[30] == 0x55 and cat[31] == 0xAA
    words = struct.unpack("<16H", bytes(cat[:32]))
    assert sum(words) & 0xFFFF == 0, "контрольная сумма каталога не сошлась"
    boot = cat[32]
    media = cat[33]
    count = struct.unpack_from("<H", cat, 38)[0]
    lba = struct.unpack_from("<I", cat, 40)[0]
    print(f"[mkiso] El Torito: boot=0x{boot:02x} media={'no-emulation' if media == 0 else media} "
          f"platform=UEFI ✓, образ LBA={lba}, {count} вирт.секторов")
    assert boot == 0x88 and media == 0

    root_lba = struct.unpack_from("<I", img, 16 * SECTOR + 156 + 2)[0]
    print(f"[mkiso] содержимое ISO:")
    parse_dir(img, root_lba, "")
    print("[mkiso] ISO структура валидна — готов для виртуалок")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image")
    ap.add_argument("--file", action="append", default=[],
                    help="DEST=SRC — файлы раскладки (как в mkesp)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    if a.list:
        list_iso(a.image)
        return

    layout_files = {}
    for spec in a.file:
        dest, _, src = spec.partition("=")
        assert src, f"нужен формат DEST=SRC: {spec!r}"
        layout_files[dest.lstrip("/")] = open(src, "rb").read()
    assert "EFI/BOOT/BOOTX64.EFI" in layout_files, "нужен BOOTX64.EFI"
    assert "KERNEL.ELF" in layout_files, "нужен KERNEL.ELF"

    # efiboot получает всё как на флешке; в корень ISO дублируем для читаемости
    iso, total = build_iso({"KERNEL.ELF": layout_files["KERNEL.ELF"]}, layout_files)
    with open(a.image, "wb") as f:
        f.write(iso)
    print(f"[mkiso] {a.image}: ISO9660+El Torito, {total} секторов "
          f"({len(iso) // 1024} КиБ)")
    list_iso(a.image)


if __name__ == "__main__":
    main()
