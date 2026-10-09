#!/bin/bash
# Shard-117: shared-engine bring-up (both kvdb and raft-engine on UUSE).
set -u
export LD_LIBRARY_PATH=/root/code/build-uuse
BASE=/build/tidb
UUSECTL=/root/code/build-uuse/uusectl
CONF=/root/code/config/uuse_nvme.conf
TIKV=/build/tikv-build/release/tikv-server
PD=$BASE/bin/pd-server
MYSQL=/home/uuse/mysql-build/runtime_output_directory/mysql

echo "== hugepages/driver =="
grep HugePages_Total /proc/meminfo
basename "$(readlink -f /sys/bus/pci/devices/0000:13:00.0/driver)"

pkill -9 -f tikv-server 2>/dev/null || true
pkill -9 -f 'tidb-server --store=tikv' 2>/dev/null || true
pkill -9 -f 'pd-server --name=pd1' 2>/dev/null || true
sleep 3
rm -rf $BASE/pd-data
setsid nohup "$PD" --name=pd1 --data-dir=$BASE/pd-data \
  --client-urls=http://127.0.0.1:2379 --peer-urls=http://127.0.0.1:2380 \
  --log-file=$BASE/logs/pd.log >$BASE/logs/pd.stdout 2>&1 &
for i in $(seq 1 30); do
  curl -s http://127.0.0.1:2379/pd/api/v1/health >/dev/null 2>&1 && { echo "pd up after ${i}s"; break; }
  sleep 1
done

rm -rf $BASE/tikv-data
mkdir -p $BASE/tikv-data
"$UUSECTL" --config="$CONF" format --yes >/tmp/shared-format.log 2>&1
echo "format rc=$?"

setsid nohup "$TIKV" --config $BASE/tikv-uuse-both.toml --pd-endpoints 127.0.0.1:2379 \
  >$BASE/logs/tikv-shared.stdout 2>&1 &
for i in $(seq 1 60); do
  curl -s http://127.0.0.1:20180/status >/dev/null 2>&1 && { echo "tikv up after ${i}s"; break; }
  sleep 1
done
if ! curl -s http://127.0.0.1:20180/status >/dev/null 2>&1; then
  echo "-- tikv failed, log tail --"
  tail -8 $BASE/logs/tikv-uuse-both.log
  echo "-- stdout tail --"
  tail -10 $BASE/logs/tikv-shared.stdout
  echo SHARED_FAIL
  exit 1
fi
echo "-- uuse engine lines (expect ONE engine) --"
grep -c 'uuse: engine created' $BASE/logs/tikv-shared.stdout 2>/dev/null || true

setsid nohup $BASE/bin/tidb-server --store=tikv --path=127.0.0.1:2379 \
  --host=127.0.0.1 -P 4000 --status=10080 \
  --log-file=$BASE/logs/tidb.log >$BASE/logs/tidb.stdout 2>&1 &
for i in $(seq 1 60); do
  curl -s http://127.0.0.1:10080/status >/dev/null 2>&1 && { echo "tidb up after ${i}s"; break; }
  sleep 1
done

sql() { "$MYSQL" --no-defaults -h127.0.0.1 -P4000 -uroot -e "$1"; }
echo "== SQL write =="
sql "CREATE DATABASE IF NOT EXISTS uuse_tidb; USE uuse_tidb; CREATE TABLE IF NOT EXISTS t (id INT PRIMARY KEY, v VARCHAR(64)); REPLACE INTO t VALUES (1,'alpha'),(2,'beta'),(3,'gamma'); SELECT COUNT(*) AS n FROM t;"
echo "sql rc=$?"

echo "== kill -9 TiKV, restart =="
TIKV_PID=$(pgrep -f 'tikv-server --config' | head -1)
kill -9 "$TIKV_PID" 2>/dev/null || true
sleep 3
setsid nohup "$TIKV" --config $BASE/tikv-uuse-both.toml --pd-endpoints 127.0.0.1:2379 \
  >>$BASE/logs/tikv-shared.stdout 2>&1 &
for i in $(seq 1 90); do
  curl -s http://127.0.0.1:20180/status >/dev/null 2>&1 && { echo "tikv back after ${i}s"; break; }
  sleep 1
done
echo "== SQL after restart =="
sql "USE uuse_tidb; SELECT COUNT(*) AS n, GROUP_CONCAT(v ORDER BY id) AS vals FROM t;"
echo "sql rc=$?"

echo "== host data dirs (expect no engine data) =="
ls -la $BASE/tikv-data/db 2>/dev/null | head -5
ls -la $BASE/tikv-data/raft-engine 2>/dev/null | head -5

echo "== stop TiKV and uusectl check =="
pkill -9 -f tikv-server 2>/dev/null || true
sleep 3
"$UUSECTL" --config="$CONF" check >/tmp/shared-check.log 2>&1
echo "check rc=$?"
grep -E 'corruption' /tmp/shared-check.log | head -3
echo "SHARED_BRINGUP_DONE"
