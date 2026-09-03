#!/usr/bin/env bash
# Verify metadata intent recovery at each persistence boundary.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config]" >&2
	exit 2
fi

for step in 1 2 3; do
	IMAGE="/var/tmp/hfs-shard30-${step}.img"
	RUN_DIR="/var/tmp/hfs-shard30-${step}"
	LOCK_PATH="$RUN_DIR/hfs.lock"
	EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"

	mkdir -p "$RUN_DIR"
	sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
		"$CONFIG" > "$EFFECTIVE_CONFIG"
	HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

	run_hfs() {
		env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
			HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" "$@"
	}

	# Establish a committed base image before injecting a crash.
	run_hfs HFS_FORMAT=1 python3 - <<'PYEOF'
import os

fd = os.open("/hfs/base", os.O_CREAT | os.O_RDWR, 0o600)
assert os.write(fd, b"base") == 4
os.fsync(fd)
os.close(fd)
PYEOF

	# Crash after the selected transaction boundary and require its exit code.
	set +e
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" \
		HFS_META_CRASH_STEP="$step" python3 - <<'PYEOF'
import os

fd = os.open("/hfs/txn", os.O_CREAT | os.O_RDWR, 0o600)
os.close(fd)
PYEOF
	crash_rc=$?
	set -e
	if [[ "$crash_rc" -ne $((200 + step)) ]]; then
		echo "metadata intent crash step $step returned $crash_rc" >&2
		exit 1
	fi

	# Mount again and verify that the create transaction is replayed.
	run_hfs python3 - <<PYEOF
import os

assert os.stat("/hfs/base").st_size == 4
assert os.stat("/hfs/txn").st_size == 0
PYEOF

	# Prepare source-last overwrite so recovery exercises inode compaction.
	run_hfs python3 - <<'PYEOF'
import os

for path, data in (("/hfs/dst", b"old"), ("/hfs/src", b"new")):
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    os.write(fd, data)
    os.fsync(fd)
    os.close(fd)
PYEOF

	# Crash through each rename boundary while replacing the destination.
	set +e
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" \
		HFS_META_CRASH_STEP="$step" python3 - <<'PYEOF'
import os

os.rename("/hfs/src", "/hfs/dst")
PYEOF
	crash_rc=$?
	set -e
	if [[ "$crash_rc" -ne $((200 + step)) ]]; then
		echo "rename intent crash step $step returned $crash_rc" >&2
		exit 1
	fi

	# The source inode must occupy the destination name after replay.
	run_hfs python3 - <<'PYEOF'
import os

fd = os.open("/hfs/dst", os.O_RDONLY)
assert os.read(fd, 16) == b"new"
os.close(fd)
try:
    os.stat("/hfs/src")
except FileNotFoundError:
    pass
else:
    raise AssertionError("source name survived rename recovery")
PYEOF

	# Repeat the same crash boundary for a file-size after-image.
	set +e
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" \
		HFS_META_CRASH_STEP="$step" python3 - <<'PYEOF'
import os

fd = os.open("/hfs/txn", os.O_RDWR)
assert os.write(fd, b"size") == 4
os.fsync(fd)
PYEOF
	crash_rc=$?
	set -e
	if [[ "$crash_rc" -ne $((200 + step)) ]]; then
		echo "size intent crash step $step returned $crash_rc" >&2
		exit 1
	fi

	# Both the inode size and data must survive recovery.
	run_hfs python3 - <<'PYEOF'
import os

fd = os.open("/hfs/txn", os.O_RDONLY)
data = os.read(fd, 16)
size = os.fstat(fd).st_size
assert data == b"size", (data, size)
assert size == 4, (data, size)
os.close(fd)
PYEOF
done

echo "METADATA TRANSACTION RECOVERY OK"
