#!/bin/bash
# Shard-120 TiKV performance A/B: TiKV on UUSE vs TiKV on host ext4.
# Same VM, same disk, interleaved rounds; sysbench oltp_point_select and
# oltp_read_write, 8 threads x 60 s, 5-run median per side.
# Evidence under /build/tidb/perf-ab-<timestamp>.
set -u
export LD_LIBRARY_PATH=/root/code/build-uuse
BASE=/build/tidb
HOST_BASE=/build/tidb-host
UUSECTL=/root/code/build-uuse/uusectl
CONF=/root/code/config/uuse_nvme.conf
TIKV=/build/tikv-build/release/tikv-server
PD=$BASE/bin/pd-server
TIDB=$BASE/bin/tidb-server
MYSQL=/home/uuse/mysql-build/runtime_output_directory/mysql
SYSBENCH=/usr/bin/sysbench
ROUNDS=${ROUNDS:-5}
BENCH_TIME=${BENCH_TIME:-60}
THREADS=${THREADS:-8}
TABLES=8
TABLE_SIZE=20000
R=$BASE/perf-ab-$(date +%Y%m%d-%H%M%S)
mkdir -p "$R"
echo "R=$R"

sqlq() { "$MYSQL" --no-defaults -h127.0.0.1 -P4000 -uroot "$@" 2>&1; }
wait_pd() { for i in $(seq 1 30); do curl -s http://127.0.0.1:2379/pd/api/v1/health >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }
wait_tikv() { for i in $(seq 1 120); do curl -s http://127.0.0.1:20180/status >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }
wait_tidb() { for i in $(seq 1 60); do curl -s http://127.0.0.1:10080/status >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }

stop_stack() {
  pkill -9 -f tikv-server 2>/dev/null || true
  pkill -9 -f 'tidb-server --store=tikv' 2>/dev/null || true
  pkill -9 -f 'pd-server --name=pd1' 2>/dev/null || true
  sleep 3
}

# Starts PD+TiKV+TiDB for one side; fresh data dirs per run.
start_stack() {
  local side="$1"
  local cfg pd_dir tikv_data do_format
  if [ "$side" = uuse ]; then
    cfg=$BASE/tikv-uuse-both.toml; pd_dir=$BASE/pd-ab-uuse; tikv_data=$BASE/tikv-data; do_format=1
  else
    cfg=$BASE/tikv-host.toml; pd_dir=$HOST_BASE/pd-ab-host; tikv_data=$HOST_BASE/tikv-data; do_format=0
  fi
  stop_stack
  rm -rf "$pd_dir" "$tikv_data"
  mkdir -p "$tikv_data"
  if [ "$do_format" = 1 ]; then
    "$UUSECTL" --config="$CONF" format --yes >"$R/format-$side.log" 2>&1 || { echo "FORMAT_FAIL"; exit 1; }
  fi
  setsid nohup "$PD" --name=pd1 --data-dir="$pd_dir" \
    --client-urls=http://127.0.0.1:2379 --peer-urls=http://127.0.0.1:2380 \
    --log-file=$BASE/logs/pd.log >$BASE/logs/pd.stdout 2>&1 &
  wait_pd || { echo "PD_FAIL"; exit 1; }
  setsid nohup "$TIKV" --config "$cfg" --pd-endpoints 127.0.0.1:2379 >>$BASE/logs/tikv.stdout 2>&1 &
  wait_tikv || { echo "TIKV_FAIL"; exit 1; }
  setsid nohup "$TIDB" --store=tikv --path=127.0.0.1:2379 --host=127.0.0.1 -P 4000 --status=10080 \
    --log-file=$BASE/logs/tidb.log >>$BASE/logs/tidb.stdout 2>&1 &
  wait_tidb || { echo "TIDB_FAIL"; exit 1; }
  # The fast-reorg DDL ingest path is not UUSE-routed yet (Shard-119 audit row 6);
  # use the classic backfill so sysbench prepare works on both sides.
  sqlq -e "SET GLOBAL tidb_ddl_enable_fast_reorg = OFF; SET GLOBAL tidb_enable_dist_task = OFF;" >/dev/null
}

sysbench_common() {
  "$SYSBENCH" --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=4000 \
    --mysql-user=root --mysql-db=sbtest --tables=$TABLES --table-size=$TABLE_SIZE \
    --threads=$THREADS --time=$BENCH_TIME --percentile=99 --report-interval=0 "$@"
}

run_side() {
  local side="$1" round="$2"
  local t0 t1
  echo "== side=$side round=$round $(date -u +%H:%M:%S) =="
  t0=$(date +%s)
  start_stack "$side"
  t1=$(date +%s); echo "stack_up_s=$((t1 - t0))" >>"$R/times.txt"
  sqlq -e "DROP DATABASE IF EXISTS sbtest; CREATE DATABASE sbtest;" >/dev/null
  # prepare (fresh tables)
  t0=$(date +%s)
  sysbench_common oltp_common prepare >"$R/$side-r$round-prepare.txt" 2>&1 || { echo "PREPARE_FAIL"; exit 1; }
  t1=$(date +%s); echo "prepare_s=$((t1 - t0))" >>"$R/times.txt"
  # point select
  iostat -x 1 $((BENCH_TIME + 2)) >"$R/$side-r$round-iostat.txt" 2>&1 &
  local iostat_pid=$!
  mpstat -P ALL 1 $((BENCH_TIME + 2)) >"$R/$side-r$round-mpstat.txt" 2>&1 &
  local mpstat_pid=$!
  sysbench_common oltp_point_select run >"$R/$side-r$round-point_select.txt" 2>&1
  wait $iostat_pid $mpstat_pid 2>/dev/null || true
  # mixed read/write
  iostat -x 1 $((BENCH_TIME + 2)) >"$R/$side-r$round-iostat-rw.txt" 2>&1 &
  iostat_pid=$!
  mpstat -P ALL 1 $((BENCH_TIME + 2)) >"$R/$side-r$round-mpstat-rw.txt" 2>&1 &
  mpstat_pid=$!
  sysbench_common oltp_read_write run >"$R/$side-r$round-read_write.txt" 2>&1
  wait $iostat_pid $mpstat_pid 2>/dev/null || true
  curl -s http://127.0.0.1:20180/metrics >"$R/$side-r$round-metrics.txt" 2>/dev/null || true
  echo "== done side=$side round=$round =="
}

for round in $(seq 1 "$ROUNDS"); do
  if [ $((round % 2)) = 1 ]; then
    run_side uuse "$round"
    run_side host "$round"
  else
    run_side host "$round"
    run_side uuse "$round"
  fi
done

# Summary: extract transactions/s and 99th percentile per run.
echo "== summary =="
for side in uuse host; do
  for bench in point_select read_write; do
    for round in $(seq 1 "$ROUNDS"); do
      f="$R/$side-r$round-$bench.txt"
      [ -f "$f" ] || continue
      tps=$(grep -oE '\([0-9]+\.[0-9]+ per sec\.\)' "$f" | head -1 | tr -dc '0-9.')
      p99=$(grep -A 20 'Latency (ms):' "$f" | grep -oE '99th percentile: *[0-9.]+' | grep -oE '[0-9.]+$' | head -1)
      echo "$side $bench round=$round tps=$tps p99_ms=$p99"
    done
  done
done | tee "$R/summary.txt"
stop_stack
echo "PERF_AB_DONE"
