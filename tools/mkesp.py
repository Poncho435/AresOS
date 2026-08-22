#!/usr/bin/env python3
"""
AresOS mkesp — генератор FAT12-образа системного раздела (ESP) 1.44 МиБ.

UEFI (и OVMF) читают FAT12/16/32 как ESP. Наш загрузчик лежит по
стандартному пути съёмного носителя: /EFI/BOOT/BOOTX64.EFI — firmware
подхватит его автоматически. Ядро — /KERNEL.ELF.

Usage:
  mkesp.py out.img --file KERNEL.ELF=build/kernel.elf --file EFI/BOOT/BOOTX64.EFI=build/BOOTX64.EFI
  mkesp.py out.img --list      # распечатать содержимое образа (проверка)
"""
import argparse
import struct
import sys

SECTOR = 512
TOTAL_SECTORS = 2880          # 1.44 МиБ
SPC = 1                       # секторов на кластер
RESERVED = 1
N_FATS = 2
ROOT_ENTRIES = 224
SPF = 9                       # секторов на FAT (классика для 1.44M)
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR   # 14
FIRST_DATA_SECTOR = RESERVED + N_FATS * SPF + ROOT_SECTORS   # 33
MEDIA = 0xF0
EOC = 0xFF8                   # end-of-chain (FAT12: >= 0xFF8)


def fat12_set(fat, n, val):
    off = n + n // 2
    if n % 2 == 0:
        fat[off] = val & 0xFF
        fat[off + 1] = (fat[off + 1] & 0xF0) | ((val >> 8) & 0x0F)
    else:
        fat[off] = (fat[off] & 0x0F) | ((val << 4) & 0xF0)
        fat[off + 1] = (val >> 4) & 0xFF


def fat12_get(fat, n):
    off = n + n // 2
    if n % 2 == 0:
        return fat[off] | ((fat[off + 1] & 0x0F) << 8)
    return (fat[off] >> 4) | (fat[off + 1] << 4)


def sfn(name):
    """'DIR/FILE.TXT' -> имя в формате 8.3 (11 байт). Только короткие имена!"""
    name = name.upper()
    base, _, ext = name.partition(".")
    assert 1 <= len(base) <= 8 and len(ext) <= 3, f"имя не 8.3: {name!r}"
    assert all(c not in name for c in ' "\\/:*?"<>|'), f"bad chars: {name!r}"
    return base.ljust(8) + ext.ljust(3)


