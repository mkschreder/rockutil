#!/usr/bin/env bats
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

@test "GPT builds a 34304-byte file (primary+backup) when no device is present" {
    local out="$BATS_TEST_TMPDIR/gpt_out.bin"
    run "$TOOL" GPT "$FIXTURE_DIR/parameter.txt" "$out"
    # Exit 0 even when no device: the tool falls back to 4 GiB default
    [ "$status" -eq 0 ]
    local size
    size=$(stat -c '%s' "$out")
    # (34 primary + 33 backup) * 512 = 34304
    [ "$size" -eq 34304 ]
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

# ---------------------------------------------------------------------------
# GPT full output — primary and backup regions
# ---------------------------------------------------------------------------

@test "GPT backup area has EFI PART signature at the correct offset" {
    local out="$BATS_TEST_TMPDIR/gpt_full.bin"
    run "$TOOL" GPT "$FIXTURE_DIR/parameter.txt" "$out"
    [ "$status" -eq 0 ]
    # Backup GPT header is the last sector of the blob:
    #   offset = (34 + 33 - 1) * 512 = 66 * 512 = 33792
    local sig
    sig=$(dd if="$out" bs=1 skip=33792 count=8 2>/dev/null | cat)
    [ "$sig" = "EFI PART" ]
}

@test "GPT primary and backup both contain EFI PART magic" {
    local out="$BATS_TEST_TMPDIR/gpt_both.bin"
    run "$TOOL" GPT "$FIXTURE_DIR/parameter.txt" "$out"
    [ "$status" -eq 0 ]
    # Primary header at byte 512
    local primary_sig
    primary_sig=$(dd if="$out" bs=1 skip=512 count=8 2>/dev/null | cat)
    [ "$primary_sig" = "EFI PART" ]
    # Backup header at byte 33792
    local backup_sig
    backup_sig=$(dd if="$out" bs=1 skip=33792 count=8 2>/dev/null | cat)
    [ "$backup_sig" = "EFI PART" ]
}

# ---------------------------------------------------------------------------
# PACK — build RKBOOT container from raw blobs
# ---------------------------------------------------------------------------

@test "PACK produces a file with BOOT magic" {
    local out="$BATS_TEST_TMPDIR/packed.bin"
    run "$TOOL" PACK 0x33353061 \
        "$FIXTURE_DIR/loader.bin" \
        "$FIXTURE_DIR/loader.bin" \
        "$FIXTURE_DIR/loader.bin" \
        "$out"
    [ "$status" -eq 0 ]
    [ -f "$out" ]
    local magic
    magic=$(dd if="$out" bs=1 count=4 2>/dev/null | cat)
    [ "$magic" = "BOOT" ]
}

@test "PACK output can be parsed by PRINT" {
    local ddr="$BATS_TEST_TMPDIR/ddr.bin"
    local usb="$BATS_TEST_TMPDIR/usb.bin"
    local ldr="$BATS_TEST_TMPDIR/ldr.bin"
    printf '\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa' > "$ddr"
    printf '\xbb\xbb\xbb\xbb\xbb\xbb\xbb\xbb' > "$usb"
    printf '\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc' > "$ldr"
    local out="$BATS_TEST_TMPDIR/pack_out.bin"
    run "$TOOL" PACK 0x33353061 "$ddr" "$usb" "$ldr" "$out"
    [ "$status" -eq 0 ]
    run "$TOOL" PRINT "$out"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "RKBOOT header" ]]
    [[ "$output" =~ "entries 471    : 1" ]]
    [[ "$output" =~ "entries 472    : 1" ]]
    [[ "$output" =~ "entries loader : 1" ]]
}

@test "PACK with missing input file exits 1" {
    run "$TOOL" PACK 0x33353061 \
        /nonexistent/ddr.bin \
        "$FIXTURE_DIR/loader.bin" \
        "$FIXTURE_DIR/loader.bin" \
        "$BATS_TEST_TMPDIR/out.bin"
    [ "$status" -eq 1 ]
}

@test "PACK with too few arguments exits 1" {
    run "$TOOL" PACK
    [ "$status" -eq 1 ]
}

# ---------------------------------------------------------------------------
# UNPACK — extract RKBOOT entries to files
# ---------------------------------------------------------------------------

