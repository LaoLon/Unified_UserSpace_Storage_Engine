#!/usr/bin/env bash
# Measure indexed metadata lookup and directory enumeration across inode counts.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-metadata-scaling.img}"
OUT="${4:-$ROOT/build/metadata_scaling}"
COUNTS_TEXT="${HFS_METADATA_COUNTS:-1000 10000 60000}"
TIMEOUT_SECONDS="${HFS_METADATA_TIMEOUT_SECONDS:-900}"
IMAGE_SIZE="${HFS_METADATA_IMAGE_SIZE:-128G}"
OBJECT_TYPE="${HFS_METADATA_OBJECT_TYPE:-dir}"
LOCK_PATH="$OUT/hfs.lock"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [output dir]" >&2
	exit 2
fi

mkdir -p "$OUT"
EFFECTIVE_CONFIG="$OUT/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
SUMMARY="$OUT/summary.tsv"
printf 'inodes\tcreate_p50_us\tcreate_p99_us\topen_p50_us\topen_p99_us\tstat_p50_us\tstat_p99_us\treaddir_p50_us\treaddir_p99_us\trename_p50_us\trename_p99_us\tunlink_p50_us\tunlink_p99_us\terrors\n' > "$SUMMARY"

for count in $COUNTS_TEXT; do
	if ! [[ "$count" =~ ^[1-9][0-9]*$ ]] || (( count > 65535 )); then
		echo "invalid inode count: $count" >&2
		exit 2
	fi
	if [[ "$OBJECT_TYPE" != "dir" && "$OBJECT_TYPE" != "file" ]]; then
		echo "invalid metadata object type: $OBJECT_TYPE" >&2
		exit 2
	fi
	tag="meta-${count}"
	run_dir="$OUT/$tag"
	mkdir -p "$run_dir"
	EFFECTIVE_RUN_CONFIG="$run_dir/hfs_db_aio.json"
	sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}.${count}\"#" \
		"$CONFIG" > "$EFFECTIVE_RUN_CONFIG"
	HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "${IMAGE}.${count}" "$IMAGE_SIZE"
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_RUN_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 \
		HFS_METADATA_OBJECT_TYPE="$OBJECT_TYPE" \
		HFS_STATS_PATH="$run_dir/stats" HFS_STATS_TIMING=1 \
	timeout --signal=TERM --kill-after=10s "${TIMEOUT_SECONDS}s" \
	python3 - "$count" "$SUMMARY" <<'PYEOF'
import os
import sys
import time

count = int(sys.argv[1])
object_type = os.environ.get("HFS_METADATA_OBJECT_TYPE", "dir")
summary_path = sys.argv[2]
root = "/hfs/meta-scaling"
samples = {key: [] for key in ("create", "open", "stat", "readdir", "rename", "unlink")}
errors = 0

def measure(name, fn):
    start = time.perf_counter_ns()
    try:
        fn()
    except BaseException:
        global errors
        errors += 1
    samples[name].append((time.perf_counter_ns() - start) / 1000.0)

os.mkdir(root)
for index in range(count):
    path = "%s/f-%05d" % (root, index)
    def create(path=path):
        if object_type == "dir":
            os.mkdir(path, 0o700)
        else:
            fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
            os.close(fd)
    measure("create", create)

for index in range(count):
    path = "%s/f-%05d" % (root, index)
    def open_one(path=path):
        flags = os.O_RDONLY | (os.O_DIRECTORY if object_type == "dir" else 0)
        fd = os.open(path, flags)
        os.close(fd)
    measure("open", open_one)
    measure("stat", lambda path=path: os.stat(path))

def readdir_once():
    with os.scandir(root) as entries:
        seen = sum(1 for entry in entries)
    if seen != count:
        raise RuntimeError("readdir count mismatch: %d != %d" % (seen, count))
measure("readdir", readdir_once)

rename_count = min(count, 1000)
for index in range(rename_count):
    old = "%s/f-%05d" % (root, index)
    new = "%s/r-%05d" % (root, index)
    measure("rename", lambda old=old, new=new: os.rename(old, new))
for index in range(rename_count):
    path = "%s/r-%05d" % (root, index)
    if object_type == "dir":
        measure("unlink", lambda path=path: os.rmdir(path))
    else:
        measure("unlink", lambda path=path: os.unlink(path))

def percentile(values, percent):
    if not values:
        return 0.0
    values = sorted(values)
    position = (len(values) - 1) * percent
    lower = int(position)
    upper = min(lower + 1, len(values) - 1)
    return values[lower] + (values[upper] - values[lower]) * (position - lower)

fields = [str(count)]
for key in ("create", "open", "stat", "readdir", "rename", "unlink"):
    values = samples[key]
    fields.extend(["%.2f" % percentile(values, 0.50),
                   "%.2f" % percentile(values, 0.99)])
fields.append(str(errors))
with open(summary_path, "a", encoding="utf-8") as output:
    output.write("\t".join(fields) + "\n")
print("METADATA SCALING %d OK errors=%d" % (count, errors))
if errors:
    raise SystemExit(1)
PYEOF
done

cat "$SUMMARY"
echo "METADATA SCALING BENCHMARK OK"
