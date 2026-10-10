#!/bin/bash
# Shard-119 functional acceptance matrix (lab runbook, host4).
# Topology: PD + TiKV (kvdb and raft-engine share one UUSE engine) + TiDB.
# Covers: CRUD, DDL (classic backfill; fast-reorg ingest is not UUSE-routed yet,
# see audit row 6), transactions, ADMIN CHECK, region split, GC, kill -9/SIGTERM
# restart, cross-process reopen, dumpling backup/restore, fail-closed probes
# (second instance, encryption), final uusectl check.
# Requires: run_single_node_vm.sh has created $BASE/tikv-uuse-both.toml; UUSE
# build at /root/code/build-uuse; PD/TiDB/dumpling under /build/tidb/bin.
set -u
export LD_LIBRARY_PATH=/root/code/build-uuse
BASE=/build/tidb
UUSECTL=/root/code/build-uuse/uusectl
CONF=/root/code/config/uuse_nvme.conf
TIKV=/build/tikv-build/release/tikv-server
PD=$BASE/bin/pd-server
TIDB=$BASE/bin/tidb-server
DUMPLING=$BASE/bin/dumpling
MYSQL=${MYSQL:-/home/uuse/mysql-build/runtime_output_directory/mysql}
RESULT=$BASE/functional-matrix-$(date +%Y%m%d-%H%M%S)
PASS=0; FAIL=0
mkdir -p "$RESULT"

for f in "$TIKV" "$PD" "$TIDB" "$DUMPLING" "$MYSQL" "$UUSECTL" "$BASE/tikv-uuse-both.toml"; do
  [ -e "$f" ] || { echo "missing: $f (run run_single_node_vm.sh first)"; exit 1; }
done

log() { echo "$@" | tee -a "$RESULT/matrix.log"; }
expect() {
  if [ "$2" = "$3" ]; then log "PASS $1"; PASS=$((PASS+1)); else log "FAIL $1: expected='$2' actual='$3'"; FAIL=$((FAIL+1)); fi
}
expect_fail() {
  local label="$1"; shift
  if "$@" >/dev/null 2>&1; then log "FAIL $label (expected failure)"; FAIL=$((FAIL+1)); else log "PASS $label (rejected)"; PASS=$((PASS+1)); fi
}
sql() { "$MYSQL" --no-defaults -h127.0.0.1 -P4000 -uroot -N -B -e "$1" 2>&1; }
sql_file() { "$MYSQL" --no-defaults -h127.0.0.1 -P4000 -uroot "$@"; }
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
    "$UUSECTL" --config="$CONF" format --yes >/tmp/matrix-format.log 2>&1 || { log "FAIL format"; exit 1; }
  fi
  setsid nohup "$TIKV" --config $BASE/tikv-uuse-both.toml --pd-endpoints 127.0.0.1:2379 \
    >>$BASE/logs/tikv.stdout 2>&1 &
  wait_tikv || { log "FAIL tikv up ($mode)"; exit 1; }
  setsid nohup "$TIDB" --store=tikv --path=127.0.0.1:2379 --host=127.0.0.1 -P 4000 --status=10080 \
    --log-file=$BASE/logs/tidb.log >>$BASE/logs/tidb.stdout 2>&1 &
  for i in $(seq 1 60); do curl -s http://127.0.0.1:10080/status >/dev/null 2>&1 && break; sleep 1; done
}

log "== Shard-119 functional matrix: $RESULT =="
start_stack fresh
log "stack up"
# Fast-reorg ADD INDEX uses the SST importer (host-FS written SSTs + engine
# ingest); the ingest path is not UUSE-routed yet, so the matrix uses the
# classic backfill and records the ingest boundary (audit row 6).
sql "SET GLOBAL tidb_ddl_enable_fast_reorg = OFF;" >/dev/null
sql "SET GLOBAL tidb_enable_dist_task = OFF;" >/dev/null

log "--- CRUD ---"
sql "CREATE DATABASE IF NOT EXISTS mt;" >/dev/null
sql "USE mt; CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(64), n INT, KEY idx_n (n));" >/dev/null
seq 1 100 | awk '{printf "(%d,\"v%d\",%d),",$1,$1,$1%10}' | sed 's/,$//' > $RESULT/insert-values.sql
sql "USE mt; INSERT INTO t (id,v,n) VALUES $(cat $RESULT/insert-values.sql);" >/dev/null
expect "crud.count" "100" "$(sql 'USE mt; SELECT COUNT(*) FROM t;')"
expect "crud.point" "v42" "$(sql 'USE mt; SELECT v FROM t WHERE id=42;')"
sql "USE mt; UPDATE t SET v='x' WHERE id=42;" >/dev/null
expect "crud.update" "x" "$(sql 'USE mt; SELECT v FROM t WHERE id=42;')"
sql "USE mt; DELETE FROM t WHERE id=42;" >/dev/null
expect "crud.delete" "99" "$(sql 'USE mt; SELECT COUNT(*) FROM t;')"
expect "crud.index" "10" "$(sql 'USE mt; SELECT COUNT(*) FROM t WHERE n=7;')"

