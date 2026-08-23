#!/usr/bin/env python3
"""
AresOS mkesp — генератор загрузочных образов AresOS.

Два режима:
  (по умолчанию) GPT-диск с разделом ESP (FAT32) — для РЕАЛЬНОГО железа
                 (записать на флешку через Rufus/balenaEtcher/dd) и для QEMU.
  --floppy       цельный FAT12 (1.44 МиБ, без таблицы разделов) — legacy-вариант.

Загрузчик ищется firmware по стандартному пути: /EFI/BOOT/BOOTX64.EFI.

Usage:
  mkesp.py out.img --file KERNEL.ELF=build/kernel.elf --file EFI/BOOT/BOOTX64.EFI=build/BOOTX64.EFI
  mkesp.py out.img --floppy --file ...
  mkesp.py out.img --list          # разобрать и проверить готовый образ (автоопределение формата)
"""
import argparse
import struct
import sys
import zlib

SECTOR = 512


# ============================================================
#  Общие помощники: имена 8.3 и записи каталогов
# ============================================================
def sfn(name):
    """Короткое имя FAT 8.3 в виде 11-байтовой строки."""
    name = name.upper()
    base, _, ext = name.partition(".")
    assert 1 <= len(base) <= 8 and len(ext) <= 3, f"имя не 8.3: {name!r}"
    assert all(c not in name for c in ' "\\/:*?"<>|'), f"bad chars: {name!r}"
    return base.ljust(8) + ext.ljust(3)


