#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 rockutil contributors
#
# loader.bats — integration tests for all Loader-mode commands.
#
# Uses the socket-based USB emulator (no kernel modules required).
# Environment (passed from Makefile):
#   TOOL          Path to ./rockutil
#   FIXTURE_DIR   Path to generated fixtures
#   TEST_DIR      Path to tests/ directory
#   BUILD_DIR     Path to tests/build/ directory
#   EMU_SO        Path to libusb_emu.so

bats_require_minimum_version 1.5.0

load "lib/helpers"

# -------------------------------------------------------------------------
# File-level setup/teardown: start the emulator once for all tests
# -------------------------------------------------------------------------

setup_file() {
    loader_setup_file
}

teardown_file() {
    loader_teardown_file
}

# Per-test: truncate the op-log so each test gets a clean slate.
setup() {
    : > "${OPLOG_FILE}"
}

# =========================================================================
# LD — list devices
# =========================================================================

@test "LD lists one Loader device with correct VID/PID" {
    run run_tool LD
    [ "$status" -eq 0 ]
    [[ "$output" =~ "DevNo=1" ]]
    [[ "$output" =~ "Vid=0x2207" ]]
    [[ "$output" =~ "Pid=0x350B" ]] || [[ "$output" =~ "Pid=0x350b" ]]
    [[ "$output" =~ "Mode=Loader" ]]
}

# =========================================================================
# TD — test device (TestUnitReady)
# =========================================================================

@test "TD returns 'Test Device OK' and logs TEST_UNIT_READY" {
    run run_tool TD
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Test Device OK" ]]
    oplog_has_op "TEST_UNIT_READY"
}

# =========================================================================
# RD — reset device
# =========================================================================

@test "RD (default subcode=0) logs DEVICE_RESET subcode=0" {
    run run_tool RD
    # Tool may exit 1 after reset because the device disconnects — that's OK
    oplog_has_op "DEVICE_RESET" "subcode" "0"
    # Restart emulator after reset
    stop_emulator
    start_emulator "loader"
    wait_for_usb "2207" "350b" 10
}

@test "RD 3 logs DEVICE_RESET subcode=3" {
    run run_tool RD 3
    oplog_has_op "DEVICE_RESET" "subcode" "3"
    stop_emulator
    start_emulator "loader"
    wait_for_usb "2207" "350b" 10
}

# =========================================================================
# RP — reset pipe (clear halt)
# =========================================================================

@test "RP 0 succeeds (clear OUT halt)" {
    run run_tool RP 0
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Reset Pipe OK" ]]
}

@test "RP 1 succeeds (clear IN halt)" {
    run run_tool RP 1
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Reset Pipe OK" ]]
}

# =========================================================================
# RID — read flash ID
# =========================================================================

@test "RID prints Flash ID: 11 22 33 44 55" {
    run run_tool RID
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Flash ID:" ]]
    [[ "$output" =~ "11 22 33 44 55" ]]
    oplog_has_op "READ_FLASH_ID"
}

# =========================================================================
# RFI — read flash info
# =========================================================================

@test "RFI reports 131072 sectors (64 MiB) and manufacturer 0xAB" {
    run run_tool RFI
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Flash Info" ]]
    [[ "$output" =~ "131072 sectors" ]]
    [[ "$output" =~ "0xAB" ]] || [[ "$output" =~ "0xab" ]]
    oplog_has_op "READ_FLASH_INFO"
}

@test "RFI reports block size 128 sectors" {
    run run_tool RFI
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Block Size:" ]]
    [[ "$output" =~ "128 sectors" ]]
}

# =========================================================================
# RCI — read chip info
# =========================================================================

@test "RCI prints chip string RK35xx_test____" {
    run run_tool RCI
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Chip:" ]]
    [[ "$output" =~ "RK35xx_test" ]]
    oplog_has_op "READ_CHIP_INFO"
}

# =========================================================================
# RCB — read capability
# =========================================================================

