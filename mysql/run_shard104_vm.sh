#!/usr/bin/env bash
# Exercise the supported MySQL/InnoDB surface on the configured UUSE NVMe.
set -euo pipefail

BUILD="${BUILD:-/home/uuse/mysql-build}"
CONFIG="${CONFIG:-/root/code/config/uuse_mysql.conf}"
UUSECTL="${UUSECTL:-/root/code/build-verify/uusectl}"
DATA="${DATA:-/home/uuse/mysql-shard104-datadir}"
RESULT="${RESULT:-/home/uuse/mysql-shard104-results}"
BIN="$BUILD/runtime_output_directory"
MYSQLD="$BIN/mysqld"
MYSQL="$BIN/mysql"
MYSQLADMIN="$BIN/mysqladmin"
MYSQLDUMP="$BIN/mysqldump"
MYSQLCHECK="$BIN/mysqlcheck"
SOCKET="$RESULT/mysql.sock"
SERVER_PID=""

for executable in "$MYSQLD" "$MYSQL" "$MYSQLADMIN" "$MYSQLDUMP" "$MYSQLCHECK" "$UUSECTL"; do
  test -x "$executable" || { printf 'missing executable: %s\n' "$executable" >&2; exit 2; }
done
mkdir -p "$RESULT" "$DATA"
export UUSE_MYSQL_CONFIG="$CONFIG" UUSE_MYSQL_PREFIX="$DATA"
export LD_LIBRARY_PATH="/root/code/build-verify:${LD_LIBRARY_PATH:-}"

# Check the configured controller binding and hugepage budget before I/O.
BDF="$(awk -F= '/^[[:space:]]*pci_bdf[[:space:]]*=/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$CONFIG")"
test -n "$BDF"
test "$(basename "$(readlink -f "/sys/bus/pci/devices/$BDF/driver")")" == uio_pci_generic
test "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)" -ge 1024

# Keep the datadir empty on the host; routed data belongs only on UUSE.
test -z "$(find "$DATA" -mindepth 1 -maxdepth 1 -print -quit)" || {
  printf 'host datadir is not empty: %s\n' "$DATA" >&2
  exit 2
}

# Require an explicit flag before erasing the selected test volume.
if [[ "${FORMAT_DEVICE:-0}" == 1 ]]; then
  "$UUSECTL" --config="$CONFIG" format --yes > "$RESULT/format.log" 2>&1
fi

sql() {
  "$MYSQL" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
    --batch --skip-column-names "$@"
}

expect_rejected() {
  local label="$1"
  shift
  if "$@" > "$RESULT/reject-${label}.log" 2>&1; then
    printf 'unsupported operation succeeded: %s\n' "$label" >&2
    return 1
  fi
  grep -qiE 'error|unsupported|disable|unknown|cannot|rejected' \
    "$RESULT/reject-${label}.log"
}

start_server() {
  "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" --datadir="$DATA" \
    --socket="$SOCKET" --pid-file="$RESULT/mysqld.pid" \
    --log-error="$RESULT/mysqld.log" --skip-networking \
    --log-bin="$DATA/mysql-bin" --server-id=104 \
    --innodb-flush-log-at-trx-commit=1 --innodb-use-native-aio=OFF \
    --disabled-storage-engines=MyISAM > "$RESULT/mysqld.stdout" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 120); do
    if sql -e 'SELECT 1' > /dev/null 2>&1; then
      printf 'server ready pid=%s\n' "$SERVER_PID"
      return
    fi
    kill -0 "$SERVER_PID" 2>/dev/null || {
      tail -n 50 "$RESULT/mysqld.log" >&2
      return 1
    }
    sleep 1
  done
  tail -n 50 "$RESULT/mysqld.log" >&2
  return 1
}

stop_server() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    "$MYSQLADMIN" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
      shutdown > "$RESULT/shutdown.log" 2>&1
    wait "$SERVER_PID"
  fi
  SERVER_PID=""
}

cleanup() {
  stop_server || true
}
trap cleanup EXIT

# Bootstrap the UUSE-backed datadir and exercise core SQL semantics.
"$MYSQLD" --no-defaults --user=root --basedir="$BUILD" --datadir="$DATA" \
  --initialize-insecure --innodb-use-native-aio=OFF \
  --log-error="$RESULT/initialize.log" \
  > "$RESULT/initialize.stdout" 2>&1