def dirent(name_83, attr, cluster, size):
    e = bytearray(32)
    e[0:11] = name_83.encode()
    e[11] = attr
    struct.pack_into("<H", e, 26, cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


# ============================================================
#  FAT12 (цельный образ 1.44 МиБ)
# ============================================================
class Fat12Vol:
    TOTAL_SECTORS = 2880
    SPC, RESERVED, NFATS, SPF = 1, 1, 2, 9
    ROOT_ENTRIES = 224
    MEDIA = 0xF0
    EOC = 0xFF8

    def __init__(self):
        self.vol = bytearray(self.TOTAL_SECTORS * SECTOR)
        self.fat = bytearray(self.SPF * SECTOR)
        self.fat[0:3] = bytes([self.MEDIA, 0xFF, 0xFF])
        self.next_cluster = 2
        self.data_buf = {}
        self._boot()

    @property
    def root_sectors(self):
        return self.ROOT_ENTRIES * 32 // SECTOR

    @property
    def first_data(self):
        return self.RESERVED + self.NFATS * self.SPF + self.root_sectors

    def _boot(self):
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x3C\x90"
        bs[3:11] = b"ARESOS  "
        struct.pack_into("<H", bs, 11, SECTOR)
        bs[13] = self.SPC
        struct.pack_into("<H", bs, 14, self.RESERVED)
        bs[16] = self.NFATS
        struct.pack_into("<H", bs, 17, self.ROOT_ENTRIES)
        struct.pack_into("<H", bs, 19, self.TOTAL_SECTORS)
        bs[21] = self.MEDIA
        struct.pack_into("<H", bs, 22, self.SPF)
        struct.pack_into("<H", bs, 24, 18)
        struct.pack_into("<H", bs, 26, 2)
        struct.pack_into("<I", bs, 28, 0)
        struct.pack_into("<I", bs, 32, 0)
        bs[38] = 0x29
        struct.pack_into("<I", bs, 39, 0xA4E501)
        bs[43:54] = b"ARESOS ESP "
        bs[54:62] = b"FAT12   "
        bs[510:512] = b"\x55\xAA"
        self.vol[0:SECTOR] = bs

    def _fat_set(self, n, val):
        off = n + n // 2
        if n % 2 == 0:
            self.fat[off] = val & 0xFF
            self.fat[off + 1] = (self.fat[off + 1] & 0xF0) | ((val >> 8) & 0x0F)
        else:
            self.fat[off] = (self.fat[off] & 0x0F) | ((val << 4) & 0xF0)
            self.fat[off + 1] = (val >> 4) & 0xFF

    def alloc(self, size):
        n = max(1, (size + self.SPC * SECTOR - 1) // (self.SPC * SECTOR))
        chain = list(range(self.next_cluster, self.next_cluster + n))
        self.next_cluster += n
        for i, cl in enumerate(chain):
            self._fat_set(cl, chain[i + 1] if i + 1 < n else 0xFFF)
        return chain

    def put_data(self, chain, payload):
        for i, cl in enumerate(chain):
            self.data_buf[cl] = payload[i * SECTOR:(i + 1) * SECTOR]

    def root_body(self):
        return getattr(self, "_root_body", b"")

    def set_root_body(self, body):
        self._root_body = body

    def finalize(self):
        root_off = (self.RESERVED + self.NFATS * self.SPF) * SECTOR
        assert len(self._root_body) <= self.root_sectors * SECTOR
        self.vol[root_off:root_off + len(self._root_body)] = self._root_body
        for cl, chunk in self.data_buf.items():
            off = (self.first_data + (cl - 2)) * SECTOR
            self.vol[off:off + len(chunk)] = chunk
        for f in range(self.NFATS):
            off = (self.RESERVED + f * self.SPF) * SECTOR
            self.vol[off:off + self.SPF * SECTOR] = self.fat
        return bytes(self.vol)


# ============================================================
#  FAT32 (том внутри GPT-раздела)
# ============================================================
class Fat32Vol:
    SPC, RESERVED, NFATS, MEDIA = 1, 32, 2, 0xF8
    EOC = 0x0FFFFFF8

    def __init__(self, sectors, hidden=2048):
        self.sectors = sectors
        self.hidden = hidden
        # подбор SPF: FAT должна вместить (clusters+2) 32-битных записей
        spf = 1
        while True:
            data_secs = sectors - self.RESERVED - self.NFATS * spf
            clusters = data_secs // self.SPC
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= spf:
                break
            spf = need
        self.SPF = spf
        self.clusters_total = clusters
        assert self.clusters_total >= 65525, (
            f"слишком мало кластеров ({self.clusters_total}) — это не FAT32 том")
        self.vol = bytearray(sectors * SECTOR)
        self.fat = [0] * (self.SPF * SECTOR // 4)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.next_cluster = 2
        self.used_clusters = 0
        self.data_buf = {}
        self._root_chain = self.alloc(SECTOR * self.SPC)   # корневой каталог
        self._boot()

    def _boot(self):
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"ARESOS  "
        struct.pack_into("<H", bs, 11, SECTOR)
        bs[13] = self.SPC
        struct.pack_into("<H", bs, 14, self.RESERVED)
        bs[16] = self.NFATS
        struct.pack_into("<H", bs, 17, 0)                 # root entries (FAT32: 0)
        struct.pack_into("<H", bs, 19, 0)                 # total16
        bs[21] = self.MEDIA
        struct.pack_into("<H", bs, 22, 0)                 # spf16
        struct.pack_into("<H", bs, 24, 63)
        struct.pack_into("<H", bs, 26, 255)
        struct.pack_into("<I", bs, 28, self.hidden)
        struct.pack_into("<I", bs, 32, self.sectors)
        struct.pack_into("<I", bs, 36, self.SPF)          # spf32
        struct.pack_into("<H", bs, 40, 0)                 # ext flags
        struct.pack_into("<H", bs, 42, 0)                 # fs version
        struct.pack_into("<I", bs, 44, self._root_chain[0])  # root cluster
        struct.pack_into("<H", bs, 48, 1)                 # FSInfo sector
        struct.pack_into("<H", bs, 50, 6)                 # backup boot sector
        bs[64] = 0x80
        bs[66] = 0x29
        struct.pack_into("<I", bs, 67, 0xA4E502)
        bs[71:82] = b"ARESOS ESP "
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xAA"
        self.vol[0:SECTOR] = bs
        self.vol[6 * SECTOR:7 * SECTOR] = bs               # резервная копия
        # FSInfo
        fs = bytearray(SECTOR)
        struct.pack_into("<I", fs, 0, 0x41615252)
        struct.pack_into("<I", fs, 484, 0x61417272)
        struct.pack_into("<I", fs, 488, 0xFFFFFFFF)        # free (заполним в finalize)
        struct.pack_into("<I", fs, 492, self.next_cluster)
        struct.pack_into("<I", fs, 508, 0xAA550000)
        self.vol[SECTOR:2 * SECTOR] = fs

    def alloc(self, size):
        n = max(1, (size + self.SPC * SECTOR - 1) // (self.SPC * SECTOR))
        chain = list(range(self.next_cluster, self.next_cluster + n))
        assert self.next_cluster + n - 2 < self.clusters_total, "том переполнен"
        self.next_cluster += n
        self.used_clusters += n
        for i, cl in enumerate(chain):
            self.fat[cl] = chain[i + 1] if i + 1 < n else self.EOC
        return chain

    def put_data(self, chain, payload):
        for i, cl in enumerate(chain):
            self.data_buf[cl] = payload[i * SECTOR:(i + 1) * SECTOR]

    def root_body(self):
        return getattr(self, "_root_body", b"")

    def set_root_body(self, body):
        self._root_body = body
        self.root_chain_ext = [self._root_chain[0]]
        need = (len(body) + SECTOR - 1) // SECTOR
        if need > 1:
            extra = self.alloc(need * SECTOR)
            self.root_chain_ext += extra

    @property
    def first_data(self):
        return self.RESERVED + self.NFATS * self.SPF

    def finalize(self):
        # FAT в обе копии
        fat_bytes = b"".join(struct.pack("<I", e) for e in self.fat)
        for f in range(self.NFATS):
            off = (self.RESERVED + f * self.SPF) * SECTOR
            self.vol[off:off + self.SPF * SECTOR] = fat_bytes
        # корневой каталог в свою цепочку
        for i, cl in enumerate(self.root_chain_ext):
            self.data_buf[cl] = self._root_body[i * SECTOR:(i + 1) * SECTOR]
        # данные
        for cl, chunk in self.data_buf.items():
            off = (self.first_data + (cl - 2)) * SECTOR
            self.vol[off:off + len(chunk)] = chunk
        # FSInfo: свободные кластеры
        free = self.clusters_total - self.used_clusters
        struct.pack_into("<I", self.vol, SECTOR + 488, free)
        struct.pack_into("<I", self.vol, SECTOR + 492, self.next_cluster)
        return bytes(self.vol)


# ============================================================
#  Дерево файлов (общее для обоих томов)
# ============================================================
def build_tree(fs, files):
    """files: {"PATH/IN/IMAGE": bytes}. Раскладывает по каталогам/кластерам fs."""
    subdirs = {}
    for path in files:
        parts = path.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            subdirs.setdefault("/".join(parts[:i]), None)
    for d in sorted(subdirs, key=lambda x: x.count("/")):
        subdirs[d] = fs.alloc(fs.SPC * SECTOR)[0]

    dir_bodies = {}
    for d, cl in subdirs.items():
        parent = "/".join(d.split("/")[:-1])
        parent_cl = subdirs.get(parent, 0)
        children = [dirent(".          ", 0x10, cl, 0),
                    dirent("..         ", 0x10, parent_cl, 0)]
        for d2, cl2 in subdirs.items():
            if d2 != d and "/".join(d2.split("/")[:-1]) == d:
                children.append(dirent(sfn(d2.split("/")[-1]), 0x10, cl2, 0))
        for path, content in files.items():
            if "/".join(path.split("/")[:-1]) == d:
                chain = fs.alloc(len(content))
                fs.put_data(chain, content)
                children.append(dirent(sfn(path.split("/")[-1]), 0x20, chain[0], len(content)))
        dir_bodies[cl] = b"".join(children)

    root_entries = []
    for d, cl in subdirs.items():
        if "/" not in d:
            root_entries.append(dirent(sfn(d), 0x10, cl, 0))
    for path, content in files.items():
        if "/" not in path:
            chain = fs.alloc(len(content))
            fs.put_data(chain, content)
            root_entries.append(dirent(sfn(path), 0x20, chain[0], len(content)))
    fs.set_root_body(b"".join(root_entries))
    for cl, body in dir_bodies.items():
        fs.put_data([cl], body)
    return fs


# ============================================================
#  GPT-диск (реальное железо + QEMU)
# ============================================================
ESP_TYPE_GUID = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")  # C12A7328-.. (LE на диске)
DISK_GUID = b"ARESOS_DISK_GPT!"
PART_GUID = b"ARESOS_ESP_PART!"

GPT_PART_START = 2048                    # 1 МиБ — стандартное выравнивание
ESP_MIB = 64


def build_gpt_image(files):
    esp_sectors = ESP_MIB * 1024 * 1024 // SECTOR          # 131072
    total = GPT_PART_START + esp_sectors + 33              # + GPT backup
    img = bytearray(total * SECTOR)

    # --- FAT32-том ESP ---
    fat = Fat32Vol(esp_sectors, hidden=GPT_PART_START)
    build_tree(fat, files)
    vol = fat.finalize()
    off = GPT_PART_START * SECTOR
    img[off:off + len(vol)] = vol

    part_end = GPT_PART_START + esp_sectors - 1
    last_lba = total - 1
    first_usable, last_usable = 34, total - 34

    # --- массив разделов (1 шт.) ---
    entry = bytearray(128)
    entry[0:16] = ESP_TYPE_GUID
    entry[16:32] = PART_GUID
    struct.pack_into("<Q", entry, 32, GPT_PART_START)
    struct.pack_into("<Q", entry, 40, part_end)
    struct.pack_into("<Q", entry, 48, 0)                   # атрибуты
    name = "EFI System".encode("utf-16-le")
    entry[56:56 + len(name)] = name
    entries = bytes(entry) + bytes(127 * 128)              # 128 слотов
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF

    def gpt_header(my_lba, alt_lba, arr_lba):
        h = bytearray(SECTOR)
        h[0:8] = b"EFI PART"
        struct.pack_into("<I", h, 8, 0x00010000)           # revision 1.0
        struct.pack_into("<I", h, 12, 92)                  # header size
        struct.pack_into("<Q", h, 24, my_lba)
        struct.pack_into("<Q", h, 32, alt_lba)
        struct.pack_into("<Q", h, 40, first_usable)
        struct.pack_into("<Q", h, 48, last_usable)
        h[56:72] = DISK_GUID
        struct.pack_into("<Q", h, 72, arr_lba)
        struct.pack_into("<I", h, 80, 128)
        struct.pack_into("<I", h, 84, 128)
        struct.pack_into("<I", h, 88, entries_crc)
        crc = zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF       # CRC с нулевым полем
        struct.pack_into("<I", h, 16, crc)
        return h

    # primary
    img[SECTOR:2 * SECTOR] = gpt_header(1, last_lba, 2)
    img[2 * SECTOR:34 * SECTOR] = entries
    # backup
    img[(total - 33) * SECTOR:(total - 1) * SECTOR] = entries
    img[(total - 1) * SECTOR:total * SECTOR] = gpt_header(last_lba, 1, total - 33)

    # --- защитный MBR ---
    mbr = bytearray(SECTOR)
    mbr[446 + 4] = 0xEE
    struct.pack_into("<I", mbr, 446 + 8, 1)
    struct.pack_into("<I", mbr, 446 + 12, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR] = mbr

    return bytes(img), esp_sectors, total


def build_floppy_image(files):
    fat = Fat12Vol()
    build_tree(fat, files)
    return fat.finalize()


# ============================================================
#  Чтение обратно (самопроверка), автоопределение формата
# ============================================================
def fat12_get(fat, n):
    off = n + n // 2
    if n % 2 == 0:
        return fat[off] | ((fat[off + 1] & 0x0F) << 8)
    return (fat[off] >> 4) | (fat[off + 1] << 4)


def walk_entries(body, read_chain, prefix):
    for i in range(0, len(body), 32):
        e = body[i:i + 32]
        if len(e) < 32 or e[0] == 0x00:
            break
        if e[0] == 0xE5 or e[11] == 0x0F or e[0] == 0x2E:
            continue
        name = e[0:11].decode()
        cl = struct.unpack_from("<H", e, 26)[0]
        size = struct.unpack_from("<I", e, 28)[0]
        if e[11] == 0x10:
            print(f"  {prefix}{name.strip()}/   (cluster {cl})")
            body2 = read_chain(cl)
            walk_entries(body2, read_chain, prefix + "  ")
        else:
            disp = name[:8].strip() + (("." + name[8:].strip()) if name[8:].strip() else "")
            print(f"  {prefix}{disp}   cluster={cl} size={size}")


def list_fat12(img):
    spf = struct.unpack_from("<H", img, 22)[0]
    reserved = struct.unpack_from("<H", img, 14)[0]
    nfats = img[16]
    root_entries = struct.unpack_from("<H", img, 17)[0]
    spc = img[13]
    root_sec = root_entries * 32 // SECTOR
    first_data = reserved + nfats * spf + root_sec
    fat = img[reserved * SECTOR:(reserved + spf) * SECTOR]

    def read_chain(cl):
        body = b""
        while 2 <= cl < 0xFF8:
            off = (first_data + (cl - 2) * spc) * SECTOR
            body += img[off:off + SECTOR * spc]
            cl = fat12_get(fat, cl)
        return body

    print(f"[mkesp] формат: цельный FAT12 (SPF={spf}, root entries={root_entries})")
    root_off = (reserved + nfats * spf) * SECTOR
    walk_entries(img[root_off:root_off + root_sec * SECTOR], read_chain, "")


def list_gpt(img):
    assert img[446 + 4] == 0xEE, "нет защитного MBR"
    assert img[SECTOR:SECTOR + 8] == b"EFI PART", "нет GPT-заголовка"
    arr_lba = struct.unpack_from("<Q", img, SECTOR + 72)[0]
    entry = img[arr_lba * SECTOR:arr_lba * SECTOR + 128]
    ptype = entry[0:16]
    assert ptype == ESP_TYPE_GUID, "первый раздел не ESP"
    p_start = struct.unpack_from("<Q", entry, 32)[0]
    p_end = struct.unpack_from("<Q", entry, 40)[0]
    vol = img[p_start * SECTOR:(p_end + 1) * SECTOR]
    print(f"[mkesp] формат: GPT, ESP раздел LBA {p_start}..{p_end} "
          f"({(p_end - p_start + 1) * SECTOR // 1024 // 1024} МиБ)")

    # FAT32 BPB
    spc = vol[13]
    reserved = struct.unpack_from("<H", vol, 14)[0]
    nfats = vol[16]
    spf32 = struct.unpack_from("<I", vol, 36)[0]
    root_cl = struct.unpack_from("<I", vol, 44)[0]
    data_secs = len(vol) // SECTOR - reserved - nfats * spf32
    print(f"[mkesp] FAT32: spc={spc} reserved={reserved} SPF={spf32} "
          f"кластеров={data_secs // spc} (>=65525 → валидный FAT32)")
    fat = vol[reserved * SECTOR:(reserved + spf32) * SECTOR]
    first_data = reserved + nfats * spf32

    def fat32_get(cl):
        return struct.unpack_from("<I", fat, cl * 4)[0] & 0x0FFFFFFF

    def read_chain(cl):
        body = b""
        seen = set()
        while 2 <= cl < 0x0FFFFFF8 and cl not in seen:
            seen.add(cl)
            off = (first_data + (cl - 2) * spc) * SECTOR
            body += vol[off:off + SECTOR * spc]
            cl = fat32_get(cl)
        return body

    walk_entries(read_chain(root_cl), read_chain, "")
    print("[mkesp] GPT + FAT32 структуры читаются корректно")


def list_image(path):
    img = open(path, "rb").read()
    assert img[510:512] == b"\x55\xAA", "нет boot-сигнатуры"
    if img[SECTOR:SECTOR + 8] == b"EFI PART":
        list_gpt(img)
    else:
        list_fat12(img)
    print("[mkesp] образ читается корректно")


# ============================================================
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image")
    ap.add_argument("--floppy", action="store_true",
                    help="цельный FAT12 1.44 МиБ вместо GPT-диска")
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

    if a.floppy:
        blob = build_floppy_image(files)
    else:
        blob, esp_sectors, total = build_gpt_image(files)

    with open(a.image, "wb") as f:
        f.write(blob)
    payload = sum(len(v) for v in files.values())
    mode = "FAT12 floppy" if a.floppy else f"GPT + ESP/FAT32 ({ESP_MIB} МиБ)"
    print(f"[mkesp] {a.image}: режим {mode}, {len(files)} файл(ов), "
          f"payload {payload} байт, образ {len(blob)} байт")
    list_image(a.image)


if __name__ == "__main__":
    main()
