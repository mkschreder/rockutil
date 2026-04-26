#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 rockutil contributors
# offline.bats — tests that require no USB device and no root privileges.
#
# Environment:
#   TOOL         path to the rockutil binary (default: ./rockutil)
#   FIXTURE_DIR  path to the generated fixtures directory

bats_require_minimum_version 1.5.0

TOOL="${TOOL:-./rockutil}"
FIXTURE_DIR="${FIXTURE_DIR:-tests/build/fixtures}"

# ---------------------------------------------------------------------------
# Help and version
# ---------------------------------------------------------------------------

@test "H prints usage and exits 0" {
    run "$TOOL" H
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Tool Usage" ]]
    [[ "$output" =~ "ListDevice" ]]
}

@test "-h prints usage and exits 0" {
    run "$TOOL" -h
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Tool Usage" ]]
}

@test "--help prints usage and exits 0" {
    run "$TOOL" --help
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Tool Usage" ]]
}

@test "V prints version and exits 0" {
    run "$TOOL" V
    [ "$status" -eq 0 ]
    [[ "$output" =~ "rockutil" ]]
    [[ "$output" =~ "libusb" ]]
}

@test "-v prints version and exits 0" {
    run "$TOOL" -v
    [ "$status" -eq 0 ]
    [[ "$output" =~ "rockutil" ]]
}

@test "--version prints version and exits 0" {
    run "$TOOL" --version
    [ "$status" -eq 0 ]
    [[ "$output" =~ "rockutil" ]]
}

@test "no arguments prints usage and exits 0" {
    run "$TOOL"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Tool Usage" ]]
}

# ---------------------------------------------------------------------------
# PRINT — offline parsing (no device)
# ---------------------------------------------------------------------------

@test "PRINT firmware.img shows RKFW header" {
    run "$TOOL" PRINT "$FIXTURE_DIR/firmware.img"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "RKFW header" ]]
    [[ "$output" =~ "version" ]]
    [[ "$output" =~ "loader" ]]
    [[ "$output" =~ "image" ]]
}

@test "PRINT firmware.img shows embedded RKBOOT with 471/472 entries" {
    run "$TOOL" PRINT "$FIXTURE_DIR/firmware.img"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "RKBOOT header" ]]
    [[ "$output" =~ "entries 471" ]]
    [[ "$output" =~ "entries 472" ]]
    [[ "$output" =~ "rc4 disabled   : yes" ]]
    [[ "$output" =~ 'name="DDR"' ]]
    [[ "$output" =~ 'name="USB"' ]]
}

@test "PRINT firmware.img shows RKAF partitions" {
    run "$TOOL" PRINT "$FIXTURE_DIR/firmware.img"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "RKAF header" ]]
    [[ "$output" =~ "partitions  : 2" ]]
    [[ "$output" =~ "parameter" ]]
    [[ "$output" =~ "misc" ]]
}

@test "PRINT loader.bin shows standalone RKBOOT" {
    run "$TOOL" PRINT "$FIXTURE_DIR/loader.bin"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "RKBOOT header" ]]
    [[ "$output" =~ "entries 471    : 1" ]]
    [[ "$output" =~ "entries 472    : 1" ]]
    [[ "$output" =~ "entries loader : 1" ]]
    [[ "$output" =~ 'name="LDR"' ]]
}

@test "PRINT loader.bin shows entry payload head bytes" {
    run "$TOOL" PRINT "$FIXTURE_DIR/loader.bin"
    [ "$status" -eq 0 ]
    # 471 entry is 0xAA * 16
    [[ "$output" =~ "head: aa aa aa aa" ]]
    # 472 entry is 0xBB * 16
    [[ "$output" =~ "head: bb bb bb bb" ]]
}

# ---------------------------------------------------------------------------
# GPT — offline path (no device → fallback size warning)
# ---------------------------------------------------------------------------

@test "GPT builds a 17408-byte file when no device is present" {
    local out="$BATS_TEST_TMPDIR/gpt_out.bin"
    run "$TOOL" GPT "$FIXTURE_DIR/parameter.txt" "$out"
    # Exit 0 even when no device: the tool falls back to 4 GiB default
    [ "$status" -eq 0 ]
    local size
    size=$(stat -c '%s' "$out")
    [ "$size" -eq 17408 ]
}

