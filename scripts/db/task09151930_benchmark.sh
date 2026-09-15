#!/usr/bin/env bash
# Run the stage-3 reactor/channel matrix and retain raw attribution evidence.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_nvme.json}"
OUT="${3:-/mnt/hfs-data/task09151930}"
RAMP="${HFS_TASK_RAMP_SECONDS:-10}"
RUNTIME="${HFS_TASK_RUNTIME_SECONDS:-30}"
RUNS="${HFS_TASK_RUNS:-3}"
SIZE="${HFS_TASK_SIZE:-256m}"
TRACE_SAMPLE="${HFS_TASK_TRACE_SAMPLE:-1000}"
RAW_SECONDS="${HFS_TASK_RAW_SECONDS:-10}"

mkdir -p "$OUT"
[[ -f "$LIB" && -f "$CONFIG" ]] || { echo "missing lib/config" >&2; exit 2; }
command -v fio >/dev/null || { echo "fio is required" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }

printf 'timestamp=%s\nhostname=%s\nhfs_commit=%s\nspdk_commit=%s\nconfig=%s\nbdev=Nvme0n1\nramp_seconds=%s\nruntime_seconds=%s\nruns=%s\nsize=%s\ntrace_sample=%s\n' \
  "$(date -Is)" "$(hostname)" "$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)" \
  "$(git -C "$ROOT/spdk" rev-parse HEAD 2>/dev/null || echo unknown)" "$(realpath "$CONFIG")" \
  "$RAMP" "$RUNTIME" "$RUNS" "$SIZE" "$TRACE_SAMPLE" > "$OUT/environment.txt"

run_fio() {
  local group="$1" data_reactors="$2" data_cpus="$3" app_cpus="$4" rw="$5" jobs="$6" run="$7"
  local tag="${group}-d${data_reactors}-${rw}-j${jobs}-r${run}"
  local total_reactors=$((data_reactors + 1))
  local json="$OUT/${tag}.json" stats="$OUT/${tag}.stats" trace="$OUT/${tag}.trace.tsv"
  local timefile="$OUT/${tag}.time" vmstatfile="$OUT/${tag}.vmstat"
  local filename="/hfs/fio/task09151930-${tag}.dat"
  local -a command=(env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$CONFIG" HFS_BDEV_NAME=Nvme0n1 HFS_FORMAT=1 HFS_AUTO_MKDIRS=/fio \
    HFS_STATS_PATH="$stats" HFS_STATS_TIMING=1 HFS_TRACE_PATH="$trace" HFS_TRACE_SAMPLE="$TRACE_SAMPLE" \
    HFS_ORDERING=ticket HFS_POSIX_QUEUE=1 HFS_POSIX_TICKET=1 HFS_DATA_REACTORS="$total_reactors" \
    HFS_DATA_REACTOR_CPUS="$data_cpus" HFS_OWNER_REACTOR_CPU=0 \
    fio --name="$tag" --ioengine=psync --thread=1 --numjobs="$jobs" --iodepth=1 --bs=4k --size="$SIZE" \
    --rw="$rw" --filename="$filename" --direct=1 --allow_file_create=1 --group_reporting=1 \
    --time_based=1 --ramp_time="$RAMP" --runtime="$RUNTIME" --output="$json" --output-format=json)

  echo "FIO group=$group data_reactors=$data_reactors app_cpus=$app_cpus rw=$rw jobs=$jobs run=$run"
  if [[ "$app_cpus" != "all" ]]; then
    command=(taskset -c "$app_cpus" "${command[@]}")
  fi
  (vmstat 1 > "$vmstatfile" 2>&1 & echo $! > "$vmstatfile.pid")
  /usr/bin/time -o "$timefile" -f 'elapsed_seconds=%e\nuser_seconds=%U\nsys_seconds=%S\ncpu_percent=%P\nmax_rss_kb=%M\nvoluntary_ctxt_switches=%w\ninvoluntary_ctxt_switches=%c' \
    timeout --signal=TERM --kill-after=10s 240s "${command[@]}" 2> "$OUT/${tag}.stderr" || true
  if [[ -f "$vmstatfile.pid" ]]; then
    kill "$(<"$vmstatfile.pid")" 2>/dev/null || true
    rm -f "$vmstatfile.pid"
  fi
}

run_raw_blob() {
  local channels="$1" rw="$2"
  local tag="raw-blob-c${channels}-${rw}"
  echo "RAW_BLOB channels=$channels rw=$rw qd=32"
  timeout --signal=TERM --kill-after=5s 60s env \
    TASK_BLOB_BS=4096 TASK_BLOB_RW="$rw" TASK_BLOB_QD=32 TASK_BLOB_CHANNELS="$channels" \
    TASK_BLOB_SECONDS="$RAW_SECONDS" TASK_BLOB_BDEV=Nvme0n1 \
    "$ROOT/scripts/db/task90150022_blob_bench/task90150022_blob_bench" "$CONFIG" \
    > "$OUT/${tag}.log" 2> "$OUT/${tag}.stderr" || true
}

make -C "$ROOT/scripts/db/task90150022_blob_bench"

# Group 1: fixed application CPUs 5-7; only the data reactor set changes.
declare -A FIXED_CPUS=( [1]=1 [2]='1,2' [3]='1,2,3' [4]='1,2,3,4' )
for data_reactors in 1 2 3 4; do
  for rw in randread randwrite; do
    for jobs in 8 16 32; do
      for run in $(seq 1 "$RUNS"); do
        run_fio fixed "${data_reactors}" "${FIXED_CPUS[$data_reactors]}" 5-7 "$rw" "$jobs" "$run"
      done
    done
  done
done

# Group 2: application CPUs are the CPUs left after the fixed data set.
declare -A DEPLOY_CPUS=( [1]=2-7 [2]=3-7 [3]=4-7 [4]=5-7 )
for data_reactors in 1 2 3 4; do
  for rw in randread randwrite; do
    for jobs in 8 16 32; do
      for run in $(seq 1 "$RUNS"); do
        run_fio deployment "${data_reactors}" "${FIXED_CPUS[$data_reactors]}" \
          "${DEPLOY_CPUS[$data_reactors]}" "$rw" "$jobs" "$run"
      done
    done
  done
done

# Raw Blobstore control: total QD is 32 for 1/2/4 channels.
for rw in randread randwrite; do
  for channels in 1 2 4; do
    run_raw_blob "$channels" "$rw"
  done
done

python3 "$ROOT/scripts/db/task09151930_summary.py" "$OUT"
