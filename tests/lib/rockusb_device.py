# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 rockutil contributors
"""
rockusb_device.py — Python Rockusb device emulator using /dev/raw-gadget.

Implements two operating modes:
  LOADER   — VID 0x2207 / PID 0x350B, class 0xFF/0x06/0x05 with bulk IN+OUT.
             Handles all Rockusb CBW/CSW opcodes against an in-memory flash.
  MASKROM  — VID 0x2207 / PID 0x350A, no bulk interface.
             Accepts device-vendor control transfers (bRequest=0x0c,
             wIndex=0x0471|0x0472) and verifies the CRC-CCITT/FALSE tail.
             After receiving all expected 471+472 blobs, closes the gadget
             and restarts as LOADER mode (transition_pid=0x350B).

All USB commands are logged to an NDJSON file so Bats tests can assert
exact protocol behavior without inspecting stdout.

Canned responses (chosen for easy test assertions):
  Flash ID        : 11 22 33 44 55
  Flash Info      : 64 MiB flash (131072 sectors of 512 B), block=128 sectors,
                    page=1, ecc=0, access=0, manufacturer=0xAB
  Chip Info       : raw bytes arranged so rockutil prints "RK35xx_test___\x00"
                    (the tool reverses every 4-byte word before printing)
  Capability byte : 0xFF (all features enabled)
"""

import json
import logging
import os
import struct
import sys
import threading
import time

from rawgadget import (
    RawGadget,
    USB_SPEED_HIGH,
    USB_RAW_EVENT_CONNECT,
    USB_RAW_EVENT_CONTROL,
    USB_RAW_EVENT_RESET,
    USB_RAW_EVENT_DISCONNECT,
    USB_DIR_IN, USB_DIR_OUT,
    USB_DIR_IN_CTRL, USB_DIR_OUT_CTRL,
    USB_TYPE_VENDOR, USB_TYPE_STANDARD,
    USB_RECIP_DEVICE, USB_RECIP_IFACE,
    USB_REQ_GET_DESCRIPTOR,
    USB_REQ_SET_CONFIGURATION,
    USB_REQ_SET_INTERFACE,
    USB_DT_DEVICE, USB_DT_CONFIG, USB_DT_STRING,
    USB_ENDPOINT_XFER_BULK,
    build_device_descriptor,
    build_config_descriptor,
    build_interface_descriptor,
    build_endpoint_descriptor,
    build_string_descriptor,
    build_lang_descriptor,
    parse_ctrl_request,
)

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

ROCKCHIP_VID     = 0x2207

# Loader mode PID (PID 0x350B = RK35xx loader)
LOADER_PID       = 0x350B

# MaskROM PID (PID 0x350A = RK35xx maskrom)
MASKROM_PID      = 0x350A

# Bulk endpoint addresses (arbitrary — libusb finds them via descriptor scan)
EP_BULK_OUT = 0x01   # host→device (OUT)
EP_BULK_IN  = 0x81   # device→host (IN)

# MSC Bulk-only Transport
CBW_SIGNATURE = 0x43425355   # "USBC" LE
CSW_SIGNATURE = 0x53425355   # "USBS" LE

CSW_STATUS_GOOD    = 0
CSW_STATUS_FAILED  = 1

# Rockusb opcodes (subset handled here)
RKOP_TEST_UNIT_READY  = 0x00
RKOP_READ_FLASH_ID    = 0x01
RKOP_READ_LBA         = 0x14
RKOP_WRITE_LBA        = 0x15
RKOP_READ_FLASH_INFO  = 0x1a
RKOP_READ_CHIP_INFO   = 0x1b
RKOP_ERASE_LBA        = 0x25
RKOP_CHANGE_STORAGE   = 0x2b
RKOP_READ_CAPABILITY  = 0xaa
RKOP_DEVICE_RESET     = 0xff
RKOP_WRITE_LOADER     = 0x57
RKOP_LOWER_FORMAT     = 0x1d
RKOP_ERASE_SECTORS    = 0x29

SECTOR_SIZE   = 512
FLASH_SECTORS = 131072   # 64 MiB

# MaskROM control-transfer wIndex codes
RKUSB_CTRL_DDR_INIT = 0x0471
RKUSB_CTRL_LOADER   = 0x0472

# ---------------------------------------------------------------------------
# CRC-CCITT/FALSE (poly=0x1021, init=0xFFFF) — mirrors rkcrc_ccitt()
# ---------------------------------------------------------------------------

