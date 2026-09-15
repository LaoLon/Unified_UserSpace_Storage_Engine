#!/usr/bin/env bash
# Run the reproducible Shard-66A HFS baseline matrix.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_nvme.json}"
OUT="${3:-/mnt/hfs-data/shard66-baseline-v2}"
RAMP="${HFS_BASELINE_RAMP_SECONDS:-10}"
RUNTIME="${HFS_BASELINE_RUNTIME_SECONDS:-30}"
RUNS="${HFS_BASELINE_RUNS:-5}"
SIZE="${HFS_BASELINE_SIZE:-256m}"
TOTAL_REACTORS="${HFS_BASELINE_TOTAL_REACTORS:-5}"
OWNER_CPU="${HFS_BASELINE_OWNER_CPU:-0}"
DATA_CPUS="${HFS_BASELINE_DATA_CPUS:-1,2,3,4}"
APP_CPUS="${HFS_BASELINE_APP_CPUS:-5-7}"
ATTRIBUTION="${HFS_BASELINE_ATTRIBUTION:-0}"
read -r -a MODELS <<< "${HFS_BASELINE_MODELS:-shared-file per-job-file}"
read -r -a BLOCK_SIZES <<< "${HFS_BASELINE_BLOCK_SIZES:-4k 16k 64k}"
read -r -a OPERATIONS <<< "${HFS_BASELINE_OPERATIONS:-randread randwrite}"
read -r -a JOB_COUNTS <<< "${HFS_BASELINE_JOB_COUNTS:-1 8 16 32}"
FORMAL_FAILURES=0

mkdir -p "$OUT"
[[ -f "$LIB" && -f "$CONFIG" ]] || { echo "missing lib/config" >&2; exit 2; }
command -v fio >/dev/null || { echo "fio is required" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }

cat > "$OUT/environment.txt" <<EOF
timestamp=$(date -Is)
hostname=$(hostname)
hfs_commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)
spdk_commit=$(git -C "$ROOT/spdk" rev-parse HEAD 2>/dev/null || echo unknown)
config=$(realpath "$CONFIG")
bdev=Nvme0n1
online_cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown)
cpu_model=$(awk -F: '/^model name/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')
cpu_mhz=$(awk -F: '/^cpu MHz/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')
ramp_seconds=$RAMP
runtime_seconds=$RUNTIME
runs=$RUNS
size=$SIZE
models=${HFS_BASELINE_MODELS:-shared-file per-job-file}
block_sizes=${HFS_BASELINE_BLOCK_SIZES:-4k 16k 64k}
operations=${HFS_BASELINE_OPERATIONS:-randread randwrite}
job_counts=${HFS_BASELINE_JOB_COUNTS:-1 8 16 32}
total_reactors=$TOTAL_REACTORS
owner_cpu=$OWNER_CPU
data_cpus=$DATA_CPUS
app_cpus=$APP_CPUS
trace_off_formal=1
stats_off_formal=1
hfs_env=$(env | awk '/^HFS_/ {print}' | sort | tr '\n' ';')
EOF

# Add the filename options for either the shared-file or per-job-file model.
filename_args() {
  local model="$1" base="$2"
  if [[ "$model" == "shared-file" ]]; then
    printf '%s\0' "--filename=${base}.dat"
  else
    printf '%s\0' "--filename=${base}.dat" "--filename_format=${base}.\$jobnum.dat"
  fi
}

# Format the mount and write every target file before a formal read/write sample.
prefill() {
  local model="$1" bs="$2" rw="$3" jobs="$4" run="$5" tag="$6" base="$7"
  local -a files
  mapfile -d '' files < <(filename_args "$model" "$base")
  env -u HFS_STATS -u HFS_STATS_PATH -u HFS_STATS_TIMING \
    -u HFS_TRACE -u HFS_TRACE_PATH -u HFS_TRACE_SAMPLE \
    LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$CONFIG" HFS_BDEV_NAME=Nvme0n1 \
    HFS_FORMAT=1 HFS_AUTO_MKDIRS=/fio HFS_ORDERING=ticket \
    HFS_POSIX_QUEUE=1 HFS_POSIX_TICKET=1 HFS_DATA_REACTORS="$TOTAL_REACTORS" \
    HFS_OWNER_REACTOR_CPU="$OWNER_CPU" HFS_DATA_REACTOR_CPUS="$DATA_CPUS" \
    taskset -c "$APP_CPUS" fio --name="prefill-$tag" --ioengine=psync --thread=1 \
    --numjobs="$jobs" --iodepth=1 --bs="$bs" --size="$SIZE" --rw=write \
    --direct=1 --allow_file_create=1 --group_reporting=1 --time_based=0 \
    "${files[@]}" --output="$OUT/${tag}.prefill.json" --output-format=json \
    > "$OUT/${tag}.prefill.stdout" 2> "$OUT/${tag}.prefill.stderr"
}