log "--- DDL ---"
sql "USE mt; ALTER TABLE t ADD COLUMN extra INT DEFAULT 0;" >/dev/null
expect "ddl.add_column" "0" "$(sql 'USE mt; SELECT extra FROM t WHERE id=1;')"
sql "USE mt; ALTER TABLE t ADD INDEX idx_v (v);" >/dev/null
expect "ddl.add_index" "1" "$(sql "SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema='mt' AND table_name='t' AND index_name='idx_v';")"
sql "USE mt; ALTER TABLE t DROP INDEX idx_n;" >/dev/null
expect "ddl.drop_index" "0" "$(sql "SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema='mt' AND table_name='t' AND index_name='idx_n';")"

log "--- transactions ---"
expect "txn.rollback" "99" "$(sql 'USE mt; BEGIN; INSERT INTO t (id,v,n) VALUES (1000,"r",0); ROLLBACK; SELECT COUNT(*) FROM t;')"
expect "txn.commit" "100" "$(sql 'USE mt; BEGIN; INSERT INTO t (id,v,n) VALUES (1000,"c",0); COMMIT; SELECT COUNT(*) FROM t;')"
expect "txn.pessimistic" "2" "$(sql 'USE mt; BEGIN PESSIMISTIC; UPDATE t SET n=n+1 WHERE id=1; COMMIT; SELECT n FROM t WHERE id=1;')"
expect_fail "txn.duplicate_key" sql 'USE mt; INSERT INTO t (id,v,n) VALUES (1,"dup",0);'

log "--- ADMIN CHECK ---"
if sql_file -e "USE mt; ADMIN CHECK TABLE t;" >/dev/null 2>&1; then log "PASS admin.check"; PASS=$((PASS+1)); else log "FAIL admin.check"; FAIL=$((FAIL+1)); fi

log "--- region split ---"
sql "USE mt; CREATE TABLE IF NOT EXISTS big (id INT PRIMARY KEY, v VARCHAR(64));" >/dev/null
seq 1 5000 | awk '{printf "(%d,\"%s\"),",$1,"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"}' | sed 's/,$//' > $RESULT/big-values.sql
sql "USE mt; REPLACE INTO big (id,v) VALUES $(cat $RESULT/big-values.sql);" >/dev/null
before=$(sql "SELECT COUNT(*) FROM information_schema.tikv_region_status WHERE table_name='big';" | tr -d '[:space:]')
sql "USE mt; SPLIT TABLE big BETWEEN (0) AND (5000) REGIONS 4;" >/dev/null 2>&1
rcount=0
for i in $(seq 1 15); do
  rcount=$(sql "SELECT COUNT(*) FROM information_schema.tikv_region_status WHERE table_name='big';" | tr -d '[:space:]')
  [ "${rcount:-0}" -gt "${before:-0}" ] && break
  sleep 1
done
if [ "${rcount:-0}" -gt "${before:-0}" ]; then
  log "PASS region.split (before=$before after=$rcount)"; PASS=$((PASS+1))
else
  log "FAIL region.split (before=$before after=$rcount)"; FAIL=$((FAIL+1))
fi

log "--- GC ---"
gc_enable=$(sql "SHOW GLOBAL VARIABLES LIKE 'tidb_gc_enable';" | awk '{print $2}')
gc_safe=$(sql "SELECT VARIABLE_VALUE FROM mysql.tidb WHERE VARIABLE_NAME='tikv_gc_safe_point';")
expect "gc.enable" "ON" "$gc_enable"
if [ -n "$gc_safe" ]; then log "PASS gc.safe_point ($gc_safe)"; PASS=$((PASS+1)); else log "FAIL gc.safe_point"; FAIL=$((FAIL+1)); fi

log "--- restart (kill -9 / SIGTERM) ---"
kill -9 "$(pgrep -f 'tikv-server --config' | head -1)" 2>/dev/null || true
sleep 3
setsid nohup "$TIKV" --config $BASE/tikv-uuse-both.toml --pd-endpoints 127.0.0.1:2379 >>$BASE/logs/tikv.stdout 2>&1 &
if wait_tikv; then log "PASS restart.kill9_up"; PASS=$((PASS+1)); else log "FAIL restart.kill9_up"; FAIL=$((FAIL+1)); fi
expect "restart.kill9_data" "100" "$(sql 'USE mt; SELECT COUNT(*) FROM t;')"
kill -TERM "$(pgrep -f 'tikv-server --config' | head -1)" 2>/dev/null || true
sleep 8
setsid nohup "$TIKV" --config $BASE/tikv-uuse-both.toml --pd-endpoints 127.0.0.1:2379 >>$BASE/logs/tikv.stdout 2>&1 &
if wait_tikv; then log "PASS restart.sigterm_up"; PASS=$((PASS+1)); else log "FAIL restart.sigterm_up"; FAIL=$((FAIL+1)); fi
expect "restart.sigterm_data" "100" "$(sql 'USE mt; SELECT COUNT(*) FROM t;')"

