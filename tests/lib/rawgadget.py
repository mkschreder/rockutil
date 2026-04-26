# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 rockutil contributors
"""
rawgadget.py — ctypes bindings for the Linux /dev/raw-gadget ABI.

Reference: include/uapi/linux/usb/raw_gadget.h
           include/uapi/linux/usb/ch9.h

This module is intentionally low-level.  Higher-level logic lives in
rockusb_device.py.  It may be imported without /dev/raw-gadget present
(the device is only opened on RawGadget.__init__).
"""

import ctypes
import fcntl
import os
import struct

# ---------------------------------------------------------------------------
# ioctl number helpers — mirrors <asm/ioctl.h>
# ---------------------------------------------------------------------------

_IOC_NONE  = 0
_IOC_WRITE = 1   # user writes to kernel
_IOC_READ  = 2   # kernel writes to user


def _IOC(direction, type_char, nr, size):
    return (direction << 30) | (size << 16) | (ord(type_char) << 8) | nr


def _IO(t, n):
    return _IOC(_IOC_NONE, t, n, 0)


def _IOR(t, n, size):
    return _IOC(_IOC_READ, t, n, size)


def _IOW(t, n, size):
    return _IOC(_IOC_WRITE, t, n, size)


def _IOWR(t, n, size):
    return _IOC(_IOC_READ | _IOC_WRITE, t, n, size)


# ---------------------------------------------------------------------------
# struct usb_raw_init
#   u8 driver_name[128]
#   u8 device_name[128]
#   u8 speed
# ---------------------------------------------------------------------------

_STRUCT_RAW_INIT_FMT = "128s128sB"
_STRUCT_RAW_INIT_SIZE = struct.calcsize(_STRUCT_RAW_INIT_FMT)  # 257

# ---------------------------------------------------------------------------
# struct usb_raw_event (variable-length, we allocate with extra bytes)
#   u32 type
#   u32 length
#   u8  data[]
# ---------------------------------------------------------------------------

_STRUCT_RAW_EVENT_HDR_FMT = "<II"
_STRUCT_RAW_EVENT_HDR_SIZE = struct.calcsize(_STRUCT_RAW_EVENT_HDR_FMT)  # 8

# ---------------------------------------------------------------------------
# struct usb_raw_ep_io (variable-length)
#   u16 ep
#   u16 flags
#   u32 length
#   u8  data[]
# ---------------------------------------------------------------------------

_STRUCT_RAW_EP_IO_HDR_FMT = "<HHI"
_STRUCT_RAW_EP_IO_HDR_SIZE = struct.calcsize(_STRUCT_RAW_EP_IO_HDR_FMT)  # 8

# ---------------------------------------------------------------------------
# struct usb_endpoint_descriptor (7 bytes, bLength=7, bDescriptorType=5)
# (ch9.h; bulk endpoints do not have the 2-byte wInterval so size is 7)
#   u8 bLength
#   u8 bDescriptorType
#   u8 bEndpointAddress
#   u8 bmAttributes
#   u16 wMaxPacketSize
#   u8 bInterval
# ---------------------------------------------------------------------------

_STRUCT_EP_DESC_FMT = "<BBBBBHB"
_STRUCT_EP_DESC_SIZE = struct.calcsize(_STRUCT_EP_DESC_FMT)  # 7

# USB constants
USB_DT_ENDPOINT = 0x05
USB_ENDPOINT_XFER_BULK = 0x02
USB_DIR_IN  = 0x80
USB_DIR_OUT = 0x00

# USB device/interface class codes
USB_CLASS_VENDOR_SPEC = 0xFF

# usb_device_speed
USB_SPEED_FULL  = 1
USB_SPEED_HIGH  = 3

# usb_raw_event_type
USB_RAW_EVENT_INVALID    = 0
USB_RAW_EVENT_CONNECT    = 1
USB_RAW_EVENT_CONTROL    = 2
USB_RAW_EVENT_SUSPEND    = 3
USB_RAW_EVENT_RESUME     = 4
USB_RAW_EVENT_RESET      = 5
USB_RAW_EVENT_DISCONNECT = 6

# USB control-request bmRequestType composition
USB_DIR_OUT_CTRL  = 0x00
USB_DIR_IN_CTRL   = 0x80
USB_TYPE_VENDOR   = 0x40
USB_TYPE_STANDARD = 0x00
USB_RECIP_DEVICE  = 0x00
USB_RECIP_IFACE   = 0x01

# Standard bRequest codes
USB_REQ_GET_DESCRIPTOR  = 0x06
USB_REQ_SET_CONFIGURATION = 0x09
USB_REQ_SET_INTERFACE   = 0x0B

