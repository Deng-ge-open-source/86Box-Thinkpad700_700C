#!/usr/bin/env python3
"""Apply the small, deterministic media initializations used by the TP700 VM."""

import hashlib
import os
import struct
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
WORK_ROOT = Path(
    os.environ.get("TP700_WORK_ROOT", REPO_ROOT / "local-assets/legacy-work")
).expanduser().resolve()

SECTOR_SIZE = 512
TP700_CYLINDERS = 370
TP700_HEADS = 64
TP700_SECTORS = 32
TP700_SYSTEM_SECTORS = 0x1800
TP700_SYSTEM_PARTITION_SHA256 = (
    "989787780683c14ead2736322f438e66fa89fe539c0e51498482de9fa6ac51ea"
)


def read_at(path: Path, offset: int, size: int) -> bytes:
    with path.open("rb") as stream:
        stream.seek(offset)
        data = stream.read(size)
    if len(data) != size:
        raise ValueError(f"short read at {offset:#x}: {len(data)} != {size}")
    return data


def read_fat12_root_file(image: Path, wanted: bytes) -> bytes:
    """Read an 8.3 root file from an IBM reference-disk image."""
    boot = read_at(image, 0, SECTOR_SIZE)
    bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
    sectors_per_cluster = boot[13]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fats = boot[16]
    root_entries = struct.unpack_from("<H", boot, 17)[0]
    sectors_per_fat = struct.unpack_from("<H", boot, 22)[0]
    if bytes_per_sector != SECTOR_SIZE or not sectors_per_cluster:
        raise ValueError("unsupported Reference Diskette FAT12 geometry")

    fat_offset = reserved * SECTOR_SIZE
    root_offset = (reserved + fats * sectors_per_fat) * SECTOR_SIZE
    root_size = root_entries * 32
    root_sectors = (root_size + SECTOR_SIZE - 1) // SECTOR_SIZE
    data_offset = root_offset + root_sectors * SECTOR_SIZE
    fat = read_at(image, fat_offset, sectors_per_fat * SECTOR_SIZE)
    root = read_at(image, root_offset, root_size)

    entry = None
    for offset in range(0, len(root), 32):
        candidate = root[offset : offset + 32]
        if candidate[:11] == wanted:
            entry = candidate
            break
    if entry is None:
        raise ValueError(f"{wanted!r} is missing from the Reference Diskette")

    cluster = struct.unpack_from("<H", entry, 26)[0]
    size = struct.unpack_from("<I", entry, 28)[0]
    result = bytearray()
    visited = set()
    while 2 <= cluster < 0xFF8 and len(result) < size:
        if cluster in visited:
            raise ValueError("loop in Reference Diskette FAT12 chain")
        visited.add(cluster)
        offset = data_offset + (cluster - 2) * sectors_per_cluster * SECTOR_SIZE
        result.extend(
            read_at(image, offset, sectors_per_cluster * SECTOR_SIZE)
        )
        fat_offset_12 = cluster + cluster // 2
        next_cluster = fat[fat_offset_12] | (fat[fat_offset_12 + 1] << 8)
        cluster = next_cluster >> 4 if cluster & 1 else next_cluster & 0xFFF
    if len(result) < size:
        raise ValueError("short Reference Diskette FAT12 chain")
    return bytes(result[:size])


