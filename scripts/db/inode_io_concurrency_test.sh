#!/usr/bin/env bash
# Verify per-inode synchronization and hashed range locking under mixed I/O.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-shard41-inode.img}"
RUN_DIR="${4:-/var/tmp/hfs-shard41-inode}"
THREADS="${HFS_INODE_THREADS:-16}"
LOCK_PATH="$RUN_DIR/hfs.lock"

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
	HFS_STATS_PATH="$RUN_DIR/stats" HFS_STATS_TIMING=1 \
	HFS_INODE_THREADS="$THREADS" python3 - <<'PYEOF'
import errno
import ctypes
import os
import threading

root = "/hfs/shard41"
unit = 4096
threads = int(os.environ["HFS_INODE_THREADS"])
os.mkdir(root)

def open_file(name):
    return os.open(root + "/" + name, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)

# Different inodes must make progress independently and retain their patterns.
fds = [open_file("independent-%02d" % i) for i in range(threads)]
errors = []
barrier = threading.Barrier(threads)

def independent_writer(index):
    try:
        barrier.wait()
        payload = bytes([0x20 + index]) * unit
        for round_id in range(8):
            offset = round_id * unit
            assert os.pwrite(fds[index], payload, offset) == unit
        assert os.pread(fds[index], unit, 7 * unit) == payload
    except BaseException as exc:
        errors.append(("independent", index, repr(exc)))

workers = [threading.Thread(target=independent_writer, args=(i,)) for i in range(threads)]
for worker in workers:
    worker.start()
for worker in workers:
    worker.join()
assert not errors, errors
for fd in fds:
    os.close(fd)

# Non-overlapping aligned writes on one inode must not corrupt each other.
fd = open_file("ranges")
errors = []
barrier = threading.Barrier(threads)

def range_writer(index):
    try:
        barrier.wait()
        payload = bytes([0x80 + index]) * unit
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
    assert data[index * unit:(index + 1) * unit] == bytes([0x80 + index]) * unit

# Same-unit RMW writers are serialized by the hashed range lock.
for _ in range(20):
    os.pwrite(fd, b"A" * unit, 0)
    errors = []
    barrier = threading.Barrier(threads)

    def rmw_writer(index):
        try:
            barrier.wait()
            payload = bytes([65 + (index % 26)]) * 17
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
    assert result[:31] == b"A" * 31
    assert result[48:] == b"A" * (unit - 48)
os.close(fd)

# O_APPEND is inode-local; independent append fds must not overlap records.
append_path = root + "/append"
append_fds = [os.open(append_path, os.O_CREAT | os.O_RDWR | os.O_APPEND, 0o600)
              for _ in range(threads)]
errors = []
barrier = threading.Barrier(threads)

def append_writer(index):
    try:
        barrier.wait()
        record = ("%02d" % index).encode() + b"\n"
        for _ in range(16):
            assert os.write(append_fds[index], record) == len(record)
    except BaseException as exc:
        errors.append(("append", index, repr(exc)))

workers = [threading.Thread(target=append_writer, args=(i,)) for i in range(threads)]
for worker in workers:
    worker.start()
for worker in workers:
    worker.join()
assert not errors, errors
for append_fd in append_fds:
    os.close(append_fd)
check = os.open(append_path, os.O_RDONLY)
records = os.read(check, threads * 16 * 3)
os.close(check)
assert len(records) == threads * 16 * 3
assert all(len(line) == 2 and line.isdigit() for line in records.splitlines())

# punch+write and fsync+write use the inode exclusive sync barrier.
fd = os.open(root + "/barriers", os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.pwrite(fd, b"Q" * (unit * 4), 0) == unit * 4
libc = ctypes.CDLL(None, use_errno=True)
libc.fallocate.argtypes = [ctypes.c_int, ctypes.c_int,
                           ctypes.c_longlong, ctypes.c_longlong]
libc.fallocate.restype = ctypes.c_int
keep_size = 0x01
punch_hole = 0x02
errors = []
stop = threading.Event()

def barrier_writer():
    try:
        for _ in range(50):
            if stop.is_set():
                return
            assert os.pwrite(fd, b"W" * unit, unit) == unit
            os.fsync(fd)
    except BaseException as exc:
        errors.append(("barrier-write", repr(exc)))
        stop.set()

def puncher():
    try:
        for _ in range(50):
            if stop.is_set():
                return
            rc = libc.fallocate(fd, punch_hole | keep_size, unit, unit)
            assert rc == 0, ctypes.get_errno()
    except BaseException as exc:
        errors.append(("punch", repr(exc)))
        stop.set()

writer = threading.Thread(target=barrier_writer)
punch = threading.Thread(target=puncher)
writer.start()
punch.start()
writer.join()
punch.join()
assert not errors, errors
os.close(fd)

print("INODE IO CONCURRENCY OK")
PYEOF

echo "== per-inode/range I/O concurrency passed =="