# USB descriptor types
USB_DT_DEVICE        = 0x01
USB_DT_CONFIG        = 0x02
USB_DT_STRING        = 0x03
USB_DT_INTERFACE     = 0x04

# IO flag
USB_RAW_IO_FLAGS_ZERO = 0x0001

# ---------------------------------------------------------------------------
# ioctl numbers
# ---------------------------------------------------------------------------

USB_RAW_IOCTL_INIT        = _IOW('U', 0,  _STRUCT_RAW_INIT_SIZE)
USB_RAW_IOCTL_RUN         = _IO( 'U', 1)
USB_RAW_IOCTL_EVENT_FETCH = _IOR('U', 2,  _STRUCT_RAW_EVENT_HDR_SIZE)
USB_RAW_IOCTL_EP0_WRITE   = _IOW('U', 3,  _STRUCT_RAW_EP_IO_HDR_SIZE)
USB_RAW_IOCTL_EP0_READ    = _IOWR('U', 4, _STRUCT_RAW_EP_IO_HDR_SIZE)
USB_RAW_IOCTL_EP_ENABLE   = _IOW('U', 5,  _STRUCT_EP_DESC_SIZE)
USB_RAW_IOCTL_EP_DISABLE  = _IOW('U', 6,  4)   # __u32
USB_RAW_IOCTL_EP_WRITE    = _IOW('U', 7,  _STRUCT_RAW_EP_IO_HDR_SIZE)
USB_RAW_IOCTL_EP_READ     = _IOWR('U', 8, _STRUCT_RAW_EP_IO_HDR_SIZE)
USB_RAW_IOCTL_CONFIGURE   = _IO( 'U', 9)
USB_RAW_IOCTL_VBUS_DRAW   = _IOW('U', 10, 4)   # __u32
USB_RAW_IOCTL_EP0_STALL   = _IO( 'U', 12)


# ---------------------------------------------------------------------------
# Helper: allocate a ctypes buffer that starts with the ep_io header
# ---------------------------------------------------------------------------

def _make_ep_io_buf(ep_handle: int, data: bytes, flags: int = 0) -> bytearray:
    """Pack a usb_raw_ep_io struct with payload into a bytearray."""
    hdr = struct.pack(_STRUCT_RAW_EP_IO_HDR_FMT, ep_handle, flags, len(data))
    buf = bytearray(hdr + data)
    return buf


def _make_ep_io_buf_recv(ep_handle: int, max_len: int) -> bytearray:
    """Allocate a usb_raw_ep_io receive buffer of max_len bytes."""
    hdr = struct.pack(_STRUCT_RAW_EP_IO_HDR_FMT, ep_handle, 0, max_len)
    buf = bytearray(hdr + bytes(max_len))
    return buf


def _ep_io_data(buf: bytearray) -> bytes:
    """Extract the data portion from a filled ep_io buffer."""
    _, _, length = struct.unpack_from(_STRUCT_RAW_EP_IO_HDR_FMT, buf)
    return bytes(buf[_STRUCT_RAW_EP_IO_HDR_SIZE:_STRUCT_RAW_EP_IO_HDR_SIZE + length])


# ---------------------------------------------------------------------------
# RawGadget — thin wrapper around the /dev/raw-gadget fd
# ---------------------------------------------------------------------------