@test "GPT output starts with protective MBR magic at byte 510" {
    local out="$BATS_TEST_TMPDIR/gpt_mbr.bin"
    run "$TOOL" GPT "$FIXTURE_DIR/parameter.txt" "$out"
    [ "$status" -eq 0 ]
    # Bytes 510-511 of the protective MBR must be 0x55 0xAA
    local b510 b511
    b510=$(od -An -tx1 -j510 -N1 "$out" | tr -d ' ')
    b511=$(od -An -tx1 -j511 -N1 "$out" | tr -d ' ')
    [ "$b510" = "55" ]
    [ "$b511" = "aa" ]
}

@test "GPT output has EFI PART signature at byte 512" {
    local out="$BATS_TEST_TMPDIR/gpt_sig.bin"
    run "$TOOL" GPT "$FIXTURE_DIR/parameter.txt" "$out"
    [ "$status" -eq 0 ]
    # First 8 bytes of GPT header (LBA1) = "EFI PART"
    local sig
    sig=$(dd if="$out" bs=1 skip=512 count=8 2>/dev/null | cat)
    [ "$sig" = "EFI PART" ]
}

@test "GPT fallback warns about missing device size" {
    local out="$BATS_TEST_TMPDIR/gpt_warn.bin"
    run "$TOOL" GPT "$FIXTURE_DIR/parameter.txt" "$out"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "warning" ]] || [[ "${lines[@]}" =~ "warning" ]] || \
        [[ "$(cat /dev/stderr 2>/dev/null)" =~ "warning" ]] || \
        [[ "$output$stderr" =~ "fallback" ]]
}

# ---------------------------------------------------------------------------
# Negative cases — no device needed
# ---------------------------------------------------------------------------

@test "unknown command exits 1 with error message" {
    run "$TOOL" BOGUSCMD
    [ "$status" -eq 1 ]
    [[ "$output" =~ "invalid" ]] || [[ "$output" =~ "usage" ]]
}

@test "PRINT with no arguments exits 1" {
    run "$TOOL" PRINT
    [ "$status" -eq 1 ]
}

@test "PRINT on a non-existent file exits 1" {
    run "$TOOL" PRINT /nonexistent/file.img
    [ "$status" -eq 1 ]
}

@test "PRINT on a file with unknown magic exits 1" {
    local bad="$BATS_TEST_TMPDIR/bad.bin"
    printf '\x00\x01\x02\x03\x04\x05\x06\x07' > "$bad"
    run "$TOOL" PRINT "$bad"
    [ "$status" -eq 1 ]
    [[ "$output" =~ "Unknown container" ]]
}

@test "GPT with missing parameter file exits 1" {
    run "$TOOL" GPT /nonexistent/parameter.txt "$BATS_TEST_TMPDIR/out.bin"
    [ "$status" -eq 1 ]
}

@test "GPT with too few arguments exits 1" {
    run "$TOOL" GPT
    [ "$status" -eq 1 ]
}

@test "WL with too few arguments exits 1" {
    run "$TOOL" WL
    [ "$status" -eq 1 ]
}

@test "RL with too few arguments exits 1" {
    run "$TOOL" RL
    [ "$status" -eq 1 ]
}

@test "EL with too few arguments exits 1" {
    run "$TOOL" EL
    [ "$status" -eq 1 ]
}

@test "DB with no arguments exits 1" {
    run "$TOOL" DB
    [ "$status" -eq 1 ]
}

@test "DB on empty file exits 1 with 'Loader is empty'" {
    run "$TOOL" DB "$FIXTURE_DIR/zero_byte.bin"
    [ "$status" -eq 1 ]
    [[ "$output" =~ "empty" ]]
}

@test "SS with unknown storage name exits 1" {
    run "$TOOL" SS badtype
    [ "$status" -eq 1 ]
    [[ "$output" =~ "Unknown storage" ]]
}
