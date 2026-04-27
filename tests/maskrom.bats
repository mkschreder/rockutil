#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 rockutil contributors
#
# maskrom.bats — integration tests for MaskROM-mode commands.
#
# The emulator starts in MaskROM mode (VID 0x2207 / PID 0x350A).
# Commands that require the MaskROM handshake (UL, UF) trigger the
# emulator to auto-transition to Loader mode after receiving the
# 0x471 + 0x472 control transfers.
#
# Uses the socket-based USB emulator (no kernel modules required).

bats_require_minimum_version 1.5.0

load "lib/helpers"

# -------------------------------------------------------------------------
# File-level setup: start in MaskROM mode
# -------------------------------------------------------------------------

setup_file() {
    maskrom_setup_file
}

teardown_file() {
    stop_emulator
}

# Each test that does a MaskROM→Loader transition must restart the
# emulator in MaskROM mode afterwards so the next test has a clean state.
_restart_maskrom() {
    stop_emulator
    start_emulator "maskrom"
    wait_for_usb "2207" "350a" 15
    : > "${OPLOG_FILE}"
}

setup() {
    : > "${OPLOG_FILE}"
}

# =========================================================================
# DB — download boot (raw MaskROM control transfer)
# =========================================================================

@test "DB loader.bin sends 0x472 control transfer with valid CRC-CCITT" {
    run run_tool DB "$FIXTURE_DIR/loader.bin"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Download Boot Success" ]]
    # Verify op-log: wIndex=0x472 (1138), crc_ok=true
    oplog_has_op "ctrl_upload" "wIndex" "1138"
    oplog_has_op "ctrl_upload" "crc_ok" "True"
    _restart_maskrom
}

@test "DB loader.bin --code 0x471 sends 0x471 control transfer" {
    run run_tool DB "$FIXTURE_DIR/loader.bin" --code 0x471
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Download Boot Success" ]]
    oplog_has_op "ctrl_upload" "wIndex" "1137"
    oplog_has_op "ctrl_upload" "crc_ok" "True"
    _restart_maskrom
}

@test "DB zero_byte.bin exits 1 with 'Loader is empty'" {
    run run_tool DB "$FIXTURE_DIR/zero_byte.bin"
    [ "$status" -eq 1 ]
    [[ "$output" =~ "empty" ]]
}

# =========================================================================
# UL — upgrade loader (full MaskROM → Loader handshake)
# =========================================================================

@test "UL loader.bin completes handshake: 471+472 sent, then WRITE_LOADER" {
    run run_tool UL "$FIXTURE_DIR/loader.bin"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Loader upgrade done" ]]

    # Verify both DDR-init (471) and loader (472) control transfers were sent
    oplog_has_op "ctrl_upload" "wIndex_name" "471"
    oplog_has_op "ctrl_upload" "wIndex_name" "472"

    # After handshake, tool should have sent WRITE_LOADER CBW
    oplog_has_op "WRITE_LOADER"

    _restart_maskrom
}

@test "UL loader.bin: each control transfer has valid CRC-CCITT" {
    run run_tool UL "$FIXTURE_DIR/loader.bin"
    [ "$status" -eq 0 ]
    # All ctrl_upload entries must have crc_ok=True
    python3 - <<PYEOF
import json, sys
lines = open("${OPLOG_FILE}").readlines()
bad = [l for l in lines
       if json.loads(l.strip()).get("op") == "ctrl_upload"
       and not json.loads(l.strip()).get("crc_ok")]
if bad:
    print("BAD CRC entries:", bad, file=sys.stderr)
    sys.exit(1)
PYEOF
    _restart_maskrom
}

@test "UL firmware.img extracts loader from RKFW and completes" {
    run run_tool UL "$FIXTURE_DIR/firmware.img"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Loader upgrade done" ]]
    oplog_has_op "ctrl_upload"
    oplog_has_op "WRITE_LOADER"
    _restart_maskrom
}