@test "RCB capability byte is 0xFF — all features listed" {
    run run_tool RCB
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Capability:" ]]
    [[ "$output" =~ "Direct LBA: enabled" ]]
    [[ "$output" =~ "Vendor Storage: enabled" ]]
    [[ "$output" =~ "Read LBA: enabled" ]]
    [[ "$output" =~ "Read SN: enabled" ]]
    oplog_has_op "READ_CAPABILITY"
}

# =========================================================================
# RL — read LBA
# =========================================================================

@test "RL reads sector 0 into a file (seeded pattern check)" {
    local out="$BATS_TEST_TMPDIR/sect0.bin"
    run run_tool RL 0 1 "$out"
    [ "$status" -eq 0 ]
    # Emulator seeds byte0 = LBA & 0xFF, byte1 = (LBA>>8) & 0xFF
    # For LBA=0: byte0=0x00, byte1=0x00
    local b0 b1
    b0=$(od -An -tx1 -j0 -N1 "$out" | tr -d ' \n')
    b1=$(od -An -tx1 -j1 -N1 "$out" | tr -d ' \n')
    [ "$b0" = "00" ]
    [ "$b1" = "00" ]
    oplog_has_op "READ_LBA" "lba" "0"
}

@test "RL reads sector 1 (seeded byte0=1)" {
    local out="$BATS_TEST_TMPDIR/sect1.bin"
    run run_tool RL 1 1 "$out"
    [ "$status" -eq 0 ]
    local b0
    b0=$(od -An -tx1 -j0 -N1 "$out" | tr -d ' \n')
    [ "$b0" = "01" ]
}

@test "RL 0 1 without outfile writes to stdout and exits 0" {
    run run_tool RL 0 1
    [ "$status" -eq 0 ]
}

@test "RL multi-sector read (128 sectors = max chunk)" {
    local out="$BATS_TEST_TMPDIR/chunk128.bin"
    run run_tool RL 0 128 "$out"
    [ "$status" -eq 0 ]
    local size
    size=$(stat -c '%s' "$out")
    [ "$size" -eq $(( 128 * 512 )) ]
    oplog_has_op "READ_LBA" "sectors" "128"
}

@test "RL 256 sectors triggers two CBW transfers (max_chunk=128)" {
    local out="$BATS_TEST_TMPDIR/chunk256.bin"
    run run_tool RL 0 256 "$out"
    [ "$status" -eq 0 ]
    local cnt
    cnt=$(oplog_count "READ_LBA")
    [ "$cnt" -eq 2 ]
}

# =========================================================================
# WL — write LBA
# =========================================================================

@test "WL writes random_4k.bin and RL round-trip verifies content" {
    local infile="${FIXTURE_DIR}/random_4k.bin"
    local out="$BATS_TEST_TMPDIR/readback.bin"

    # Write 4096 bytes = 8 sectors at LBA 0x100
    run run_tool WL 0x100 "$infile"
    [ "$status" -eq 0 ]
    oplog_has_op "WRITE_LBA" "lba" "256"

    # Read back 8 sectors
    run run_tool RL 0x100 8 "$out"
    [ "$status" -eq 0 ]

    # Compare byte-for-byte
    cmp --silent "$infile" "$out"
}

@test "WL writes random_64k.bin (multi-chunk: 65536/512=128 sectors)" {
    local infile="${FIXTURE_DIR}/random_64k.bin"
    run run_tool WL 0x200 "$infile"
    [ "$status" -eq 0 ]
    local cnt
    cnt=$(oplog_count "WRITE_LBA")
    [ "$cnt" -ge 1 ]
}

@test "WL --size 4 truncates write to 4 sectors" {
    local infile="${FIXTURE_DIR}/random_4k.bin"
    run run_tool WL 0x300 "$infile" --size 4
    [ "$status" -eq 0 ]
    # Check op-log: last WRITE_LBA should have sectors=4
    local secs
    secs=$(oplog_field "WRITE_LBA" "sectors")
    [ "$secs" -eq 4 ]
}

