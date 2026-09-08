#!/usr/bin/env bash
# Verify one blob spreads independent stripes while preserving same-stripe ordering.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-data-reactor-stripe.img}"
RUN_DIR="${4:-/var/tmp/hfs-data-reactor-stripe}"
REACTORS="${HFS_TEST_REACTORS:-4}"
LOCK_PATH="$RUN_DIR/hfs.lock"
STATS_PATH="$RUN_DIR/data-reactor-stripe.stats"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi
if [[ ! "$REACTORS" =~ ^[2-8]$ ]]; then
	echo "HFS_TEST_REACTORS must be between 2 and 8" >&2
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
	HFS_DATA_STRIPE_KIB=64 HFS_IO_PIPELINE_DEPTH=4 HFS_RUNTIME_BATCH=1 \
python3 - "$REACTORS" <<'PYEOF'
from concurrent.futures import ThreadPoolExecutor
import os
import sys

reactors = int(sys.argv[1])

root = "/hfs/data-reactor-stripe"
os.mkdir(root)
fd = os.open(f"{root}/data", os.O_CREAT | os.O_RDWR, 0o600)
stripe = 64 * 1024
stripe_count = int(os.environ.get("HFS_TEST_STRIPES", "128"))
os.ftruncate(fd, stripe * stripe_count)

# Queue one synchronous request per stripe in parallel so the selector can use all data reactors.
def write_stripe(index):
    payload = bytes([(index % 251) + 1]) * stripe
    return os.pwrite(fd, payload, index * stripe)

with ThreadPoolExecutor(max_workers=min(32, stripe_count)) as executor:
    results = list(executor.map(write_stripe, range(stripe_count)))
assert all(result == stripe for result in results), results

for index in range(stripe_count):
    expected = bytes([(index % 251) + 1]) * stripe
    assert os.pread(fd, stripe, index * stripe) == expected, index

# Same-stripe writes remain ordered for ordinary synchronous callers.
first = b"A" * stripe
second = b"B" * stripe
assert os.pwrite(fd, first, 0) == stripe
assert os.pwrite(fd, second, 0) == stripe
assert os.pread(fd, stripe, 0) == second

# Punching a stripe must not race a later write or expose stale data.
import ctypes
libc = ctypes.CDLL(None, use_errno=True)
libc.fallocate.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_longlong,
                           ctypes.c_longlong]
assert libc.fallocate(fd, 0x01 | 0x02, stripe, stripe) == 0
assert os.pread(fd, stripe, stripe) == bytes(stripe)
assert os.pwrite(fd, b"C" * stripe, stripe) == stripe
assert os.pread(fd, stripe, stripe) == b"C" * stripe
os.close(fd)
print(f"DATA REACTOR STRIPE IO OK reactors={reactors} stripes={stripe_count}")
PYEOF

total_submit=0
total_complete=0
active_reactors=0
for ((index=0; index<REACTORS; index++)); do
	submit="$(awk -F= -v key="data_reactor_${index}_submit" '$1 == key {print $2}' "$STATS_PATH")"
	complete="$(awk -F= -v key="data_reactor_${index}_complete" '$1 == key {print $2}' "$STATS_PATH")"
	submit="${submit:-0}"
	complete="${complete:-0}"
	echo "data_reactor_${index}: submit=${submit} complete=${complete}"
	if [[ "$submit" != "$complete" ]]; then
		echo "data reactor accounting mismatch" >&2
		exit 1
	fi
	if (( submit > 0 )); then
		active_reactors=$((active_reactors + 1))
	fi
	total_submit=$((total_submit + submit))
	total_complete=$((total_complete + complete))
done
if (( total_submit == 0 || total_submit != total_complete || active_reactors < 2 )); then
	echo "insufficient stripe distribution: active=${active_reactors} submit=${total_submit} complete=${total_complete}" >&2
	exit 1
fi
echo "DATA REACTOR STRIPE ACCOUNTING OK active=${active_reactors} submit=${total_submit} complete=${total_complete}"
