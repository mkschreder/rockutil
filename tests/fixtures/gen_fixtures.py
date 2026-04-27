#!/usr/bin/env python3
"""
gen_fixtures.py  <output_dir>

Build synthetic Rockchip firmware fixtures consumed by the Bats test suite.
Nothing in here is a real device image; all blobs are deliberately simple
patterns chosen to make test assertions easy.

Binary formats implemented (all little-endian unless stated):
  RKBOOT  – rkimage.h struct rkboot_header  + rkboot_entry_raw tables
  RKFW    – rkimage.h struct rkfw_header    (outer wrapper)
  RKAF    – rkimage.h struct rkaf_header    (Android firmware archive)
"""

import os
import sys
import struct
import random
import hashlib

# ---------------------------------------------------------------------------
# CRC helpers mirroring rkcrc.c
# ---------------------------------------------------------------------------

def crc_ccitt(data: bytes) -> int:
    """CRC-CCITT/FALSE: poly=0x1021, init=0xFFFF (matches rkcrc_ccitt)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def crc_rkfw(data: bytes) -> int:
    """Rockchip RKFW CRC32: poly=0x04c10db7, init=0, non-reflected."""
    crc = 0
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04c10db7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


# ---------------------------------------------------------------------------
# RKBOOT builder
# ---------------------------------------------------------------------------
# Header layout (packed, LE):
#   char[4]  magic         "BOOT"
#   u16      hdr_size      (46 in the real tool)
#   u32      version
#   u32      merge_version
#   u16      year
#   u8       month, day, hour, minute, second
#   u32      chip
#   u8       e471_count
#   u32      e471_offset
#   u8       e471_size
#   u8       e472_count
#   u32      e472_offset
#   u8       e472_size
#   u8       loader_count
#   u32      loader_offset
#   u8       loader_size
#   u8       sign_flag
#   u8       rc4_disable
# Total: 4+2+4+4+2+5+4+1+4+1+1+4+1+1+4+1+1+1 = 45 bytes
# hdr_size field says 46 so pad one byte.

HDR_STRUCT = struct.Struct("<4sHIIH5sIBIBBIBBIBBB")
# magic, hdr_size, version, merge_version,
# year, [month,day,hour,min,sec as 5-byte packed],
# chip,
# e471_count, e471_offset, e471_size,
# e472_count, e472_offset, e472_size,
# loader_count, loader_offset, loader_size,
# sign_flag, rc4_disable

_HDR_STRUCT = struct.Struct("<4sHIIHBBBBBIBIBBIBBIBBB")
# magic(4) hdr_size(H) version(I) merge_version(I)
# year(H) month(B) day(B) hour(B) minute(B) second(B)
# chip(I)
# e471_count(B) e471_offset(I) e471_size(B)
# e472_count(B) e472_offset(I) e472_size(B)
# loader_count(B) loader_offset(I) loader_size(B)
# sign_flag(B) rc4_disable(B)

assert _HDR_STRUCT.size == 45

# entry_raw:
#   u8   size   (typically 0x39 = 57)
#   u32  type
#   u16[20] name  (UTF-16 LE, null-terminated)
#   u32  data_offset
#   u32  data_size
#   u32  data_delay
ENTRY_RAW_STRUCT = struct.Struct("<BI40sIII")
# size(B) type(I) name(40s) data_offset(I) data_size(I) data_delay(I)
assert ENTRY_RAW_STRUCT.size == 57  # 0x39


def utf16le(text: str, word_count: int = 20) -> bytes:
    enc = text.encode("utf-16-le")
    enc = enc[:word_count * 2]
    return enc + b"\x00" * (word_count * 2 - len(enc))


def build_rkboot(
    e471_payload: bytes,
    e472_payload: bytes,
    loader_payload: bytes,
    chip: int = 0x33353061,  # "RK35" LE
    rc4_disable: int = 1,
) -> bytes:
    """
    Build a minimal RKBOOT container with one entry of each kind.
    All entry sizes use 0x39 (57 bytes) as specified by the real tool.
    The file ends with the 4-byte Rockchip CRC32.
    """
    hdr_size = 46  # as stored in the real tool

    # Compute layout of all tables after header + 1-byte pad.
    # Table offsets must point to actual entry bytes inside the file.
    # Layout: [header (45+1 bytes)][e471 table][e472 table][loader table]
    #         [e471 data][e472 data][loader data]
    e471_count = 1
    e472_count = 1
    loader_count = 1
    entry_size = ENTRY_RAW_STRUCT.size  # 57

    base = hdr_size  # 46
    e471_tbl_off = base
    e472_tbl_off = e471_tbl_off + e471_count * entry_size
    loader_tbl_off = e472_tbl_off + e472_count * entry_size

    data_base = loader_tbl_off + loader_count * entry_size
    e471_data_off = data_base
    e472_data_off = e471_data_off + len(e471_payload)
    loader_data_off = e472_data_off + len(e472_payload)

    total_without_crc = loader_data_off + len(loader_payload)

    # --- Header ---
    hdr = _HDR_STRUCT.pack(
        b"BOOT",
        hdr_size,
        0x00010000,   # version 1.0.0.0
        0x00010000,   # merge_version
        2024, 1, 1, 0, 0, 0,   # date
        chip,
        e471_count, e471_tbl_off, entry_size,
        e472_count, e472_tbl_off, entry_size,
        loader_count, loader_tbl_off, entry_size,
        0,            # sign_flag (not 'S')
        rc4_disable,
    )
    assert len(hdr) == 45
    hdr += b"\x00"  # pad to hdr_size = 46

    # --- Entry tables ---
    def make_entry(type_code: int, data_off: int, data_size: int,
                   name: str) -> bytes:
        return ENTRY_RAW_STRUCT.pack(
            entry_size,
            type_code,
            utf16le(name),
            data_off,
            data_size,
            0,   # delay ms
        )

    e471_table = make_entry(0x01, e471_data_off, len(e471_payload), "DDR")
    e472_table = make_entry(0x02, e472_data_off, len(e472_payload), "USB")
    loader_table = make_entry(0x04, loader_data_off, len(loader_payload), "LDR")

    body = hdr + e471_table + e472_table + loader_table
    body += e471_payload + e472_payload + loader_payload

    assert len(body) == total_without_crc

    crc = crc_rkfw(body)
    return body + struct.pack("<I", crc)


# ---------------------------------------------------------------------------
# RKFW outer wrapper builder
# ---------------------------------------------------------------------------
# Header: magic(4) hdr_size(H) major(B) minor(B) patch(B) build(B)
#         code(I) year(H) month(B) day(B) hour(B) minute(B) second(B)
#         chip(I) loader_offset(I) loader_size(I) image_offset(I) image_size(I)
#         unknown1(B) fs_type(I) backup_offset(I) backup_size(I)
#         reserved[0x66-0x39 = 0x2D bytes]
# Total = 0x66 = 102 bytes

_RKFW_HDR_STRUCT = struct.Struct("<4sHBBBBIHBBBBBIIIIIBIII")
# magic hdr_size major minor patch build code
# year month day hour minute second
# chip loader_offset loader_size image_offset image_size
# unknown1 fs_type backup_offset backup_size
# Struct is 54 bytes; RKFW header is 0x66=102 bytes.
_RKFW_RESERVED = 0x66 - _RKFW_HDR_STRUCT.size

# MD5 is 16 bytes trailing the file.
_RKFW_MD5_LEN = 16


def build_rkfw(boot_blob: bytes, rkaf_blob: bytes, chip: int = 0x33353061) -> bytes:
    """
    Wrap a RKBOOT blob and an RKAF blob into a minimal RKFW container.
    """
    hdr_size = 0x66
    loader_offset = hdr_size
    loader_size = len(boot_blob)
    image_offset = loader_offset + loader_size
    image_size = len(rkaf_blob)

    hdr = _RKFW_HDR_STRUCT.pack(
        b"RKFW",
        hdr_size,
        1, 0, 0, 0,     # major minor patch build
        0x01000000,     # code
        2024, 1, 1, 0, 0,  # year month day hour minute
        0,              # second
        chip,
        loader_offset, loader_size,
        image_offset, image_size,
        0,              # unknown1
        0,              # fs_type
        0, 0,           # backup_offset, backup_size
    )
    hdr += b"\x00" * _RKFW_RESERVED

    assert len(hdr) == 0x66

    body = hdr + boot_blob + rkaf_blob
    md5 = hashlib.md5(body).digest()
    return body + md5


# ---------------------------------------------------------------------------
# RKAF builder
# ---------------------------------------------------------------------------
# Header: magic(4) length(I) model(34s) id(30s) manufacturer(56s)
#         unknown1(I) version(I) num_parts(I)
#         parts[16] × 112 bytes each
#         reserved[0x6D = 109 bytes]
# rkaf_part: name(32s) mount(60s) nand_size(I) pos(I) nand_addr(I)
#            padded_size(I) image_size(I) = 32+60+4+4+4+4+4 = 112 bytes

_RKAF_PART_STRUCT = struct.Struct("<32s60sIIIII")
assert _RKAF_PART_STRUCT.size == 112

_RKAF_HDR_CORE = struct.Struct("<4sI34s30s56sIII")
# magic length model id manufacturer unknown1 version num_parts
assert _RKAF_HDR_CORE.size == 4+4+34+30+56+4+4+4

_RKAF_HDR_RESERVED = 0x6D  # 109 bytes after the parts array
_RKAF_HDR_TOTAL = _RKAF_HDR_CORE.size + 16 * _RKAF_PART_STRUCT.size + _RKAF_HDR_RESERVED


def build_rkaf(parts: list) -> bytes:
    """
    parts: list of dicts with keys:
        name (str), nand_addr (int), data (bytes)
    Returns a valid RKAF blob.
    The `length` field = total_file_size - 4, matching rkaf_print assertion.
    """
    # We place all part payloads after the header.
    data_base = _RKAF_HDR_TOTAL
    part_dicts = []
    offset = data_base
    for p in parts:
        part_dicts.append({
            "name": p["name"],
            "nand_addr": p["nand_addr"],
            "pos": offset,
            "data": p["data"],
        })
        offset += len(p["data"])

    total_size = offset

    # Build part records
    part_records = b""
    for p in part_dicts:
        data_len = len(p["data"])
        sectors = (data_len + 511) // 512
        part_records += _RKAF_PART_STRUCT.pack(
            p["name"].encode()[:32].ljust(32, b"\x00"),
            b"",   # mount (empty)
            sectors,                  # nand_size
            p["pos"],                 # pos (offset in archive)
            p["nand_addr"],           # nand_addr (LBA on flash)
            sectors,                  # padded_size
            data_len,                 # image_size
        )
    # Pad part records to 16 slots
    part_records += b"\x00" * (_RKAF_PART_STRUCT.size * (16 - len(parts)))

    core = _RKAF_HDR_CORE.pack(
        b"RKAF",
        total_size - 4,    # length field
        b"TestModel",      # model
        b"007",            # id
        b"TestVendor",     # manufacturer
        0, 0x01000000, len(parts),  # unknown1, version, num_parts
    )

    hdr = core + part_records + b"\x00" * _RKAF_HDR_RESERVED
    assert len(hdr) == _RKAF_HDR_TOTAL, \
        f"RKAF header mismatch: {len(hdr)} vs {_RKAF_HDR_TOTAL}"

    payload = b""
    for p in part_dicts:
        payload += p["data"]

    return hdr + payload


# ---------------------------------------------------------------------------
# parameter.txt builder
# ---------------------------------------------------------------------------

PARAMETER_TXT = """\
FIRMWARE_VER:1.0.0
MACHINE_MODEL:TestBoard
MACHINE_ID:007
CHECK_MASK:0x80
PWR_HLD:0,0,A,0,1
TYPE:GPT
CMDLINE:mtdparts=rk29xxnand:0x00002000@0x00002000(uboot),0x00002000@0x00004000(misc),0x00010000@0x00006000(boot),0x00100000@0x00016000(rootfs,-)
"""


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(out_dir: str) -> None:
    os.makedirs(out_dir, exist_ok=True)

    rng = random.Random(42)  # deterministic

    # -----------------------------------------------------------------------
    # random payloads
    # -----------------------------------------------------------------------
    def rand_bytes(n: int) -> bytes:
        return bytes(rng.randint(0, 255) for _ in range(n))

    with open(os.path.join(out_dir, "random_4k.bin"), "wb") as f:
        f.write(rand_bytes(4096))

    with open(os.path.join(out_dir, "random_64k.bin"), "wb") as f:
        f.write(rand_bytes(65536))

    # -----------------------------------------------------------------------
    # Loader blob entries (rc4_disable=1 so no RC4 scrambling needed)
    # -----------------------------------------------------------------------
    e471_data = b"\xaa" * 16
    e472_data = b"\xbb" * 16
    loader_data = b"\xcc" * 64

    loader_bin = build_rkboot(
        e471_payload=e471_data,
        e472_payload=e472_data,
        loader_payload=loader_data,
        rc4_disable=1,
    )
    loader_bin_path = os.path.join(out_dir, "loader.bin")
    with open(loader_bin_path, "wb") as f:
        f.write(loader_bin)
    print(f"  loader.bin: {len(loader_bin)} bytes")

    # -----------------------------------------------------------------------
    # Corrupted RKBOOT (flip last 4 CRC bytes)
    # -----------------------------------------------------------------------
    corrupted = bytearray(loader_bin)
    corrupted[-1] ^= 0xFF
    with open(os.path.join(out_dir, "loader_corrupt.bin"), "wb") as f:
        f.write(bytes(corrupted))
    print("  loader_corrupt.bin written")

    # -----------------------------------------------------------------------
    # Empty loader (0-byte file, triggers "Loader is empty." in cmd_download_boot)
    # -----------------------------------------------------------------------
    with open(os.path.join(out_dir, "zero_byte.bin"), "wb") as _f:
        pass
    print("  zero_byte.bin written")

    # -----------------------------------------------------------------------
    # RKAF — three partitions
    #   parameter @ LBA 0        (nand_addr=0)
    #   misc      @ LBA 0x2000   (8 MiB into the 64 MiB emulated flash)
    #   rootfs    @ LBA 0x4000   (UBI image — triggers block-erase before write)
    # -----------------------------------------------------------------------
    param_data = PARAMETER_TXT.encode()
    misc_data = b"\xdd" * 4096

    ubi_magic = struct.pack("<I", 0x23494255)  # 'U','B','I','#' as LE u32
    rootfs_data = ubi_magic + b"\xee" * (4096 - len(ubi_magic))

    rkaf_blob = build_rkaf([
        {"name": "parameter", "nand_addr": 0,       "data": param_data},
        {"name": "misc",      "nand_addr": 0x2000,  "data": misc_data},
        {"name": "rootfs",    "nand_addr": 0x4000,  "data": rootfs_data},
    ])
    print(f"  rkaf (internal): {len(rkaf_blob)} bytes")

    # -----------------------------------------------------------------------
    # RKFW = loader.bin + rkaf
    # -----------------------------------------------------------------------
    firmware_img = build_rkfw(loader_bin, rkaf_blob)
    firmware_path = os.path.join(out_dir, "firmware.img")
    with open(firmware_path, "wb") as f:
        f.write(firmware_img)
    print(f"  firmware.img: {len(firmware_img)} bytes")

    # -----------------------------------------------------------------------
    # parameter.txt (standalone, used by DI / EF / GPT tests)
    # -----------------------------------------------------------------------
    param_path = os.path.join(out_dir, "parameter.txt")
    with open(param_path, "w") as f:
        f.write(PARAMETER_TXT)
    print("  parameter.txt written")

    # -----------------------------------------------------------------------
    # spl.bin — 512 random bytes representing a U-Boot SPL payload
    # -----------------------------------------------------------------------
    spl_data = rand_bytes(512)
    with open(os.path.join(out_dir, "spl.bin"), "wb") as f:
        f.write(spl_data)
    print(f"  spl.bin: {len(spl_data)} bytes")

    # -----------------------------------------------------------------------
    # ubi_4k.img — 4096-byte UBI image starting with UBI magic
    # The C code checks the LE u32 at offset 0 == 0x23494255 ("UBI#").
    # Stored LE: bytes 0x55, 0x42, 0x49, 0x23.
    # -----------------------------------------------------------------------
    ubi_magic = struct.pack("<I", 0x23494255)  # 'U','B','I','#' as LE u32
    ubi_data = ubi_magic + b"\x00" * (4096 - len(ubi_magic))
    with open(os.path.join(out_dir, "ubi_4k.img"), "wb") as f:
        f.write(ubi_data)
    print(f"  ubi_4k.img: {len(ubi_data)} bytes (magic: {ubi_magic.hex()})")

    # -----------------------------------------------------------------------
    # sparse_4k.img — minimal Android sparse image
    # Sparse file header (28 bytes):
    #   magic=0xED26FF3A  major=1  minor=0  file_hdr_sz=28
    #   chunk_hdr_sz=12  blk_sz=512  total_blks=8  total_chunks=1  crc32=0
    # Chunk header (12 bytes):
    #   type=CHUNK_TYPE_RAW=0xCAC1  reserved=0  chunk_sz=8  total_sz=12+4096
    # Raw data: 8 × 512 = 4096 bytes
    # -----------------------------------------------------------------------
    SPARSE_MAGIC      = 0xED26FF3A
    CHUNK_TYPE_RAW    = 0xCAC1
    blk_sz            = 512
    chunk_blks        = 8
    raw_data          = b"\xAB" * (chunk_blks * blk_sz)
    chunk_total_sz    = 12 + len(raw_data)

    file_hdr  = struct.pack("<IHHHHIIIi",
                            SPARSE_MAGIC,
                            1, 0,      # major, minor
                            28,        # file_hdr_sz
                            12,        # chunk_hdr_sz
                            blk_sz,
                            chunk_blks,  # total_blks
                            1,           # total_chunks
                            0)           # image_checksum
    chunk_hdr = struct.pack("<HHII",
                            CHUNK_TYPE_RAW,
                            0,           # reserved
                            chunk_blks,  # chunk_sz (blocks)
                            chunk_total_sz)

    sparse_data = file_hdr + chunk_hdr + raw_data
    with open(os.path.join(out_dir, "sparse_4k.img"), "wb") as f:
        f.write(sparse_data)
    print(f"  sparse_4k.img: {len(sparse_data)} bytes "
          f"(header={len(file_hdr)}B + 1 chunk of {len(raw_data)}B raw)")

    print(f"\nFixtures written to {out_dir}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <output_dir>", file=sys.stderr)
        sys.exit(1)
    main(sys.argv[1])
