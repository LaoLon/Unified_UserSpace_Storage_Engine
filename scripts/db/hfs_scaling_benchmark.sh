#!/usr/bin/env bash
# Measure fair raw-SPDK and HFS sync-path scaling with one reproducible matrix.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build-cmake/libhfs.so}"
OUT="${2:-$ROOT/build/hfs_scaling_benchmark}"
CONFIG="${3:-$ROOT/config/hfs.json}"
SPDK_PLUGIN="${4:-$ROOT/spdk/build/fio/spdk_bdev}"
DURATION="${HFS_SCALING_DURATION_SECONDS:-5}"
SIZE="${HFS_SCALING_SIZE:-256m}"
CPU_AFFINITY="${HFS_CPU_AFFINITY:-all}"
THREADS_TEXT="${HFS_SCALING_THREADS:-1 2 4 8 16}"
QDS_TEXT="${HFS_SCALING_QDS:-1 4 16 32}"
METADATA_TEXT="${HFS_SCALING_METADATA_COUNTS:-1000 10000 60000}"

HFS_LIB="$(realpath "$LIB")"
mkdir -p "$OUT"

if [[ ! -f "$HFS_LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [output dir] [hfs.json] [spdk_bdev plugin]" >&2
	exit 2
fi
if ! command -v fio >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
	echo "ERROR: fio and python3 are required" >&2
	exit 2
fi
if [[ ! -x /usr/bin/time ]]; then
	echo "ERROR: /usr/bin/time is required for CPU/RSS accounting" >&2
	exit 2
fi
if [[ ! -f "$SPDK_PLUGIN" ]]; then
	echo "ERROR: SPDK fio plugin not found: $SPDK_PLUGIN" >&2
	exit 2
fi

rm -f "$OUT/summary.tsv" "$OUT/summary.json" "$OUT/metadata.tsv" "$OUT/environment.txt"
{
	echo "timestamp=$(date -Is)"
	echo "hostname=$(hostname)"
	echo "kernel=$(uname -srmo)"
	echo "cpu_count=$(nproc)"
	echo "cpu_affinity=$CPU_AFFINITY"
	echo "fio=$(fio --version 2>&1 | head -1)"
	echo "lib=$HFS_LIB"
	echo "config=$(realpath "$CONFIG")"
	echo "spdk_plugin=$(realpath "$SPDK_PLUGIN")"
	echo "hfs_commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "spdk_commit=$(git -C "$ROOT/spdk" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "duration_seconds=$DURATION"
	echo "size=$SIZE"
	echo "threads=$THREADS_TEXT"
	echo "qds=$QDS_TEXT"
	echo "metadata_counts=$METADATA_TEXT"
	echo "stats_timing=${HFS_STATS_TIMING:-0}"
	echo "fairness=raw uses spdk_bdev fio engine; HFS uses single-process LD_PRELOAD sync psync"
} > "$OUT/environment.txt"

# Run fio with an optional CPU affinity mask and retain JSON plus HFS statistics.
run_fio() {
	local tag="$1"
	local engine="$2"
	local rw="$3"
	local bs="$4"
	local depth="$5"
	local jobs="$6"
	local raw="$OUT/${tag}.json"
	local stats="$OUT/${tag}.stats"
	local timing="$OUT/${tag}.time"
	local filename
	local -a command
	local -a stats_env=()

	filename="/hfs/fio/scaling-${tag}.dat"
	if [[ "$engine" == "hfs" ]]; then
		if [[ "${HFS_STATS_DISABLED:-0}" == "1" ]]; then
			# Remove inherited switches so the off sample is always genuinely stats-free.
			stats_env=(-u HFS_STATS_PATH -u HFS_STATS -u HFS_STATS_TIMING)
		else
			stats_env=(HFS_STATS_PATH="$stats")
		fi
		command=(env LD_PRELOAD="$HFS_LIB" HFS_FORMAT=1 HFS_AUTO_MKDIRS=/fio
			"${stats_env[@]}" fio --name="$tag" --ioengine=psync
			--thread=1 --numjobs="$jobs" --iodepth=1 --bs="$bs" --size="$SIZE"
			--rw="$rw" --filename="$filename" --direct=1 --allow_file_create=1
			--group_reporting=1 --time_based=1 --runtime="$DURATION"
			--output="$raw" --output-format=json)
	else
		command=(env LD_PRELOAD="$SPDK_PLUGIN" fio --name="$tag"
			--ioengine=spdk_bdev --thread=1 --spdk_conf="$CONFIG"
			--filename=Malloc0 --bs="$bs" --size="$SIZE" --rw="$rw"
			--direct=1 --iodepth="$depth" --group_reporting=1
			--time_based=1 --runtime="$DURATION" --output="$raw"
			--output-format=json)
	fi
	if [[ "$CPU_AFFINITY" != "all" ]]; then
		command=(taskset -c "$CPU_AFFINITY" "${command[@]}")
	fi
	echo "run tag=$tag engine=$engine rw=$rw bs=$bs qd=$depth jobs=$jobs"
	/usr/bin/time -o "$timing" -f 'elapsed_seconds=%e\nuser_seconds=%U\nsys_seconds=%S\ncpu_percent=%P\nmax_rss_kb=%M' \
		timeout --signal=TERM --kill-after=5s 180s "${command[@]}" 2>"$OUT/${tag}.stderr"
}

# Keep the matrix explicit so every result can be compared without hidden fio defaults.
for workload in randread randwrite randrw; do
	for bs in 4k 128k; do
		for threads in $THREADS_TEXT; do
			if [[ "$bs" == "128k" && "$workload" != "randrw" ]]; then
				continue
			fi
			run_fio "hfs-${workload}-${bs}-${threads}t" hfs "$workload" "$bs" 1 "$threads"
		done
	done
done

# Keep the required 128K sequential read/write points explicit for HFS.
for workload in seqread seqwrite; do
	if [[ "$workload" == "seqread" ]]; then
		rw="read"
	else
		rw="write"
	fi
	for threads in $THREADS_TEXT; do
		run_fio "hfs-${workload}-128k-${threads}t" hfs "$rw" 128k 1 "$threads"
	done
done

for workload in read write; do
	for bs in 4k 128k; do
		for qd in $QDS_TEXT; do
			run_fio "raw-${workload}-${bs}-q${qd}" raw "$workload" "$bs" "$qd" 1
		done
	done
done

# Run a small stats-off/on pair to quantify instrumentation overhead directly.
HFS_STATS_DISABLED=1 run_fio "hfs-overhead-off" hfs randrw 4k 1 1
run_fio "hfs-overhead-on" hfs randrw 4k 1 1

# Exercise metadata mutation, lookup, directory enumeration, rename and unlink at scale.
for count in $METADATA_TEXT; do
	tag="metadata-${count}"
	stats="$OUT/${tag}.stats"
	HFS_FORMAT=1 HFS_STATS_PATH="$stats" LD_PRELOAD="$HFS_LIB" HFS_AUTO_MKDIRS=/meta \
		python3 - "$count" "$OUT/${tag}.tsv" <<'PYEOF'
import os
import sys
import time

count = int(sys.argv[1])
result = sys.argv[2]
root = "/hfs/meta"
os.makedirs(root, exist_ok=True)

def measure(fn):
    start = time.monotonic()
    fn()
    return time.monotonic() - start

def create():
    for index in range(count):
        path = f"{root}/f-{index:06d}"
        fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
        os.write(fd, b"x")
        os.close(fd)

def stat_all():
    for index in range(count):
        os.stat(f"{root}/f-{index:06d}")

def rename_all():
    for index in range(count):
        os.rename(f"{root}/f-{index:06d}", f"{root}/r-{index:06d}")

def unlink_all():
    for index in range(count):
        os.unlink(f"{root}/r-{index:06d}")

create_s = measure(create)
stat_s = measure(stat_all)
readdir_start = time.monotonic()
readdir_count = len(os.listdir(root))
readdir_s = time.monotonic() - readdir_start
rename_s = measure(rename_all)
unlink_s = measure(unlink_all)
with open(result, "w", encoding="utf-8") as stream:
    stream.write("count\tcreate_s\tstat_s\treaddir_s\trename_s\tunlink_s\treaddir_count\n")
    stream.write(f"{count}\t{create_s:.6f}\t{stat_s:.6f}\t{readdir_s:.6f}\t"
                 f"{rename_s:.6f}\t{unlink_s:.6f}\t{readdir_count}\n")
PYEOF
done

# Parse fio JSON and HFS key-value counters into a stable report.
python3 - "$OUT" <<'PYEOF'
import json
import re
import sys
from pathlib import Path

out = Path(sys.argv[1])
rows = []
for path in sorted(out.glob("*.json")):
    data = json.loads(path.read_text(encoding="utf-8"))
    job = data["jobs"][0]
    read = job.get("read", {})
    write = job.get("write", {})
    match = re.match(r"(hfs|raw)-(.+)-(4k|128k)-(?:([0-9]+)t|q([0-9]+))", path.stem)
    if not match:
        continue
    engine, workload, bs, threads, qd = match.groups()
    stats = {}
    stats_path = path.with_suffix(".stats")
    if stats_path.exists():
        for line in stats_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                stats[key] = value
    timing = {}
    timing_path = path.with_suffix(".time")
    if timing_path.exists():
        for line in timing_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                timing[key] = value
    runtime_qd = int(stats.get("runtime_queue_depth_peak", 0))
    rows.append({
        "engine": engine,
        "workload": workload,
        "bs": bs,
        "threads": int(threads or 1),
        "qd": int(qd or 1),
        "iops": float(read.get("iops", 0)) + float(write.get("iops", 0)),
        "read_iops": float(read.get("iops", 0)),
        "write_iops": float(write.get("iops", 0)),
        "read_p50_ns": read.get("clat_ns", {}).get("percentile", {}).get("50.000000", "n/a"),
        "read_p99_ns": read.get("clat_ns", {}).get("percentile", {}).get("99.000000", "n/a"),
        "write_p50_ns": write.get("clat_ns", {}).get("percentile", {}).get("50.000000", "n/a"),
        "write_p99_ns": write.get("clat_ns", {}).get("percentile", {}).get("99.000000", "n/a"),
        "elapsed_seconds": float(timing.get("elapsed_seconds", "nan")),
        "user_seconds": float(timing.get("user_seconds", "nan")),
        "sys_seconds": float(timing.get("sys_seconds", "nan")),
        "cpu_percent": timing.get("cpu_percent", "n/a"),
        "max_rss_kb": int(timing.get("max_rss_kb", 0)),
        "error": int(job.get("error", 0)),
        "runtime_queue_depth_peak": int(stats.get("runtime_queue_depth_peak", 0)),
        "runtime_queue_wait_ns": int(stats.get("runtime_queue_wait_ns", 0)),
        "runtime_owner_ns": int(stats.get("runtime_owner_ns", 0)),
        "spdk_submit": int(stats.get("spdk_submit", 0)),
        "spdk_complete": int(stats.get("spdk_complete", 0)),
        "spdk_reject": int(stats.get("spdk_reject", 0)),
        "effective_qd": runtime_qd if runtime_qd else int(qd or 1),
        "io_copy_bytes": int(stats.get("io_copy_bytes", 0)),
        "io_rmw_ops": int(stats.get("io_rmw_ops", 0)),
        "metadata_image_bytes": int(stats.get("metadata_image_bytes", 0)),
        "flush_count": int(stats.get("flush_count", 0)),
        "lock_meta_contention": int(stats.get("lock_meta_contention", 0)),
    })
if not rows:
    raise SystemExit("no fio JSON rows were produced")
columns = list(rows[0].keys())
with (out / "summary.tsv").open("w", encoding="utf-8") as stream:
    stream.write("\t".join(columns) + "\n")
    for row in rows:
        stream.write("\t".join(str(row[column]) for column in columns) + "\n")
(out / "summary.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
print("engine\tworkload\tbs\tthreads\tqd\tiops\terror")
for row in rows:
    print("%s\t%s\t%s\t%s\t%s\t%.2f\t%s" %
          (row["engine"], row["workload"], row["bs"], row["threads"],
           row["qd"], row["iops"], row["error"]))
PYEOF

cat "$OUT"/metadata-*.tsv > "$OUT/metadata.tsv"
python3 - "$OUT" <<'PYEOF'
import json
import sys
from pathlib import Path

out = Path(sys.argv[1])
def iops(name):
    data = json.loads((out / (name + ".json")).read_text(encoding="utf-8"))
    job = data["jobs"][0]
    return float(job.get("read", {}).get("iops", 0)) + float(job.get("write", {}).get("iops", 0))

off = iops("hfs-overhead-off")
on = iops("hfs-overhead-on")
overhead = ((off - on) / off * 100.0) if off else 0.0
(out / "overhead.tsv").write_text(
    "stats_off_iops\tstats_on_iops\toverhead_percent\n"
    f"{off:.2f}\t{on:.2f}\t{overhead:.2f}\n", encoding="utf-8")
print(f"stats-off iops={off:.2f} stats-on iops={on:.2f} overhead={overhead:.2f}%")
PYEOF
echo "HFS SCALING BENCHMARK OK"
