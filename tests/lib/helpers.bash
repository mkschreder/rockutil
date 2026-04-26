# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 rockutil contributors
#
# helpers.bash — shared Bats setup/teardown and utility functions.
#
# Source this file from loader.bats and maskrom.bats:
#   load "lib/helpers"
#
# Expected environment variables (set by the Makefile):
#   TOOL          Path to ./rockutil
#   FIXTURE_DIR   Path to the generated fixtures directory
#   TEST_DIR      Path to the tests/ directory
#   BUILD_DIR     Path to the build directory
#   EMU_SO        Path to libusb_emu.so (LD_PRELOAD mock)

TOOL="${TOOL:-./rockutil}"
FIXTURE_DIR="${FIXTURE_DIR:-tests/build/fixtures}"
TEST_DIR="${TEST_DIR:-tests}"
BUILD_DIR="${BUILD_DIR:-tests/build}"
EMU_SO="${EMU_SO:-tests/build/libusb_emu.so}"

EMULATOR_PID_FILE="${BUILD_DIR}/emulator.pid"
EMULATOR_LOG_FILE="${BUILD_DIR}/emulator.log"
OPLOG_FILE="${BUILD_DIR}/oplog.jsonl"

# Unique socket path per test run (avoids conflicts with concurrent runs)
EMU_SOCKET="${EMU_SOCKET:-/tmp/rockutil-emu-$$.sock}"
export EMU_SOCKET

# Fake sysfs root so rkusb_open_live's descriptor reads work without real USB
EMU_SYSFS_ROOT="${BUILD_DIR}/fake-sysfs"
export ROCKUTIL_SYSFS_ROOT="${EMU_SYSFS_ROOT}"

# ---------------------------------------------------------------------------
# Emulator lifecycle
# ---------------------------------------------------------------------------

# start_emulator MODE
# MODE = "loader" | "maskrom"
start_emulator() {
    local mode="$1"

    mkdir -p "${BUILD_DIR}"
    : > "${OPLOG_FILE}"
    mkdir -p "${EMU_SYSFS_ROOT}"

    # Remove stale socket and PID file
    rm -f "${EMU_SOCKET}" "${EMULATOR_PID_FILE}"

    python3 "${TEST_DIR}/lib/run_socket_emu.py" \
        --mode   "${mode}" \
        --socket "${EMU_SOCKET}" \
        --oplog  "${OPLOG_FILE}" \
        --sysfs-root "${EMU_SYSFS_ROOT}" \
        --daemon \
        --pidfile "${EMULATOR_PID_FILE}" \
        --log-level INFO \
        >> "${EMULATOR_LOG_FILE}" 2>&1

    # Wait up to 5 s for the PID file to appear
    local deadline=$(( SECONDS + 5 ))
    while [ ! -f "${EMULATOR_PID_FILE}" ] && [ "$SECONDS" -lt "$deadline" ]; do
        sleep 0.1
    done
    if [ ! -f "${EMULATOR_PID_FILE}" ]; then
        echo "ERROR: emulator PID file not created" >&2
        cat "${EMULATOR_LOG_FILE}" >&2
        return 1
    fi
}

# wait_for_usb VID PID [timeout_secs]
# With the socket emulator there is no real USB device; instead we poll
# for the Unix socket file to appear (created by the emulator before it
# starts listening).
wait_for_usb() {
    local _vid="$1" _pid="$2" timeout="${3:-10}"
    local deadline=$(( SECONDS + timeout ))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -S "${EMU_SOCKET}" ]; then
            return 0
        fi
        sleep 0.1
    done
    echo "ERROR: emulator socket ${EMU_SOCKET} not ready within ${timeout}s" >&2
    cat "${EMULATOR_LOG_FILE}" >&2
    return 1
}

# stop_emulator
stop_emulator() {
    if [ ! -f "${EMULATOR_PID_FILE}" ]; then
        return 0
    fi
    local pid
    pid=$(cat "${EMULATOR_PID_FILE}" 2>/dev/null) || return 0
    kill -TERM "$pid" 2>/dev/null || true

    local deadline=$(( SECONDS + 5 ))
    while kill -0 "$pid" 2>/dev/null && [ "$SECONDS" -lt "$deadline" ]; do
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    rm -f "${EMULATOR_PID_FILE}" "${EMU_SOCKET}"
}

# run_tool ARGS...
# Wrapper: run $TOOL with LD_PRELOAD and required environment variables.
run_tool() {
    LD_PRELOAD="${EMU_SO}" \
    ROCKUTIL_EMU_SOCKET="${EMU_SOCKET}" \
    ROCKUTIL_SYSFS_ROOT="${EMU_SYSFS_ROOT}" \
        "$TOOL" "$@"
}

# ---------------------------------------------------------------------------
# Op-log helpers
# ---------------------------------------------------------------------------

# oplog_has_op OP_NAME [KEY VALUE]
oplog_has_op() {
    local op_name="$1"
    local key="${2:-}"
    local value="${3:-}"
    python3 - <<PYEOF
import json, sys
lines = open("${OPLOG_FILE}").readlines()
for line in lines:
    try:
        entry = json.loads(line.strip())
    except Exception:
        continue
    if entry.get("op") != "${op_name}":
        continue
    if "${key}" and str(entry.get("${key}")) != "${value}":
        continue
    sys.exit(0)
sys.exit(1)
PYEOF
}

# oplog_field OP_NAME KEY
oplog_field() {
    local op_name="$1" key="$2"
    python3 - <<PYEOF
import json, sys
lines = open("${OPLOG_FILE}").readlines()
for line in reversed(lines):
    try:
        entry = json.loads(line.strip())
    except Exception:
        continue
    if entry.get("op") == "${op_name}":
        val = entry.get("${key}")
        if val is not None:
            print(val)
            sys.exit(0)
sys.exit(1)
PYEOF
}

# oplog_count OP_NAME
oplog_count() {
    local op_name="$1"
    python3 -c "
import json
lines = open('${OPLOG_FILE}').readlines()
n = sum(1 for l in lines if json.loads(l.strip()).get('op') == '${op_name}')
print(n)
" 2>/dev/null
}

# ---------------------------------------------------------------------------
# Shared setup_file / teardown_file helpers
# ---------------------------------------------------------------------------

loader_setup_file() {
    start_emulator "loader"
    wait_for_usb "2207" "350b" 10
}

loader_teardown_file() {
    stop_emulator
}

maskrom_setup_file() {
    start_emulator "maskrom"
    wait_for_usb "2207" "350a" 10
}

maskrom_teardown_file() {
    stop_emulator
}
