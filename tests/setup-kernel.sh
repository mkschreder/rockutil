# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 rockutil contributors
#!/usr/bin/env bash
# setup-kernel.sh — load USB gadget kernel modules needed for integration tests.
#
# Must be run as root.  Idempotent: safe to call multiple times.
# Exits non-zero if any prerequisite is missing, with a clear remediation hint.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die() { echo "FATAL: $*" >&2; exit 1; }
info() { echo "[setup-kernel] $*"; }

# ---------------------------------------------------------------------------
# 1. Verify we are root
# ---------------------------------------------------------------------------
[ "$(id -u)" -eq 0 ] || die "Must be run as root (sudo $0)"

# ---------------------------------------------------------------------------
# 2. Load required kernel modules
# ---------------------------------------------------------------------------

load_module() {
    local mod="$1"
    shift
    if ! lsmod | grep -q "^${mod} "; then
        info "Loading module: $mod $*"
        if ! modprobe "$mod" "$@" 2>/dev/null; then
            echo "ERROR: cannot load $mod. Ensure the following kernel configs are set:" >&2
            case "$mod" in
                dummy_hcd)       echo "  CONFIG_USB_DUMMY_HCD=m or =y" >&2 ;;
                raw_gadget)      echo "  CONFIG_USB_RAW_GADGET=m or =y" >&2 ;;
                libcomposite)    echo "  CONFIG_USB_LIBCOMPOSITE=m or =y" >&2 ;;
            esac
            die "Module $mod could not be loaded."
        fi
    else
        info "Module $mod already loaded."
    fi
}

load_module libcomposite
load_module dummy_hcd num=1
load_module raw_gadget

# ---------------------------------------------------------------------------
# 3. Mount configfs (if not already mounted)
# ---------------------------------------------------------------------------
if ! mountpoint -q /sys/kernel/config 2>/dev/null; then
    info "Mounting configfs at /sys/kernel/config"
    mount -t configfs none /sys/kernel/config || \
        die "Cannot mount configfs"
else
    info "configfs already mounted."
fi

# ---------------------------------------------------------------------------
# 4. Verify the dummy UDC appeared
# ---------------------------------------------------------------------------
UDC_DIR="/sys/class/udc"
if [ ! -d "$UDC_DIR" ]; then
    die "UDC directory $UDC_DIR not found after loading dummy_hcd"
fi

# Find a dummy UDC instance
UDC_NAME=""
for udc in "$UDC_DIR"/dummy_udc.*; do
    [ -e "$udc" ] && UDC_NAME="$(basename "$udc")" && break
done

if [ -z "$UDC_NAME" ]; then
    die "No dummy_udc.N found under $UDC_DIR. Is CONFIG_USB_DUMMY_HCD set?"
fi
info "Dummy UDC found: $UDC_NAME"

# ---------------------------------------------------------------------------
# 5. Verify /dev/raw-gadget exists
# ---------------------------------------------------------------------------
if [ ! -c /dev/raw-gadget ]; then
    die "/dev/raw-gadget not found. Is CONFIG_USB_RAW_GADGET set? Try:" \
        "  modprobe raw_gadget"
fi
info "/dev/raw-gadget OK"

info "Kernel setup complete. UDC=$UDC_NAME"
echo "$UDC_NAME"   # print UDC name on stdout for callers to capture