class Image:
    def __init__(self):
        self.img = bytearray(TOTAL_SECTORS * SECTOR)
        self.fat = bytearray(SPF * SECTOR)
        self.fat[0] = MEDIA
        self.fat[1] = self.fat[2] = 0xFF     # два служебных элемента
        self.next_cluster = 2
        self.data_buf = {}                   # cluster -> bytes
        self._write_boot_sector()

    # ---------- низкий уровень ----------
    def _write_boot_sector(self):
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x3C\x90"                    # jmp + nop
        bs[3:11] = b"ARESOS  "                       # OEM
        struct.pack_into("<H", bs, 11, SECTOR)       # bytes/sector
        bs[13] = SPC
        struct.pack_into("<H", bs, 14, RESERVED)
        bs[16] = N_FATS
        struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
        struct.pack_into("<H", bs, 19, TOTAL_SECTORS if TOTAL_SECTORS < 0x10000 else 0)
        bs[21] = MEDIA
        struct.pack_into("<H", bs, 22, SPF)
        struct.pack_into("<H", bs, 24, 18)           # sectors/track
        struct.pack_into("<H", bs, 26, 2)            # heads
        struct.pack_into("<I", bs, 28, 0)            # hidden
        struct.pack_into("<I", bs, 32, 0)            # total32
        bs[36] = 0                                   # drive
        bs[38] = 0x29                                # ext boot signature
        struct.pack_into("<I", bs, 39, 0xA4E501)     # volume id
        bs[43:54] = b"ARESOS ESP "
        bs[54:62] = b"FAT12   "
        bs[510:512] = b"\x55\xAA"
        self.img[0:SECTOR] = bs

    def alloc_clusters(self, size):
        """Цепочка кластеров под size байт. Возвращает список номеров."""
        n = max(1, (size + SPC * SECTOR - 1) // (SPC * SECTOR))
        chain = list(range(self.next_cluster, self.next_cluster + n))
        self.next_cluster += n
        assert self.next_cluster - 2 < (TOTAL_SECTORS - FIRST_DATA_SECTOR), "образ переполнен"
        for i, cl in enumerate(chain):
            fat12_set(self.fat, cl, chain[i + 1] if i + 1 < n else 0xFFF)
        return chain

    def put_data(self, chain, payload):
        for i, cl in enumerate(chain):
            chunk = payload[i * SECTOR * SPC:(i + 1) * SECTOR * SPC]
            self.data_buf[cl] = chunk

    # ---------- каталоги ----------
    def dirent(self, name_83, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name_83.encode()
        e[11] = attr
        struct.pack_into("<H", e, 26, cluster if cluster < 0x10000 else 0)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    def build_dir(self, entries, self_cluster, parent_cluster):
        """entries: список байтовых 32-байтовых записей; добавляет . и .."""
        body = self.dirent(".          ", 0x10, self_cluster, 0)
        body += self.dirent("..         ", 0x10, parent_cluster, 0)
        for e in entries:
            body += e
        return body

    def add_tree(self, files):
        """
        files: {"KERNEL.ELF": bytes, "EFI/BOOT/BOOTX64.EFI": bytes}
        Строит каталоги/подкаталоги, раскидывает данные по кластерам.
        """
        root_entries = []
        dir_bodies = {}     # cluster -> bytes тела каталога
        subdirs = {}        # "EFI", "EFI/BOOT" -> cluster

        # собрать множество каталогов из путей
        for path in files:
            parts = path.split("/")[:-1]
            for i in range(1, len(parts) + 1):
                d = "/".join(parts[:i])
                subdirs.setdefault(d, None)

        # выделить кластеры каталогам (порядок: верхние уровни первыми)
        for d in sorted(subdirs, key=lambda x: x.count("/")):
            chain = self.alloc_clusters(SPC * SECTOR)
            subdirs[d] = chain[0]

        # тела каталогов: файлы и подкаталоги
        for d, cl in subdirs.items():
            parent = "/".join(d.split("/")[:-1])
            parent_cl = subdirs.get(parent, 0)
            children = []
            # подкаталоги этого каталога
            for d2, cl2 in subdirs.items():
                if d2 != d and "/".join(d2.split("/")[:-1]) == d:
                    children.append(self.dirent(sfn(d2.split("/")[-1]), 0x10, cl2, 0))
            # файлы этого каталога
            for path, content in files.items():
                if "/".join(path.split("/")[:-1]) == d:
                    chain = self.alloc_clusters(len(content))
                    self.put_data(chain, content)
                    children.append(self.dirent(sfn(path.split("/")[-1]), 0x20, chain[0], len(content)))
            dir_bodies[cl] = self.build_dir(children, cl, parent_cl)

        # корневой каталог
        for d, cl in subdirs.items():
            if "/" not in d:
                root_entries.append(self.dirent(sfn(d), 0x10, cl, 0))
        for path, content in files.items():
            if "/" not in path:
                chain = self.alloc_clusters(len(content))
                self.put_data(chain, content)
                root_entries.append(self.dirent(sfn(path), 0x20, chain[0], len(content)))

        root_body = b"".join(root_entries)
        assert len(root_body) <= ROOT_SECTORS * SECTOR, "слишком много файлов в корне"

        # записать всё в образ
        root_off = (RESERVED + N_FATS * SPF) * SECTOR
        self.img[root_off:root_off + len(root_body)] = root_body
        for cl, body in dir_bodies.items():
            self.put_data([cl], body)   # тело каталога тоже данные в кластере
        for cl, chunk in self.data_buf.items():
            off = (FIRST_DATA_SECTOR + (cl - 2) * SPC) * SECTOR
            self.img[off:off + len(chunk)] = chunk
        # FAT в обе копии
        for f in range(N_FATS):
            off = (RESERVED + f * SPF) * SECTOR
            self.img[off:off + SPF * SECTOR] = self.fat

    def save(self, path):
        with open(path, "wb") as f:
            f.write(self.img)


# ---------- чтение обратно (самопроверка) ----------
def list_image(path):
    img = open(path, "rb").read()
    assert img[510:512] == b"\x55\xAA", "нет boot-сигнатуры"
    spf = struct.unpack_from("<H", img, 22)[0]
    reserved = struct.unpack_from("<H", img, 14)[0]
    nfats = img[16]
    root_entries = struct.unpack_from("<H", img, 17)[0]
    spc = img[13]
    root_sec = root_entries * 32 // SECTOR
    first_data = reserved + nfats * spf + root_sec
    fat = img[reserved * SECTOR:(reserved + spf) * SECTOR]

    def cluster_bytes(cl):
        off = (first_data + (cl - 2) * spc) * SECTOR
        return img[off:off + SECTOR * spc]

    def walk_dir(body, prefix):
        for i in range(0, len(body), 32):
            e = body[i:i + 32]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5 or e[11] == 0x0F:
                continue
            name = e[0:11].decode()
            if name.startswith(".") or name.startswith(".."):
                if e[11] == 0x10 and e[0] == 0x2E:
                    continue
            cl = struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            if e[11] == 0x10:
                print(f"  {prefix}{name.strip()}/   (cluster {cl})")
                walk_dir(cluster_bytes(cl), prefix + "  ")
            else:
                disp = name[:8].strip() + (("." + name[8:].strip()) if name[8:].strip() else "")
                print(f"  {prefix}{disp}   cluster={cl} size={size}")

    print(f"[mkesp] listing {path}: FAT12, SPF={spf}, root entries={root_entries}")
    root_off = (reserved + nfats * spf) * SECTOR
    walk_dir(img[root_off:root_off + root_sec * SECTOR], "")
    print("[mkesp] образ читается корректно")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image")
    ap.add_argument("--file", action="append", default=[],
                    help="DEST_IN_IMAGE=local/path (DEST — путь в образе, 8.3-имена)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    if a.list:
        list_image(a.image)
        return

    files = {}
    for spec in a.file:
        dest, _, src = spec.partition("=")
        assert src, f"нужен формат DEST=SRC: {spec!r}"
        files[dest.lstrip("/")] = open(src, "rb").read()

    im = Image()
    im.add_tree(files)
    im.save(a.image)
    total = sum(len(v) for v in files.values())
    print(f"[mkesp] {a.image}: {len(files)} файл(ов), payload {total} байт, образ {TOTAL_SECTORS * SECTOR} байт")
    list_image(a.image)


if __name__ == "__main__":
    main()