# =========================================================================
# UF — upgrade full firmware
# =========================================================================

@test "UF firmware.img completes MaskROM handshake and flashes partitions" {
    run run_tool UF "$FIXTURE_DIR/firmware.img"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "Firmware upgrade complete" ]]

    # Handshake must have happened
    oplog_has_op "ctrl_upload" "wIndex_name" "471"
    oplog_has_op "ctrl_upload" "wIndex_name" "472"

    # Flash info was read
    oplog_has_op "READ_FLASH_INFO"

    # Both partitions from the RKAF were written
    oplog_has_op "WRITE_LBA"

    _restart_maskrom
}

@test "UF firmware.img writes 'parameter' partition at LBA 0" {
    run run_tool UF "$FIXTURE_DIR/firmware.img"
    [ "$status" -eq 0 ]
    oplog_has_op "WRITE_LBA" "lba" "0"
    _restart_maskrom
}

@test "UF firmware.img writes 'misc' partition at LBA 0x2000" {
    run run_tool UF "$FIXTURE_DIR/firmware.img"
    [ "$status" -eq 0 ]
    oplog_has_op "WRITE_LBA" "lba" "8192"
    _restart_maskrom
}

@test "UF firmware.img: after flash, readback of misc partition matches fixture data" {
    run run_tool UF "$FIXTURE_DIR/firmware.img"
    [ "$status" -eq 0 ]

    # Read back misc partition (4096 bytes = 8 sectors at LBA 0x2000=8192)
    # The emulator is now in Loader mode (post-transition)
    local out="$BATS_TEST_TMPDIR/misc_readback.bin"
    run run_tool RL 0x2000 8 "$out"
    [ "$status" -eq 0 ]

    # misc_data in fixture is 0xDD * 4096
    local b0
    b0=$(od -An -tx1 -j0 -N1 "$out" | tr -d ' \n')
    [ "$b0" = "dd" ]

    _restart_maskrom
}

@test "UF firmware.img uses ERASE_FORCE (not ERASE_LBA) for rootfs UBI partition" {
    # The firmware.img fixture contains a 'rootfs' partition at LBA 0x4000
    # with UBI magic.  The UF path must issue ERASE_FORCE (block-level erase)
    # instead of ERASE_LBA to avoid the non-block-aligned split boundary bug.
    # Emulator reports sector_per_blk=128; LBA 0x4000 / 128 = 0x80 = 128.
    run run_tool UF "$FIXTURE_DIR/firmware.img"
    [ "$status" -eq 0 ]

    # ERASE_FORCE must appear for the rootfs UBI partition
    oplog_has_op "ERASE_FORCE"
    oplog_has_op "ERASE_FORCE" "block" "128"

    # ERASE_LBA must NOT be emitted for UBI partitions
    local cnt
    cnt=$(oplog_count "ERASE_LBA")
    [ "$cnt" -eq 0 ]

    # rootfs data must still be present after the erase+write
    local out="$BATS_TEST_TMPDIR/rootfs_readback.bin"
    run run_tool RL 0x4000 8 "$out"
    [ "$status" -eq 0 ]

    _restart_maskrom
}

# =========================================================================
# DI — download image
# =========================================================================

@test "DI parameter writes PARM header at LBA 0, 0x400, 0x800, 0xc00" {
    stop_emulator
    start_emulator "loader"
    wait_for_usb "2207" "350b" 10
    : > "${OPLOG_FILE}"

    run run_tool DI parameter "$FIXTURE_DIR/parameter.txt" \
                               "$FIXTURE_DIR/parameter.txt"
    [ "$status" -eq 0 ]

    # Four WRITE_LBA calls: one per copy (0, 0x400, 0x800, 0xc00)
    local cnt
    cnt=$(oplog_count "WRITE_LBA")
    [ "$cnt" -eq 4 ]

    # Verify PARM magic in flash at LBA 0
    local out="$BATS_TEST_TMPDIR/parm.bin"
    run run_tool RL 0 4 "$out"
    [ "$status" -eq 0 ]
    [[ "$(od -An -tx1 -j0 -N4 "$out" | tr -d ' \n')" = "5041524d" ]]

    # Return to maskrom for remaining tests
    stop_emulator
    start_emulator "maskrom"
    wait_for_usb "2207" "350a" 10
    : > "${OPLOG_FILE}"
}