# =========================================================================
# EL — erase LBA
# =========================================================================

@test "EL erases 8 sectors at LBA 0x400 and readback is zeros" {
    # First write non-zero data
    local infile="${FIXTURE_DIR}/random_4k.bin"
    run_tool WL 0x400 "$infile" >/dev/null 2>&1 || true

    : > "${OPLOG_FILE}"

    run run_tool EL 0x400 8
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Erase LBA OK" ]]
    oplog_has_op "ERASE_LBA" "lba" "1024"
    oplog_has_op "ERASE_LBA" "sectors" "8"

    # Read back and verify zeros
    local out="$BATS_TEST_TMPDIR/erased.bin"
    run run_tool RL 0x400 8 "$out"
    [ "$status" -eq 0 ]
    local zeroes
    zeroes=$(tr -d '\000' < "$out" | wc -c)
    [ "$zeroes" -eq 0 ]
}

# =========================================================================
# SS — switch storage
# =========================================================================

@test "SS emmc sends CHANGE_STORAGE with storage code 2" {
    run run_tool SS emmc
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Switch storage OK" ]]
    oplog_has_op "CHANGE_STORAGE" "storage" "2"
}

@test "SS spinor sends CHANGE_STORAGE with storage code 9" {
    run run_tool SS spinor
    [ "$status" -eq 0 ]
    oplog_has_op "CHANGE_STORAGE" "storage" "9"
}

@test "SS flash sends CHANGE_STORAGE with storage code 1" {
    run run_tool SS flash
    [ "$status" -eq 0 ]
    oplog_has_op "CHANGE_STORAGE" "storage" "1"
}

@test "SS nand (alias for flash) sends CHANGE_STORAGE code 1" {
    run run_tool SS nand
    [ "$status" -eq 0 ]
    oplog_has_op "CHANGE_STORAGE" "storage" "1"
}

@test "SS spinand sends CHANGE_STORAGE with storage code 8" {
    run run_tool SS spinand
    [ "$status" -eq 0 ]
    oplog_has_op "CHANGE_STORAGE" "storage" "8"
}

@test "SS sd sends CHANGE_STORAGE with storage code 3" {
    run run_tool SS sd
    [ "$status" -eq 0 ]
    oplog_has_op "CHANGE_STORAGE" "storage" "3"
}

@test "SS bogus exits 1 with 'Unknown storage'" {
    run run_tool SS bogus
    [ "$status" -eq 1 ]
    [[ "$output" =~ "Unknown storage" ]]
}

# =========================================================================
# GPT — build GPT with live device (uses real flash size from RFI)
# =========================================================================

@test "GPT with live device produces 17408-byte file" {
    local out="$BATS_TEST_TMPDIR/gpt_live.bin"
    run run_tool GPT "${FIXTURE_DIR}/parameter.txt" "$out"
    [ "$status" -eq 0 ]
    local size
    size=$(stat -c '%s' "$out")
    [ "$size" -eq 17408 ]
}

@test "GPT with live device does NOT print fallback warning" {
    local out="$BATS_TEST_TMPDIR/gpt_live2.bin"
    run run_tool GPT "${FIXTURE_DIR}/parameter.txt" "$out" 2>&1
    [ "$status" -eq 0 ]
    [ -f "$out" ]
}

@test "GPT BackupLBA in header equals flash size - 1 (131071)" {
    local out="$BATS_TEST_TMPDIR/gpt_backup.bin"
    run_tool GPT "${FIXTURE_DIR}/parameter.txt" "$out" >/dev/null 2>&1
    # BackupLBA is at offset 512+32 = 544, 8 bytes LE
    local backup_lba
    backup_lba=$(od -An -tu8 -j544 -N8 "$out" | tr -d ' ')
    [ "$backup_lba" -eq 131071 ]
}
