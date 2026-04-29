#!/usr/bin/env python3
"""
gen_header.py  <var_name> <input.bin> <output.h>

Converts a flat binary file into a C header declaring:

    static const uint8_t <var_name>[] = { ... };
"""

import sys
import os

if len(sys.argv) != 4:
    sys.exit(f"usage: {sys.argv[0]} <var_name> <input.bin> <output.h>")

var_name, bin_path, hdr_path = sys.argv[1], sys.argv[2], sys.argv[3]
src_name = os.path.splitext(os.path.basename(bin_path))[0] + ".s"

data = open(bin_path, "rb").read()

with open(hdr_path, "w") as f:
    f.write(f"/* Auto-generated from payloads/{src_name} — do not edit.\n")
    f.write(f" * Regenerate with:  make -C payloads  */\n")
    f.write(f"static const uint8_t {var_name}[] = {{\n")
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        f.write("\t" + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    f.write("};\n")
