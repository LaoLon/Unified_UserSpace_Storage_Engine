#!/usr/bin/env bash
# Exercise aligned non-overlapping I/O with progressively larger worker windows.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-io-concurrency-matrix.img}"
RUN_DIR="${4:-/var/tmp/hfs-io-concurrency-matrix}"
LOCK_PATH="$RUN_DIR/hfs.lock"
STATS_PATH="$RUN_DIR/io-concurrency.stats"

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
	HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 HFS_DATA_REACTORS=4 \
	HFS_DATA_STRIPE_KIB=64 HFS_IO_PIPELINE_DEPTH=8 HFS_RUNTIME_BATCH=1 \
python3 <<'PYEOF'
from concurrent.futures import ThreadPoolExecutor
import os

root = "/hfs/io-concurrency-matrix"
os.mkdir(root)
fd = os.open(f"{root}/data", os.O_CREAT | os.O_RDWR, 0o600)
unit = 64 * 1024
slots = 64
os.ftruncate(fd, unit * slots)

for workers in (1, 2, 4, 8, 16, 32):
    def round_trip(index):
        offset = index * unit
        value = (workers + index) % 251 + 1
        payload = bytes([value]) * unit
        assert os.pwrite(fd, payload, offset) == unit
        assert os.pread(fd, unit, offset) == payload
        return index

    with ThreadPoolExecutor(max_workers=workers) as executor:
        completed = list(executor.map(round_trip, range(slots)))
    assert completed == list(range(slots)), (workers, completed)
    print(f"threads={workers} requests={len(completed)}")

os.close(fd)
print("IO CONCURRENCY MATRIX OK")
PYEOF

total_submit=0
total_complete=0
for key in "$STATS_PATH"; do
	while IFS='=' read -r name value; do
		case "$name" in
			data_reactor_*_submit) total_submit=$((total_submit + value)) ;;
			data_reactor_*_complete) total_complete=$((total_complete + value)) ;;
		esac
	done < "$key"
done
if (( total_submit == 0 || total_submit != total_complete )); then
	echo "data reactor accounting mismatch: submit=$total_submit complete=$total_complete" >&2
	exit 1
fi
echo "IO CONCURRENCY ACCOUNTING OK submit=$total_submit complete=$total_complete"
