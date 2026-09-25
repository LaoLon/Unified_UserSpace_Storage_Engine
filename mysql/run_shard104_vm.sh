#!/usr/bin/env bash
# Exercise the supported MySQL/InnoDB surface on the configured HFS NVMe.
set -euo pipefail

BUILD="${BUILD:-/home/hfs/mysql-build}"
CONFIG="${CONFIG:-/root/code/config/hfs_mysql.conf}"
HFSCTL="${HFSCTL:-/root/code/build-verify/hfsctl}"
DATA="${DATA:-/home/hfs/mysql-shard104-datadir}"
RESULT="${RESULT:-/home/hfs/mysql-shard104-results}"
BIN="$BUILD/runtime_output_directory"
MYSQLD="$BIN/mysqld"
MYSQL="$BIN/mysql"
MYSQLADMIN="$BIN/mysqladmin"
MYSQLDUMP="$BIN/mysqldump"
MYSQLCHECK="$BIN/mysqlcheck"
SOCKET="$RESULT/mysql.sock"
SERVER_PID=""

for executable in "$MYSQLD" "$MYSQL" "$MYSQLADMIN" "$MYSQLDUMP" "$MYSQLCHECK" "$HFSCTL"; do
  test -x "$executable" || { printf 'missing executable: %s\n' "$executable" >&2; exit 2; }
done
mkdir -p "$RESULT" "$DATA"
export HFS_MYSQL_CONFIG="$CONFIG" HFS_MYSQL_PREFIX="$DATA"
export LD_LIBRARY_PATH="/root/code/build-verify:${LD_LIBRARY_PATH:-}"

# Check the configured controller binding and hugepage budget before I/O.
BDF="$(awk -F= '/^[[:space:]]*pci_bdf[[:space:]]*=/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$CONFIG")"
test -n "$BDF"
test "$(basename "$(readlink -f "/sys/bus/pci/devices/$BDF/driver")")" == uio_pci_generic
test "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)" -ge 1024

# Keep the datadir empty on the host; routed data belongs only on HFS.
test -z "$(find "$DATA" -mindepth 1 -maxdepth 1 -print -quit)" || {
  printf 'host datadir is not empty: %s\n' "$DATA" >&2
  exit 2
}

# Require an explicit flag before erasing the selected test volume.
if [[ "${FORMAT_DEVICE:-0}" == 1 ]]; then
  "$HFSCTL" --config="$CONFIG" format --yes > "$RESULT/format.log" 2>&1
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

# Bootstrap the HFS-backed datadir and exercise core SQL semantics.
"$MYSQLD" --no-defaults --user=root --basedir="$BUILD" --datadir="$DATA" \
  --initialize-insecure --innodb-use-native-aio=OFF \
  --log-error="$RESULT/initialize.log" \
  > "$RESULT/initialize.stdout" 2>&1
start_server
sql -e "CREATE DATABASE hfs_matrix;
CREATE TABLE hfs_matrix.items(id INT PRIMARY KEY, v VARCHAR(80)) ENGINE=InnoDB;
INSERT INTO hfs_matrix.items VALUES (1,'alpha'),(2,'beta');
START TRANSACTION; UPDATE hfs_matrix.items SET v='changed' WHERE id=1; ROLLBACK;
SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED;
START TRANSACTION; INSERT INTO hfs_matrix.items VALUES(3,'gamma'); COMMIT;
SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ;
START TRANSACTION; UPDATE hfs_matrix.items SET v='delta' WHERE id=2; COMMIT;
ALTER TABLE hfs_matrix.items ADD INDEX idx_v(v), ALGORITHM=INPLACE, LOCK=NONE;
ALTER TABLE hfs_matrix.items ADD COLUMN tag INT DEFAULT 7, ALGORITHM=INSTANT;
DELETE FROM hfs_matrix.items WHERE id=3;
CREATE TABLE hfs_matrix.child(id INT PRIMARY KEY, parent_id INT,
  CONSTRAINT fk_parent FOREIGN KEY(parent_id) REFERENCES items(id)) ENGINE=InnoDB;
INSERT INTO hfs_matrix.child VALUES(1,1);
CREATE TABLE hfs_matrix.words(id INT PRIMARY KEY, body TEXT,
  FULLTEXT KEY ft_body(body)) ENGINE=InnoDB;
INSERT INTO hfs_matrix.words VALUES(1,'hfs fulltext smoke');
CREATE TABLE hfs_matrix.places(id INT PRIMARY KEY,
  pos POINT NOT NULL SRID 4326, SPATIAL KEY sp_pos(pos)) ENGINE=InnoDB;
INSERT INTO hfs_matrix.places VALUES(1,ST_GeomFromText('POINT(1 2)',4326));
SELECT COUNT(*),MIN(tag),MAX(tag) FROM hfs_matrix.items;
SELECT COUNT(*) FROM hfs_matrix.child;
SELECT COUNT(*) FROM hfs_matrix.words WHERE MATCH(body) AGAINST('hfs');
SELECT ST_AsText(pos) FROM hfs_matrix.places;
SHOW BINARY LOGS;" > "$RESULT/sql-matrix.tsv"
test "$(sql -e 'SELECT COUNT(*) FROM hfs_matrix.items')" == 2
test "$(sql -e 'SELECT v FROM hfs_matrix.items WHERE id=1')" == alpha
expect_rejected myisam sql -e \
  'CREATE TABLE hfs_matrix.unsupported_myisam(id INT) ENGINE=MyISAM'
expect_rejected aria sql -e \
  'CREATE TABLE hfs_matrix.unsupported_aria(id INT) ENGINE=Aria'
expect_rejected compression sql -e \
  "CREATE TABLE hfs_matrix.unsupported_compression(id INT PRIMARY KEY)
     ENGINE=InnoDB COMPRESSION='zlib';
   INSERT INTO hfs_matrix.unsupported_compression VALUES(1)"
expect_rejected clone sql -e \
  "INSTALL PLUGIN clone SONAME 'mysql_clone.so'"
"$MYSQLCHECK" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
  --check hfs_matrix > "$RESULT/mysqlcheck.log"
"$MYSQLDUMP" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
  --single-transaction hfs_matrix > "$RESULT/hfs_matrix.sql"
sql -e 'CREATE DATABASE hfs_restore'
sql hfs_restore < "$RESULT/hfs_matrix.sql"
test "$(sql -e 'SELECT COUNT(*) FROM hfs_restore.items')" == 2
stop_server

# Reopen in a new process and check the durable contents and HFS metadata.
start_server
test "$(sql -e 'SELECT COUNT(*) FROM hfs_matrix.items')" == 2
test "$(sql -e 'SELECT COUNT(*) FROM hfs_restore.items')" == 2
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
  HFS_MYSQL_CONFIG="$RESULT/missing.conf" \
  HFS_MYSQL_PREFIX="$RESULT/invalid-config-host" \
  "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" \
  --datadir="$RESULT/invalid-config-host" --initialize-insecure \
  --innodb-use-native-aio=OFF
test -z "$(find "$RESULT/invalid-config-host" -mindepth 1 -print -quit)"
"$HFSCTL" --config="$CONFIG" check > "$RESULT/hfs-check.log" 2>&1
grep -q 'corruption=none' "$RESULT/hfs-check.log"
printf 'Shard-104 functional matrix passed\n'
