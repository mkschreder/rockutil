# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 rockutil contributors
"""
usb_socket_emu.py — Socket-based Rockchip Rockusb device emulator.

Replaces the raw-gadget emulator with a Unix-socket server that the
libusb_emu.so shared library (LD_PRELOAD) connects to directly.  No
kernel modules are required.

Protocol (all integers little-endian):
  Request:  u8 type, u32 payload_len, u8[payload_len] payload
  Response: i32 status, u32 payload_len, u8[payload_len] payload

Canned responses (identical to rockusb_device.py for compatibility):
  Flash ID        : 11 22 33 44 55
  Flash Info      : 64 MiB (131072 × 512 B), block=128, page=1, mfr=0xAB
  Chip Info       : "RK35xx_test____\\x00" (word-reversed per tool ABI)
  Capability      : 0xFF
  Flash seeding   : sector N → byte[0]=N&0xFF, byte[1]=(N>>8)&0xFF
"""

import json
import logging
import os
import signal
import socket
import struct
import sys
from pathlib import Path
from typing import Optional

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Protocol constants (must match libusb_emu.c)
# ---------------------------------------------------------------------------
EMU_INIT       = 1
EMU_EXIT       = 2
EMU_DEV_LIST   = 3
EMU_GET_CONFIG = 4
EMU_OPEN       = 5
EMU_CLOSE      = 6
EMU_CLAIM      = 7
EMU_RELEASE    = 8
EMU_BULK_OUT   = 9
EMU_BULK_IN    = 10
EMU_CTRL_OUT   = 11
EMU_CTRL_IN    = 12
EMU_CLEAR_HALT = 13

# ---------------------------------------------------------------------------
# Rockchip constants
# ---------------------------------------------------------------------------
ROCKCHIP_VID = 0x2207
LOADER_PID   = 0x350B
MASKROM_PID  = 0x350A
EP_BULK_OUT  = 0x01
EP_BULK_IN   = 0x81

CBW_SIGNATURE = 0x43425355  # "USBC"
CSW_SIGNATURE = 0x53425355  # "USBS"
CSW_OK   = 0
CSW_FAIL = 1

RKOP_TEST_UNIT_READY = 0x00
RKOP_READ_FLASH_ID   = 0x01
RKOP_READ_LBA        = 0x14
RKOP_WRITE_LBA       = 0x15
RKOP_READ_FLASH_INFO = 0x1A
RKOP_READ_CHIP_INFO  = 0x1B
RKOP_ERASE_LBA       = 0x25
RKOP_CHANGE_STORAGE  = 0x2B
RKOP_READ_CAPABILITY = 0xAA
RKOP_DEVICE_RESET    = 0xFF
RKOP_WRITE_LOADER    = 0x57

SECTOR_SIZE   = 512
FLASH_SECTORS = 131072  # 64 MiB

RKUSB_CTRL_DDR_INIT = 0x0471
RKUSB_CTRL_LOADER   = 0x0472

# ---------------------------------------------------------------------------
# CRC-CCITT/FALSE (poly=0x1021, init=0xFFFF)
# ---------------------------------------------------------------------------

def crc_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# ---------------------------------------------------------------------------
# Canned device responses
# ---------------------------------------------------------------------------

FLASH_ID_RESPONSE = bytes([0x11, 0x22, 0x33, 0x44, 0x55])

FLASH_INFO_RESPONSE = struct.pack("<IHBHBB", 131072, 128, 1, 0, 0, 0xAB)


def _build_chip_info() -> bytes:
    target = b"RK35xx_test____\x00"
    result = bytearray(16)
    for i in range(0, 16, 4):
        result[i + 0] = target[i + 3]
        result[i + 1] = target[i + 2]
        result[i + 2] = target[i + 1]
        result[i + 3] = target[i + 0]
    return bytes(result)


CHIP_INFO_RESPONSE   = _build_chip_info()
CAPABILITY_RESPONSE  = bytes([0xFF, 0, 0, 0, 0, 0, 0, 0])