def crc_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


# ---------------------------------------------------------------------------
# Canned response payloads
# ---------------------------------------------------------------------------

FLASH_ID_RESPONSE = bytes([0x11, 0x22, 0x33, 0x44, 0x55])

def _build_flash_info() -> bytes:
    """
    11-byte flash info matching the cmd_read_flash_info() response format:
      bytes 0-3 : flash_size (u32 LE) = 131072 sectors (64 MiB)
      bytes 4-5 : block_size (u16 LE) = 128 sectors
      byte  6   : page_size           = 1 sector
      bytes 7-8 : ecc_bits (u16 LE)   = 0
      byte  9   : access_time         = 0
      byte  10  : manufacturer code   = 0xAB
    """
    # <I H B H B B = 4+2+1+2+1+1 = 11 bytes exactly
    return struct.pack("<IHBHBB", 131072, 128, 1, 0, 0, 0xAB)

FLASH_INFO_RESPONSE = _build_flash_info()


def _build_chip_info() -> bytes:
    """
    16 bytes that produce "RK35xx_test___\x00" when the tool reverses each
    4-byte word (rockutil.c byte-swap loop):
      text[i+0] = info[i+3],  text[i+1] = info[i+2],
      text[i+2] = info[i+1],  text[i+3] = info[i+0]
    So for a target string word "RK35" we store bytes 0x35,0x4b,0x52 (little-end
    reversed) … easier to compute: for each 4-char group just reverse the bytes.
    Target: "RK35" "xx_t" "est_" "__\0\0"
    """
    target = b"RK35xx_test____\x00"   # 16 bytes
    result = bytearray(16)
    for i in range(0, 16, 4):
        result[i + 0] = target[i + 3]
        result[i + 1] = target[i + 2]
        result[i + 2] = target[i + 1]
        result[i + 3] = target[i + 0]
    return bytes(result)

CHIP_INFO_RESPONSE = _build_chip_info()

CAPABILITY_RESPONSE = bytes([0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])


# ---------------------------------------------------------------------------
# Op-log writer
# ---------------------------------------------------------------------------

class OpLog:
    def __init__(self, path: str):
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        self._path = path
        self._fh = open(path, "a", encoding="utf-8")

    def write(self, entry: dict) -> None:
        self._fh.write(json.dumps(entry) + "\n")
        self._fh.flush()

    def close(self) -> None:
        self._fh.close()


# ---------------------------------------------------------------------------
# USB descriptor sets for Loader and MaskROM modes
# ---------------------------------------------------------------------------

def _loader_descriptors(pid: int, i_manufacturer: int = 1):
    """Return (device_desc, config_desc, strings) for Loader mode."""
    iface = (
        build_interface_descriptor(
            iface_num=0, num_endpoints=2,
            iface_class=0xFF, iface_subclass=0x06, iface_protocol=0x05,
            i_interface=0,
        )
        + build_endpoint_descriptor(EP_BULK_OUT, USB_ENDPOINT_XFER_BULK, 512)
        + build_endpoint_descriptor(EP_BULK_IN,  USB_ENDPOINT_XFER_BULK, 512)
    )
    dev = build_device_descriptor(
        vid=ROCKCHIP_VID, pid=pid,
        bcd_device=0x0100,
        i_manufacturer=i_manufacturer,
        i_product=2,
        i_serial=3,
        bcd_usb=0x0200,
    )
    cfg = build_config_descriptor(iface, max_power_ma=500)
    strings = {
        0: build_lang_descriptor(),
        1: build_string_descriptor("RockChip"),
        2: build_string_descriptor("USB-MSC"),
        3: build_string_descriptor("TEST0001"),
    }
    return dev, cfg, strings


def _maskrom_descriptors(pid: int):
    """
    MaskROM mode: no bulk interface.  No string descriptors (iManufacturer=0).
    """
    dev = build_device_descriptor(
        vid=ROCKCHIP_VID, pid=pid,
        bcd_device=0x0100,
        i_manufacturer=0,
        i_product=0,
        i_serial=0,
        bcd_usb=0x0200,
    )
    # Minimal config with no endpoints
    iface = build_interface_descriptor(
        iface_num=0, num_endpoints=0,
        iface_class=0xFF, iface_subclass=0x06, iface_protocol=0x05,
    )
    cfg = build_config_descriptor(iface, max_power_ma=100)
    strings = {0: build_lang_descriptor()}
    return dev, cfg, strings