log "--- cross-process reopen ---"
start_stack reopen
expect "reopen.data" "100" "$(sql 'USE mt; SELECT COUNT(*) FROM t;')"

log "--- backup / restore (dumpling) ---"
"$DUMPLING" -h 127.0.0.1 -P 4000 -uroot -o $RESULT/dump -B mt --filetype sql --threads 4 \
  --logfile $RESULT/dumpling.log >/dev/null 2>&1
if [ -d $RESULT/dump ] && ls $RESULT/dump/*.sql >/dev/null 2>&1; then log "PASS backup.dumpling"; PASS=$((PASS+1)); else log "FAIL backup.dumpling"; FAIL=$((FAIL+1)); fi
sql "USE mt; DROP TABLE t;" >/dev/null
sql "CREATE DATABASE IF NOT EXISTS mt;" >/dev/null
# LC_ALL=C: locale collation ignores '-' vs '.', which can sort the data file
# (mt.t.000000000.sql) before its schema (mt.t-schema.sql). Byte order keeps
# every '*-schema.sql' ahead of the matching '*.NNN.sql'.
for f in $(ls $RESULT/dump/*.sql | LC_ALL=C sort); do
  sql_file -D mt < "$f" >/dev/null 2>&1 || true
done
restored=0
for i in $(seq 1 15); do
  restored=$(sql 'USE mt; SELECT COUNT(*) FROM t;' | tr -d '[:space:]')
  [ "${restored:-0}" = "100" ] && break
  sleep 1
done
expect "restore.count" "100" "$restored"

log "--- fail-closed probes ---"
setsid nohup "$TIKV" --config $BASE/tikv-uuse-both.toml --pd-endpoints 127.0.0.1:2379 \
  --addr 127.0.0.1:20161 --status-addr 127.0.0.1:20181 >$RESULT/second-instance.log 2>&1 &
sleep 20
tail -200 $BASE/logs/tikv-uuse-both.log > $RESULT/second-instance-tail.log 2>/dev/null || true
if pgrep -f 'addr 127.0.0.1:20161' >/dev/null; then
  log "FAIL probe.second_instance (still running)"; FAIL=$((FAIL+1))
  pkill -9 -f 'addr 127.0.0.1:20161' 2>/dev/null || true
elif grep -qiE 'busy|already|resourcebusy|another engine|lock .*failed|uuse env create failed' \
    $RESULT/second-instance.log $RESULT/second-instance-tail.log 2>/dev/null; then
  log "PASS probe.second_instance (fail-closed)"; PASS=$((PASS+1))
else
  log "FAIL probe.second_instance (unexpected)"; FAIL=$((FAIL+1))
fi

pkill -9 -f tikv-server 2>/dev/null || true
sleep 3
cat >$RESULT/tikv-enc.toml <<'EOF'
[server]
addr = "127.0.0.1:20160"
status-addr = "127.0.0.1:20180"
[storage]
data-dir = "/build/tidb/tikv-data"
uuse-conf = "/root/code/config/uuse_nvme.conf"
reserve-space = "0GB"
reserve-raft-space = "0GB"
[raft-engine]
dir = "/build/tidb/tikv-data/raft-engine"
uuse-conf = "/root/code/config/uuse_nvme.conf"
[security.encryption]
data-encryption-method = "aes128-ctr"
[log]
level = "info"
file.filename = "/build/tidb/logs/tikv-enc.log"
EOF
setsid nohup "$TIKV" --config $RESULT/tikv-enc.toml --pd-endpoints 127.0.0.1:2379 >$RESULT/enc.log 2>&1 &
sleep 20
if pgrep -f 'tikv-enc.toml' >/dev/null; then
  log "FAIL probe.encryption (started)"; FAIL=$((FAIL+1)); pkill -9 -f 'tikv-enc.toml' 2>/dev/null || true
elif grep -qiE 'encryption|unsupported|not.?supported' $RESULT/enc.log $BASE/logs/tikv-enc.log 2>/dev/null; then
  log "PASS probe.encryption (fail-closed)"; PASS=$((PASS+1))
else
  log "FAIL probe.encryption (unexpected)"; FAIL=$((FAIL+1))
fi

log "--- final uusectl check ---"
pkill -9 -f tikv-server 2>/dev/null || true
pkill -9 -f 'tidb-server --store=tikv' 2>/dev/null || true
pkill -9 -f 'pd-server --name=pd1' 2>/dev/null || true
sleep 3
"$UUSECTL" --config="$CONF" check >$RESULT/uusectl-check.log 2>&1
if grep -q 'corruption=none' $RESULT/uusectl-check.log; then log "PASS uusectl.check"; PASS=$((PASS+1)); else log "FAIL uusectl.check"; FAIL=$((FAIL+1)); fi

log "== summary: PASS=$PASS FAIL=$FAIL =="
echo "PASS=$PASS FAIL=$FAIL" > $RESULT/matrix-summary.txt
echo "SHARD119_DONE"
