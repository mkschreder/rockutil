#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 rockutil contributors
"""
run_socket_emu.py — CLI wrapper to launch the socket-based Rockusb emulator.

Usage:
    python3 run_socket_emu.py [options]

Options:
    --mode        loader | maskrom  (default: loader)
    --socket      Unix socket path  (default: /tmp/rockutil-emu.sock)
    --oplog       NDJSON op-log path (default: tests/build/oplog.jsonl)
    --sysfs-root  Fake sysfs root dir (optional)
    --daemon      Daemonize
    --pidfile     PID file path
    --log-level   DEBUG|INFO|WARNING|ERROR (default: INFO)
"""

import argparse
import logging
import os
import signal
import sys

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from usb_socket_emu import SocketRockusbEmulator


def _daemonize() -> None:
    pid = os.fork()
    if pid > 0:
        sys.exit(0)
    os.setsid()
    pid = os.fork()
    if pid > 0:
        sys.exit(0)


def main() -> int:
    ap = argparse.ArgumentParser(description="Socket-based Rockusb emulator")
    ap.add_argument("--mode",       default="loader",
                    choices=["loader", "maskrom"])
    ap.add_argument("--socket",     default="/tmp/rockutil-emu.sock")
    ap.add_argument("--oplog",      default="tests/build/oplog.jsonl")
    ap.add_argument("--sysfs-root", default=None)
    ap.add_argument("--daemon",     action="store_true")
    ap.add_argument("--pidfile",    default="tests/build/emulator.pid")
    ap.add_argument("--log-level",  default="INFO",
                    choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    args = ap.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    if args.daemon:
        _daemonize()

    os.makedirs(os.path.dirname(os.path.abspath(args.pidfile)) or ".",
                exist_ok=True)
    with open(args.pidfile, "w") as f:
        f.write(str(os.getpid()) + "\n")

    emu = SocketRockusbEmulator(
        mode=args.mode,
        sock_path=args.socket,
        oplog_path=args.oplog,
        sysfs_root=args.sysfs_root,
    )

    def _on_signal(sig, frame):
        logging.info("Signal %d received, stopping", sig)
        emu.stop()

    signal.signal(signal.SIGTERM, _on_signal)
    signal.signal(signal.SIGINT,  _on_signal)

    try:
        emu.run()
    except Exception as exc:  # noqa: BLE001
        logging.error("Emulator error: %s", exc, exc_info=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
