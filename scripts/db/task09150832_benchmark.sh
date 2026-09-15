#!/usr/bin/env bash
# Run the task09150832 measurement matrix and preserve raw evidence per sample.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_nvme.json}"
OUT="${3:-/mnt/hfs-data/task09150832}"
RAMP="${HFS_TASK_RAMP_SECONDS:-10}"
RUNTIME="${HFS_TASK_RUNTIME_SECONDS:-30}"
A3_SECONDS="${HFS_TASK_A3_SECONDS:-5}"
RUNS="${HFS_TASK_RUNS:-3}"
SIZE="${HFS_TASK_SIZE:-256m}"
TRACE_SAMPLE="${HFS_TASK_TRACE_SAMPLE:-1000}"

mkdir -p "$OUT"
[[ -f "$LIB" && -f "$CONFIG" ]] || { echo "missing lib/config" >&2; exit 2; }
command -v fio >/dev/null || { echo "fio is required" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }

printf 'timestamp=%s\nhostname=%s\nhfs_commit=%s\nspdk_commit=%s\nconfig=%s\nbdev=Nvme0n1\nramp_seconds=%s\nruntime_seconds=%s\nruns=%s\nsize=%s\n' \
  "$(date -Is)" "$(hostname)" "$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)" \
  "$(git -C "$ROOT/spdk" rev-parse HEAD 2>/dev/null || echo unknown)" "$(realpath "$CONFIG")" \
  "$RAMP" "$RUNTIME" "$RUNS" "$SIZE" > "$OUT/environment.txt"
printf 'trace_sample=%s\n' "$TRACE_SAMPLE" >> "$OUT/environment.txt"

run_a3() {
  local tag="$1" workers="$2" rw="$3"
  local log="$OUT/${tag}.log" stats="$OUT/${tag}.stats"
  echo "A3 tag=$tag workers=$workers rw=$rw"
  timeout --signal=TERM --kill-after=5s 120s env \
    HFS_CONFIG_PATH="$CONFIG" HFS_BDEV_NAME=Nvme0n1 HFS_FORMAT=1 \
    HFS_INTERNAL_BENCH=1 HFS_INTERNAL_BENCH_THREADS="$workers" \
    HFS_INTERNAL_BENCH_SECONDS="$A3_SECONDS" HFS_INTERNAL_BENCH_RW="$rw" \
    HFS_STATS_PATH="$stats" HFS_STATS_TIMING=1 \
    "$ROOT/build/bin/spdk-hfs" > "$log" 2>&1
}

run_fio() {
  local tag="$1" rw="$2" jobs="$3" ordering="$4" reactors="$5" reactor_cpus="$6" owner_cpu="$7" affinity="$8" filename_mode="$9"
  local json="$OUT/${tag}.json" stats="$OUT/${tag}.stats" trace="$OUT/${tag}.trace.tsv" timefile="$OUT/${tag}.time" vmstatfile="$OUT/${tag}.vmstat"
  local filename="/hfs/fio/task09150832-${tag}.dat"
  local -a command=(env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$CONFIG" HFS_BDEV_NAME=Nvme0n1 HFS_FORMAT=1 HFS_AUTO_MKDIRS=/fio \
    HFS_STATS_PATH="$stats" HFS_STATS_TIMING=1 HFS_TRACE_PATH="$trace" HFS_TRACE_SAMPLE="$TRACE_SAMPLE" \
    HFS_ORDERING="$ordering" HFS_DATA_REACTORS="$reactors" HFS_DATA_REACTOR_CPUS="$reactor_cpus" HFS_OWNER_REACTOR_CPU="$owner_cpu" \
    fio --name="$tag" --ioengine=psync --thread=1 --numjobs="$jobs" --iodepth=1 --bs=4k --size="$SIZE" \
    --rw="$rw" --filename="$filename" --direct=1 --allow_file_create=1 --group_reporting=1 \
    --time_based=1 --ramp_time="$RAMP" --runtime="$RUNTIME" --output="$json" --output-format=json)
  if [[ "$filename_mode" == "independent" ]]; then
    command+=(--filename_format=/hfs/fio/task09150832-${tag}-\$jobnum.dat)
  fi
  if [[ "$affinity" != "all" ]]; then
    command=(taskset -c "$affinity" "${command[@]}")
  fi
  echo "FIO tag=$tag rw=$rw jobs=$jobs ordering=$ordering reactors=$reactors affinity=$affinity"
  (vmstat 1 > "$vmstatfile" 2>&1 & echo $! > "$vmstatfile.pid")
  /usr/bin/time -o "$timefile" -f 'elapsed_seconds=%e\nuser_seconds=%U\nsys_seconds=%S\ncpu_percent=%P\nmax_rss_kb=%M\nvoluntary_ctxt_switches=%w\ninvoluntary_ctxt_switches=%c' \
    timeout --signal=TERM --kill-after=10s 240s "${command[@]}" 2> "$OUT/${tag}.stderr" || true
  if [[ -f "$vmstatfile.pid" ]]; then kill "$(<"$vmstatfile.pid")" 2>/dev/null || true; rm -f "$vmstatfile.pid"; fi
}

for rw in randread randwrite; do
  for workers in 1 4 8 16 32; do
    run_a3 "a3-${rw}-${workers}w" "$workers" "$rw"
  done
done

for rw in randread randwrite; do
  for jobs in 1 2 4 8 16 32; do
    for run in $(seq 1 "$RUNS"); do
      run_fio "scale-${rw}-${jobs}j-r${run}" "$rw" "$jobs" ticket 5 "1,2,3,4" 0 all shared
    done
  done
done

for rw in randread randwrite; do
  run_fio "affinity-${rw}-4reactor" "$rw" 32 ticket 5 "1,2,3,4" 0 "5-7" shared
  run_fio "affinity-${rw}-2reactor" "$rw" 32 ticket 3 "1,2" 0 "5-7" shared
done

for ordering in ticket range none; do
  for rw in randread randwrite; do
    for jobs in 1 8 32; do
      for run in $(seq 1 "$RUNS"); do
        mode=shared
        [[ "$ordering" == none ]] && mode=independent
        run_fio "ordering-${ordering}-${rw}-${jobs}j-r${run}" "$rw" "$jobs" "$ordering" 5 "1,2,3,4" 0 all "$mode"
      done
    done
  done
done

python3 "$ROOT/scripts/db/task09150832_summary.py" "$OUT"
