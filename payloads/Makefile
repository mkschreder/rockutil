# Makefile for the MaskROM hexdump payload stubs.
#
# Assembles dump_arm32.s and dump_arm64.s into flat binary blobs, then
# generates C headers (dump_arm32.h / dump_arm64.h) that embed those blobs
# as static const uint8_t arrays ready for inclusion in rkusb.c.
#
# Usage (from the rockutil root):
#   make payloads     — build headers (called automatically by the main build)
#   make -C payloads  — same, from this directory
#   make -C payloads verify  — assert bit-perfect match against rkusb.c arrays
#   make -C payloads clean   — remove all generated artefacts
#
# Cross-assemblers required:
#   arm-linux-gnueabihf-as / arm-linux-gnueabihf-objcopy
#   aarch64-linux-gnu-as   / aarch64-linux-gnu-objcopy

ARM32_AS      ?= arm-linux-gnueabihf-as
ARM32_OBJCOPY ?= arm-linux-gnueabihf-objcopy
ARM64_AS      ?= aarch64-linux-gnu-as
ARM64_OBJCOPY ?= aarch64-linux-gnu-objcopy

PYTHON3 ?= python3

.PHONY: all verify clean

all: dump_arm32.h dump_arm64.h

# ---- ARM32 -----------------------------------------------------------

dump_arm32.o: dump_arm32.s
	$(ARM32_AS) -march=armv7-a $< -o $@

dump_arm32.bin: dump_arm32.o
	$(ARM32_OBJCOPY) -O binary $< $@

dump_arm32.h: dump_arm32.bin gen_header.py
	$(PYTHON3) gen_header.py dump_arm32_tmpl $< $@
	@echo "  GEN $@"

# ---- ARM64 -----------------------------------------------------------

dump_arm64.o: dump_arm64.s
	$(ARM64_AS) $< -o $@

dump_arm64.bin: dump_arm64.o
	$(ARM64_OBJCOPY) -O binary $< $@

dump_arm64.h: dump_arm64.bin gen_header.py
	$(PYTHON3) gen_header.py dump_arm64_tmpl $< $@
	@echo "  GEN $@"

# ---- Verification ----------------------------------------------------

verify: dump_arm32.bin dump_arm64.bin
	@$(PYTHON3) verify.py dump_arm32.bin dump_arm64.bin && echo "verify: PASSED"

# ---- Clean -----------------------------------------------------------

clean:
	$(RM) dump_arm32.o dump_arm32.bin dump_arm32.h
	$(RM) dump_arm64.o dump_arm64.bin dump_arm64.h