def validate_system_partition(image: Path) -> tuple[bool, str]:
    """Check the IBM 9552 hidden 3 MiB System Partition container.

    I9990303 means that the IML code could not be loaded from this area.  The
    ROM reserves exactly 1800h sectors at the native end of the ESDI disk and
    expects a small active FAT12 volume inside its private MBR.  Validate the
    physical placement and boot-critical directory entries before each run.
    """
    expected_size = (
        TP700_CYLINDERS * TP700_HEADS * TP700_SECTORS * SECTOR_SIZE
    )
    if image.stat().st_size != expected_size:
        return False, f"disk size is {image.stat().st_size}, expected {expected_size}"

    system_offset = (expected_size // SECTOR_SIZE - TP700_SYSTEM_SECTORS) * SECTOR_SIZE
    try:
        mbr = read_at(image, system_offset, SECTOR_SIZE)
        if mbr[510:512] != b"\x55\xaa":
            return False, "System Partition MBR signature is missing"
        entry = mbr[446:462]
        start, sectors = struct.unpack_from("<II", entry, 8)
        if (entry[0], entry[4], start, sectors) != (0x80, 0x01, 32, 6112):
            return False, "System Partition MBR entry is not active FAT12 32+6112"

        boot = read_at(image, system_offset + start * SECTOR_SIZE, SECTOR_SIZE)
        if boot[3:11] != b"IBM 5.3y" or boot[510:512] != b"\x55\xaa":
            return False, "System Partition FAT12 boot sector is invalid"
        bpb = (
            struct.unpack_from("<H", boot, 11)[0],
            boot[13],
            struct.unpack_from("<H", boot, 14)[0],
            boot[16],
            struct.unpack_from("<H", boot, 17)[0],
            struct.unpack_from("<H", boot, 19)[0],
            struct.unpack_from("<H", boot, 22)[0],
            struct.unpack_from("<H", boot, 24)[0],
            struct.unpack_from("<H", boot, 26)[0],
            struct.unpack_from("<I", boot, 28)[0],
        )
        if bpb != (512, 8, 1, 2, 512, 6112, 3, 32, 64, 32):
            return False, f"System Partition FAT12 BPB is invalid: {bpb}"

        root_offset = system_offset + (start + 1 + 2 * 3) * SECTOR_SIZE
        root = read_at(image, root_offset, 512 * 32)
        names = {
            root[i : i + 11]
            for i in range(0, len(root), 32)
            if root[i] not in (0x00, 0xE5) and root[i + 11] != 0x0F
        }
        required = {b"IBMBIO  COM", b"IBMDOS  COM", b"$BOOTEXTDCS", b"$CONFHDRDCS"}
        missing = required - names
        if missing:
            return False, "System Partition is missing " + ", ".join(
                sorted(name.decode("ascii") for name in missing)
            )

        # Before INT 19h the 9552 ROM searches backwards through the final
        # 99 sectors, three sectors at a time, for its master IML record.  A
        # DOS/FAT restore alone leaves those raw sectors zeroed and produces
        # I9990303 even though the later operating-system boot can succeed.
        iml = read_at(image, expected_size - 3 * SECTOR_SIZE, 3 * SECTOR_SIZE)
        if iml[:3] != b"IML" or (sum(iml) & 0xFF):
            return False, "System Partition master IML record is missing"
    except (OSError, ValueError) as error:
        return False, str(error)
    return True, "ok"


def restore_system_partition(image: Path, pristine: Path) -> None:
    """Restore only the reserved 3 MiB area; never touch the Windows volume."""
    pristine_data = pristine.read_bytes()
    if len(pristine_data) != TP700_SYSTEM_SECTORS * SECTOR_SIZE:
        raise SystemExit(f"{pristine}: wrong System Partition backup size")
    digest = hashlib.sha256(pristine_data).hexdigest()
    if digest != TP700_SYSTEM_PARTITION_SHA256:
        raise SystemExit(f"{pristine}: System Partition backup checksum mismatch")

    system_offset = (
        TP700_CYLINDERS * TP700_HEADS * TP700_SECTORS - TP700_SYSTEM_SECTORS
    ) * SECTOR_SIZE
    with image.open("r+b") as stream:
        stream.seek(system_offset)
        stream.write(pristine_data)
        stream.flush()


def install_system_partition_iml(image: Path, reference_disk: Path) -> None:
    """Install the IBM-supplied three-sector master IML at disk end."""
    iml = read_fat12_root_file(reference_disk, b"$BOOT   IML")
    if len(iml) != 3 * SECTOR_SIZE or iml[:3] != b"IML" or (sum(iml) & 0xFF):
        raise SystemExit(f"{reference_disk}: invalid $BOOT.IML record")
    expected_size = (
        TP700_CYLINDERS * TP700_HEADS * TP700_SECTORS * SECTOR_SIZE
    )
    with image.open("r+b") as stream:
        stream.seek(expected_size - len(iml))
        stream.write(iml)
        stream.flush()


def build_monochrome_rom(source: Path, destination: Path) -> None:
    """Derive the 700 (F1EE) build from the surviving 700C (F1EC) dump.

    The 39G9996 image available in the public ROM set is the 700C build: both
    copies of its POST/IML code contain three F1EC planar-ID operands.  IBM's
    monochrome Reference Diskette identifies the 700 planar as F1EE.  Change
    only those six operands and the checksum byte of each affected 64 KiB
    bank; retaining the original separately makes this a reproducible ROM
    variant conversion rather than a forged hardware response.
    """
    data = bytearray(source.read_bytes())
    if len(data) != 0x40000:
        raise SystemExit(f"{source}: expected a 256 KiB 39G9996 image")

    planar_id_operands = (0x1057B, 0x107F2, 0x11BE1, 0x30378, 0x305EE, 0x31517)
    for offset in planar_id_operands:
        if data[offset : offset + 2] not in (b"\xEC\xF1", b"\xEE\xF1"):
            raise SystemExit(
                f"{source}: unexpected planar ID at {offset:#x}: "
                f"{data[offset:offset + 2].hex()}"
            )
        data[offset : offset + 2] = b"\xEE\xF1"

    # Three EC->EE changes add six to each affected bank's byte checksum.
    for offset, original, converted in (
        (0x1FFFF, 0x16, 0x10),
        (0x3FFFF, 0x39, 0x33),
    ):
        if data[offset] not in (original, converted):
            raise SystemExit(
                f"{source}: unexpected checksum byte {data[offset]:02x} "
                f"at {offset:#x}"
            )
        data[offset] = converted

    for bank in range(4):
        start = bank * 0x10000
        if sum(data[start : start + 0x10000]) & 0xFF:
            raise SystemExit(f"monochrome ROM bank {bank} has a bad checksum")

    destination.write_bytes(data)


def patch_byte(path: Path, offset: int, expected: set[int], value: int) -> None:
    data = bytearray(path.read_bytes())
    if data[offset] not in expected:
        raise SystemExit(
            f"{path}: unexpected byte {data[offset]:02x} at {offset:#x}"
        )
    data[offset] = value
    path.write_bytes(data)


def patch_bytes(path: Path, offset: int, expected: set[bytes], value: bytes) -> None:
    data = bytearray(path.read_bytes())
    current = bytes(data[offset : offset + len(value)])
    if current not in expected:
        raise SystemExit(f"{path}: unexpected bytes {current.hex()} at {offset:#x}")
    data[offset : offset + len(value)] = value
    path.write_bytes(data)


def patch_fixed_file(
    path: Path, offset: int, old: bytes, new: bytes, allocated: int
) -> None:
    if len(new) > allocated:
        raise SystemExit(f"replacement is too large: {len(new)} > {allocated}")
    data = bytearray(path.read_bytes())
    current = bytes(data[offset : offset + allocated])
    old_padded = old + bytes(allocated - len(old))
    new_padded = new + bytes(allocated - len(new))
    if current not in (old_padded, new_padded):
        raise SystemExit(f"{path}: AUTOEXEC.BAT contents did not match")
    data[offset : offset + allocated] = new_padded
    path.write_bytes(data)


def patch_fat16_file_text(
    image: Path,
    partition_lba: int,
    components: tuple[bytes, ...],
    old: bytes,
    new: bytes,
) -> None:
    """Replace a same-sized byte sequence inside one FAT16 file.

    This is deliberately a small, read/modify/write FAT helper rather than a
    fixed absolute-offset patch: Windows rewrites WIN.INI and may move its
    cluster chain.  Resolving the live directory entry on every launch keeps
    the monochrome color fix attached to the real file.
    """
    if len(old) != len(new):
        raise SystemExit("FAT16 text replacement must preserve file length")

    partition_offset = partition_lba * SECTOR_SIZE
    boot = read_at(image, partition_offset, SECTOR_SIZE)
    bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
    sectors_per_cluster = boot[13]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    fats = boot[16]
    root_entries = struct.unpack_from("<H", boot, 17)[0]
    sectors_per_fat = struct.unpack_from("<H", boot, 22)[0]
    if bytes_per_sector != SECTOR_SIZE or not sectors_per_cluster:
        raise SystemExit("unsupported Windows-volume FAT16 geometry")

    fat_offset = partition_offset + reserved * bytes_per_sector
    fat = read_at(image, fat_offset, sectors_per_fat * bytes_per_sector)
    root_offset = partition_offset + (reserved + fats * sectors_per_fat) * bytes_per_sector
    root_size = root_entries * 32
    root_sectors = (root_size + bytes_per_sector - 1) // bytes_per_sector
    data_offset = root_offset + root_sectors * bytes_per_sector
    cluster_size = sectors_per_cluster * bytes_per_sector

    def chain(first: int):
        current = first
        visited = set()
        while 2 <= current < 0xFFF8:
            if current in visited:
                raise SystemExit("loop in Windows-volume FAT16 chain")
            visited.add(current)
            yield current
            fat_entry = current * 2
            if fat_entry + 2 > len(fat):
                raise SystemExit("FAT16 chain extends beyond the FAT")
            current = struct.unpack_from("<H", fat, fat_entry)[0]

    def cluster_offset(cluster: int) -> int:
        return data_offset + (cluster - 2) * cluster_size

    directory = read_at(image, root_offset, root_size)
    entry = None
    for index, wanted in enumerate(components):
        entry = None
        for offset in range(0, len(directory), 32):
            candidate = directory[offset : offset + 32]
            if candidate[0] == 0x00:
                break
            if candidate[0] == 0xE5 or candidate[11] == 0x0F:
                continue
            if candidate[:11] == wanted:
                entry = candidate
                break
        if entry is None:
            raise SystemExit(f"FAT16 path component {wanted!r} was not found")
        first_cluster = struct.unpack_from("<H", entry, 26)[0]
        if index + 1 < len(components):
            directory = b"".join(
                read_at(image, cluster_offset(cluster), cluster_size)
                for cluster in chain(first_cluster)
            )

    assert entry is not None
    file_size = struct.unpack_from("<I", entry, 28)[0]
    clusters = list(chain(struct.unpack_from("<H", entry, 26)[0]))
    contents = bytearray()
    for cluster in clusters:
        contents.extend(read_at(image, cluster_offset(cluster), cluster_size))
        if len(contents) >= file_size:
            break
    contents = contents[:file_size]
    if new in contents:
        return
    occurrence = contents.count(old)
    if occurrence != 1:
        raise SystemExit(
            f"WIN.INI monochrome colors matched {occurrence} times, expected once"
        )
    contents = contents.replace(old, new, 1)

    with image.open("r+b") as stream:
        position = 0
        for cluster in clusters:
            if position >= file_size:
                break
            length = min(cluster_size, file_size - position)
            stream.seek(cluster_offset(cluster))
            stream.write(contents[position : position + length])
            position += length
        stream.flush()


# Preserve the public 700C dump and generate the monochrome machine's F1EE
# variant as a separate, checksum-correct file used by this VM.
rom_dir = WORK_ROOT / "tp700-runtime/roms/machines/ibmtp700"
build_monochrome_rom(rom_dir / "39G9996.bin", rom_dir / "39G9996-700.bin")

# The 39G9996 ROM only exposes its extended error-log services when the
# secondary-NVRAM signature byte at 038Ah is FFh.  The three 37-byte records
# that follow are already zero/invalid; this initializes the log, rather than
# hiding individual POST results.
patch_byte(
    WORK_ROOT / "tp700-runtime/ThinkPad 700/nvr/ibmtp700_39g9996_sec.nvr",
    0x038A,
    {0x00, 0xFF},
    0xFF,
)

# The firmware latches failed processor/cache diagnostics in the same
# secondary NVRAM and deliberately skips retesting them on later boots.  A
# service-board diagnostic clears these bits after repair.  Clear only the
# two documented failure latches so the real POST paths run again; do not
# suppress the error-reporting service or alter the POST code.
secondary_nvr = (
    WORK_ROOT / "tp700-runtime/ThinkPad 700/nvr/ibmtp700_39g9996_sec.nvr"
)
patch_byte(secondary_nvr, 0x04EA, {0x00, 0x01}, 0x00)
patch_byte(secondary_nvr, 0x0192, {0x00, 0x40}, 0x00)

# Validate the real hidden System Partition that the 9552 ROM uses for IML.
# If its boot container has been damaged, restore only the reserved final
# 3 MiB from the known-good Reference-Diskette restoration.  The visible
# Windows partition is outside this range and is never overwritten here.
image = WORK_ROOT / "720cc-system/TP700-restore-scratch-370.img"
system_partition_backup = (
    WORK_ROOT / "720cc-system/ThinkPad-700-system-partition-restored-3MiB.img"
)
reference_disk = WORK_ROOT / "tp700-runtime/r700m120-tp700.ima"
valid, reason = validate_system_partition(image)
if not valid:
    print(f"Repairing TP700 System Partition/IML: {reason}")
    if reason != "System Partition master IML record is missing":
        restore_system_partition(image, system_partition_backup)
    install_system_partition_iml(image, reference_disk)
    valid, reason = validate_system_partition(image)
    if not valid:
        raise SystemExit(f"System Partition repair failed: {reason}")

# The restored color profile came from the 700C installation and used a pale
# blue selection with black text.  On the 700's grayscale LCD this collapses
# to nearly the same shade as the white menu, so Windows 3.2 list and popup
# selections appear not to move.  A real monochrome UI uses black selection
# with white text.  The paired replacement is exactly the same byte length,
# leaving WIN.INI's FAT chain and logical size untouched.
patch_fat16_file_text(
    image,
    32,
    (b"WINDOWS    ", b"WIN     INI"),
    b"Hilight=164 200 240\r\nHilightText=0 0 0",
    b"Hilight=0 0 0\r\nHilightText=255 255 255",
)

# AUTOEXEC.BAT occupies one fixed FAT16 cluster beginning at this offset.
# Keep its directory size (161 bytes) stable while making a Reference-Diskette
# "Start operating system" hand-off enter Windows 3.2 automatically.  The DOS
# mouse TSR is unnecessary because Windows has its own PS/2 mouse driver, and
# avoiding it also prevents an old MOUSE.COM divide fault on a fast emulated
# timer.
old_autoexec = (
    b"C:\\WINDOWS\\SMARTDRV.EXE\r\n"
    b"@ECHO OFF\r\n"
    b"PROMPT $p$g\r\n"
    b"PATH C:\\WINWORD;C:\\WINDOWS;C:\\DOS\r\n"
    b"SET TEMP=C:\\DOS\r\n"
    b"C:\\DOS\\MOUSE.COM\r\n"
    b"REM Windows is available with STARTWIN\r\n\r\n"
)
new_autoexec = (
    b"C:\\WINDOWS\\SMARTDRV.EXE\r\n"
    b"@ECHO OFF\r\n"
    b"PROMPT $p$g\r\n"
    b"PATH C:\\WINWORD;C:\\WINDOWS;C:\\DOS\r\n"
    b"SET TEMP=C:\\DOS\r\n"
    b"C:\\WINDOWS\\WIN.COM /S\r\n"
)
patch_fixed_file(image, 0x09DBD600, old_autoexec, new_autoexec, 161)

# FAT directory entries carry the logical file size separately from the
# allocated cluster.  Keep the one-cluster allocation, but expose exactly the
# replacement batch file rather than the zero-padded former 161-byte length.
patch_bytes(
    image,
    0x0002F99C,
    {len(old_autoexec).to_bytes(4, "little"), len(new_autoexec).to_bytes(4, "little")},
    len(new_autoexec).to_bytes(4, "little"),
)
