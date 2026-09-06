#!/usr/bin/env bash
# Measure per-inode range-lock scaling with a reproducible HFS-only matrix.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-inode-io-benchmark.img}"
OUT="${4:-$ROOT/build/inode_io_benchmark}"
DURATION="${HFS_INODE_BENCH_SECONDS:-5}"
SIZE="${HFS_INODE_BENCH_SIZE:-64m}"
THREADS_TEXT="${HFS_INODE_BENCH_THREADS:-1 2 4 8 16}"
LOCK_PATH="$OUT/hfs.lock"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [output dir]" >&2
	exit 2
fi
if ! command -v fio >/dev/null 2>&1 || [[ ! -x /usr/bin/time ]]; then
	echo "ERROR: fio and /usr/bin/time are required" >&2
	exit 2
fi

mkdir -p "$OUT"
EFFECTIVE_CONFIG="$OUT/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

SUMMARY="$OUT/summary.tsv"
printf 'threads\tiops\terror\tcpu_percent\tmax_rss_kb\truntime_queue_peak\tio_contention\tmeta_contention\tappend_contention\n' > "$SUMMARY"

for threads in $THREADS_TEXT; do
	tag="inode-io-${threads}t"
	json="$OUT/${tag}.json"
	stats="$OUT/${tag}.stats"
	timing="$OUT/${tag}.time"
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 \
		HFS_STATS_PATH="$stats" HFS_STATS_TIMING=1 \
	/usr/bin/time -o "$timing" -f 'cpu_percent=%P\nmax_rss_kb=%M' \
	timeout --signal=TERM --kill-after=5s 180s \
	fio --name="$tag" --ioengine=psync --thread=1 --numjobs="$threads" \
		--iodepth=1 --bs=4k --size="$SIZE" --rw=randrw \
		--filename=/hfs/inode-io.dat --direct=1 --allow_file_create=1 \
		--group_reporting=1 --time_based=1 --runtime="$DURATION" \
		--output="$json" --output-format=json

	python3 - "$threads" "$json" "$stats" "$timing" >> "$SUMMARY" <<'PYEOF'
import json
import sys

threads, json_path, stats_path, timing_path = sys.argv[1:]
job = json.load(open(json_path, encoding="utf-8"))["jobs"][0]
read = job.get("read", {})
write = job.get("write", {})
stats = {}
for line in open(stats_path, encoding="utf-8", errors="replace"):
    if "=" in line:
        key, value = line.rstrip().split("=", 1)
        stats[key] = value
timing = {}
for line in open(timing_path, encoding="utf-8", errors="replace"):
    if "=" in line:
        key, value = line.rstrip().split("=", 1)
        timing[key] = value
print("\t".join([
    threads,
    f"{float(read.get('iops', 0)) + float(write.get('iops', 0)):.2f}",
    str(job.get("error", 0)),
    timing.get("cpu_percent", "n/a"),
    timing.get("max_rss_kb", "n/a"),
    stats.get("runtime_queue_depth_peak", "0"),
    stats.get("lock_io_contention", "0"),
    stats.get("lock_meta_contention", "0"),
    stats.get("lock_append_contention", "0"),
]))
PYEOF
done

cat "$SUMMARY"
echo "INODE IO BENCHMARK OK"
