# rockutil

[![License: GPL-2.0](https://img.shields.io/badge/License-GPL--2.0-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)
[![Language: C11](https://img.shields.io/badge/Language-C11-lightgrey.svg)]()

A GPLv2-licensed command-line tool for flashing and managing Rockchip
RK-series devices over USB, built on top of **libusb-1.0**.

---

## Overview

`rockutil` speaks the Rockchip **Rockusb** wire protocol (USB vendor class
`0xFF/0x06/0x05`, Mass-Storage Bulk-Only Transport) and the **MaskROM** loader
upload sequence (device-vendor control transfers `wIndex 0x0471/0x0472`). It
handles every stage of the device lifecycle:

- **Discovery** — enumerate all Rockusb and MaskROM devices on the system
- **MaskROM** — upload DDR-init and USB-bootloader blobs to an uninitialized SoC
- **Loader** — flash individual LBA sectors, erase, read back, query identifiers
- **Full firmware** — write complete RKFW + RKAF images in one command
- **Partitioning** — build a GPT image from Rockchip `parameter.txt` syntax

It produces the same wire traffic as the reference tool, so it is safe to use
with any official Rockchip loader and firmware image.

---

## Features

- **All Rockusb commands** — LD, TD, RD, RP, RID, RFI, RCI, RCB, RL, WL, EL, DB, UL, UF, DI, GPT, EF, PRINT, SS
- **RKFW / RKAF / RKBOOT parsing** — inspect or extract any Rockchip container image
- **Android sparse image expansion** — writes sparse RKAF partitions correctly
- **Full MaskROM → Loader handoff** — automatic mode-switch with re-enumeration wait
- **GPT builder** — converts `parameter.txt` to a standard GUID Partition Table
- **No kernel drivers** — pure userspace via libusb-1.0; no device-specific kernel modules needed
- **Comprehensive test suite** — 75 Bats tests including a Python-driven USB emulator
  (`dummy_hcd` + Raw Gadget), so every command can be validated without physical hardware

---

## Requirements

| Dependency | Minimum version | Notes |
|---|---|---|
| libusb-1.0 | 1.0.20 | `apt install libusb-1.0-0-dev` |
| C compiler | C11 | gcc ≥ 7 or clang ≥ 6 |
| make | any GNU make | — |

For running the integration tests only:

| Dependency | Notes |
|---|---|
| bats | ≥ 1.5 (`apt install bats`) |
| python3 | ≥ 3.10 |
| usbutils | `apt install usbutils` (`lsusb`) |
| kernel modules | `dummy_hcd`, `raw_gadget`, `libcomposite` (see Testing) |

---

## Building

```bash
# Install the libusb development package (Debian/Ubuntu)
sudo apt install libusb-1.0-0-dev

# Build
make

# The binary is placed in the current directory as ./rockutil
./rockutil -v
```

Alternatively, to install system-wide:

```bash
sudo make install PREFIX=/usr/local   # installs to /usr/local/bin/rockutil
```

To uninstall:

```bash
sudo make uninstall
```

---

## Usage

```
rockutil <COMMAND> [arguments ...]
rockutil -h | --help
rockutil -v | --version
```

### Device management

| Command | Syntax | Description |
|---|---|---|
| `LD` | `rockutil LD` | List all attached Rockusb / MaskROM devices |
| `TD` | `rockutil TD` | Test unit ready (device presence check) |
| `RD` | `rockutil RD [subcode]` | Reset device; optional subcode (default 0) |
| `RP` | `rockutil RP [0\|1]` | Reset bulk pipe: 0 = OUT, 1 = IN |
| `SS` | `rockutil SS <type>` | Switch active storage: `emmc`, `nand`, `sd`, `spinor`, `spinand` |

### Device identification

| Command | Syntax | Description |
|---|---|---|
| `RID` | `rockutil RID` | Read flash chip manufacturer/device ID (5 bytes) |
| `RFI` | `rockutil RFI` | Read flash geometry: size in sectors, block size, manufacturer |
| `RCI` | `rockutil RCI` | Read SoC chip info string (e.g. `RK3566`) |
| `RCB` | `rockutil RCB` | Read capability bitmap: Direct LBA, Vendor Storage, Read SN, … |

### Flash read / write / erase

All LBA addresses and sector counts are in **512-byte sectors**. Hex values
(`0x2000`) and decimal values (`8192`) are both accepted.

| Command | Syntax | Description |
|---|---|---|
| `RL` | `rockutil RL <start_lba> <count> [outfile]` | Read sectors; writes to *outfile* or stdout |
| `WL` | `rockutil WL <start_lba> <file> [--size N]` | Write sectors from a raw binary file; `--size` limits the sector count |
| `EL` | `rockutil EL <start_lba> <count>` | Erase sectors (fill with zeros) |

Examples:

```bash
# Read 128 sectors (64 KiB) starting at LBA 0x2000 into misc.bin
rockutil RL 0x2000 128 misc.bin

# Write a 4 KiB binary to LBA 0x2000
rockutil WL 0x2000 data.bin

# Write only the first 8 sectors of a larger file
rockutil WL 0x2000 data.bin --size 8

# Erase 8 sectors at LBA 0x400
rockutil EL 0x400 8

# Dump raw sector data to stdout and pipe to xxd
rockutil RL 0 1 | xxd | head
```

### Bootloader and firmware upgrade

These commands target a device in **MaskROM mode** (PID `0x350A`). The tool
automatically waits for the MaskROM → Loader transition.

| Command | Syntax | Description |
|---|---|---|
| `DB` | `rockutil DB <loader.bin> [--code 0x471\|0x472]` | Download a single raw bootloader blob via MaskROM control transfer. Default code is `0x472` (USB loader). Use `--code 0x471` to send the DDR-init blob. |
| `UL` | `rockutil UL <loader.bin\|firmware.img>` | Full MaskROM loader upgrade: uploads DDR-init (0x471) and USB-loader (0x472) blobs, waits for re-enumeration as Loader (PID `0x350B`), then writes the loader entries via `WRITE_LOADER`. |
| `UF` | `rockutil UF <firmware.img>` | Full firmware upgrade from an RKFW image: runs the MaskROM handshake, reads flash geometry, then writes every RKAF partition at its declared LBA address. |

Examples:

```bash
# Full loader upgrade from a standalone RKBOOT file
rockutil UL loader.bin

# Full loader upgrade extracting the loader from an RKFW image
rockutil UL firmware.img

# Flash a complete firmware image (bootloader + all partitions)
rockutil UF firmware.img
```

### Partition image download

`DI` writes a single partition image to its declared flash offset. Both raw
binary and Android sparse images are accepted.

```
rockutil DI <partition_name> <image_file> [parameter.txt]
```

If `parameter.txt` is not provided, `rockutil` queries the device's active
parameter partition to discover the partition table.

Examples:

```bash
# Flash the kernel partition using the on-device parameter table
rockutil DI boot boot.img

# Flash the misc partition using an explicit parameter file
rockutil DI misc misc.img parameter.txt
```

### GPT builder

Converts a Rockchip `parameter.txt` file into a UEFI GPT image that can be
written to the beginning of the flash.

```
rockutil GPT <parameter.txt> <output.bin>
```

If a live Rockusb device is present its flash size is used for the `BackupLBA`
field; otherwise a 4 GiB default is applied with a warning.

```bash
rockutil GPT parameter.txt gpt.bin
# Then flash the GPT image
rockutil WL 0 gpt.bin
```

### Erase flash by partition table

Erases every partition defined in `parameter.txt`:

```bash
rockutil EF parameter.txt
```

### Inspect firmware images

Parses and prints headers from RKFW, RKBOOT, and RKAF container files without
connecting to any device.

```bash
# Dump all headers from a complete firmware image
rockutil PRINT firmware.img

# Dump RKBOOT loader info
rockutil PRINT loader.bin
```

Example output:

```
RKFW header:
  version     : 1.2.0.0 (code 0x01020000)
  date        : 2023-09-15 10:30:00
  chip code   : 0x33353636
  loader      : offset=0x00000066 size=0x0002a000
  image       : offset=0x0002a066 size=0x00f80000
RKBOOT header:
  version        : 2.15.0.0
  rc4 disabled   : yes
  entries 471    : 1
  471[0]: type=0x01 off=0x0000002e size=204800 delay=0 ms name="DDR"
  entries 472    : 1
  472[0]: type=0x02 off=0x00032032 size=143360 delay=0 ms name="USB"
RKAF header:
  partitions  : 9
   [ 0] uboot                pos=0x00004000 size=0x00100000 flash@LBA=0x00002000
   ...
```

---

## Typical workflows

### Recover a bricked device (MaskROM recovery)

```bash
# 1. Hold MASKROM button while connecting USB; confirm it appears:
rockutil LD
#    DevNo=1  Vid=0x2207  Pid=0x350A  Mode=Loader

# 2. Full firmware reflash (handles MaskROM→Loader transition automatically)
rockutil UF firmware.img
```

### Flash a fresh eMMC

```bash
# 1. Write the GPT
rockutil GPT parameter.txt gpt.bin
rockutil WL 0 gpt.bin

# 2. Write each partition
rockutil DI parameter  parameter.txt
rockutil DI uboot      uboot.img
rockutil DI boot       boot.img
rockutil DI rootfs     rootfs.img
```

### Update only the kernel

```bash
rockutil LD                        # confirm device is in Loader mode
rockutil DI boot new_boot.img parameter.txt
rockutil RD                        # reboot
```

### Dump flash for inspection

```bash
# Read the first 4 MiB (8192 sectors) into a file
rockutil RL 0 8192 flash_backup.bin
```

---

## Testing

The test suite is split into three tiers:

| Target | Requires root | Requires hardware |
|---|---|---|
| `make test-offline` | No | No |
| `make test-loader` | Yes (sudo) | No (`dummy_hcd` + Raw Gadget emulator) |
| `make test-maskrom` | Yes (sudo) | No (emulator handles MaskROM→Loader) |
| `make test` | Yes | No |

### Running offline tests (no hardware, no sudo)

```bash
make test-offline
```

Tests cover: help/version output, `PRINT` parsing of all container formats,
`GPT` fallback mode, and all negative-case argument validation.

### Running integration tests

Integration tests start a Python-based Rockusb device emulator on a virtual
USB bus (`dummy_hcd` kernel module + `/dev/raw-gadget`). The emulator handles
all opcodes, maintains an in-memory flash, and writes an NDJSON op-log to
`tests/build/oplog.jsonl` that the tests assert against.

**Prerequisites:**

```bash
# Kernel modules (CONFIG_USB_DUMMY_HCD=m, CONFIG_USB_RAW_GADGET=m)
sudo modprobe dummy_hcd num=1
sudo modprobe raw_gadget

# Python packages (only stdlib is needed: ctypes, fcntl, struct)
python3 --version   # >= 3.10
```

Check kernel support:

```bash
make test-kernel
```

**Run the full integration suite:**

```bash
make test          # runs offline + loader + maskrom (needs sudo for the latter two)
```

Or individually:

```bash
make test-loader   # 30 tests: LD, TD, RD, RP, RID, RFI, RCI, RCB, RL, WL, EL, SS, GPT
make test-maskrom  # 17 tests: DB, UL, UF, DI, EF and MaskROM→Loader handshake
```

All generated fixtures and logs go to `tests/build/` which is gitignored.

---

## Source layout

```
rockutil.c       main CLI driver; command dispatch
rkusb.c/.h       libusb-1.0 Rockusb/MaskROM layer; CBW/CSW; device discovery
rkimage.c/.h     RKFW / RKBOOT / RKAF container parsers
rkparam.c/.h     parameter.txt parser and GPT builder
rkcrc.c/.h       CRC variants: CRC-CCITT/FALSE, Rockchip RKFW CRC32, IEEE 802.3 CRC32
rkrc4.c/.h       Rockchip fixed-key RC4 (loader blob de-obfuscation)
rksparse.c/.h    Android sparse image decoder

tests/
  offline.bats         rootless tests (28 cases)
  loader.bats          Loader-mode integration tests (30 cases)
  maskrom.bats         MaskROM-mode integration tests (17 cases)
  setup-kernel.sh      loads kernel modules for integration tests
  lib/
    helpers.bash       shared Bats setup/teardown helpers
    rawgadget.py       /dev/raw-gadget ctypes bindings
    rockusb_device.py  Python Rockusb emulator (Loader + MaskROM modes)
    run_emulator.py    CLI to daemonize the emulator
  fixtures/
    gen_fixtures.py    generates RKBOOT / RKFW / parameter.txt test blobs
```

---

## Troubleshooting

**`No Rockusb device found.`**  
Confirm the device is connected and in Loader or MaskROM mode. Check `lsusb`
for VID `0x2207`. The tool requires read/write permission on the USB device
node — run as root or add a udev rule:

```bash
# /etc/udev/rules.d/51-rockchip.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="2207", MODE="0660", GROUP="plugdev"
```

Then reload: `sudo udevadm control --reload-rules && sudo udevadm trigger`.

**`LIBUSB_ERROR_ACCESS`**  
Missing permissions on the USB device node. See udev rule above, or run with
`sudo`.

**`LIBUSB_ERROR_TIMEOUT` on MaskROM control transfer**  
The CRC-CCITT on the loader payload did not match what the MaskROM expected.
Ensure `rc4_disable=1` is set in the RKBOOT header, or that the loader blob
is not RC4-encrypted (the tool handles both cases automatically).

**`parameter.txt: -22`**  
The parameter file could not be parsed. Check:
- The `CMDLINE` line ends without a trailing semicolon
- LBA values are in hex sectors (`0x2000`), not bytes
- Partition names contain no spaces

**Integration tests fail with `dummy_hcd: No such file`**  
Load the kernel module: `sudo modprobe dummy_hcd num=1`. If that fails, the
kernel was built without `CONFIG_USB_DUMMY_HCD`. Check:

```bash
grep USB_DUMMY_HCD /boot/config-$(uname -r)
# Should show: CONFIG_USB_DUMMY_HCD=m
```

---

## Contributing

Contributions are welcome. Please open an issue first to discuss larger
changes. All code must:

- Compile cleanly under `cc -std=c11 -Wall -Wextra -Wshadow` (no new warnings)
- Pass `make test-offline`
- Include a new or updated Bats test for any user-visible behaviour change
- Carry the `SPDX-License-Identifier: GPL-2.0-only` header

---

## License

Released under the **GNU General Public License v2.0 only**.  
See the `SPDX-License-Identifier: GPL-2.0-only` header in each source file, or:
<https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>
