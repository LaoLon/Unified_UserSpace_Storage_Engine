#!/usr/bin/env bash
# Verify recovery after a process stops at each ordered fsync durability boundary.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-durability-crash.img}"
RUN_DIR="${4:-/var/tmp/hfs-durability-crash}"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"

run_step() {
	local step="$1"
	local step_image="${IMAGE}.${step}"
	local step_dir="$RUN_DIR/step-${step}"
	local config="$step_dir/hfs_db_aio.json"
	local lock_path="$step_dir/hfs.lock"
	local rc

	mkdir -p "$step_dir"
	sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${step_image}\"#" \
		"$CONFIG" > "$config"
	HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$step_image" 1G

	# Establish a fully durable file before testing the new write's fsync.
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$config" HFS_BDEV_NAME=Aio0 \
		HFS_LOCK_PATH="$lock_path" HFS_FORMAT=1 python3 - <<'PYEOF'
import os

root = "/hfs/durability-crash"
os.mkdir(root)
fd = os.open(root + "/data", os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
assert os.pwrite(fd, b"seed", 0) == 4
os.fsync(fd)
os.close(fd)
print("DURABILITY CRASH SEED OK")
PYEOF

	set +e
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$config" HFS_BDEV_NAME=Aio0 \
		HFS_LOCK_PATH="$lock_path" HFS_FSYNC_CRASH_STEP="$step" python3 - <<'PYEOF'
import os

fd = os.open("/hfs/durability-crash/data", os.O_RDWR)
assert os.pwrite(fd, b"next", 4096) == 4
os.fsync(fd)
raise AssertionError("fsync returned after injected crash point")
PYEOF
	rc=$?
	set -e
	if [[ "$rc" -ne $((210 + step)) ]]; then
		echo "unexpected fsync crash exit for step ${step}: ${rc}" >&2
		exit 1
	fi

	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$config" HFS_BDEV_NAME=Aio0 \
		HFS_LOCK_PATH="$lock_path" STEP="$step" python3 - <<'PYEOF'
import os

step = int(os.environ["STEP"])
fd = os.open("/hfs/durability-crash/data", os.O_RDONLY)
size = os.fstat(fd).st_size
prefix = os.pread(fd, 4, 0)
suffix = os.pread(fd, 4, 4096)
os.close(fd)
assert prefix == b"seed", (step, prefix)

# The operation never returned. Before the last barrier either image is legal,
# but after that barrier the extended inode and payload must both survive.
if step == 3:
    assert size == 4100 and suffix == b"next", (step, size, suffix)
else:
    assert (size, suffix) in ((4, b""), (4100, b"next")), \
        (step, size, suffix)
print("DURABILITY CRASH RECOVERY STEP {} OK".format(step))
PYEOF
}

for step in 1 2 3; do
	run_step "$step"
done

echo "== durability crash recovery test passed =="
