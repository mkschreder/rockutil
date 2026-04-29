#!/usr/bin/env python3
"""
verify.py  <dump_arm32.bin> <dump_arm64.bin>

Checks that the two freshly assembled payload binaries are byte-for-byte
identical to the reference snapshots embedded in the header files
(dump_arm32.h / dump_arm64.h).  Exits with status 0 on success, 1 on any
mismatch.

The reference bytes are extracted from the *_tmpl[] arrays produced by
gen_header.py so that the build is always self-consistent.
"""

import re
import sys
import os


def extract_header_bytes(header_path, array_name):
    """Return the bytes of the named array from a generated .h file."""
    text = open(header_path).read()
    start = text.index(f"static const uint8_t {array_name}[]") 
    block = text[start:]
    arr_start = block.index("{") + 1
    arr_end   = block.index("};", arr_start)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", block[arr_start:arr_end]))


def compare(label, built_path, ref_bytes):
    built = open(built_path, "rb").read()
    if len(built) != len(ref_bytes):
        print(f"FAIL {label}: size {len(built)} != {len(ref_bytes)}")
        return False
    diffs = [(i, built[i], ref_bytes[i]) for i in range(len(built)) if built[i] != ref_bytes[i]]
    if diffs:
        print(f"FAIL {label}: {len(diffs)} byte difference(s):")
        for off, bv, rv in diffs[:20]:
            print(f"  +0x{off:04x}: built=0x{bv:02x}  reference=0x{rv:02x}")
        return False
    print(f"OK   {label}: {len(built)} bytes, bit-perfect match")
    return True


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} dump_arm32.bin dump_arm64.bin")

    arm32_bin, arm64_bin = sys.argv[1], sys.argv[2]
    script_dir = os.path.dirname(os.path.abspath(__file__))

    ref32 = extract_header_bytes(os.path.join(script_dir, "dump_arm32.h"), "dump_arm32_tmpl")
    ref64 = extract_header_bytes(os.path.join(script_dir, "dump_arm64.h"), "dump_arm64_tmpl")

    ok32 = compare("ARM32", arm32_bin, ref32)
    ok64 = compare("ARM64", arm64_bin, ref64)

    sys.exit(0 if (ok32 and ok64) else 1)


if __name__ == "__main__":
    main()