# ---------------------------------------------------------------------------
# OpLog
# ---------------------------------------------------------------------------

class OpLog:
    def __init__(self, path: str) -> None:
        os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
        self._fh = open(path, "a", encoding="utf-8")

    def write(self, entry: dict) -> None:
        self._fh.write(json.dumps(entry) + "\n")
        self._fh.flush()

    def close(self) -> None:
        self._fh.close()


# ---------------------------------------------------------------------------
# Per-handle bulk transfer state machine
# ---------------------------------------------------------------------------

class HandleState:
    def __init__(self) -> None:
        self.state       = "IDLE"   # IDLE | DATA_OUT | DATA_IN | STATUS
        self.cbw: Optional[dict] = None
        self.pending_in  = b""
        self.csw_status  = CSW_OK
        self.write_buf   = b""
        self.write_len   = 0
        self.write_lba   = 0
        self.write_op    = 0


# ---------------------------------------------------------------------------
# Main emulator class
# ---------------------------------------------------------------------------

class SocketRockusbEmulator:
    """
    Unix-socket Rockusb emulator.  One connection handled at a time.

    Parameters
    ----------
    mode         : "loader" | "maskrom" — initial operating mode.
    sock_path    : Unix socket path to listen on.
    oplog_path   : Path to write NDJSON op-log entries.
    sysfs_root   : If set, writes fake sysfs descriptor files under this root.
    loader_pid   : PID used in Loader mode.
    maskrom_pid  : PID used in MaskROM mode.
    """

    def __init__(
        self,
        mode: str = "loader",
        sock_path: str = "/tmp/rockutil-emu.sock",
        oplog_path: str = "tests/build/oplog.jsonl",
        sysfs_root: Optional[str] = None,
        loader_pid: int = LOADER_PID,
        maskrom_pid: int = MASKROM_PID,
    ) -> None:
        self.initial_mode = mode
        self.mode         = mode
        self.sock_path    = sock_path
        self.oplog        = OpLog(oplog_path)
        self.sysfs_root   = sysfs_root
        self.loader_pid   = loader_pid
        self.maskrom_pid  = maskrom_pid

        # In-memory flash (64 MiB)
        self.flash = bytearray(FLASH_SECTORS * SECTOR_SIZE)
        self._seed_flash()

        # Handle tracking
        self.handles: dict[int, HandleState] = {}
        self.next_handle = 1

        # MaskROM handshake state
        self.got_471 = False
        self.got_472 = False

        self._stop        = False
        self._server_sock: Optional[socket.socket] = None

    # ------------------------------------------------------------------
    # Flash seeding
    # ------------------------------------------------------------------

    def _seed_flash(self) -> None:
        """Each sector N: byte[0]=N&0xFF, byte[1]=(N>>8)&0xFF."""
        for lba in range(FLASH_SECTORS):
            off = lba * SECTOR_SIZE
            self.flash[off]     = lba & 0xFF
            self.flash[off + 1] = (lba >> 8) & 0xFF

    # ------------------------------------------------------------------
    # Fake sysfs
    # ------------------------------------------------------------------

    def _update_sysfs(self) -> None:
        """Write /sys/bus/usb/devices/1-1/descriptors under sysfs_root."""
        if not self.sysfs_root:
            return
        sysfs_dir = (Path(self.sysfs_root) / "sys" / "bus" / "usb"
                     / "devices" / "1-1")
        sysfs_dir.mkdir(parents=True, exist_ok=True)
        descs_file = sysfs_dir / "descriptors"

        if self.mode == "loader":
            dev_desc = self._make_dev_desc(self.loader_pid, iMfr=1, iProd=2)
        else:
            dev_desc = self._make_dev_desc(self.maskrom_pid, iMfr=0, iProd=0)
        cfg_desc = self._make_cfg_desc_with_bulk()
        descs_file.write_bytes(dev_desc + cfg_desc)

    @staticmethod
    def _make_dev_desc(pid: int, iMfr: int = 0, iProd: int = 0) -> bytes:
        return struct.pack("<BBHBBBBHHHBBBB",
                           18, 0x01, 0x0200, 0, 0, 0, 64,
                           ROCKCHIP_VID, pid, 0x0100, iMfr, iProd, 0, 1)

    @staticmethod
    def _make_cfg_desc_with_bulk() -> bytes:
        iface = struct.pack("BBBBBBBBB", 9, 0x04, 0, 0, 2, 0xFF, 0x06, 0x05, 0)
        ep_out = struct.pack("<BBBBHB", 7, 0x05, 0x01, 0x02, 512, 0)
        ep_in  = struct.pack("<BBBBHB", 7, 0x05, 0x81, 0x02, 512, 0)
        iface_data  = iface + ep_out + ep_in
        total_len   = 9 + len(iface_data)
        cfg = struct.pack("<BBHBBBBB", 9, 0x02, total_len, 1, 1, 0, 0x80, 250)
        return cfg + iface_data

    # ------------------------------------------------------------------
    # Server lifecycle
    # ------------------------------------------------------------------

    def run(self) -> None:
        self._update_sysfs()
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass

        self._server_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._server_sock.bind(self.sock_path)
        self._server_sock.listen(4)
        log.info("Listening on %s (mode=%s)", self.sock_path, self.mode)

        while not self._stop:
            try:
                self._server_sock.settimeout(1.0)
                conn, _ = self._server_sock.accept()
                log.debug("Client connected")
                self._serve_connection(conn)
                log.debug("Client disconnected")
            except socket.timeout:
                continue
            except OSError:
                if not self._stop:
                    raise

        self._server_sock.close()
        self.oplog.close()

    def stop(self) -> None:
        self._stop = True
        if self._server_sock:
            try:
                self._server_sock.close()
            except OSError:
                pass

    # ------------------------------------------------------------------
    # Connection handler
    # ------------------------------------------------------------------

    @staticmethod
    def _recv_all(conn: socket.socket, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = conn.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("socket closed")
            buf += chunk
        return buf

    def _send_response(self, conn: socket.socket,
                       status: int, payload: bytes = b"") -> None:
        conn.sendall(struct.pack("<iI", status, len(payload)) + payload)

    def _serve_connection(self, conn: socket.socket) -> None:
        try:
            while True:
                hdr = self._recv_all(conn, 5)
                msg_type, payload_len = struct.unpack("<BI", hdr)
                payload = self._recv_all(conn, payload_len) if payload_len else b""
                try:
                    status, resp = self._dispatch(msg_type, payload)
                    self._send_response(conn, status, resp)
                except Exception:  # noqa: BLE001
                    log.exception("Error dispatching msg_type=%d", msg_type)
                    self._send_response(conn, -1)
        except ConnectionError:
            pass
        finally:
            conn.close()

    # ------------------------------------------------------------------
    # Message dispatch
    # ------------------------------------------------------------------

    def _dispatch(self, msg_type: int, payload: bytes):
        handlers = {
            EMU_INIT:       self._h_init,
            EMU_EXIT:       self._h_exit,
            EMU_DEV_LIST:   self._h_dev_list,
            EMU_GET_CONFIG: self._h_get_config,
            EMU_OPEN:       self._h_open,
            EMU_CLOSE:      self._h_close,
            EMU_CLAIM:      self._h_claim,
            EMU_RELEASE:    self._h_release,
            EMU_BULK_OUT:   self._h_bulk_out,
            EMU_BULK_IN:    self._h_bulk_in,
            EMU_CTRL_OUT:   self._h_ctrl_out,
            EMU_CTRL_IN:    self._h_ctrl_in,
            EMU_CLEAR_HALT: lambda p: (0, b""),
        }
        fn = handlers.get(msg_type)
        if fn is None:
            log.warning("Unknown msg_type=%d", msg_type)
            return -1, b""
        return fn(payload)

    # ------------------------------------------------------------------
    # Handler implementations
    # ------------------------------------------------------------------

    def _h_init(self, _payload: bytes):
        log.debug("INIT (mode=%s)", self.mode)
        return 0, b""

    def _h_exit(self, _payload: bytes):
        log.debug("EXIT")
        return 0, b""

    def _h_dev_list(self, _payload: bytes):
        """Return one device matching current mode."""
        if self.mode == "loader":
            pid, iMfr, iProd = self.loader_pid, 1, 2
        else:
            pid, iMfr, iProd = self.maskrom_pid, 0, 0

        # 16-byte device entry: u16 vid, u16 pid, bus, addr, port_path_len,
        # ports[7], iManufacturer, iProduct
        dev = struct.pack("<HHBBB7sBB",
                          ROCKCHIP_VID, pid, 1, 1, 1,
                          b'\x01\x00\x00\x00\x00\x00\x00', iMfr, iProd)
        return 0, bytes([1]) + dev

    def _h_get_config(self, payload: bytes):
        """Return interface/endpoint info for config descriptor allocation."""
        # 1 interface, class=0xFF, subclass=6, proto=5, 2 bulk endpoints
        # Format: u8 bNumInterfaces, then per-iface: ifnum, cls, sub, proto, neps,
        #         then per-ep: addr, attrs, u16 maxpkt (LE)
        data = bytes([
            1,                               # bNumInterfaces
            0, 0xFF, 0x06, 0x05, 2,          # iface 0 descriptor
            0x01, 0x02, 0x00, 0x02,          # ep OUT 0x01 BULK 512
            0x81, 0x02, 0x00, 0x02,          # ep IN  0x81 BULK 512
        ])
        return 0, data

    def _h_open(self, payload: bytes):
        hid = self.next_handle
        self.next_handle += 1
        self.handles[hid] = HandleState()
        log.debug("OPEN -> handle=%d", hid)
        return 0, bytes([hid])

    def _h_close(self, payload: bytes):
        hid = payload[0]
        self.handles.pop(hid, None)
        log.debug("CLOSE handle=%d", hid)
        return 0, b""

    def _h_claim(self, payload: bytes):
        return 0, b""

    def _h_release(self, payload: bytes):
        return 0, b""

    def _h_bulk_out(self, payload: bytes):
        hid = payload[0]
        # ep = payload[1]
        data_len = struct.unpack_from("<I", payload, 2)[0]
        data = payload[6:6 + data_len]

        hs = self.handles.get(hid)
        if hs is None:
            return -9, b""  # LIBUSB_ERROR_PIPE

        if hs.state == "IDLE":
            cbw = self._parse_cbw(data)
            if cbw is None:
                log.error("Invalid CBW (handle=%d len=%d)", hid, len(data))
                return -1, b""

            result = self._process_cbw(hid, hs, cbw)
            if result is None:
                # Needs a DATA_OUT phase
                hs.state     = "DATA_OUT"
                hs.cbw       = cbw
                hs.write_buf = b""
                hs.write_len = cbw["data_length"]
            else:
                data_in, csw_status = result
                hs.cbw        = cbw
                hs.csw_status = csw_status
                if data_in:
                    hs.state      = "DATA_IN"
                    hs.pending_in = data_in
                else:
                    hs.state = "STATUS"
            return 0, struct.pack("<I", len(data))

        if hs.state == "DATA_OUT":
            hs.write_buf += data
            if len(hs.write_buf) >= hs.write_len:
                self._process_write(hid, hs, hs.cbw,
                                    hs.write_buf[:hs.write_len])
                hs.state = "STATUS"
            return 0, struct.pack("<I", len(data))

        log.warning("Unexpected BULK_OUT in state %s (handle=%d)", hs.state, hid)
        return -1, b""

    def _h_bulk_in(self, payload: bytes):
        hid = payload[0]
        maxlen = struct.unpack_from("<I", payload, 2)[0]

        hs = self.handles.get(hid)
        if hs is None:
            return -9, b""

        if hs.state == "DATA_IN":
            chunk = hs.pending_in[:maxlen]
            hs.pending_in = hs.pending_in[maxlen:]
            if not hs.pending_in:
                hs.state = "STATUS"
            return 0, struct.pack("<I", len(chunk)) + chunk

        if hs.state == "STATUS":
            csw = self._build_csw(hs.cbw["tag"] if hs.cbw else 0,
                                   0, hs.csw_status)
            hs.state = "IDLE"
            hs.cbw   = None
            return 0, struct.pack("<I", len(csw)) + csw

        if hs.state == "IDLE":
            log.warning("BULK_IN in IDLE state (handle=%d)", hid)
            return -9, b""

        log.warning("Unexpected BULK_IN in state %s (handle=%d)", hs.state, hid)
        return -1, b""

    def _h_ctrl_out(self, payload: bytes):
        hid = payload[0]
        # bm  = payload[1]
        req = payload[2]
        val, idx, data_len = struct.unpack_from("<HHH", payload, 3)
        data = payload[9:9 + data_len]

        if req == 0x0C and idx in (RKUSB_CTRL_DDR_INIT, RKUSB_CTRL_LOADER):
            crc_ok = False
            if len(data) >= 2:
                body = data[:-2]
                expected = (data[-2] << 8) | data[-1]
                crc_ok = crc_ccitt(body) == expected

            name = "471" if idx == RKUSB_CTRL_DDR_INIT else "472"
            self.oplog.write({
                "op":          "ctrl_upload",
                "wIndex":      idx,
                "wIndex_name": name,
                "data_len":    len(data),
                "crc_ok":      crc_ok,
            })
            log.debug("ctrl_upload wIndex=0x%04x crc_ok=%s", idx, crc_ok)

            if idx == RKUSB_CTRL_DDR_INIT:
                self.got_471 = True
            elif idx == RKUSB_CTRL_LOADER:
                self.got_472 = True
                # Transition to Loader mode
                self.mode = "loader"
                log.info("MaskROM → Loader transition")
                self._update_sysfs()

        return 0, struct.pack("<I", len(data))

    def _h_ctrl_in(self, payload: bytes):
        return 0, struct.pack("<I", 0)

    # ------------------------------------------------------------------
    # CBW helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _parse_cbw(data: bytes) -> Optional[dict]:
        if len(data) < 31:
            return None
        sig, tag, dlen, flags, lun, cdb_len = struct.unpack_from("<IIIBBB", data)
        if sig != CBW_SIGNATURE:
            return None
        cdb = data[15:15 + min(cdb_len, 16)]
        return {
            "tag":         tag,
            "data_length": dlen,
            "flags":       flags,
            "cdb":         cdb,
            "opcode":      cdb[0] if cdb else 0,
        }

    @staticmethod
    def _build_csw(tag: int, residue: int, status: int) -> bytes:
        return struct.pack("<IIIB", CSW_SIGNATURE, tag, residue, status)

    # ------------------------------------------------------------------
    # Command dispatch (CBW phase)
    # ------------------------------------------------------------------

    def _process_cbw(self, hid: int, hs: HandleState, cbw: dict):
        """
        Returns (data_in: bytes, csw_status: int), or None for DATA_OUT.
        """
        op  = cbw["opcode"]
        cdb = cbw["cdb"]

        if op == RKOP_TEST_UNIT_READY:
            self.oplog.write({"op": "TEST_UNIT_READY"})
            return b"", CSW_OK

        if op == RKOP_READ_FLASH_ID:
            self.oplog.write({"op": "READ_FLASH_ID"})
            return FLASH_ID_RESPONSE, CSW_OK

        if op == RKOP_READ_FLASH_INFO:
            self.oplog.write({"op": "READ_FLASH_INFO"})
            return FLASH_INFO_RESPONSE, CSW_OK

        if op == RKOP_READ_CHIP_INFO:
            self.oplog.write({"op": "READ_CHIP_INFO"})
            return CHIP_INFO_RESPONSE, CSW_OK

        if op == RKOP_READ_CAPABILITY:
            self.oplog.write({"op": "READ_CAPABILITY"})
            return CAPABILITY_RESPONSE, CSW_OK

        if op == RKOP_READ_LBA:
            # CDB layout: [0]=opcode [1]=0 [2:6]=LBA(BE) [6]=0 [7:9]=sectors(BE)
            lba     = struct.unpack_from(">I", cdb, 2)[0] if len(cdb) >= 6 else 0
            sectors = struct.unpack_from(">H", cdb, 7)[0] if len(cdb) >= 9 else 1
            self.oplog.write({"op": "READ_LBA", "lba": lba, "sectors": sectors})
            return self._flash_read(lba, sectors), CSW_OK

        if op == RKOP_WRITE_LBA:
            lba     = struct.unpack_from(">I", cdb, 2)[0] if len(cdb) >= 6 else 0
            sectors = struct.unpack_from(">H", cdb, 7)[0] if len(cdb) >= 9 else 1
            self.oplog.write({"op": "WRITE_LBA", "lba": lba, "sectors": sectors})
            hs.write_lba = lba
            hs.write_op  = op
            return None  # DATA_OUT phase

        if op == RKOP_ERASE_LBA:
            lba     = struct.unpack_from(">I", cdb, 2)[0] if len(cdb) >= 6 else 0
            sectors = struct.unpack_from(">H", cdb, 7)[0] if len(cdb) >= 9 else 1
            self.oplog.write({"op": "ERASE_LBA", "lba": lba, "sectors": sectors})
            self._flash_erase(lba, sectors)
            return b"", CSW_OK

        if op == RKOP_CHANGE_STORAGE:
            storage = cdb[5] if len(cdb) > 5 else 0
            self.oplog.write({"op": "CHANGE_STORAGE", "storage": storage})
            return b"", CSW_OK

        if op == RKOP_DEVICE_RESET:
            subcode = cdb[1] if len(cdb) > 1 else 0
            self.oplog.write({"op": "DEVICE_RESET", "subcode": subcode})
            return b"", CSW_OK

        if op == RKOP_WRITE_LOADER:
            self.oplog.write({"op": "WRITE_LOADER"})
            hs.write_op = op
            return None  # DATA_OUT phase

        log.warning("Unknown opcode 0x%02X", op)
        self.oplog.write({"op": "UNKNOWN_CMD", "opcode": op})
        return b"", CSW_FAIL

    def _process_write(self, hid: int, hs: HandleState,
                       cbw: dict, data: bytes) -> None:
        op = hs.write_op
        if op == RKOP_WRITE_LBA:
            self._flash_write(hs.write_lba, data)
        # WRITE_LOADER data is accepted and discarded
        hs.csw_status = CSW_OK

    # ------------------------------------------------------------------
    # Flash operations
    # ------------------------------------------------------------------

    def _flash_read(self, lba: int, sectors: int) -> bytes:
        start = lba * SECTOR_SIZE
        end   = (lba + sectors) * SECTOR_SIZE
        end   = min(end, len(self.flash))
        return bytes(self.flash[start:end])

    def _flash_write(self, lba: int, data: bytes) -> None:
        start = lba * SECTOR_SIZE
        end   = start + len(data)
        end   = min(end, len(self.flash))
        n     = end - start
        self.flash[start:start + n] = data[:n]

    def _flash_erase(self, lba: int, sectors: int) -> None:
        start = lba * SECTOR_SIZE
        end   = (lba + sectors) * SECTOR_SIZE
        end   = min(end, len(self.flash))
        self.flash[start:end] = b'\x00' * (end - start)