start_server
sql -e "CREATE DATABASE uuse_matrix;
CREATE TABLE uuse_matrix.items(id INT PRIMARY KEY, v VARCHAR(80)) ENGINE=InnoDB;
INSERT INTO uuse_matrix.items VALUES (1,'alpha'),(2,'beta');
START TRANSACTION; UPDATE uuse_matrix.items SET v='changed' WHERE id=1; ROLLBACK;
SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED;
START TRANSACTION; INSERT INTO uuse_matrix.items VALUES(3,'gamma'); COMMIT;
SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ;
START TRANSACTION; UPDATE uuse_matrix.items SET v='delta' WHERE id=2; COMMIT;
ALTER TABLE uuse_matrix.items ADD INDEX idx_v(v), ALGORITHM=INPLACE, LOCK=NONE;
ALTER TABLE uuse_matrix.items ADD COLUMN tag INT DEFAULT 7, ALGORITHM=INSTANT;
DELETE FROM uuse_matrix.items WHERE id=3;
CREATE TABLE uuse_matrix.child(id INT PRIMARY KEY, parent_id INT,
  CONSTRAINT fk_parent FOREIGN KEY(parent_id) REFERENCES items(id)) ENGINE=InnoDB;
INSERT INTO uuse_matrix.child VALUES(1,1);
CREATE TABLE uuse_matrix.words(id INT PRIMARY KEY, body TEXT,
  FULLTEXT KEY ft_body(body)) ENGINE=InnoDB;
INSERT INTO uuse_matrix.words VALUES(1,'uuse fulltext smoke');
CREATE TABLE uuse_matrix.places(id INT PRIMARY KEY,
  pos POINT NOT NULL SRID 4326, SPATIAL KEY sp_pos(pos)) ENGINE=InnoDB;
INSERT INTO uuse_matrix.places VALUES(1,ST_GeomFromText('POINT(1 2)',4326));
SELECT COUNT(*),MIN(tag),MAX(tag) FROM uuse_matrix.items;
SELECT COUNT(*) FROM uuse_matrix.child;
SELECT COUNT(*) FROM uuse_matrix.words WHERE MATCH(body) AGAINST('uuse');
SELECT ST_AsText(pos) FROM uuse_matrix.places;
SHOW BINARY LOGS;" > "$RESULT/sql-matrix.tsv"
test "$(sql -e 'SELECT COUNT(*) FROM uuse_matrix.items')" == 2
test "$(sql -e 'SELECT v FROM uuse_matrix.items WHERE id=1')" == alpha
expect_rejected myisam sql -e \
  'CREATE TABLE uuse_matrix.unsupported_myisam(id INT) ENGINE=MyISAM'
expect_rejected aria sql -e \
  'CREATE TABLE uuse_matrix.unsupported_aria(id INT) ENGINE=Aria'
expect_rejected compression sql -e \
  "CREATE TABLE uuse_matrix.unsupported_compression(id INT PRIMARY KEY)
     ENGINE=InnoDB COMPRESSION='zlib';
   INSERT INTO uuse_matrix.unsupported_compression VALUES(1)"
expect_rejected clone sql -e \
  "INSTALL PLUGIN clone SONAME 'mysql_clone.so'"
"$MYSQLCHECK" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
  --check uuse_matrix > "$RESULT/mysqlcheck.log"
"$MYSQLDUMP" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
  --single-transaction uuse_matrix > "$RESULT/uuse_matrix.sql"
sql -e 'CREATE DATABASE uuse_restore'
sql uuse_restore < "$RESULT/uuse_matrix.sql"
test "$(sql -e 'SELECT COUNT(*) FROM uuse_restore.items')" == 2
stop_server

# Reopen in a new process and check the durable contents and UUSE metadata.
start_server
test "$(sql -e 'SELECT COUNT(*) FROM uuse_matrix.items')" == 2
test "$(sql -e 'SELECT COUNT(*) FROM uuse_restore.items')" == 2
stop_server
expect_rejected daemonize "$MYSQLD" --no-defaults --user=root \
  --basedir="$BUILD" --datadir="$DATA" --daemonize --validate-config \
  --innodb-use-native-aio=OFF --disabled-storage-engines=MyISAM
expect_rejected missing-myisam-flag "$MYSQLD" --no-defaults --user=root \
  --basedir="$BUILD" --datadir="$DATA" --validate-config \
  --innodb-use-native-aio=OFF
expect_rejected native-aio "$MYSQLD" --no-defaults --user=root \
  --basedir="$BUILD" --datadir="$DATA" --socket="$RESULT/reject-aio.sock" \
  --skip-networking --innodb-use-native-aio=ON \
  --disabled-storage-engines=MyISAM
mkdir -p "$RESULT/invalid-config-host"
expect_rejected invalid-config env \
  UUSE_MYSQL_CONFIG="$RESULT/missing.conf" \
  UUSE_MYSQL_PREFIX="$RESULT/invalid-config-host" \
  "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" \
  --datadir="$RESULT/invalid-config-host" --initialize-insecure \
  --innodb-use-native-aio=OFF
test -z "$(find "$RESULT/invalid-config-host" -mindepth 1 -print -quit)"
"$UUSECTL" --config="$CONFIG" check > "$RESULT/uuse-check.log" 2>&1
grep -q 'corruption=none' "$RESULT/uuse-check.log"
printf 'Shard-104 functional matrix passed\n'
