# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 rockutil contributors
#!/usr/bin/env python3
"""
run_emulator.py — CLI wrapper that starts the Rockusb emulator.

Usage (as root):
    python3 run_emulator.py [options]

Options:
    --mode       loader | maskrom   (default: loader)
    --udc        UDC device name    (default: dummy_udc.0)
    --driver     UDC driver name    (default: dummy_udc)
    --pid        Loader PID hex     (default: 0x350B)
    --maskrom-pid MaskROM PID hex   (default: 0x350A)
    --oplog      Path to op-log file (default: tests/build/oplog.jsonl)
    --gadget     Path to raw-gadget  (default: /dev/raw-gadget)
    --daemon     Daemonize and write PID to --pidfile
    --pidfile    PID file path       (default: tests/build/emulator.pid)
    --log-level  DEBUG|INFO|WARNING  (default: INFO)

The process writes its PID to --pidfile before the gadget is bound.
Send SIGTERM to stop gracefully.
"""

import argparse
import logging
import os
import signal
import sys

# Add the lib directory to the path so we can import rawgadget + rockusb_device
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from rockusb_device import RockusbEmulator


def _daemonize() -> None:
    """Double-fork daemonise. Stdout/stderr stay connected for debugging."""
    pid = os.fork()
    if pid > 0:
        sys.exit(0)
    os.setsid()
    pid = os.fork()
    if pid > 0:
        sys.exit(0)


def main() -> int:
    ap = argparse.ArgumentParser(description="Rockusb USB emulator")
    ap.add_argument("--mode",       default="loader",
                    choices=["loader", "maskrom"])
    ap.add_argument("--udc",        default="dummy_udc.0",
                    help="UDC device name")
    ap.add_argument("--driver",     default="dummy_udc",
                    help="UDC driver name")
    ap.add_argument("--pid",        default="0x350B",
                    help="Loader mode PID (hex)")
    ap.add_argument("--maskrom-pid", default="0x350A",
                    help="MaskROM mode PID (hex)")
    ap.add_argument("--transition-pid", default=None,
                    help="PID after MaskROM→Loader transition (defaults to --pid)")
    ap.add_argument("--oplog",      default="tests/build/oplog.jsonl",
                    help="Path for the NDJSON op-log")
    ap.add_argument("--gadget",     default="/dev/raw-gadget",
                    help="Path to /dev/raw-gadget")
    ap.add_argument("--daemon",     action="store_true",
                    help="Daemonize the process")
    ap.add_argument("--pidfile",    default="tests/build/emulator.pid",
                    help="Path to write PID")
    ap.add_argument("--log-level",  default="INFO",
                    choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    args = ap.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    loader_pid     = int(args.pid, 16) if args.pid.startswith("0x") \
                     else int(args.pid)
    maskrom_pid    = int(args.maskrom_pid, 16) \
                     if args.maskrom_pid.startswith("0x") \
                     else int(args.maskrom_pid)
    if args.transition_pid is not None:
        trans_pid = int(args.transition_pid, 16) \
                    if args.transition_pid.startswith("0x") \
                    else int(args.transition_pid)
    else:
        trans_pid = loader_pid

    if args.daemon:
        _daemonize()

    # Write PID file before binding so helpers can find us immediately.
    os.makedirs(os.path.dirname(args.pidfile) or ".", exist_ok=True)
    with open(args.pidfile, "w") as f:
        f.write(str(os.getpid()) + "\n")

    emu = RockusbEmulator(
        mode=args.mode,
        udc_driver=args.driver,
        udc_device=args.udc,
        loader_pid=loader_pid,
        maskrom_pid=maskrom_pid,
        transition_pid=trans_pid,
        oplog_path=args.oplog,
        raw_gadget_path=args.gadget,
    )

    def _on_sigterm(sig, frame):
        logging.info("SIGTERM received, stopping emulator")
        emu.stop()

    signal.signal(signal.SIGTERM, _on_sigterm)
    signal.signal(signal.SIGINT,  _on_sigterm)

    try:
        emu.run()
    except Exception as exc:  # noqa: BLE001
        logging.error("Emulator exited with error: %s", exc, exc_info=True)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
