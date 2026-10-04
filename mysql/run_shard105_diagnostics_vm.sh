#!/usr/bin/env bash
# Sample UUSE SPDK device I/O counters during MySQL read and write workloads.
set -euo pipefail

BUILD="${BUILD:-/home/uuse/mysql-build}"
CONFIG="${CONFIG:-/root/code/config/uuse_mysql.conf}"
DATA="${DATA:-/home/uuse/mysql-shard104-datadir}"
RESULT="${RESULT:-/home/uuse/mysql-shard105-diagnostics}"
BIN="$BUILD/runtime_output_directory"
SOCKET="$RESULT/mysql.sock"
mkdir -p "$RESULT"
sed 's/^stats = false$/stats = true/' "$CONFIG" > "$RESULT/uuse_mysql_stats.conf"
grep -q '^stats = true$' "$RESULT/uuse_mysql_stats.conf"
export UUSE_MYSQL_CONFIG="$RESULT/uuse_mysql_stats.conf"
export UUSE_MYSQL_PREFIX="$DATA"
export LD_LIBRARY_PATH="/root/code/build-verify:${LD_LIBRARY_PATH:-}"

"$BIN/mysqld" --no-defaults --user=root --basedir="$BUILD" --datadir="$DATA" \
  --socket="$SOCKET" --pid-file="$RESULT/mysqld.pid" \
  --log-error="$RESULT/mysqld.log" --skip-networking \
  --server-id=105 --innodb-flush-log-at-trx-commit=1 \
  --innodb-use-native-aio=OFF --disabled-storage-engines=MyISAM \
  > "$RESULT/mysqld.stdout" 2>&1 &
SERVER_PID=$!

# Clean up a started server if a diagnostic command fails.
cleanup() {
  if kill -0 "$SERVER_PID" 2>/dev/null; then
    "$BIN/mysqladmin" --no-defaults --protocol=socket --socket="$SOCKET" \
      -uroot shutdown > /dev/null 2>&1 || kill -TERM "$SERVER_PID"
    wait "$SERVER_PID" || true
  fi
}
trap cleanup EXIT

for _ in $(seq 1 120); do
  if "$BIN/mysql" --no-defaults --protocol=socket --socket="$SOCKET" \
      -uroot -e 'SELECT 1' > /dev/null 2>&1; then break; fi
  kill -0 "$SERVER_PID" 2>/dev/null || {
    tail -n 40 "$RESULT/mysqld.log" >&2
    exit 1
  }
  sleep 1
done
ps -L -p "$SERVER_PID" -o tid,psr,pcpu,comm > "$RESULT/cpu-map.tsv"
for workload in oltp_read_only oltp_read_write; do
  sysbench --db-driver=mysql --mysql-user=root --mysql-socket="$SOCKET" \
    --mysql-db=uuse_perf --tables=1 --table-size=10000 --threads=8 \
    --time=5 --percentile=99 --histogram=on "$workload" run \
    > "$RESULT/${workload}.log" 2>&1
done
cleanup
trap - EXIT
grep -q 'uuse_vfs: device_io read_ops=' "$RESULT/mysqld.log"
grep 'uuse_vfs: device_io read_ops=' "$RESULT/mysqld.log" | tail -n 1
