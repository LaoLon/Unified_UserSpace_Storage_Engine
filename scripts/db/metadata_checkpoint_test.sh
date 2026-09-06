#!/usr/bin/env bash
# Verify that v4 metadata commits use dirty byte ranges instead of full images.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-metadata-checkpoint.img}"
RUN_DIR="${4:-/var/tmp/hfs-metadata-checkpoint}"
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

# Format once without including the one-time full image in the measurement.
run_hfs HFS_FORMAT=1 python3 - <<'PYEOF'
import os

os.mkdir("/hfs/checkpoint")
PYEOF

rm -f "$STATS_PATH"
run_hfs HFS_STATS_PATH="$STATS_PATH" python3 - <<'PYEOF'
import os

root = "/hfs/checkpoint"
for index in range(256):
    os.mkdir(f"{root}/dir-{index:04d}")
for index in range(0, 256, 2):
    os.rename(f"{root}/dir-{index:04d}", f"{root}/renamed-{index:04d}")
for index in range(0, 256, 4):
    os.rmdir(f"{root}/renamed-{index:04d}")
PYEOF

if [[ ! -s "$STATS_PATH" ]]; then
	echo "metadata stats report was not created" >&2
	exit 1
fi

transactions="$(awk -F= '$1 == "metadata_transactions" {print $2}' "$STATS_PATH")"
image_bytes="$(awk -F= '$1 == "metadata_image_bytes" {print $2}' "$STATS_PATH")"
page_bytes="$(awk -F= '$1 == "metadata_page_bytes" {print $2}' "$STATS_PATH")"

if [[ -z "$transactions" || -z "$image_bytes" || -z "$page_bytes" ||
	"$transactions" -lt 256 || "$image_bytes" -ne 0 || "$page_bytes" -le 0 ]]; then
	echo "unexpected metadata checkpoint stats: transactions=$transactions image_bytes=$image_bytes page_bytes=$page_bytes" >&2
	exit 1
fi

echo "METADATA CHECKPOINT OK transactions=$transactions image_bytes=$image_bytes page_bytes=$page_bytes"
