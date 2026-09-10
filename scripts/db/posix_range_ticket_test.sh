#!/usr/bin/env bash
# Verify POSIX ticket admission, dependency ordering, and range-safe I/O.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-shard63-ticket.img}"
RUN_DIR="${4:-/var/tmp/hfs-shard63-ticket}"
THREADS="${HFS_TICKET_THREADS:-16}"
LOCK_PATH="$RUN_DIR/hfs.lock"
STATS_PATH="$RUN_DIR/stats"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
	HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 \
	HFS_STATS_PATH="$STATS_PATH" HFS_STATS_TIMING=1 HFS_POSIX_TICKET=1 \
	HFS_POSIX_TICKET_DEPTH=64 HFS_DATA_REACTORS=4 \
	HFS_TICKET_THREADS="$THREADS" python3 - <<'PYEOF'
import ctypes
import os
import threading

root = "/hfs/shard63"
unit = 4096
threads = int(os.environ["HFS_TICKET_THREADS"])
os.mkdir(root)

fd = os.open(root + "/ranges", os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.pwrite(fd, b"A" * (threads * unit), 0) == threads * unit
errors = []
barrier = threading.Barrier(threads)

# Independent ranges should execute concurrently without corrupting neighbors.
def range_writer(index):
    try:
        barrier.wait()
        payload = bytes([0x30 + index]) * unit
        assert os.pwrite(fd, payload, index * unit) == unit
    except BaseException as exc:
        errors.append(("range", index, repr(exc)))

workers = [threading.Thread(target=range_writer, args=(i,)) for i in range(threads)]
for worker in workers:
    worker.start()
for worker in workers:
    worker.join()
assert not errors, errors
data = os.pread(fd, threads * unit, 0)
for index in range(threads):
    assert data[index * unit:(index + 1) * unit] == bytes([0x30 + index]) * unit

# Same-unit partial writes must preserve bytes outside the RMW interval.
for _ in range(8):
    assert os.pwrite(fd, b"B" * unit, 0) == unit
    errors = []
    barrier = threading.Barrier(threads)

    def rmw_writer(index):
        try:
            barrier.wait()
            payload = bytes([0x50 + (index % 16)]) * 17
            assert os.pwrite(fd, payload, 31) == len(payload)
        except BaseException as exc:
            errors.append(("rmw", index, repr(exc)))

    workers = [threading.Thread(target=rmw_writer, args=(i,)) for i in range(threads)]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    assert not errors, errors
    result = os.pread(fd, unit, 0)
    if result[:31] != b"B" * 31 or result[48:] != b"B" * (unit - 48):
        print("RMW outside bytes changed", result[:64], result[-16:])
    assert result[:31] == b"B" * 31
    assert result[48:] == b"B" * (unit - 48)

# A dependency-heavy overlap burst exercises ticket wait and wake paths.
errors = []
barrier = threading.Barrier(threads)

def overlapping_writer(index):
    try:
        barrier.wait()
        for round_id in range(12):
            payload = bytes([0x70 + (index % 16)]) * 64
            assert os.pwrite(fd, payload, (round_id % 8) * 512) == len(payload)
    except BaseException as exc:
        errors.append(("overlap", index, repr(exc)))

workers = [threading.Thread(target=overlapping_writer, args=(i,)) for i in range(threads)]
for worker in workers:
    worker.start()
for worker in workers:
    worker.join()
assert not errors, errors

# Exclusive control operations must remain ordered with data I/O.
libc = ctypes.CDLL(None, use_errno=True)
libc.fallocate.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_longlong, ctypes.c_longlong]
libc.fallocate.restype = ctypes.c_int
keep_size = 0x01
punch_hole = 0x02
for _ in range(12):
    assert os.pwrite(fd, b"Q" * unit, unit) == unit
    os.fsync(fd)
    assert libc.fallocate(fd, punch_hole | keep_size, unit, unit) == 0
os.close(fd)

print("POSIX RANGE TICKET OK")
PYEOF

admit="$(awk -F= '$1 == "ticket_admit" {print $2}' "$STATS_PATH")"
ready="$(awk -F= '$1 == "ticket_ready" {print $2}' "$STATS_PATH")"
complete="$(awk -F= '$1 == "ticket_complete" {print $2}' "$STATS_PATH")"
pending_peak="$(awk -F= '$1 == "ticket_pending_peak" {print $2}' "$STATS_PATH")"
dependency_waits="$(awk -F= '$1 == "ticket_dependency_waits" {print $2}' "$STATS_PATH")"
submit="$(awk -F= '$1 == "spdk_submit" {print $2}' "$STATS_PATH")"
completed="$(awk -F= '$1 == "spdk_complete" {print $2}' "$STATS_PATH")"
errors="$(awk -F= '$1 == "errors" {print $2}' "$STATS_PATH")"

[[ "${admit:-0}" == "${complete:-0}" ]]
[[ "${admit:-0}" == "${ready:-0}" ]]
[[ "${submit:-0}" == "${completed:-0}" ]]
[[ "${errors:-0}" == "0" ]]
echo "ticket_admit=$admit ticket_ready=$ready ticket_complete=$complete ticket_pending_peak=$pending_peak dependency_waits=$dependency_waits spdk_submit=$submit spdk_complete=$completed errors=$errors"
echo "POSIX RANGE TICKET TEST PASSED"
