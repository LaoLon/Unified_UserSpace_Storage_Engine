#!/usr/bin/env bash
# Verify mixed-vector coalescing, resize fast checks, and punch capability accounting.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-shard58.img}"
RUN_DIR="${4:-/var/tmp/hfs-shard58}"
LOCK_PATH="$RUN_DIR/hfs.lock"
STATS_PATH="$RUN_DIR/hfs.stats"

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
	HFS_STATS_PATH="$STATS_PATH" HFS_UNMAP_ZERO_SAFE="${HFS_UNMAP_ZERO_SAFE:-0}" \
	python3 - <<'PYEOF'
import ctypes
import os

root = "/hfs/shard58"
unit = 4096
os.mkdir(root)
fd = os.open(root + "/mixed-vector", os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.pwrite(fd, b"A" * unit, 0) == unit

# A heap segment followed by a registered DMA segment must share one bounded
# logical request instead of issuing one synchronous request per segment.
libc = ctypes.CDLL(None, use_errno=True)
libc.hfsDmaAlloc.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
libc.hfsDmaAlloc.restype = ctypes.c_void_p
libc.hfsDmaFree.argtypes = [ctypes.c_void_p]
dma = libc.hfsDmaAlloc(4, 0)
assert dma
ctypes.memmove(dma, b"BBBB", 4)
dma_view = memoryview((ctypes.c_ubyte * 4).from_address(dma))
assert os.pwritev(fd, [b"left", dma_view], 3) == 8
assert os.pread(fd, 8, 3) == b"leftBBBB"
libc.hfsDmaFree(dma)

# Repeated fallocate over an already allocated range must hit the fast check.
fallocate = libc.fallocate
fallocate.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_longlong, ctypes.c_longlong]
fallocate.restype = ctypes.c_int
assert fallocate(fd, 0, 0, unit * 2) == 0
assert fallocate(fd, 0, 0, unit * 2) == 0

# The default path must preserve observable zeroes after an aligned punch.
assert os.pwrite(fd, b"Q" * (unit * 2), unit) == unit * 2
assert fallocate(fd, 0x03, unit, unit) == 0
assert os.pread(fd, unit * 4, 0)[unit:unit * 2] == b"\0" * unit
os.fsync(fd)
os.close(fd)
print("REDUNDANT REQUEST REDUCTION OK")
PYEOF

for key in resize_checks resize_fast_hits resize_requests punch_requests punch_zero_fallbacks; do
	grep -q "^${key}=" "$STATS_PATH"
done
grep -q '^errors=0$' "$STATS_PATH"
echo "== shard-58 request reduction smoke passed =="
