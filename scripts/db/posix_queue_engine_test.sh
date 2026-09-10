#!/usr/bin/env bash
# Exercise the POSIX reactor queue with a small per-reactor credit window.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-posix-queue.img}"
RUN_DIR="${4:-/var/tmp/hfs-posix-queue}"
REACTORS="${HFS_TEST_REACTORS:-5}"
QUEUE_DEPTH="${HFS_TEST_QUEUE_DEPTH:-4}"
REQUESTS="${HFS_TEST_QUEUE_REQUESTS:-64}"
LOCK_PATH="$RUN_DIR/hfs.lock"
STATS_PATH="$RUN_DIR/posix-queue.stats"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [aio-config] [image] [run-dir]" >&2
	exit 2
fi
mkdir -p "$RUN_DIR"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

env LD_PRELOAD="$LIB" HFS_LIBHFS_PATH="$LIB" HFS_STATS_PATH="$STATS_PATH" \
	HFS_STATS_TIMING=1 HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" HFS_BDEV_NAME=Aio0 \
	HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 HFS_DATA_REACTORS="$REACTORS" \
	HFS_POSIX_QUEUE=1 HFS_POSIX_QUEUE_DEPTH="$QUEUE_DEPTH" HFS_DATA_STRIPE_KIB=64 \
	HFS_IO_POOL_CLASS_COUNTS=4,4,4,4 HFS_IO_POOL_BYTES=8M HFS_IO_PIPELINE_DEPTH=8 \
	HFS_RUNTIME_BATCH=1 \
python3 - "$REQUESTS" <<'PYEOF'
from concurrent.futures import ThreadPoolExecutor
import os
import sys

count = int(sys.argv[1])
root = "/hfs/posix-queue"
os.mkdir(root)
fd = os.open(f"{root}/data", os.O_CREAT | os.O_RDWR, 0o600)
stripe = 4 * 1024 * 1024
os.ftruncate(fd, count * stripe)

def run_one(index):
    size = 1024 * 1024
    payload = bytes([(index % 251) + 1]) * size
    offset = index * stripe
    assert os.pwrite(fd, payload, offset) == size
    assert os.pread(fd, size, offset) == payload
    return index

with ThreadPoolExecutor(max_workers=min(count, 64)) as executor:
    list(executor.map(run_one, range(count)))
os.close(fd)
print(f"POSIX QUEUE ENGINE IO OK reactors={os.environ.get('HFS_DATA_REACTORS')} requests={count}")
PYEOF

submitted="$(awk -F= '$1 == "spdk_submit" {print $2}' "$STATS_PATH")"
completed="$(awk -F= '$1 == "spdk_complete" {print $2}' "$STATS_PATH")"
errors="$(awk -F= '$1 == "errors" {print $2}' "$STATS_PATH")"
if [[ "$submitted" != "$completed" || "${errors:-0}" != "0" ]]; then
	echo "POSIX QUEUE ACCOUNTING FAILED submit=$submitted complete=$completed errors=$errors" >&2
	exit 1
fi
while IFS= read -r line; do
	depth="${line#*=}"
	if (( depth > QUEUE_DEPTH )); then
		echo "POSIX QUEUE CREDIT FAILED: $line limit=$QUEUE_DEPTH" >&2
		exit 1
	fi
done < <(grep '^queue_reactor_[0-9]\+_depth_peak=' "$STATS_PATH")

echo "POSIX QUEUE ACCOUNTING OK submit=$submitted complete=$completed depth_limit=$QUEUE_DEPTH"
grep -E 'queue_reactor_[0-9]+_(credit_waits|credit_wait_ns|depth_peak|drain_batches|drain_items)=' "$STATS_PATH"
