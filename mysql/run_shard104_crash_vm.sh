#!/usr/bin/env bash
# Recover HFS-backed InnoDB after writes and an online DDL interrupted by SIGKILL.
set -euo pipefail

BUILD="${BUILD:-/home/hfs/mysql-build}"
CONFIG="${CONFIG:-/root/code/config/hfs_mysql.conf}"
DATA="${DATA:-/home/hfs/mysql-shard104-datadir}"
RESULT="${RESULT:-/home/hfs/mysql-shard104-crash}"
HFSCTL="${HFSCTL:-/root/code/build-verify/hfsctl}"
BIN="$BUILD/runtime_output_directory"
SOCKET="$RESULT/mysql.sock"
SERVER_PID=""
mkdir -p "$RESULT"
export HFS_MYSQL_CONFIG="$CONFIG" HFS_MYSQL_PREFIX="$DATA"
export LD_LIBRARY_PATH="/root/code/build-verify:${LD_LIBRARY_PATH:-}"

sql() {
  "$BIN/mysql" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
    --batch --skip-column-names "$@"
}

start_server() {
  local run="$1"
  "$BIN/mysqld" --no-defaults --user=root --basedir="$BUILD" --datadir="$DATA" \
    --socket="$SOCKET" --pid-file="$RESULT/mysqld.pid" \
    --log-error="$RESULT/${run}-mysqld.log" --skip-networking \
    --log-bin="$DATA/mysql-bin" --server-id=104 \
    --innodb-flush-log-at-trx-commit=1 --innodb-use-native-aio=OFF \
    --disabled-storage-engines=MyISAM > "$RESULT/${run}-stdout.log" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 120); do
    if sql -e 'SELECT 1' > /dev/null 2>&1; then return; fi
    kill -0 "$SERVER_PID" 2>/dev/null || {
      tail -n 50 "$RESULT/${run}-mysqld.log" >&2
      return 1
    }
    sleep 1
  done
  return 1
}

stop_server() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    "$BIN/mysqladmin" --no-defaults --protocol=socket --socket="$SOCKET" \
      -uroot shutdown > /dev/null
    wait "$SERVER_PID"
  fi
  SERVER_PID=""
}
trap 'stop_server || true' EXIT

# A larger table keeps index creation active long enough to interrupt it.
start_server prepare
sql -e "CREATE DATABASE IF NOT EXISTS hfs_matrix"
sql -e "CREATE TABLE IF NOT EXISTS hfs_matrix.crash_data(
  id INT AUTO_INCREMENT PRIMARY KEY, payload VARCHAR(120) NOT NULL)
  ENGINE=InnoDB"
if [[ "$(sql -e 'SELECT COUNT(*) FROM hfs_matrix.crash_data')" == 0 ]]; then
  sql -e "INSERT INTO hfs_matrix.crash_data(payload)
    VALUES(REPEAT('x',100))"
  for _ in $(seq 1 17); do
    sql -e "INSERT INTO hfs_matrix.crash_data(payload)
      SELECT payload FROM hfs_matrix.crash_data" > /dev/null
  done
fi
EXPECTED="$(sql -e 'SELECT COUNT(*) FROM hfs_matrix.crash_data')"
test "$EXPECTED" -ge 131072
stop_server

# Interrupt an online index build while another client changes rows.
start_server online-ddl
sql -e "ALTER TABLE hfs_matrix.crash_data ADD INDEX idx_payload(payload),
  ALGORITHM=INPLACE, LOCK=NONE" > "$RESULT/ddl-client.log" 2>&1 &
DDL_PID=$!
(
  for i in $(seq 1 200); do
    sql -e "UPDATE hfs_matrix.crash_data SET payload=CONCAT('u', $i)
      WHERE id BETWEEN 1 AND 100" > /dev/null 2>&1 || break
  done
) &
WRITER_PID=$!
sleep 0.2
kill -0 "$DDL_PID" 2>/dev/null || {
  printf 'online DDL finished before the crash point\n' >&2
  exit 1
}
kill -KILL "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
wait "$DDL_PID" 2>/dev/null || true
wait "$WRITER_PID" 2>/dev/null || true
printf 'online DDL was active at SIGKILL\n' > "$RESULT/ddl-crash-point.log"

# InnoDB must recover the committed rows and permit further writes.
start_server ddl-recovery
test "$(sql -e 'SELECT COUNT(*) FROM hfs_matrix.crash_data')" == "$EXPECTED"
sql -e 'CHECK TABLE hfs_matrix.crash_data' > "$RESULT/ddl-check-table.tsv"
grep -q $'\tOK$' "$RESULT/ddl-check-table.tsv"
sql -e "UPDATE hfs_matrix.crash_data SET payload='recovered' WHERE id=1"
stop_server
"$HFSCTL" --config="$CONFIG" check > "$RESULT/ddl-hfs-check.log" 2>&1
grep -q 'corruption=none' "$RESULT/ddl-hfs-check.log"

# Interrupt repeated data-page updates to exercise doublewrite recovery.
start_server torn-page
(
  for i in $(seq 1 400); do
    sql -e "UPDATE hfs_matrix.crash_data
      SET payload=CONCAT('page', $i, REPEAT('x',80))
      WHERE id BETWEEN 1000 AND 3000" > /dev/null 2>&1 || break
  done
) &
WRITER_PID=$!
sleep 0.5
kill -0 "$WRITER_PID" 2>/dev/null || {
  printf 'page writer finished before the crash point\n' >&2
  exit 1
}
kill -KILL "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
wait "$WRITER_PID" 2>/dev/null || true
printf 'page writer was active at SIGKILL\n' > "$RESULT/page-crash-point.log"
start_server page-recovery
test "$(sql -e 'SELECT COUNT(*) FROM hfs_matrix.crash_data')" == "$EXPECTED"
sql -e 'CHECK TABLE hfs_matrix.crash_data' > "$RESULT/page-check-table.tsv"
grep -q $'\tOK$' "$RESULT/page-check-table.tsv"
stop_server
"$HFSCTL" --config="$CONFIG" check > "$RESULT/page-hfs-check.log" 2>&1
grep -q 'corruption=none' "$RESULT/page-hfs-check.log"
printf 'Shard-104 crash matrix passed, rows=%s\n' "$EXPECTED"
