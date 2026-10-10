#!/bin/bash
# Shard-117 single-node TiKV/UUSE bring-up (self-contained lab runbook, host4).
# Topology: PD + TiKV (kvdb and raft-engine share one UUSE engine) + TiDB.
# Requires: 0000:13:00.0 bound to uio_pci_generic, hugepages=1024, UUSE build
# at /root/code/build-uuse, PD/TiDB binaries under /build/tidb/bin.
set -u
export LD_LIBRARY_PATH=/root/code/build-uuse
BASE=/build/tidb
UUSECTL=/root/code/build-uuse/uusectl
CONF=/root/code/config/uuse_nvme.conf
TIKV=/build/tikv-build/release/tikv-server
PD=$BASE/bin/pd-server
TIDB=$BASE/bin/tidb-server
MYSQL=${MYSQL:-/home/uuse/mysql-build/runtime_output_directory/mysql}

mkdir -p $BASE/logs/rocksdb $BASE/tikv-data

cat >$BASE/tikv-uuse-both.toml <<'EOF'
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

[log]
level = "info"
file.filename = "/build/tidb/logs/tikv-uuse-both.log"

[rocksdb]
info-log-dir = "/build/tidb/logs/rocksdb"
defaultcf.block-cache-size = "256MB"
writecf.block-cache-size = "128MB"
lockcf.block-cache-size = "64MB"

[raftdb]
info-log-dir = "/build/tidb/logs/rocksdb"
defaultcf.block-cache-size = "64MB"
EOF

cat >$BASE/tikv-uuse.toml <<'EOF'
[server]
addr = "127.0.0.1:20160"
status-addr = "127.0.0.1:20180"

[storage]
data-dir = "/build/tidb/tikv-data"
reserve-space = "0GB"
reserve-raft-space = "0GB"

[raft-engine]
dir = "/build/tidb/tikv-data/raft-engine"
uuse-conf = "/root/code/config/uuse_nvme.conf"

[log]
level = "info"
file.filename = "/build/tidb/logs/tikv-uuse.log"

[rocksdb]
info-log-dir = "/build/tidb/logs/rocksdb"
defaultcf.block-cache-size = "256MB"
writecf.block-cache-size = "128MB"
lockcf.block-cache-size = "64MB"

[raftdb]
info-log-dir = "/build/tidb/logs/rocksdb"
defaultcf.block-cache-size = "64MB"
EOF

echo "== preflight =="
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
"$UUSECTL" --config="$CONF" format --yes >/tmp/tikv-uuse-format.log 2>&1
echo "format rc=$?"

setsid nohup "$TIKV" --config $BASE/tikv-uuse-both.toml --pd-endpoints 127.0.0.1:2379 \
  >$BASE/logs/tikv.stdout 2>&1 &
for i in $(seq 1 60); do
  curl -s http://127.0.0.1:20180/status >/dev/null 2>&1 && { echo "tikv up after ${i}s"; break; }
  sleep 1
done
if ! curl -s http://127.0.0.1:20180/status >/dev/null 2>&1; then
  echo "-- tikv failed --"
  tail -8 $BASE/logs/tikv-uuse-both.log
  echo BRINGUP_FAIL
  exit 1
fi
echo "uuse engines: $(grep -c 'uuse: engine created' $BASE/logs/tikv.stdout 2>/dev/null || echo 0)"

setsid nohup "$TIDB" --store=tikv --path=127.0.0.1:2379 \
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
  >>$BASE/logs/tikv.stdout 2>&1 &
for i in $(seq 1 90); do
  curl -s http://127.0.0.1:20180/status >/dev/null 2>&1 && { echo "tikv back after ${i}s"; break; }
  sleep 1
done
echo "== SQL after restart =="
sql "USE uuse_tidb; SELECT COUNT(*) AS n, GROUP_CONCAT(v ORDER BY id) AS vals FROM t;"
echo "sql rc=$?"

echo "== host files under tikv-data (expect only last_tikv.toml) =="
find $BASE/tikv-data -maxdepth 2 -type f | sort
echo "== host dirs =="
ls $BASE/tikv-data

echo "== stop TiKV and uusectl check =="
pkill -9 -f tikv-server 2>/dev/null || true
sleep 3
"$UUSECTL" --config="$CONF" check >/tmp/tikv-uuse-check.log 2>&1
echo "check rc=$?"
grep -E 'corruption' /tmp/tikv-uuse-check.log | head -3
echo "BRINGUP_DONE"