# Run one trace-off formal point and retain independent CPU/run-queue evidence.
run_sample() {
  local model="$1" bs="$2" rw="$3" jobs="$4" run="$5"
  local tag="${model}-b${bs}-j${jobs}-${rw}-r${run}"
  local base="/hfs/fio/shard66-${model}-b${bs}-j${jobs}-r${run}"
  local json="$OUT/${tag}.json" timefile="$OUT/${tag}.time" vmstatfile="$OUT/${tag}.vmstat"
  local stderr="$OUT/${tag}.stderr" status=0
  local -a files command

  mapfile -d '' files < <(filename_args "$model" "$base")
  prefill "$model" "$bs" "$rw" "$jobs" "$run" "$tag" "$base"
  command=(env -u HFS_STATS -u HFS_STATS_PATH -u HFS_STATS_TIMING \
    -u HFS_TRACE -u HFS_TRACE_PATH -u HFS_TRACE_SAMPLE \
    LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$CONFIG" HFS_BDEV_NAME=Nvme0n1 \
    HFS_FORMAT=0 HFS_AUTO_MKDIRS=/fio HFS_ORDERING=ticket \
    HFS_POSIX_QUEUE=1 HFS_POSIX_TICKET=1 HFS_DATA_REACTORS="$TOTAL_REACTORS" \
    HFS_OWNER_REACTOR_CPU="$OWNER_CPU" HFS_DATA_REACTOR_CPUS="$DATA_CPUS" \
    fio --name="$tag" --ioengine=psync --thread=1 --numjobs="$jobs" --iodepth=1 \
    --bs="$bs" --size="$SIZE" --rw="$rw" --direct=1 --allow_file_create=1 \
    --group_reporting=1 --time_based=1 --ramp_time="$RAMP" --runtime="$RUNTIME" \
    "${files[@]}" --output="$json" --output-format=json)
  if [[ "$APP_CPUS" != "all" ]]; then
    command=(taskset -c "$APP_CPUS" "${command[@]}")
  fi
  echo "FIO model=$model bs=$bs rw=$rw jobs=$jobs run=$run"
  (vmstat 1 > "$vmstatfile" 2>&1 & echo $! > "$vmstatfile.pid")
  set +e
  /usr/bin/time -o "$timefile" -f \
    'elapsed_seconds=%e\nuser_seconds=%U\nsys_seconds=%S\ncpu_percent=%P\nmax_rss_kb=%M\nvoluntary_ctxt_switches=%w\ninvoluntary_ctxt_switches=%c' \
    timeout --signal=TERM --kill-after=10s 240s "${command[@]}" \
    > "$OUT/${tag}.stdout" 2> "$stderr"
  status=$?
  set -e
  if [[ -f "$vmstatfile.pid" ]]; then
    kill "$(<"$vmstatfile.pid")" 2>/dev/null || true
    rm -f "$vmstatfile.pid"
  fi
  if (( status != 0 )); then
    echo "sample failed tag=$tag status=$status" >&2
    FORMAL_FAILURES=$((FORMAL_FAILURES + 1))
  fi
}

# Run one optional trace-on attribution point outside the formal matrix.
run_attribution() {
  local model=shared-file bs=4k rw=randrw jobs=16 run=1
  local tag="attribution-${model}-b${bs}-j${jobs}-${rw}-r${run}"
  local base="/hfs/fio/shard66-${model}-b${bs}-j${jobs}-r${run}"
  local stats="$OUT/${tag}.stats" trace="$OUT/${tag}.trace.tsv"
  local -a files command
  mapfile -d '' files < <(filename_args "$model" "$base")
  prefill "$model" "$bs" "$rw" "$jobs" "$run" "$tag" "$base"
  command=(env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$CONFIG" HFS_BDEV_NAME=Nvme0n1 \
    HFS_FORMAT=0 HFS_AUTO_MKDIRS=/fio HFS_STATS_PATH="$stats" HFS_STATS_TIMING=1 \
    HFS_TRACE_PATH="$trace" HFS_TRACE_SAMPLE=100 HFS_ORDERING=ticket \
    HFS_POSIX_QUEUE=1 HFS_POSIX_TICKET=1 HFS_DATA_REACTORS="$TOTAL_REACTORS" \
    HFS_OWNER_REACTOR_CPU="$OWNER_CPU" HFS_DATA_REACTOR_CPUS="$DATA_CPUS" \
    fio --name="$tag" --ioengine=psync --thread=1 --numjobs="$jobs" --iodepth=1 \
    --bs="$bs" --size="$SIZE" --rw="$rw" --direct=1 --allow_file_create=1 \
    --group_reporting=1 --time_based=1 --ramp_time="$RAMP" --runtime="$RUNTIME" \
    "${files[@]}" --output="$OUT/${tag}.json" --output-format=json)
  if [[ "$APP_CPUS" != "all" ]]; then
    command=(taskset -c "$APP_CPUS" "${command[@]}")
  fi
  "${command[@]}" > "$OUT/${tag}.stdout" 2> "$OUT/${tag}.stderr" || true
}

for model in "${MODELS[@]}"; do
  for bs in "${BLOCK_SIZES[@]}"; do
    for rw in "${OPERATIONS[@]}"; do
      for jobs in "${JOB_COUNTS[@]}"; do
        for run in $(seq 1 "$RUNS"); do
          run_sample "$model" "$bs" "$rw" "$jobs" "$run"
        done
      done
    done
  done
done

if [[ "$ATTRIBUTION" != "0" ]]; then
  run_attribution
fi

python3 "$ROOT/scripts/db/shard66_summary.py" "$OUT"
if (( FORMAL_FAILURES != 0 )); then
  echo "formal samples failed: $FORMAL_FAILURES" >&2
  exit 1
fi
