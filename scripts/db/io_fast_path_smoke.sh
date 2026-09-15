#!/usr/bin/env bash
# Verify request-local I/O, vector operations, sparse reads, and fallocate modes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-shard31.img}"
RUN_DIR="${4:-/var/tmp/hfs-shard31}"
LOCK_PATH="$RUN_DIR/hfs.lock"
BDEV_NAME="${HFS_BDEV_NAME:-Aio0}"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"
if [[ "$BDEV_NAME" == "Aio0" ]]; then
	EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
	sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
		"$CONFIG" > "$EFFECTIVE_CONFIG"
	HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G
else
	# Use the supplied real-device configuration without creating a file-backed bdev.
	EFFECTIVE_CONFIG="$CONFIG"
fi

env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
	HFS_BDEV_NAME="$BDEV_NAME" HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 \
	python3 - <<'PYEOF'
import ctypes
import errno
import os
import threading

root = "/hfs/shard31"
unit = 4096
os.mkdir(root)

fd = os.open(root + "/vector.dat", os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
original = b"A" * (unit * 4)
assert os.pwrite(fd, original, 0) == len(original)

# A partial write must preserve both neighboring regions.
assert os.pwrite(fd, b"edge", 3) == 4
check = os.pread(fd, len(original), 0)
assert check[:3] == b"AAA" and check[3:7] == b"edge" and check[7:] == b"A" * (len(original) - 7)

# Aligned writes take the direct path, while pwritev stays allocation-free.
aligned = b"B" * (unit * 2)
assert os.pwrite(fd, aligned, unit * 2) == len(aligned)
assert os.pwritev(fd, [b"left", b"-right"], unit * 4) == 10
first = bytearray(4)
second = bytearray(6)
assert os.preadv(fd, [first, second], unit * 4) == 10
assert bytes(first) + bytes(second) == b"left-right"
os.close(fd)

# Sequential writev/readv use the open-file offset lock and no big aggregate buffer.
fd = os.open(root + "/sequential.dat", os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.writev(fd, [b"one", b"-", b"two"]) == 7
os.lseek(fd, 0, os.SEEK_SET)
part1 = bytearray(3)
part2 = bytearray(4)
assert os.readv(fd, [part1, part2]) == 7
assert bytes(part1) + bytes(part2) == b"one-two"
os.close(fd)

# A write beyond EOF leaves a zero-filled sparse gap.
fd = os.open(root + "/sparse.dat", os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
written = os.pwrite(fd, b"tail", unit * 3 + 17)
if written != 4:
    raise RuntimeError(("sparse pwrite", written, ctypes.get_errno()))
sparseData = os.pread(fd, unit * 3 + 21, 0)
expectedSparse = b"\0" * (unit * 3 + 17) + b"tail"
if sparseData != expectedSparse:
    raise RuntimeError(("sparse read", len(sparseData), len(expectedSparse),
                        sparseData[:32], sparseData[-32:]))
os.close(fd)

# KEEP_SIZE reserves storage without changing the logical size.
fd = os.open(root + "/keep-size.dat", os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.write(fd, b"x") == 1
libc = ctypes.CDLL(None, use_errno=True)
libc.fallocate.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_longlong, ctypes.c_longlong]
libc.fallocate.restype = ctypes.c_int
keep_size = 0x01
if libc.fallocate(fd, keep_size, unit * 2, unit) != 0:
    raise RuntimeError(("keep-size fallocate", ctypes.get_errno()))
if os.fstat(fd).st_size != 1:
    raise RuntimeError(("keep-size size", os.fstat(fd).st_size))
if os.pwrite(fd, b"z", unit * 2) != 1:
    raise RuntimeError(("keep-size pwrite", ctypes.get_errno()))
if os.fstat(fd).st_size != unit * 2 + 1:
    raise RuntimeError(("keep-size growth", os.fstat(fd).st_size))
keepData = os.pread(fd, unit * 2 + 1, 0)
expectedKeep = b"x" + b"\0" * (unit * 2 - 1) + b"z"
if keepData != expectedKeep:
    raise RuntimeError(("keep-size read", len(keepData), len(expectedKeep),
                        keepData[:32], keepData[-32:]))
os.close(fd)

# PUNCH_HOLE preserves file size and zeros only the requested aligned range.
fd = os.open(root + "/punch.dat", os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.write(fd, b"Q" * (unit * 4)) == unit * 4
punch_hole = 0x02
if libc.fallocate(fd, punch_hole | keep_size, unit, unit * 2) != 0:
    raise RuntimeError(("punch fallocate", ctypes.get_errno()))
if os.fstat(fd).st_size != unit * 4:
    raise RuntimeError(("punch size", os.fstat(fd).st_size))
punched = os.pread(fd, unit * 4, 0)
expectedPunch = b"Q" * unit + b"\0" * (unit * 2) + b"Q" * unit
if punched != expectedPunch:
    raise RuntimeError(("punch read", len(punched), len(expectedPunch),
                        punched[:32], punched[-32:]))
os.close(fd)

# A punch fully inside one unit must preserve both outer edges.
fd = os.open(root + "/punch-edge.dat", os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.write(fd, b"R" * (unit * 2)) == unit * 2
edge_offset = 17
edge_length = 23
if libc.fallocate(fd, punch_hole | keep_size, edge_offset, edge_length) != 0:
    raise RuntimeError(("punch edge fallocate", ctypes.get_errno()))
edgeData = os.pread(fd, unit * 2, 0)
expectedEdge = b"R" * edge_offset + b"\0" * edge_length + b"R" * (unit * 2 - edge_offset - edge_length)
if edgeData != expectedEdge:
    raise RuntimeError(("punch edge read", len(edgeData), len(expectedEdge),
                        edgeData[:64], edgeData[-64:]))
os.close(fd)

# Positioned writes on one description can run concurrently at disjoint ranges.
fd = os.open(root + "/parallel.dat", os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
errors = []

def writer(index):
    try:
        payload = bytes([65 + index]) * unit
        assert os.pwrite(fd, payload, index * unit) == unit
    except BaseException as exc:
        errors.append((index, repr(exc)))

threads = [threading.Thread(target=writer, args=(index,)) for index in range(8)]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
assert not errors, errors
parallel = os.pread(fd, unit * 8, 0)
for index in range(8):
    assert parallel[index * unit:(index + 1) * unit] == bytes([65 + index]) * unit
os.fsync(fd)
os.close(fd)

# A fork child must not reuse the parent's SPDK runtime or HFS locks.
pid = os.fork()
if pid == 0:
    try:
        try:
            child_fd = os.open(root + "/fork-child.dat", os.O_CREAT | os.O_RDWR, 0o600)
        except OSError as exc:
            os._exit(0 if exc.errno in (errno.ENOENT, errno.ECHILD) else 2)
        os.close(child_fd)
        os._exit(3)
    except BaseException:
        os._exit(4)
_, child_status = os.waitpid(pid, 0)
if child_status != 0:
    raise RuntimeError(("fork child", child_status))

print("REQUEST-LOCAL FAST IO OK")
PYEOF

echo "== request-local fast I/O smoke passed =="
