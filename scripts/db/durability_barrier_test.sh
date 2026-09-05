#!/usr/bin/env bash
# Verify that optional trace events preserve the fsync durability order.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-durability-barrier.img}"
RUN_DIR="${4:-/var/tmp/hfs-durability-barrier}"
LOCK_PATH="$RUN_DIR/hfs.lock"
TRACE_PATH="$RUN_DIR/trace.txt"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"
rm -f "$TRACE_PATH"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
	HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 \
	HFS_DURABILITY_TRACE_PATH="$TRACE_PATH" python3 - <<'PYEOF'
import os

root = "/hfs/durability-barrier"
os.mkdir(root)
fd = os.open(root + "/data", os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
assert os.pwrite(fd, b"first", 0) == 5
os.fsync(fd)
assert os.pwrite(fd, b"second", 5) == 6
os.fdatasync(fd)
os.close(fd)
print("DURABILITY BARRIER I/O OK")
PYEOF

python3 - "$TRACE_PATH" <<'PYEOF'
import sys

trace_path = sys.argv[1]
events = []
with open(trace_path, encoding="utf-8") as trace:
    for line in trace:
        fields = dict(field.split("=", 1) for field in line.split())
        events.append((int(fields["sequence"]), fields["phase"],
                       int(fields["result"])))

assert events, "durability trace was not written"
assert events == sorted(events), events
expected = ["data-barrier", "metadata-commit", "post-metadata-barrier"]
phase_index = 0
completed = 0
for _, phase, result in events:
    if result == 0 and phase == expected[phase_index]:
        phase_index += 1
        if phase_index == len(expected):
            completed += 1
            phase_index = 0
assert completed >= 2, (completed, events)
print("DURABILITY BARRIER TRACE OK")
PYEOF

echo "== durability barrier test passed =="
