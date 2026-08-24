#!/usr/bin/env python3
"""
AresOS mkvmdk — генерирует .vmdk-дескриптор (monolithicFlat) поверх raw-образа.

VirtualBox умеет прикреплять VMDK напрямую («Выбрать существующий диск»),
а .vmdk ссылается на наш aresos.img — конвертация в VDI не нужна:
пользователь просто распаковывает два файла и крепит aresos.vmdk.

Usage: mkvmdk.py aresos.vmdk aresos.img
"""
import os
import struct
import sys

def main():
    assert len(sys.argv) == 3, __doc__
    vmdk_path, img_path = sys.argv[1], sys.argv[2]
    size = os.path.getsize(img_path)
    assert size % 512 == 0, "raw-образ должен быть кратен 512 байтам"
    sectors = size // 512
    heads, spt = 16, 63
    cyl = (sectors + heads * spt - 1) // (heads * spt)
    img_name = os.path.basename(img_path)

    text = (
        "# Disk DescriptorFile\n"
        "version=1\n"
        "CID=a5e3d2c1\n"
        "parentCID=ffffffff\n"
        "createType=\"monolithicFlat\"\n"
        "\n"
        "# Extent description\n"
        f"RW {sectors} FLAT \"{img_name}\" 0\n"
        "\n"
        "# The Disk Data Base\n"
        "#DDB\n"
        "ddb.virtualHWVersion = \"4\"\n"
        f"ddb.geometry.cylinders = \"{cyl}\"\n"
        f"ddb.geometry.heads = \"{heads}\"\n"
        f"ddb.geometry.sectors = \"{spt}\"\n"
        "ddb.adapterType = \"ide\"\n"
    )
    with open(vmdk_path, "w", newline="\n") as f:
        f.write(text)
    print(f"[mkvmdk] {vmdk_path}: extent {img_name} ({sectors} секторов, {size} байт)")

if __name__ == "__main__":
    main()