class RawGadget:
    """
    Manages one /dev/raw-gadget file descriptor.

    Usage::

        g = RawGadget()
        g.init("dummy_udc", "dummy_udc.0", USB_SPEED_HIGH)
        g.run()

        # event loop:
        while True:
            ev_type, ev_data = g.event_fetch(4096)
            if ev_type == USB_RAW_EVENT_CONNECT:
                g.vbus_draw(500 // 2)   # 250 × 2 mA = 500 mA
            elif ev_type == USB_RAW_EVENT_CONTROL:
                setup = parse_setup(ev_data)
                ...
                g.ep0_write(response)

        g.close()
    """

    def __init__(self, path: str = "/dev/raw-gadget"):
        self._fd = os.open(path, os.O_RDWR)

    def close(self) -> None:
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    # ------------------------------------------------------------------
    # Core lifecycle
    # ------------------------------------------------------------------

    def init(self, driver_name: str, device_name: str,
             speed: int = USB_SPEED_HIGH) -> None:
        buf = struct.pack(
            _STRUCT_RAW_INIT_FMT,
            driver_name.encode()[:128].ljust(128, b"\x00"),
            device_name.encode()[:128].ljust(128, b"\x00"),
            speed,
        )
        self._ioctl(USB_RAW_IOCTL_INIT, bytearray(buf))

    def run(self) -> None:
        self._ioctl(USB_RAW_IOCTL_RUN, None)

    def configure(self) -> None:
        self._ioctl(USB_RAW_IOCTL_CONFIGURE, None)

    def vbus_draw(self, current_2ma: int) -> None:
        """Set VBUS current limit; argument is in 2-mA units."""
        buf = bytearray(struct.pack("<I", current_2ma))
        self._ioctl(USB_RAW_IOCTL_VBUS_DRAW, buf)

    # ------------------------------------------------------------------
    # Endpoint 0
    # ------------------------------------------------------------------

    def event_fetch(self, max_data_len: int = 4096) -> tuple[int, bytes]:
        """
        Block until an event arrives.
        Returns (event_type, event_data_bytes).
        event_data is a usb_ctrlrequest (8 bytes) for CONTROL events.
        """
        buf = bytearray(_STRUCT_RAW_EVENT_HDR_SIZE + max_data_len)
        struct.pack_into("<II", buf, 0, 0, max_data_len)
        n = self._ioctl(USB_RAW_IOCTL_EVENT_FETCH, buf)
        ev_type, ev_len = struct.unpack_from("<II", buf)
        return ev_type, bytes(buf[_STRUCT_RAW_EVENT_HDR_SIZE:
                                  _STRUCT_RAW_EVENT_HDR_SIZE + ev_len])

    def ep0_write(self, data: bytes, zero: bool = False) -> int:
        """Send data on EP0 (IN direction, device→host)."""
        flags = USB_RAW_IO_FLAGS_ZERO if zero else 0
        buf = _make_ep_io_buf(0, data, flags)
        return self._ioctl(USB_RAW_IOCTL_EP0_WRITE, buf)

    def ep0_read(self, max_len: int = 4096) -> bytes:
        """Receive data on EP0 (OUT direction, host→device)."""
        buf = _make_ep_io_buf_recv(0, max_len)
        n = self._ioctl(USB_RAW_IOCTL_EP0_READ, buf)
        # kernel fills in length field
        _, _, length = struct.unpack_from(_STRUCT_RAW_EP_IO_HDR_FMT, buf)
        actual = min(length, n) if n > 0 else length
        return bytes(buf[_STRUCT_RAW_EP_IO_HDR_SIZE:
                         _STRUCT_RAW_EP_IO_HDR_SIZE + actual])

    def ep0_stall(self) -> None:
        self._ioctl(USB_RAW_IOCTL_EP0_STALL, None)

    # ------------------------------------------------------------------
    # Non-control endpoints
    # ------------------------------------------------------------------

    def ep_enable(self, addr: int, transfer_type: int,
                  max_packet: int = 512) -> int:
        """
        Enable an endpoint matching the given address and transfer type.
        Returns the endpoint handle used for subsequent EP_READ/WRITE calls.
        """
        buf = bytearray(struct.pack(
            _STRUCT_EP_DESC_FMT,
            7,                  # bLength
            USB_DT_ENDPOINT,    # bDescriptorType
            addr,               # bEndpointAddress
            transfer_type,      # bmAttributes
            0,                  # padding byte (bDescriptorType low nibble)
            max_packet,         # wMaxPacketSize
            0,                  # bInterval
        ))
        handle = self._ioctl(USB_RAW_IOCTL_EP_ENABLE, buf)
        return handle

    def ep_disable(self, handle: int) -> None:
        buf = bytearray(struct.pack("<I", handle))
        self._ioctl(USB_RAW_IOCTL_EP_DISABLE, buf)

    def ep_write(self, handle: int, data: bytes, zero: bool = False) -> int:
        """Send data on a bulk IN endpoint (device→host)."""
        flags = USB_RAW_IO_FLAGS_ZERO if zero else 0
        buf = _make_ep_io_buf(handle, data, flags)
        return self._ioctl(USB_RAW_IOCTL_EP_WRITE, buf)

    def ep_read(self, handle: int, max_len: int) -> bytes:
        """Receive data on a bulk OUT endpoint (host→device)."""
        buf = _make_ep_io_buf_recv(handle, max_len)
        n = self._ioctl(USB_RAW_IOCTL_EP_READ, buf)
        _, _, length = struct.unpack_from(_STRUCT_RAW_EP_IO_HDR_FMT, buf)
        actual = min(length, n) if n > 0 else length
        return bytes(buf[_STRUCT_RAW_EP_IO_HDR_SIZE:
                         _STRUCT_RAW_EP_IO_HDR_SIZE + actual])

    def ep_set_halt(self, handle: int) -> None:
        buf = bytearray(struct.pack("<I", handle))
        self._ioctl(0x4004550d, buf)  # USB_RAW_IOCTL_EP_SET_HALT

    def ep_clear_halt(self, handle: int) -> None:
        buf = bytearray(struct.pack("<I", handle))
        self._ioctl(0x4004550e, buf)  # USB_RAW_IOCTL_EP_CLEAR_HALT

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _ioctl(self, request: int, arg) -> int:
        if arg is None:
            return fcntl.ioctl(self._fd, request, 0)
        return fcntl.ioctl(self._fd, request, arg, True)


