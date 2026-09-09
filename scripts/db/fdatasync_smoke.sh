#!/usr/bin/env bash
# Verify fdatasync data-only completion and same-inode durability coalescing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-fdatasync.img}"
RUN_DIR="${4:-/var/tmp/hfs-fdatasync}"
LOCK_PATH="$RUN_DIR/hfs.lock"
STATS_PATH="$RUN_DIR/stats"

mkdir -p "$RUN_DIR"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G
rm -f "$STATS_PATH"

env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
	HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 \
	HFS_STATS_PATH="$STATS_PATH" python3 - <<'PYEOF'
import os
import threading

path = "/hfs/fdatasync-smoke"
fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.write(fd, b"A" * 4096) == 4096
os.fdatasync(fd)
assert os.pread(fd, 4096, 0) == b"A" * 4096

# A same-size overwrite must avoid the metadata transaction in fdatasync.
assert os.pwrite(fd, b"B" * 4096, 0) == 4096
os.fdatasync(fd)

# Concurrent calls on duplicated descriptions share one durability epoch.
fds = [os.dup(fd) for _ in range(8)]
barrier = threading.Barrier(len(fds))
errors = []

def flush(value):
    try:
        barrier.wait()
        os.fdatasync(value)
    except BaseException as exc:
        errors.append(exc)

threads = [threading.Thread(target=flush, args=(value,)) for value in fds]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
assert not errors, errors
for value in fds:
    os.close(value)
os.close(fd)
print("FDATASYNC COORDINATOR OK")
PYEOF

grep -q '^fdatasync_fast=[1-9]' "$STATS_PATH"
grep -q '^durability_leader=[1-9]' "$STATS_PATH"
echo "FDATASYNC STATS OK"
