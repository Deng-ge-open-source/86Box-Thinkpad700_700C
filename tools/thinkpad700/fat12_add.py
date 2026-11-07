#!/usr/bin/env python3
"""Add one 8.3 file to the root directory of a FAT12 disk image."""

import struct
import sys
from pathlib import Path


def fat12_get(fat: bytearray, cluster: int) -> int:
    offset = cluster + cluster // 2
    word = fat[offset] | (fat[offset + 1] << 8)
    return (word >> 4) & 0x0FFF if cluster & 1 else word & 0x0FFF


def fat12_set(fat: bytearray, cluster: int, value: int) -> None:
    offset = cluster + cluster // 2
    if cluster & 1:
        fat[offset] = (fat[offset] & 0x0F) | ((value << 4) & 0xF0)
        fat[offset + 1] = (value >> 4) & 0xFF
    else:
        fat[offset] = value & 0xFF
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)


def main() -> None:
    image_path = Path(sys.argv[1])
    source_path = Path(sys.argv[2])
    dos_name = sys.argv[3].upper()
    stem, suffix = (dos_name.split(".", 1) + [""])[:2]
    if len(stem) > 8 or len(suffix) > 3:
        raise SystemExit("destination must be an 8.3 filename")
    name_field = (stem.ljust(8) + suffix.ljust(3)).encode("ascii")

    disk = bytearray(image_path.read_bytes())
    payload = source_path.read_bytes()
    bps = struct.unpack_from("<H", disk, 11)[0]
    spc = disk[13]
    reserved = struct.unpack_from("<H", disk, 14)[0]
    fat_count = disk[16]
    root_entries = struct.unpack_from("<H", disk, 17)[0]
    total_sectors = struct.unpack_from("<H", disk, 19)[0]
    sectors_per_fat = struct.unpack_from("<H", disk, 22)[0]
    root_sectors = (root_entries * 32 + bps - 1) // bps
    fat_start = reserved * bps
    fat_size = sectors_per_fat * bps
    root_start = (reserved + fat_count * sectors_per_fat) * bps
    data_start_sector = reserved + fat_count * sectors_per_fat + root_sectors
    cluster_size = bps * spc
    cluster_count = (total_sectors - data_start_sector) // spc

    root_slot = None
    for entry in range(root_entries):
        offset = root_start + entry * 32
        first = disk[offset]
        if disk[offset:offset + 11] == name_field:
            raise SystemExit(f"{dos_name} already exists")
        if root_slot is None and first in (0x00, 0xE5):
            root_slot = offset
    if root_slot is None:
        raise SystemExit("root directory is full")

    fat = bytearray(disk[fat_start:fat_start + fat_size])
    needed = max(1, (len(payload) + cluster_size - 1) // cluster_size)
    free = [c for c in range(2, cluster_count + 2) if fat12_get(fat, c) == 0]
    if len(free) < needed:
        raise SystemExit("not enough free clusters")
    chain = free[:needed]
    for index, cluster in enumerate(chain):
        fat12_set(fat, cluster, chain[index + 1] if index + 1 < len(chain) else 0xFFF)
        target = (data_start_sector + (cluster - 2) * spc) * bps
        chunk = payload[index * cluster_size:(index + 1) * cluster_size]
        disk[target:target + cluster_size] = chunk.ljust(cluster_size, b"\0")

    for copy in range(fat_count):
        start = fat_start + copy * fat_size
        disk[start:start + fat_size] = fat

    entry = bytearray(32)
    entry[:11] = name_field
    entry[11] = 0x20
    struct.pack_into("<H", entry, 26, chain[0])
    struct.pack_into("<I", entry, 28, len(payload))
    disk[root_slot:root_slot + 32] = entry
    image_path.write_bytes(disk)


if __name__ == "__main__":
    main()
