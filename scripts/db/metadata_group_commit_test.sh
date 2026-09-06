#!/usr/bin/env bash
# Verify that one fsync durably checkpoints other dirty inodes in its epoch.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-metadata-group.img}"
RUN_DIR="${4:-/var/tmp/hfs-metadata-group}"
LOCK_PATH="$RUN_DIR/hfs.lock"
STATS_PATH="$RUN_DIR/stats.txt"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

run_hfs() {
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" "$@"
}

# Establish names and a durable baseline before creating dirty peers.
run_hfs HFS_FORMAT=1 python3 - <<'PYEOF'
import os

for suffix in "abcdefg":
    path = f"/hfs/group-{suffix}"
    fd = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o600)
    os.fsync(fd)
    os.close(fd)
PYEOF

rm -f "$STATS_PATH"
run_hfs HFS_STATS_PATH="$STATS_PATH" python3 - <<'PYEOF'
import os

fd_a = os.open("/hfs/group-a", os.O_RDWR)
fd_b = os.open("/hfs/group-b", os.O_RDWR)
assert os.write(fd_b, b"dirty-b") == 7
assert os.write(fd_a, b"durable-a") == 9
# The A epoch must include B's dirty inode as well.
os.fsync(fd_a)
os.close(fd_b)
os.close(fd_a)
PYEOF

# Exercise the v4 four-slot overflow fallback with five additional dirty files.
run_hfs HFS_STATS_PATH="$STATS_PATH" python3 - <<'PYEOF'
import os

fds = []
for suffix in "cdefg":
    fd = os.open(f"/hfs/group-{suffix}", os.O_RDWR)
    assert os.write(fd, f"dirty-{suffix}".encode()) == 7
    fds.append(fd)
os.fsync(fds[0])
for fd in fds:
    os.close(fd)
PYEOF

run_hfs python3 - <<'PYEOF'
import os

for suffix in "bcdefg":
    fd = os.open(f"/hfs/group-{suffix}", os.O_RDONLY)
    expected = f"dirty-{suffix}".encode()
    assert os.read(fd, 32) == expected, (suffix, expected)
    os.close(fd)
PYEOF

batch_max="$(awk -F= '$1 == "metadata_batch_max" {print $2}' "$STATS_PATH")"
metadata_syncs="$(awk -F= '$1 == "metadata_sync_count" {print $2}' "$STATS_PATH")"
if [[ -z "$batch_max" || "$batch_max" -lt 2 || -z "$metadata_syncs" ||
	"$metadata_syncs" -le 0 ]]; then
	echo "metadata group commit was not observed: batch_max=$batch_max metadata_sync_count=$metadata_syncs" >&2
	exit 1
fi

echo "METADATA GROUP COMMIT OK batch_max=$batch_max metadata_sync_count=$metadata_syncs"