# ---------------------------------------------------------------------------
# USB descriptor builders
# ---------------------------------------------------------------------------

def build_device_descriptor(
    vid: int,
    pid: int,
    bcd_device: int = 0x0100,
    i_manufacturer: int = 0,
    i_product: int = 0,
    i_serial: int = 0,
    bcd_usb: int = 0x0200,
    device_class: int = 0,
    device_subclass: int = 0,
    device_protocol: int = 0,
    max_packet0: int = 64,
) -> bytes:
    return struct.pack(
        "<BBHBBBBHHHBBBB",
        18,               # bLength
        USB_DT_DEVICE,    # bDescriptorType
        bcd_usb,          # bcdUSB
        device_class,     # bDeviceClass
        device_subclass,  # bDeviceSubClass
        device_protocol,  # bDeviceProtocol
        max_packet0,      # bMaxPacketSize0
        vid,              # idVendor
        pid,              # idProduct
        bcd_device,       # bcdDevice
        i_manufacturer,   # iManufacturer
        i_product,        # iProduct
        i_serial,         # iSerialNumber
        1,                # bNumConfigurations
    )


def build_config_descriptor(interfaces: bytes, max_power_ma: int = 500) -> bytes:
    total = 9 + len(interfaces)
    return struct.pack(
        "<BBHBBBBB",
        9,              # bLength
        USB_DT_CONFIG,  # bDescriptorType
        total,          # wTotalLength
        1,              # bNumInterfaces
        1,              # bConfigurationValue
        0,              # iConfiguration
        0x80,           # bmAttributes (bus-powered)
        max_power_ma // 2,  # bMaxPower (2-mA units)
    ) + interfaces


def build_interface_descriptor(
    iface_num: int = 0,
    num_endpoints: int = 2,
    iface_class: int = 0xFF,
    iface_subclass: int = 0x06,
    iface_protocol: int = 0x05,
    i_interface: int = 0,
    alt_setting: int = 0,
) -> bytes:
    return struct.pack(
        "<BBBBBBBBB",
        9,                  # bLength
        USB_DT_INTERFACE,   # bDescriptorType
        iface_num,          # bInterfaceNumber
        alt_setting,        # bAlternateSetting
        num_endpoints,      # bNumEndpoints
        iface_class,        # bInterfaceClass
        iface_subclass,     # bInterfaceSubClass
        iface_protocol,     # bInterfaceProtocol
        i_interface,        # iInterface
    )


def build_endpoint_descriptor(
    addr: int,
    transfer_type: int = USB_ENDPOINT_XFER_BULK,
    max_packet: int = 512,
    interval: int = 0,
) -> bytes:
    return struct.pack(
        "<BBBBHB",
        7,                  # bLength
        USB_DT_ENDPOINT,    # bDescriptorType
        addr,               # bEndpointAddress
        transfer_type,      # bmAttributes
        max_packet,         # wMaxPacketSize
        interval,           # bInterval
    )


def build_string_descriptor(text: str) -> bytes:
    encoded = text.encode("utf-16-le")
    length = 2 + len(encoded)
    return bytes([length, USB_DT_STRING]) + encoded


def build_lang_descriptor() -> bytes:
    """Language ID descriptor for English (0x0409)."""
    return bytes([4, USB_DT_STRING, 0x09, 0x04])


# ---------------------------------------------------------------------------
# Utility: parse usb_ctrlrequest (8 bytes)
# ---------------------------------------------------------------------------

def parse_ctrl_request(data: bytes) -> dict:
    """
    Parse 8-byte usb_ctrlrequest.
    Returns dict with keys: bmRequestType, bRequest, wValue, wIndex, wLength.
    """
    bm, req, val, idx, length = struct.unpack_from("<BBHHH", data)
    return {
        "bmRequestType": bm,
        "bRequest": req,
        "wValue": val,
        "wIndex": idx,
        "wLength": length,
    }
