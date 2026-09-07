#!/usr/bin/env bash
# Run repeated database benchmarks and summarize median performance safely.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build-cmake/libhfs.so}"
PREFIX="${2:-/hfs}"
CONFIG="${3:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${4:-/var/tmp/hfs-stage48-benchmark.img}"
OUT="${5:-$ROOT/build/stage48_matrix}"
ROUNDS="${HFS_STAGE48_ROUNDS:-3}"
DURATION="${HFS_STAGE48_DURATION_SECONDS:-60}"
NUM="${HFS_BENCH_NUM:-20000}"
VALUE_SIZE="${HFS_BENCH_VALUE_SIZE:-256}"
CPU_AFFINITY="${HFS_CPU_AFFINITY:-all}"
BDEV_TYPE="${HFS_BENCH_BDEV_TYPE:-persistent-aio}"
RAW_REFERENCE="${HFS_BENCH_RAW_REFERENCE:-not-run}"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [mount prefix] [config] [image] [output dir]" >&2
	exit 2
fi
if ! [[ "$ROUNDS" =~ ^[1-9][0-9]*$ ]]; then
	echo "HFS_STAGE48_ROUNDS must be a positive integer" >&2
	exit 2
fi
if ! [[ "$DURATION" =~ ^[1-9][0-9]*$ ]]; then
	echo "HFS_STAGE48_DURATION_SECONDS must be a positive integer" >&2
	exit 2
fi

LIB="$(realpath "$LIB")"
mkdir -p "$OUT"
rm -f "$OUT/runs.tsv" "$OUT/summary.tsv" "$OUT/summary.json" "$OUT/environment.txt"
{
	echo "timestamp=$(date -Is)"
	echo "hostname=$(hostname)"
	echo "kernel=$(uname -srmo)"
	echo "cpu_count=$(nproc)"
	echo "lib=$LIB"
	echo "config=$(realpath "$CONFIG")"
	echo "image=$IMAGE"
	echo "output=$OUT"
	echo "rounds=$ROUNDS"
	echo "duration_seconds=$DURATION"
	echo "num=$NUM"
	echo "value_size=$VALUE_SIZE"
	echo "cpu_affinity=$CPU_AFFINITY"
	echo "bdev_type=$BDEV_TYPE"
	echo "raw_reference=$RAW_REFERENCE"
	echo "hfs_commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "spdk_commit=$(git -C "$ROOT/spdk" rev-parse HEAD 2>/dev/null || echo unknown)"
} > "$OUT/environment.txt"

for round in $(seq 1 "$ROUNDS"); do
	round_out="$OUT/round-$round"
	mkdir -p "$round_out"
	echo "== stage48 round $round/$ROUNDS =="
	HFS_BENCH_NUM="$NUM" \
	HFS_BENCH_VALUE_SIZE="$VALUE_SIZE" \
	HFS_BENCH_DURATION_SECONDS="$DURATION" \
	HFS_CPU_AFFINITY="$CPU_AFFINITY" \
	HFS_BENCH_BDEV_TYPE="$BDEV_TYPE" \
	HFS_BENCH_RAW_REFERENCE="$RAW_REFERENCE" \
	bash "$ROOT/scripts/db/database_benchmark.sh" \
		"$LIB" "$PREFIX" "$CONFIG" "$IMAGE" "$round_out"
done

# Combine raw summaries and validate every round before calculating medians.
python3 - "$OUT" "$ROUNDS" <<'PYEOF'
import csv
import json
import statistics
import sys
from pathlib import Path

out = Path(sys.argv[1])
rounds = int(sys.argv[2])
rows = []
required_cases = {"fillseq", "readwhilewriting-1t", "readwhilewriting-4t"}

for round_id in range(1, rounds + 1):
    path = out / f"round-{round_id}" / "summary.tsv"
    if not path.is_file():
        raise SystemExit(f"missing benchmark summary: {path}")
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            row["round"] = round_id
            rows.append(row)

seen = {row.get("case") for row in rows}
if seen != required_cases:
    raise SystemExit(f"unexpected benchmark cases: {sorted(seen)}")

def as_float(row, key):
    value = row.get(key, "")
    if value in ("", "None", "nan", "n/a"):
        return None
    if value.endswith("%"):
        value = value[:-1]
    return float(value)

for row in rows:
    ops = as_float(row, "ops_per_sec")
    errors = int(row.get("errors", "0"))
    submits = int(row.get("spdk_submit", "0"))
    completes = int(row.get("spdk_complete", "0"))
    rejects = int(row.get("spdk_reject", "0"))
    if ops is None or ops <= 0:
        raise SystemExit(f"non-positive ops/sec in round {row['round']} {row['case']}")
    if errors != 0 or rejects != 0 or submits != completes:
        raise SystemExit(
            f"counter failure in round {row['round']} {row['case']}: "
            f"errors={errors} rejects={rejects} submit={submits} complete={completes}"
        )

def metric_values(rows_for_case, key):
    values = [as_float(row, key) for row in rows_for_case]
    return [value for value in values if value is not None]


def metric_summary(rows_for_case, key):
    values = metric_values(rows_for_case, key)
    if not values:
        return {
            f"{key}_median": None,
            f"{key}_min": None,
            f"{key}_max": None,
            f"{key}_stdev": None,
        }
    return {
        f"{key}_median": statistics.median(values),
        f"{key}_min": min(values),
        f"{key}_max": max(values),
        f"{key}_stdev": statistics.pstdev(values),
    }

summary = []
for case in sorted(required_cases):
    case_rows = [row for row in rows if row["case"] == case]
    item = {"case": case, "rounds": len(case_rows)}
    for key in (
        "ops_per_sec", "p50_us", "p99_us", "elapsed_seconds",
        "cpu_percent", "max_rss_kb", "effective_qd",
    ):
        item.update(metric_summary(case_rows, key))
    summary.append(item)

raw_columns = ["round"] + [key for key in rows[0] if key != "round"]
with (out / "runs.tsv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=raw_columns, delimiter="\t", extrasaction="ignore")
    writer.writeheader()
    writer.writerows(rows)

summary_columns = list(summary[0].keys())
with (out / "summary.tsv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=summary_columns, delimiter="\t")
    writer.writeheader()
    writer.writerows(summary)
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

print("case\trounds\tops/sec median\tops/sec min\tops/sec max\tops/sec stdev\tp99_us median\tp99_us stdev\tRSS KiB median")
for item in summary:
    print("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s" % (
        item["case"], item["rounds"], item["ops_per_sec_median"],
        item["ops_per_sec_min"], item["ops_per_sec_max"],
        item["ops_per_sec_stdev"], item["p99_us_median"],
        item["p99_us_stdev"], item["max_rss_kb_median"],
    ))
PYEOF

echo "STAGE48 MATRIX OK"
