#!/usr/bin/env bash
# Collect reproducible RocksDB throughput, latency, CPU, error, and HFS stats.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build-cmake/libhfs.so}"
PREFIX="${2:-/hfs}"
CONFIG="${3:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${4:-/var/tmp/hfs-shard35-benchmark.img}"
OUT="${5:-$ROOT/build/database_benchmark}"
HFS_LIB="$(realpath "$LIB")"
DBDIR="$PREFIX/rocks/benchmark"
LOCK_PATH="$OUT/hfs.lock"
EFFECTIVE_CONFIG="$OUT/hfs_db_aio.json"
NUM="${HFS_BENCH_NUM:-20000}"
VALUE_SIZE="${HFS_BENCH_VALUE_SIZE:-256}"
DURATION="${HFS_BENCH_DURATION_SECONDS:-60}"

if [[ ! -f "$HFS_LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [mount prefix] [config] [image] [output dir]" >&2
	exit 2
fi
if ! command -v db_bench >/dev/null 2>&1; then
	echo "ERROR: required RocksDB tool not found: db_bench" >&2
	exit 2
fi
if ! command -v /usr/bin/time >/dev/null 2>&1; then
	echo "ERROR: /usr/bin/time is required for CPU accounting" >&2
	exit 2
fi

mkdir -p "$OUT"
for name in fillseq readwhilewriting-1t readwhilewriting-4t; do
	rm -f "$OUT/${name}.log" "$OUT/${name}.time" "$OUT/${name}.stats"
done
rm -f "$OUT/summary.tsv" "$OUT/summary.json" "$OUT/environment.txt"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
cp "$EFFECTIVE_CONFIG" "$OUT/config.json"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

# Preserve the exact host and benchmark inputs beside all raw measurements.
{
	echo "timestamp=$(date -Is)"
	echo "hostname=$(hostname)"
	echo "kernel=$(uname -srmo)"
	echo "cpu_count=$(nproc)"
	echo "lib=$(realpath "$HFS_LIB")"
	echo "db_bench=$(command -v db_bench)"
	db_bench --version 2>&1 | head -1 || true
	fio --version 2>&1 | head -1 || true
	echo "num=$NUM"
	echo "value_size=$VALUE_SIZE"
	echo "duration_seconds=$DURATION"
	echo "latency_metric=rocksdb.db.write.micros (microseconds)"
	echo "db=$DBDIR"
	echo "image=$IMAGE"
	echo "config=$EFFECTIVE_CONFIG"
	cat "$EFFECTIVE_CONFIG"
} > "$OUT/environment.txt"

# Run one bounded benchmark and retain its raw output and /usr/bin/time data.
run_bench() {
	local name="$1"
	shift
	local log="$OUT/${name}.log"
	local timeLog="$OUT/${name}.time"
	local stats="$OUT/${name}.stats"
	local format=0
	local rc
	if [[ "$name" == "fillseq" ]]; then
		format=1
	fi
	set +e
	/usr/bin/time -f 'elapsed_seconds=%e\nuser_seconds=%U\nsys_seconds=%S\ncpu_percent=%P\nmax_rss_kb=%M' \
		-o "$timeLog" timeout --signal=TERM --kill-after=5s 180s \
		env LD_PRELOAD="$HFS_LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT="$format" \
		HFS_LOG_LEVEL=error HFS_STATS_PATH="$stats" HFS_AUTO_MKDIRS=/rocks \
		db_bench "$@" >"$log" 2>&1
	rc=$?
	set -e
	if [[ "$rc" -ne 0 ]]; then
		echo "ERROR: benchmark $name failed with exit code $rc" >&2
		tail -120 "$log" >&2 || true
		return "$rc"
	fi
	if grep -Eiq 'corruption|no such device|segmentation fault|assertion failed|i/o error|io error' "$log"; then
		echo "ERROR: benchmark $name contains a fatal data-path error" >&2
		tail -120 "$log" >&2 || true
		return 1
	fi
}

echo "== Database performance benchmark =="
echo "  lib=$HFS_LIB"
echo "  output=$OUT"
echo "-- fillseq --"
run_bench fillseq \
	--benchmarks=fillseq --db="$DBDIR" --num="$NUM" \
	--value_size="$VALUE_SIZE" --key_size=16 --use_existing_db=0 \
	--disable_wal=0 --mmap_read=0 --mmap_write=0 --statistics=1 --histogram=1

for threads in 1 4; do
	echo "-- readwhilewriting threads=$threads --"
	run_bench "readwhilewriting-${threads}t" \
		--benchmarks=readwhilewriting --db="$DBDIR" --num="$NUM" \
		--value_size="$VALUE_SIZE" --key_size=16 --use_existing_db=1 \
		--disable_wal=0 --mmap_read=0 --mmap_write=0 --threads="$threads" \
		--duration="$DURATION" --statistics=1 --histogram=1
done

# Parse all raw logs into a stable tabular report and a machine-readable JSON file.
python3 - "$OUT" <<'PYEOF'
import json
import re
import sys
from pathlib import Path

out = Path(sys.argv[1])
cases = ["fillseq", "readwhilewriting-1t", "readwhilewriting-4t"]
number = r"([0-9]+(?:\.[0-9]+)?)"


def read_key_value(path):
    values = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


def parse_case(name):
    text = (out / (name + ".log")).read_text(encoding="utf-8", errors="replace")
    ops = re.findall(number + r"\s+ops/sec", text, re.IGNORECASE)
    latency_lines = [
        line for line in text.splitlines()
        if "rocksdb.db.write.micros" in line and "COUNT : 0" not in line
    ]
    if not latency_lines:
        latency_lines = [
            line for line in text.splitlines()
            if "rocksdb.read.block.get.micros" in line and "COUNT : 0" not in line
        ]
    p50 = re.findall(r"P50\s*:\s*" + number, "\n".join(latency_lines))
    p99 = re.findall(r"P99\s*:\s*" + number, "\n".join(latency_lines))
    errors = len(re.findall(
        r"corruption|no such device|segmentation fault|assertion failed|i/o error|io error",
        text,
        re.IGNORECASE,
    ))
    timing = read_key_value(out / (name + ".time"))
    stats = read_key_value(out / (name + ".stats"))
    return {
        "case": name,
        "ops_per_sec": float(ops[-1]) if ops else None,
        "p50_us": float(p50[-1]) if p50 else None,
        "p99_us": float(p99[-1]) if p99 else None,
        "elapsed_seconds": float(timing.get("elapsed_seconds", "nan")),
        "user_seconds": float(timing.get("user_seconds", "nan")),
        "sys_seconds": float(timing.get("sys_seconds", "nan")),
        "cpu_percent": timing.get("cpu_percent", "n/a"),
        "max_rss_kb": int(timing.get("max_rss_kb", "0")),
        "errors": errors + int(stats.get("errors", "0")),
        "stats": stats,
    }


rows = [parse_case(case) for case in cases]
if any(row["ops_per_sec"] is None or row["ops_per_sec"] <= 0 for row in rows):
    raise SystemExit("benchmark output did not contain positive ops/sec for every case")

columns = [
    "case", "ops_per_sec", "p50_us", "p99_us", "elapsed_seconds", "user_seconds",
    "sys_seconds", "cpu_percent", "max_rss_kb", "errors",
]
with (out / "summary.tsv").open("w", encoding="utf-8") as stream:
    stream.write("\t".join(columns) + "\n")
    for row in rows:
        stream.write("\t".join(str(row[column]) for column in columns) + "\n")
(out / "summary.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")

print("case\tops/sec\tp50_us\tp99_us\telapsed\tcpu\terrors")
for row in rows:
    print("%s\t%s\t%s\t%s\t%s\t%s\t%s" % (
        row["case"], row["ops_per_sec"], row["p50_us"], row["p99_us"],
        row["elapsed_seconds"], row["cpu_percent"], row["errors"],
    ))
PYEOF

echo "DATABASE BENCHMARK OK"
