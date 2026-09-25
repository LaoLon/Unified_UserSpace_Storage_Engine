#!/usr/bin/env bash
# Compare the same MySQL binary in alternating HFS and native sessions.
set -euo pipefail

BUILD="${BUILD:-/home/hfs/mysql-build}"
HFS_CONFIG="${HFS_CONFIG:-/root/code/config/hfs_mysql.conf}"
HFS_DATA="${HFS_DATA:-/home/hfs/mysql-shard104-datadir}"
NATIVE_DATA="${NATIVE_DATA:-/home/hfs/mysql-native-datadir}"
RESULT="${RESULT:-/home/hfs/mysql-shard105-results}"
BIN="$BUILD/runtime_output_directory"
MYSQLD="$BIN/mysqld"
MYSQL="$BIN/mysql"
MYSQLADMIN="$BIN/mysqladmin"
MYSQLSLAP="$BIN/mysqlslap"
SOCKET="$RESULT/mysql.sock"
SERVER_PID=""

for executable in "$MYSQLD" "$MYSQL" "$MYSQLADMIN" "$MYSQLSLAP" sysbench; do
  command -v "$executable" > /dev/null || { printf 'missing: %s\n' "$executable" >&2; exit 2; }
done
mkdir -p "$RESULT" "$NATIVE_DATA"
export LD_LIBRARY_PATH="/root/code/build-verify:${LD_LIBRARY_PATH:-}"

# Initialize the native comparator only once so repeated sessions reuse data.
if [[ ! -f "$NATIVE_DATA/auto.cnf" ]]; then
  env -u HFS_MYSQL_CONFIG -u HFS_MYSQL_PREFIX \
    "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" \
    --datadir="$NATIVE_DATA" --initialize-insecure \
    --log-error="$RESULT/native-initialize.log" \
    > "$RESULT/native-initialize.stdout" 2>&1
fi

sql() {
  "$MYSQL" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
    --batch --skip-column-names "$@"
}

stop_server() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    "$MYSQLADMIN" --no-defaults --protocol=socket --socket="$SOCKET" -uroot shutdown
    wait "$SERVER_PID"
  fi
  SERVER_PID=""
}
trap 'stop_server || true' EXIT

start_server() {
  local mode="$1" run="$2" data
  if [[ "$mode" == hfs ]]; then
    data="$HFS_DATA"
    export HFS_MYSQL_CONFIG="$HFS_CONFIG" HFS_MYSQL_PREFIX="$HFS_DATA"
  else
    data="$NATIVE_DATA"
    unset HFS_MYSQL_CONFIG HFS_MYSQL_PREFIX
  fi
  "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" --datadir="$data" \
    --socket="$SOCKET" --pid-file="$RESULT/mysqld.pid" \
    --log-error="$RESULT/${run}-mysqld.log" --skip-networking \
    --server-id=105 --innodb-flush-log-at-trx-commit=1 \
    --innodb-use-native-aio=OFF --disabled-storage-engines=MyISAM \
    > "$RESULT/${run}-mysqld.stdout" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 120); do
    if sql -e 'SELECT 1' > /dev/null 2>&1; then return; fi
    kill -0 "$SERVER_PID" 2>/dev/null || {
      tail -n 40 "$RESULT/${run}-mysqld.log" >&2
      return 1
    }
    sleep 1
  done
  tail -n 40 "$RESULT/${run}-mysqld.log" >&2
  return 1
}

sysbench_mysql() {
  sysbench --db-driver=mysql --mysql-user=root --mysql-socket="$SOCKET" \
    --mysql-db=hfs_perf --tables=1 --table-size=10000 "$@"
}

# Alternate modes to expose session drift while keeping both datasets stable.
for run in hfs-1 native-1 hfs-2 native-2; do
  mode="${run%-*}"
  start_server "$mode" "$run"
  sql -e 'CREATE DATABASE IF NOT EXISTS hfs_perf'
  if ! sql -e "SHOW TABLES FROM hfs_perf LIKE 'sbtest1'" | grep -q sbtest1; then
    sysbench_mysql oltp_read_write prepare > "$RESULT/${run}-prepare.log" 2>&1
  fi
  sql -e "SHOW VARIABLES LIKE 'innodb_flush_log_at_trx_commit';
    SHOW GLOBAL STATUS LIKE 'Innodb_os_log_fsyncs'" \
    > "$RESULT/${run}-durability-before.tsv"
  ps -L -p "$SERVER_PID" -o tid,psr,pcpu,comm \
    > "$RESULT/${run}-cpu-map.tsv"
  "$MYSQLSLAP" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
    --create-schema=hfs_perf --query='SELECT k FROM sbtest1 WHERE id=1' \
    --concurrency=8 --number-of-queries=10000 --iterations=3 \
    > "$RESULT/${run}-mysqlslap-read.log" 2>&1
  "$MYSQLSLAP" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
    --create-schema=hfs_perf \
    --query='UPDATE sbtest1 SET k=k+1 WHERE id=FLOOR(RAND()*10000)+1' \
    --concurrency=8 --number-of-queries=3000 --iterations=3 \
    > "$RESULT/${run}-mysqlslap-write.log" 2>&1
  sysbench_mysql --threads=8 --time=10 --percentile=99 --histogram=on \
    oltp_read_only run > "$RESULT/${run}-sysbench-read.log" 2>&1
  sysbench_mysql --threads=8 --time=10 --percentile=99 --histogram=on \
    oltp_read_write run > "$RESULT/${run}-sysbench-write.log" 2>&1
  sql -e "SHOW GLOBAL STATUS LIKE 'Innodb_os_log_fsyncs'" \
    > "$RESULT/${run}-durability-after.tsv"
  stop_server
  printf '%s complete\n' "$run"
done
