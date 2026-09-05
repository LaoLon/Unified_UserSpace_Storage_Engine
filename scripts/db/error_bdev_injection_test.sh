#!/usr/bin/env bash
# Verify real SPDK error-bdev completions reach HFS POSIX callers unchanged.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
RUN_DIR="${3:-/var/tmp/hfs-error-bdev-injection}"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"

write_config() {
	local config_path="$1"
	local image_path="$2"
	local io_type="$3"

	python3 - "$config_path" "$image_path" "$io_type" <<'PYEOF'
import json
import sys

config_path, image_path, io_type = sys.argv[1:]
config = {
    "subsystems": [{
        "subsystem": "bdev",
        "config": [
            {"method": "bdev_aio_create", "params": {
                "filename": image_path, "name": "Aio0", "block_size": 4096,
            }},
            {"method": "bdev_error_create", "params": {"base_name": "Aio0"}},
            {"method": "bdev_error_inject_error", "params": {
                "name": "EE_Aio0", "io_type": io_type,
                "error_type": "failure", "num": 1,
            }},
        ],
    }],
}
with open(config_path, "w", encoding="utf-8") as output:
    json.dump(config, output)
PYEOF
}

run_hfs() {
	local config_path="$1"
	local lock_path="$2"
	shift 2
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$config_path" HFS_BDEV_NAME=EE_Aio0 \
		HFS_LOCK_PATH="$lock_path" "$@"
}

run_flush_case() {
	local image_path="$RUN_DIR/flush.img"
	local config_path="$RUN_DIR/flush.json"
	local lock_path="$RUN_DIR/flush.lock"

	HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$image_path" 1G
	write_config "$config_path" "$image_path" flush
	run_hfs "$config_path" "$lock_path" HFS_FORMAT=1 python3 - <<'PYEOF'
import errno
import os

root = "/hfs/error-bdev-flush"
os.mkdir(root)
fd = os.open(root + "/data", os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
assert os.write(fd, b"flush") == 5
try:
    os.fsync(fd)
except OSError as exc:
    assert exc.errno == errno.EIO, exc.errno
else:
    raise AssertionError("real error-bdev flush returned success")
assert os.pread(fd, 5, 0) == b"flush"
os.close(fd)
print("ERROR BDEV FLUSH OK")
PYEOF
}

run_flush_case
echo "== error-bdev injection passed =="