@test "UNPACK extracts 3 entry files from loader.bin" {
    local outdir="$BATS_TEST_TMPDIR/unpack_out"
    mkdir -p "$outdir"
    run "$TOOL" UNPACK "$FIXTURE_DIR/loader.bin" "$outdir"
    [ "$status" -eq 0 ]
    # Should produce 471, 472, and loader files
    local count
    count=$(ls "$outdir"/*.bin 2>/dev/null | wc -l)
    [ "$count" -ge 3 ]
}

@test "UNPACK 471 entry has correct content (0xAA bytes)" {
    local outdir="$BATS_TEST_TMPDIR/unpack_aa"
    mkdir -p "$outdir"
    run "$TOOL" UNPACK "$FIXTURE_DIR/loader.bin" "$outdir"
    [ "$status" -eq 0 ]
    local f471
    f471=$(ls "$outdir"/471_*.bin 2>/dev/null | head -1)
    [ -n "$f471" ]
    local first
    first=$(od -An -tx1 -N1 "$f471" | tr -d ' ')
    [ "$first" = "aa" ]
}

@test "UNPACK on non-RKBOOT file exits 1" {
    run "$TOOL" UNPACK "$FIXTURE_DIR/random_4k.bin" "$BATS_TEST_TMPDIR"
    [ "$status" -eq 1 ]
}

@test "UNPACK with too few arguments exits 1" {
    run "$TOOL" UNPACK
    [ "$status" -eq 1 ]
}

# ---------------------------------------------------------------------------
# TAGSPL — prepend 4-byte chip tag to SPL binary
# ---------------------------------------------------------------------------

@test "TAGSPL output starts with the 4-byte chip tag" {
    local out="$BATS_TEST_TMPDIR/tagged_spl.bin"
    run "$TOOL" TAGSPL 0x33353061 "$FIXTURE_DIR/spl.bin" "$out"
    [ "$status" -eq 0 ]
    [ -f "$out" ]
    # Chip tag 0x33353061 stored LE: 61 30 35 33
    local b0 b1 b2 b3
    b0=$(od -An -tx1 -j0 -N1 "$out" | tr -d ' ')
    b1=$(od -An -tx1 -j1 -N1 "$out" | tr -d ' ')
    b2=$(od -An -tx1 -j2 -N1 "$out" | tr -d ' ')
    b3=$(od -An -tx1 -j3 -N1 "$out" | tr -d ' ')
    [ "$b0" = "61" ]
    [ "$b1" = "30" ]
    [ "$b2" = "35" ]
    [ "$b3" = "33" ]
}

@test "TAGSPL output is 4 bytes longer than the input SPL" {
    local out="$BATS_TEST_TMPDIR/tagged_len.bin"
    run "$TOOL" TAGSPL 0x33353061 "$FIXTURE_DIR/spl.bin" "$out"
    [ "$status" -eq 0 ]
    local in_size out_size
    in_size=$(stat -c '%s' "$FIXTURE_DIR/spl.bin")
    out_size=$(stat -c '%s' "$out")
    [ "$out_size" -eq $(( in_size + 4 )) ]
}

@test "TAGSPL content after tag matches original SPL" {
    local out="$BATS_TEST_TMPDIR/tagged_content.bin"
    run "$TOOL" TAGSPL 0x33353061 "$FIXTURE_DIR/spl.bin" "$out"
    [ "$status" -eq 0 ]
    local in_size
    in_size=$(stat -c '%s' "$FIXTURE_DIR/spl.bin")
    # Strip the 4-byte tag and compare to original
    local stripped="$BATS_TEST_TMPDIR/stripped.bin"
    dd if="$out" bs=1 skip=4 of="$stripped" 2>/dev/null
    cmp "$FIXTURE_DIR/spl.bin" "$stripped"
}

@test "TAGSPL with too few arguments exits 1" {
    run "$TOOL" TAGSPL
    [ "$status" -eq 1 ]
}

# ---------------------------------------------------------------------------
# New negative cases
# ---------------------------------------------------------------------------

@test "WLX with too few arguments exits 1" {
    run "$TOOL" WLX
    [ "$status" -eq 1 ]
}

@test "WGPT with no arguments exits 1" {
    run "$TOOL" WGPT
    [ "$status" -eq 1 ]
}

@test "DUMP with too few arguments exits 1" {
    run "$TOOL" DUMP
    [ "$status" -eq 1 ]
}

@test "WRITE with too few arguments exits 1" {
    run "$TOOL" WRITE
    [ "$status" -eq 1 ]
}

@test "EXEC with no arguments exits 1" {
    run "$TOOL" EXEC
    [ "$status" -eq 1 ]
}

@test "OTP with no arguments exits 1" {
    run "$TOOL" OTP
    [ "$status" -eq 1 ]
}

@test "STORAGE no-arg lists storage types without error" {
    run "$TOOL" STORAGE
    [ "$status" -eq 0 ]
    [[ "$output" =~ "emmc" ]]
    [[ "$output" =~ "nand" ]]
}
