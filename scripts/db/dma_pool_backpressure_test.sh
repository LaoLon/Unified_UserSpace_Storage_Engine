#!/usr/bin/env bash
# Exercise per-reactor DMA classes, bounded pool counts, and completion accounting.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-dma-pool.img}"
RUN_DIR="${4:-/var/tmp/hfs-dma-pool}"
REACTORS="${HFS_TEST_REACTORS:-4}"
LOCK_PATH="$RUN_DIR/hfs.lock"
STATS_PATH="$RUN_DIR/dma-pool.stats"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi
mkdir -p "$RUN_DIR"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

env LD_PRELOAD="$LIB" HFS_LIBHFS_PATH="$LIB" HFS_STATS_PATH="$STATS_PATH" \
	HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" HFS_BDEV_NAME=Aio0 \
	HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 HFS_DATA_REACTORS="$REACTORS" \
	HFS_DATA_STRIPE_KIB=64 HFS_IO_POOL_CLASS_COUNTS=2,2,1,1 \
	HFS_IO_POOL_BYTES=2M HFS_IO_PIPELINE_DEPTH=8 HFS_RUNTIME_BATCH=1 \
python3 - "$REACTORS" <<'PYEOF'
from concurrent.futures import ThreadPoolExecutor
import os
import sys

reactors = int(sys.argv[1])
root = "/hfs/dma-pool"
os.mkdir(root)
fd = os.open(f"{root}/data", os.O_CREAT | os.O_RDWR, 0o600)
classes = [4 * 1024, 16 * 1024, 64 * 1024, 1024 * 1024]
stripe = 2 * 1024 * 1024
count = 64
os.ftruncate(fd, count * stripe)

def write_one(index):
    size = classes[index % len(classes)]
    offset = index * stripe
    payload = bytes([(index % 239) + 1]) * size
    return os.pwrite(fd, payload, offset), size, offset, payload[:1]

with ThreadPoolExecutor(max_workers=32) as executor:
    results = list(executor.map(write_one, range(count)))
for written, size, _, _ in results:
    assert written == size, (written, size)
for index, (_, size, offset, marker) in enumerate(results):
    data = os.pread(fd, size, offset)
    assert data == marker * size, index
os.fsync(fd)
os.close(fd)
print(f"DMA POOL BACKPRESSURE IO OK reactors={reactors} requests={count}")
PYEOF

for key in dma_acquire dma_release dma_active_peak dma_wait_ns dma_exhaust; do
	value="$(awk -F= -v key="$key" '$1 == key {print $2}' "$STATS_PATH")"
	echo "$key=${value:-0}"
done
acquire="$(awk -F= '$1 == "dma_acquire" {print $2}' "$STATS_PATH")"
release="$(awk -F= '$1 == "dma_release" {print $2}' "$STATS_PATH")"
if [[ "${acquire:-0}" != "${release:-0}" ]]; then
	echo "DMA lease accounting mismatch" >&2
	exit 1
fi
echo "DMA POOL ACCOUNTING OK acquire=${acquire:-0} release=${release:-0}"
