#!/usr/bin/env bash
# Verify that two HFS processes can reuse one persistent blobstore.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-db.img}"
RUN_DIR="${4:-/var/tmp/hfs-db-stage}"
LOCK_PATH="$RUN_DIR/hfs.lock"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"
EFFECTIVE_CONFIG="$CONFIG"
if [[ "$IMAGE" != "/var/tmp/hfs-db.img" ]]; then
	EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
	sed "s#/var/tmp/hfs-db.img#${IMAGE}#g" "$CONFIG" > "$EFFECTIVE_CONFIG"
fi
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

run_hfs() {
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" HFS_BDEV_NAME=Aio0 \
		HFS_LOCK_PATH="$LOCK_PATH" "$@"
}

echo "-- process 0: reject an unformatted backend --"
if run_hfs python3 -c 'import os; os.open("/hfs/unformatted.txt", os.O_CREAT | os.O_RDWR, 0o600)'; then
	echo "ERROR: unformatted backend unexpectedly loaded" >&2
	exit 1
fi

echo "-- process A: explicit format and write --"
run_hfs HFS_FORMAT=1 python3 - <<'PYEOF'
import os

fd = os.open("/hfs/persist.txt", os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o644)
payload = b"persistent-hfs-data"
assert os.write(fd, payload) == len(payload)
os.fsync(fd)
os.close(fd)
PYEOF

echo "-- process B: load and read --"
run_hfs python3 - <<'PYEOF'
import os

fd = os.open("/hfs/persist.txt", os.O_RDONLY)
assert os.read(fd, 128) == b"persistent-hfs-data"
os.close(fd)
PYEOF

echo "-- process C: backend lock --"
run_hfs python3 -c 'import os, time; os.stat("/hfs/persist.txt"); time.sleep(5)' &
HOLDER_PID=$!
sleep 1
if run_hfs python3 -c 'import os; os.stat("/hfs/persist.txt")'; then
	echo "ERROR: second backend mount unexpectedly succeeded" >&2
	kill "$HOLDER_PID" 2>/dev/null || true
	wait "$HOLDER_PID" 2>/dev/null || true
	exit 1
fi
wait "$HOLDER_PID"

echo "PERSISTENT SMOKE OK"