@test "DI misc random_4k.bin writes to misc partition LBA" {
    stop_emulator
    start_emulator "loader"
    wait_for_usb "2207" "350b" 10
    : > "${OPLOG_FILE}"

    run run_tool DI misc "$FIXTURE_DIR/random_4k.bin" \
                          "$FIXTURE_DIR/parameter.txt"
    [ "$status" -eq 0 ]

    # misc is at lba_start = 0x4000 = 16384 in parameter.txt
    oplog_has_op "WRITE_LBA" "lba" "16384"

    stop_emulator
    start_emulator "maskrom"
    wait_for_usb "2207" "350a" 10
    : > "${OPLOG_FILE}"
}

@test "DI bogus partition exits 1 with 'not in parameter.txt'" {
    stop_emulator
    start_emulator "loader"
    wait_for_usb "2207" "350b" 10

    run run_tool DI bogus_partition \
                    "$FIXTURE_DIR/random_4k.bin" \
                    "$FIXTURE_DIR/parameter.txt"
    [ "$status" -eq 1 ]
    [[ "$output" =~ "not in parameter.txt" ]]

    stop_emulator
    start_emulator "maskrom"
    wait_for_usb "2207" "350a" 10
    : > "${OPLOG_FILE}"
}

# =========================================================================
# EF — erase flash per parameter.txt
# =========================================================================

@test "EF parameter.txt erases each defined partition (one ERASE_LBA per part)" {
    stop_emulator
    start_emulator "loader"
    wait_for_usb "2207" "350b" 10
    : > "${OPLOG_FILE}"

    run run_tool EF "$FIXTURE_DIR/parameter.txt"
    [ "$status" -eq 0 ]

    # parameter.txt defines 4 partitions: uboot, misc, boot, rootfs
    local cnt
    cnt=$(oplog_count "ERASE_LBA")
    [ "$cnt" -ge 3 ]

    stop_emulator
    start_emulator "maskrom"
    wait_for_usb "2207" "350a" 10
    : > "${OPLOG_FILE}"
}

# =========================================================================
# Loader-mode commands must refuse to run against a MaskROM device
# =========================================================================

@test "RID on MaskROM device exits 1 with MaskROM mode message" {
    run run_tool RID
    [ "$status" -eq 1 ]
    [[ "$output" =~ "MaskROM" ]]
}

@test "RFI on MaskROM device exits 1 with MaskROM mode message" {
    run run_tool RFI
    [ "$status" -eq 1 ]
    [[ "$output" =~ "MaskROM" ]]
}

@test "TD on MaskROM device exits 1 with MaskROM mode message" {
    run run_tool TD
    [ "$status" -eq 1 ]
    [[ "$output" =~ "MaskROM" ]]
}

@test "SS on MaskROM device exits 1 with MaskROM mode message" {
    run run_tool SS nand
    [ "$status" -eq 1 ]
    [[ "$output" =~ "MaskROM" ]]
}

# =========================================================================
# Negative MaskROM tests
# =========================================================================

@test "DB loader_corrupt.bin handles gracefully (does not crash)" {
    run run_tool DB "$FIXTURE_DIR/loader_corrupt.bin"
    [ "$status" -eq 0 ] || [ "$status" -eq 1 ]
}

@test "UL non-existent file exits 1" {
    run run_tool UL /nonexistent/loader.bin
    [ "$status" -eq 1 ]
}

@test "UF non-existent firmware file exits 1" {
    run run_tool UF /nonexistent/firmware.img
    [ "$status" -eq 1 ]
}
