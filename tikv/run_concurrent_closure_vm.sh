#!/bin/bash
# Shard-118 multi-threaded load closure: concurrent SQL writers/readers through
# TiDB on UUSE, graceful SIGTERM stop, restart, data and closure checks.
# Requires the same lab prerequisites as tikv/run_functional_matrix_vm.sh.
set -u
export LD_LIBRARY_PATH=/root/code/build-uuse
BASE=/build/tidb
UUSECTL=/root/code/build-uuse/uusectl
CONF=/root/code/config/uuse_nvme.conf
TIKV=/build/tikv-build/release/tikv-server
PD=$BASE/bin/pd-server
TIDB=$BASE/bin/tidb-server
MYSQL=${MYSQL:-/home/uuse/mysql-build/runtime_output_directory/mysql}
RESULT=$BASE/closure-$(date +%Y%m%d-%H%M%S)
PASS=0; FAIL=0
mkdir -p "$RESULT"

log() { echo "$@" | tee -a "$RESULT/closure.log"; }
sql() { "$MYSQL" --no-defaults -h127.0.0.1 -P4000 -uroot -N -B -e "$1" 2>&1; }
wait_tikv() { for i in $(seq 1 90); do curl -s http://127.0.0.1:20180/status >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }

start_stack() {
  local mode="${1:-fresh}"
  pkill -9 -f tikv-server 2>/dev/null || true
  pkill -9 -f 'tidb-server --store=tikv' 2>/dev/null || true
  pkill -9 -f 'pd-server --name=pd1' 2>/dev/null || true
  sleep 3
  [ "$mode" = fresh ] && rm -rf $BASE/pd-data
  setsid nohup "$PD" --name=pd1 --data-dir=$BASE/pd-data \
    --client-urls=http://127.0.0.1:2379 --peer-urls=http://127.0.0.1:2380 \
    --log-file=$BASE/logs/pd.log >$BASE/logs/pd.stdout 2>&1 &
  for i in $(seq 1 30); do curl -s http://127.0.0.1:2379/pd/api/v1/health >/dev/null 2>&1 && break; sleep 1; done
  if [ "$mode" = fresh ]; then
    rm -rf $BASE/tikv-data; mkdir -p $BASE/tikv-data
    "$UUSECTL" --config="$CONF" format --yes >/tmp/closure-format.log 2>&1 || { log "FAIL format"; exit 1; }
  fi
  setsid nohup "$TIKV" --config $BASE/tikv-uuse-both.toml --pd-endpoints 127.0.0.1:2379 \
    >>$BASE/logs/tikv.stdout 2>&1 &
  wait_tikv || { log "FAIL tikv up ($mode)"; exit 1; }
  setsid nohup "$TIDB" --store=tikv --path=127.0.0.1:2379 --host=127.0.0.1 -P 4000 --status=10080 \
    --log-file=$BASE/logs/tidb.log >>$BASE/logs/tidb.stdout 2>&1 &
  for i in $(seq 1 60); do curl -s http://127.0.0.1:10080/status >/dev/null 2>&1 && break; sleep 1; done
}

log "== Shard-118 concurrent closure: $RESULT =="
start_stack fresh
log "stack up"
sql "SET GLOBAL tidb_ddl_enable_fast_reorg = OFF;" >/dev/null
sql "CREATE DATABASE IF NOT EXISTS mt;" >/dev/null
sql "USE mt; CREATE TABLE load_t (id INT PRIMARY KEY AUTO_INCREMENT, worker INT, v VARCHAR(64), n INT);" >/dev/null

# Eight writers insert batches in a loop; two readers keep scans in flight.
# Collect the load job PIDs explicitly: `wait $(jobs -p)` evaluates to an
# empty list in a command substitution (non-interactive shell), which makes
# `wait` block on every child, including the long-running PD/TiKV/TiDB.
load_pids=""
for w in $(seq 1 8); do
  (
    end=$((SECONDS + 45))
    batch=0
    while [ $SECONDS -lt $end ]; do
      batch=$((batch + 1))
      values=$(seq 1 20 | awk -v w=$w -v b=$batch '{printf "(%d,\"w%d-%d-%d\",%d),", w*1000000+b*100+$1, w, b, $1, $1}')
      "$MYSQL" --no-defaults -h127.0.0.1 -P4000 -uroot -N -B \
        -e "USE mt; INSERT INTO load_t (worker,v,n) VALUES ${values%,};" >/dev/null 2>&1 || break
    done
  ) &
  load_pids="$load_pids $!"
done
for r in 1 2; do
  (
    end=$((SECONDS + 45))
    while [ $SECONDS -lt $end ]; do
      "$MYSQL" --no-defaults -h127.0.0.1 -P4000 -uroot -N -B \
        -e "USE mt; SELECT COUNT(*) FROM load_t;" >/dev/null 2>&1 || break
    done
  ) &
  load_pids="$load_pids $!"
done
wait $load_pids 2>/dev/null
rows=$(sql "USE mt; SELECT COUNT(*) FROM load_t;" | tr -d '[:space:]')
log "load done: rows=$rows"
if [ "${rows:-0}" -gt 0 ]; then log "PASS load.concurrent (rows=$rows)"; PASS=$((PASS+1)); else log "FAIL load.concurrent"; FAIL=$((FAIL+1)); fi

# Graceful SIGTERM while the engine is idle after load.
tikv_pid=$(pgrep -f 'tikv-server --config' | head -1)
kill -TERM "$tikv_pid" 2>/dev/null || true
for i in $(seq 1 60); do pgrep -f 'tikv-server --config' >/dev/null || break; sleep 1; done
if pgrep -f 'tikv-server --config' >/dev/null; then
  log "FAIL stop.graceful (still running)"; FAIL=$((FAIL+1))
else
  log "PASS stop.graceful (exited)"; PASS=$((PASS+1))
fi
if grep -q 'starting graceful shutdown' $BASE/logs/tikv-uuse-both.log; then
  log "PASS stop.graceful_path"; PASS=$((PASS+1))
else
  log "FAIL stop.graceful_path"; FAIL=$((FAIL+1))
fi
# Engine closure stats are printed by an in-process Engine::stop(); TiKV exits
# via process::exit after its graceful shutdown, so the shim's engine state is
# never destroyed and the line is informational only (the lifecycle test covers
# the in-process stop path).
closure=$(grep 'async closure:' $BASE/logs/tikv.stdout | tail -1)
log "INFO engine.closure (${closure:-not printed on process exit})"

# Restart and verify persistence, then uusectl check.
start_stack reopen
restored=$(sql "USE mt; SELECT COUNT(*) FROM load_t;" | tr -d '[:space:]')
if [ "$restored" = "$rows" ]; then log "PASS restart.persist (rows=$restored)"; PASS=$((PASS+1)); else log "FAIL restart.persist (expected=$rows actual=$restored)"; FAIL=$((FAIL+1)); fi

pkill -9 -f tikv-server 2>/dev/null || true
pkill -9 -f 'tidb-server --store=tikv' 2>/dev/null || true
pkill -9 -f 'pd-server --name=pd1' 2>/dev/null || true
sleep 3
"$UUSECTL" --config="$CONF" check >$RESULT/uusectl-check.log 2>&1
if grep -q 'corruption=none' $RESULT/uusectl-check.log; then log "PASS uusectl.check"; PASS=$((PASS+1)); else log "FAIL uusectl.check"; FAIL=$((FAIL+1)); fi

log "== summary: PASS=$PASS FAIL=$FAIL =="
echo "PASS=$PASS FAIL=$FAIL" > $RESULT/closure-summary.txt
echo "SHARD118_CLOSURE_DONE"