# ---------------------------------------------------------------------------
# CBW / CSW helpers
# ---------------------------------------------------------------------------

def parse_cbw(data: bytes) -> dict | None:
    """Parse a 31-byte Command Block Wrapper."""
    if len(data) < 31:
        return None
    sig, tag, data_len, flags, lun, cdb_len = struct.unpack_from("<IIIBBB", data)
    if sig != CBW_SIGNATURE:
        return None
    cdb = data[15:15 + min(cdb_len, 16)]
    return {
        "tag": tag,
        "data_length": data_len,
        "flags": flags,
        "lun": lun,
        "cdb_length": cdb_len,
        "opcode": cdb[0] if cdb else 0,
        "cdb": cdb,
    }


def build_csw(tag: int, residue: int, status: int) -> bytes:
    return struct.pack("<IIIB", CSW_SIGNATURE, tag, residue, status)


# ---------------------------------------------------------------------------
# RockusbEmulator — the main class
# ---------------------------------------------------------------------------

class RockusbEmulator:
    """
    Emulates a Rockchip Rockusb device over /dev/raw-gadget.

    Parameters
    ----------
    mode : "loader" | "maskrom"
        Starting mode.
    udc_driver : str
        UDC driver name (e.g. "dummy_udc").
    udc_device : str
        UDC device name (e.g. "dummy_udc.0").
    loader_pid : int
        PID to use in Loader mode.
    maskrom_pid : int
        PID to use in MaskROM mode.
    transition_pid : int
        After MaskROM handshake succeeds, rebind as Loader with this PID.
    oplog_path : str
        Path to the NDJSON op-log file.
    raw_gadget_path : str
        Path to the raw-gadget device node.
    flash_size_sectors : int
        Number of 512-byte sectors in the emulated flash.
    """

    def __init__(
        self,
        mode: str = "loader",
        udc_driver: str = "dummy_udc",
        udc_device: str = "dummy_udc.0",
        loader_pid: int = LOADER_PID,
        maskrom_pid: int = MASKROM_PID,
        transition_pid: int = LOADER_PID,
        oplog_path: str = "tests/build/oplog.jsonl",
        raw_gadget_path: str = "/dev/raw-gadget",
        flash_size_sectors: int = FLASH_SECTORS,
    ):
        self._mode = mode
        self._udc_driver = udc_driver
        self._udc_device = udc_device
        self._loader_pid = loader_pid
        self._maskrom_pid = maskrom_pid
        self._transition_pid = transition_pid
        self._raw_gadget_path = raw_gadget_path

        self._oplog = OpLog(oplog_path)
        self._flash = bytearray(flash_size_sectors * SECTOR_SIZE)
        # Seed flash with a recognisable pattern: LBA × 0x11 in the first
        # 2 bytes of each sector so tests can assert round-trip correctness.
        for lba in range(flash_size_sectors):
            off = lba * SECTOR_SIZE
            self._flash[off] = lba & 0xFF
            self._flash[off + 1] = (lba >> 8) & 0xFF

        # MaskROM handshake state
        self._maskrom_got_471 = False
        self._maskrom_got_472 = False

        self._ep_out_handle = -1
        self._ep_in_handle  = -1
        self._gadget: RawGadget | None = None
        self._stop_flag = threading.Event()

    # ------------------------------------------------------------------
    # Public interface
    # ------------------------------------------------------------------

    def run(self) -> None:
        """Main blocking loop — returns when stop() is called or an error."""
        if self._mode == "loader":
            self._run_loader()
        elif self._mode == "maskrom":
            self._run_maskrom()
        else:
            raise ValueError(f"Unknown mode: {self._mode!r}")

    def stop(self) -> None:
        self._stop_flag.set()

    def flash_read(self, lba: int, sectors: int) -> bytes:
        start = lba * SECTOR_SIZE
        end   = start + sectors * SECTOR_SIZE
        return bytes(self._flash[start:end])

    # ------------------------------------------------------------------
    # Loader mode
    # ------------------------------------------------------------------

    def _run_loader(self, pid: int | None = None) -> None:
        if pid is None:
            pid = self._loader_pid
        dev_desc, cfg_desc, strings = _loader_descriptors(pid)
        self._event_loop(dev_desc, cfg_desc, strings,
                         has_bulk=True, is_maskrom=False)

    # ------------------------------------------------------------------
    # MaskROM mode
    # ------------------------------------------------------------------

    def _run_maskrom(self) -> None:
        dev_desc, cfg_desc, strings = _maskrom_descriptors(self._maskrom_pid)
        self._event_loop(dev_desc, cfg_desc, strings,
                         has_bulk=False, is_maskrom=True)
        # If we get here because the handshake completed, transition.
        if self._maskrom_got_471 and self._maskrom_got_472:
            log.info("MaskROM handshake complete – transitioning to Loader "
                     "(PID 0x%04X)", self._transition_pid)
            time.sleep(0.1)   # brief settle
            self._mode = "loader"
            self._stop_flag.clear()
            self._run_loader(pid=self._transition_pid)

    # ------------------------------------------------------------------
    # Shared event loop
    # ------------------------------------------------------------------

    def _event_loop(
        self,
        dev_desc: bytes,
        cfg_desc: bytes,
        strings: dict,
        has_bulk: bool,
        is_maskrom: bool,
    ) -> None:
        with RawGadget(self._raw_gadget_path) as g:
            self._gadget = g
            g.init(self._udc_driver, self._udc_device, USB_SPEED_HIGH)
            g.run()

            self._ep_out_handle = -1
            self._ep_in_handle  = -1

            while not self._stop_flag.is_set():
                try:
                    ev_type, ev_data = g.event_fetch(4096)
                except OSError as exc:
                    if self._stop_flag.is_set():
                        break
                    log.error("event_fetch: %s", exc)
                    break

                if ev_type == USB_RAW_EVENT_CONNECT:
                    log.debug("CONNECT")
                    g.vbus_draw(250)   # 500 mA

                elif ev_type == USB_RAW_EVENT_CONTROL:
                    req = parse_ctrl_request(ev_data)
                    self._handle_setup(g, req, dev_desc, cfg_desc, strings,
                                       has_bulk, is_maskrom)

                elif ev_type == USB_RAW_EVENT_RESET:
                    log.debug("RESET")
                    if self._ep_out_handle >= 0:
                        try:
                            g.ep_disable(self._ep_out_handle)
                        except OSError:
                            pass
                        self._ep_out_handle = -1
                    if self._ep_in_handle >= 0:
                        try:
                            g.ep_disable(self._ep_in_handle)
                        except OSError:
                            pass
                        self._ep_in_handle = -1

                elif ev_type == USB_RAW_EVENT_DISCONNECT:
                    log.debug("DISCONNECT")
                    break

                # For MaskROM: check if handshake is done and break early
                if is_maskrom and self._maskrom_got_471 and self._maskrom_got_472:
                    break

            self._gadget = None

    # ------------------------------------------------------------------
    # Setup-packet handler
    # ------------------------------------------------------------------

    def _handle_setup(
        self, g: RawGadget, req: dict,
        dev_desc: bytes, cfg_desc: bytes, strings: dict,
        has_bulk: bool, is_maskrom: bool,
    ) -> None:
        bm     = req["bmRequestType"]
        breq   = req["bRequest"]
        wval   = req["wValue"]
        widx   = req["wIndex"]
        wlen   = req["wLength"]

        direction = bm & 0x80
        req_type  = bm & 0x60
        recipient = bm & 0x1F

        # --- Standard GET_DESCRIPTOR ---
        if (req_type == USB_TYPE_STANDARD and
                breq == USB_REQ_GET_DESCRIPTOR):
            desc_type = (wval >> 8) & 0xFF
            desc_idx  = wval & 0xFF

            if desc_type == USB_DT_DEVICE:
                g.ep0_write(dev_desc[:wlen])
                return

            if desc_type == USB_DT_CONFIG:
                g.ep0_write(cfg_desc[:wlen])
                return

            if desc_type == USB_DT_STRING:
                data = strings.get(desc_idx)
                if data:
                    g.ep0_write(data[:wlen])
                else:
                    g.ep0_stall()
                return

            g.ep0_stall()
            return

        # --- Standard SET_CONFIGURATION ---
        if (req_type == USB_TYPE_STANDARD and
                breq == USB_REQ_SET_CONFIGURATION):
            if has_bulk:
                # Enable bulk endpoints
                if self._ep_out_handle < 0:
                    self._ep_out_handle = g.ep_enable(
                        EP_BULK_OUT, USB_ENDPOINT_XFER_BULK, 512)
                    log.debug("EP_OUT enabled handle=%d", self._ep_out_handle)
                if self._ep_in_handle < 0:
                    self._ep_in_handle = g.ep_enable(
                        EP_BULK_IN, USB_ENDPOINT_XFER_BULK, 512)
                    log.debug("EP_IN  enabled handle=%d", self._ep_in_handle)
                g.configure()
                # Start the bulk handler thread
                t = threading.Thread(
                    target=self._bulk_handler, args=(g,), daemon=True)
                t.start()
            g.ep0_write(b"")   # zero-length status
            return

        # --- Standard SET_INTERFACE ---
        if (req_type == USB_TYPE_STANDARD and
                breq == USB_REQ_SET_INTERFACE):
            g.ep0_write(b"")
            return

        # --- Vendor DEVICE control transfer (MaskROM loader upload) ---
        if (req_type == USB_TYPE_VENDOR and
                recipient == USB_RECIP_DEVICE and
                breq == 0x0c):
            if widx in (RKUSB_CTRL_DDR_INIT, RKUSB_CTRL_LOADER):
                self._handle_maskrom_ctrl(g, widx, wlen, is_maskrom)
                return
            g.ep0_stall()
            return

        # --- MSC class reset (bRequest=0xFF) ---
        if (req_type == 0x20 and breq == 0xFF):  # class, interface
            g.ep0_write(b"")
            return

        # --- Get Max LUN (bRequest=0xFE) ---
        if (req_type == 0x20 and breq == 0xFE):
            g.ep0_write(bytes([0]))
            return

        # Unknown — stall EP0
        log.warning("Unknown SETUP bm=0x%02x req=0x%02x val=0x%04x idx=0x%04x "
                    "len=%d", bm, breq, wval, widx, wlen)
        g.ep0_stall()

    # ------------------------------------------------------------------
    # MaskROM vendor-class control transfer
    # ------------------------------------------------------------------

    def _handle_maskrom_ctrl(
        self, g: RawGadget, windex: int, wlen: int, is_maskrom: bool,
    ) -> None:
        """Receive a loader blob, verify its CRC-CCITT tail, log it."""
        payload = g.ep0_read(wlen)
        if len(payload) < 2:
            g.ep0_stall()
            return

        body = payload[:-2]
        stored_crc = (payload[-2] << 8) | payload[-1]
        computed_crc = crc_ccitt(body)
        crc_ok = (stored_crc == computed_crc)

        self._oplog.write({
            "op": "ctrl_upload",
            "bRequest": 0x0c,
            "wIndex": windex,
            "wIndex_name": "471" if windex == RKUSB_CTRL_DDR_INIT else "472",
            "payload_len": len(payload),
            "body_len": len(body),
            "crc_stored": stored_crc,
            "crc_computed": computed_crc,
            "crc_ok": crc_ok,
        })
        log.debug("MaskROM ctrl wIndex=0x%04x len=%d crc_ok=%s",
                  windex, len(payload), crc_ok)

        # ACK (zero-length IN status)
        g.ep0_write(b"")

        if windex == RKUSB_CTRL_DDR_INIT:
            self._maskrom_got_471 = True
        elif windex == RKUSB_CTRL_LOADER:
            self._maskrom_got_472 = True

    # ------------------------------------------------------------------
    # Bulk endpoint handler (runs in a daemon thread)
    # ------------------------------------------------------------------

    def _bulk_handler(self, g: RawGadget) -> None:
        """
        Read CBWs from EP_OUT, process them, write data + CSW to EP_IN.
        Runs until the stop flag is set or an unrecoverable error occurs.
        """
        while not self._stop_flag.is_set():
            try:
                cbw_data = g.ep_read(self._ep_out_handle, 31)
            except OSError as exc:
                if not self._stop_flag.is_set():
                    log.debug("ep_read(CBW): %s", exc)
                break

            if len(cbw_data) == 0:
                continue

            cbw = parse_cbw(cbw_data)
            if cbw is None:
                log.warning("Invalid CBW (%d bytes)", len(cbw_data))
                continue

            log.debug("CBW tag=0x%08x op=0x%02x data_len=%d flags=0x%02x",
                      cbw["tag"], cbw["opcode"], cbw["data_length"], cbw["flags"])

            try:
                self._dispatch_cbw(g, cbw)
            except OSError as exc:
                if not self._stop_flag.is_set():
                    log.debug("dispatch error: %s", exc)
                break

    def _dispatch_cbw(self, g: RawGadget, cbw: dict) -> None:
        op       = cbw["opcode"]
        tag      = cbw["tag"]
        cdb      = cbw["cdb"]
        data_len = cbw["data_length"]
        is_in    = bool(cbw["flags"] & 0x80)

        status = CSW_STATUS_GOOD
        response = b""

        if op == RKOP_TEST_UNIT_READY:
            self._oplog.write({"op": "TEST_UNIT_READY"})

        elif op == RKOP_READ_FLASH_ID:
            response = FLASH_ID_RESPONSE
            self._oplog.write({"op": "READ_FLASH_ID"})

        elif op == RKOP_READ_FLASH_INFO:
            response = FLASH_INFO_RESPONSE
            self._oplog.write({"op": "READ_FLASH_INFO"})

        elif op == RKOP_READ_CHIP_INFO:
            response = CHIP_INFO_RESPONSE
            self._oplog.write({"op": "READ_CHIP_INFO"})

        elif op == RKOP_READ_CAPABILITY:
            response = CAPABILITY_RESPONSE
            self._oplog.write({"op": "READ_CAPABILITY"})

        elif op == RKOP_DEVICE_RESET:
            subcode = cdb[1] if len(cdb) > 1 else 0
            self._oplog.write({"op": "DEVICE_RESET", "subcode": subcode})
            # Kick the stop flag so the test can clean up
            self._stop_flag.set()

        elif op == RKOP_READ_LBA:
            lba  = (cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5])
            secs = (cdb[7] << 8 | cdb[8])
            response = self.flash_read(lba, secs)
            self._oplog.write({"op": "READ_LBA", "lba": lba, "sectors": secs})

        elif op == RKOP_WRITE_LBA:
            lba  = (cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5])
            secs = (cdb[7] << 8 | cdb[8])
            data = g.ep_read(self._ep_out_handle, secs * SECTOR_SIZE)
            off  = lba * SECTOR_SIZE
            self._flash[off:off + len(data)] = data
            self._oplog.write({"op": "WRITE_LBA", "lba": lba,
                               "sectors": secs, "len": len(data)})
            self._send_csw(g, tag, 0, status)
            return

        elif op == RKOP_ERASE_LBA:
            lba  = (cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5])
            secs = (cdb[7] << 8 | cdb[8])
            off  = lba * SECTOR_SIZE
            end  = off + secs * SECTOR_SIZE
            self._flash[off:end] = bytes(end - off)
            self._oplog.write({"op": "ERASE_LBA", "lba": lba, "sectors": secs})

        elif op == RKOP_CHANGE_STORAGE:
            storage = cdb[5]
            self._oplog.write({"op": "CHANGE_STORAGE", "storage": storage})

        elif op == RKOP_WRITE_LOADER:
            wlen = (cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5])
            data = g.ep_read(self._ep_out_handle, wlen)
            self._oplog.write({"op": "WRITE_LOADER", "len": len(data)})
            self._send_csw(g, tag, 0, status)
            return

        elif op == RKOP_LOWER_FORMAT:
            self._oplog.write({"op": "LOWER_FORMAT"})

        elif op == RKOP_ERASE_SECTORS:
            lba  = (cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5])
            secs = (cdb[7] << 8 | cdb[8])
            self._oplog.write({"op": "ERASE_SECTORS", "lba": lba,
                               "sectors": secs})

        else:
            log.warning("Unknown opcode 0x%02x", op)
            self._oplog.write({"op": "UNKNOWN", "opcode": op})
            status = CSW_STATUS_FAILED

        # Send IN data if needed
        if response and is_in:
            send_len = min(len(response), data_len)
            try:
                g.ep_write(self._ep_in_handle, response[:send_len])
            except OSError as exc:
                log.debug("ep_write data: %s", exc)
                status = CSW_STATUS_FAILED

        residue = max(0, data_len - len(response)) if response else 0
        self._send_csw(g, tag, residue, status)

    def _send_csw(self, g: RawGadget, tag: int,
                  residue: int, status: int) -> None:
        csw = build_csw(tag, residue, status)
        try:
            g.ep_write(self._ep_in_handle, csw)
        except OSError as exc:
            log.debug("ep_write CSW: %s", exc)
