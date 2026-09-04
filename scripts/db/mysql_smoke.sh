#!/usr/bin/env bash
# Validate MariaDB initialization, SQL operations, restart, and recovery on HFS.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build-cmake/libhfs.so}"
PREFIX="${2:-/hfs}"
CONFIG="${3:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${4:-/var/tmp/hfs-mysql.img}"
RUN_DIR="${5:-/var/tmp/hfs-mysql}"
HFS_LIB="$(realpath "$LIB")"
DATADIR="$PREFIX/mysql"
SHADOW_ROOT=""
SHADOW_DATADIR=""
SOCK="$RUN_DIR/mariadb.sock"
PID_FILE="$RUN_DIR/mariadbd.pid"
LOCK_PATH="$RUN_DIR/hfs.lock"
INIT_LOG="$RUN_DIR/install-db.log"
SERVER_LOG="$RUN_DIR/mariadbd.log"
TX_LOG="$RUN_DIR/uncommitted.log"
MYSQLD_PID=""
INIT_PID=""

if [[ ! -f "$HFS_LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [mount prefix] [config] [image] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"
SHADOW_ROOT="$(mktemp -d "$RUN_DIR/datadir-shadow.XXXXXX")"
SHADOW_DATADIR="$SHADOW_ROOT/mysql"
mkdir -p "$SHADOW_DATADIR"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G
rm -f "$SOCK" "$PID_FILE" "$INIT_LOG" "$SERVER_LOG" "$TX_LOG"

# Run one HFS process against the persistent Aio backend.
run_hfs() {
	env LD_PRELOAD="$HFS_LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" "$@"
}

# Stop the current server through its SQL socket, with a kill fallback.
stop_server() {
	if [[ -z "$MYSQLD_PID" ]]; then
		return 0
	fi
	if kill -0 "$MYSQLD_PID" 2>/dev/null; then
		if ! mysqladmin --protocol=SOCKET --socket="$SOCK" -uroot shutdown \
			>/dev/null 2>&1; then
			kill "$MYSQLD_PID" 2>/dev/null || true
		fi
	fi
	wait "$MYSQLD_PID" 2>/dev/null || true
	MYSQLD_PID=""
}

# Reap the bootstrap process group so a failed helper cannot retain HFS locks.
stop_bootstrap() {
	if [[ -n "$INIT_PID" ]] && kill -0 "$INIT_PID" 2>/dev/null; then
		kill -KILL -- -"$INIT_PID" 2>/dev/null || true
		wait "$INIT_PID" 2>/dev/null || true
	fi
	INIT_PID=""
}

# Always release the server and temporary socket on test exit.
cleanup() {
	stop_bootstrap
	stop_server
	rm -f "$SOCK" "$PID_FILE"
	if [[ -n "$SHADOW_ROOT" && "$SHADOW_ROOT" == "$RUN_DIR"/datadir-shadow.* &&
		"$SHADOW_ROOT" != "/" ]]; then
		rm -rf -- "$SHADOW_ROOT"
	fi
}
trap cleanup EXIT INT TERM

# Start MariaDB in the foreground so the shell owns its exact process id.
start_server() {
	rm -f "$SOCK" "$PID_FILE"
	setsid env LD_PRELOAD="$HFS_LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_AUTO_MKDIRS=/mysql \
		mariadbd \
		--datadir="$DATADIR" --socket="$SOCK" --port=3307 \
		--bind-address=127.0.0.1 --skip-networking=0 \
		--pid-file="$PID_FILE" --log-error="$SERVER_LOG" --console \
		--user=root --skip-name-resolve --innodb-use-native-aio=0 \
		--innodb-flush-method=fsync >"$SERVER_LOG" 2>&1 &
	MYSQLD_PID=$!
	for _ in $(seq 1 90); do
		if mysqladmin --protocol=SOCKET --socket="$SOCK" -uroot ping \
			>/dev/null 2>&1; then
			echo "  ready (pid $MYSQLD_PID)"
			return 0
		fi
		if ! kill -0 "$MYSQLD_PID" 2>/dev/null; then
			echo "ERROR: mariadbd exited before readiness" >&2
			tail -80 "$SERVER_LOG" >&2 || true
			return 1
		fi
		sleep 1
	done
	echo "ERROR: mariadbd did not become ready" >&2
	tail -80 "$SERVER_LOG" >&2 || true
	return 1
}

# Execute SQL through a client that never loads libhfs.
sql() {
	mysql --protocol=SOCKET --socket="$SOCK" -uroot --batch --skip-column-names "$@"
}

# Check one scalar query result and include the query in a useful failure.
assert_query() {
	local query="$1"
	local expected="$2"
	local actual
	actual="$(sql -e "$query")"
	if [[ "$actual" != "$expected" ]]; then
		echo "ERROR: query result mismatch: expected '$expected', got '$actual'" >&2
		echo "  query: $query" >&2
		return 1
	fi
}

echo "== MariaDB persistent HFS smoke test =="
echo "  lib:     $HFS_LIB"
echo "  datadir: $DATADIR"
echo "  image:   $IMAGE"

echo "-- initialize persistent datadir --"
set +e
setsid env -u LD_PRELOAD MYSQLD_BOOTSTRAP="$ROOT/scripts/db/mariadb_bootstrap_wrapper.sh" \
	HFS_LIB="$HFS_LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
	HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 \
	HFS_AUTO_MKDIRS=/mysql HFS_VIRTUAL_DATADIR="$DATADIR" \
	HFS_MYSQLD_BIN="$(command -v mariadbd)" \
	mariadb-install-db --datadir="$SHADOW_DATADIR" \
		--auth-root-authentication-method=normal --skip-test-db \
		--skip-name-resolve --innodb-use-native-aio=0 \
		--innodb-flush-method=fsync >"$INIT_LOG" 2>&1 &
INIT_PID=$!
wait "$INIT_PID"
INIT_RC=$?
set -e
if [[ "$INIT_RC" -ne 0 ]]; then
	stop_bootstrap
	echo "ERROR: mariadb-install-db failed" >&2
	tail -120 "$INIT_LOG" >&2 || true
	exit 1
fi
INIT_PID=""
tail -12 "$INIT_LOG"

echo "-- start and exercise SQL --"
start_server
sql <<'SQL'
CREATE DATABASE hfsdb;
USE hfsdb;
CREATE TABLE t(id INT PRIMARY KEY, value VARCHAR(64));
INSERT INTO t VALUES (1, 'one'), (2, 'two'), (3, 'three');
UPDATE t SET value = 'two-updated' WHERE id = 2;
DELETE FROM t WHERE id = 3;
START TRANSACTION;
INSERT INTO t VALUES (4, 'rollback');
ROLLBACK;
START TRANSACTION;
INSERT INTO t VALUES (5, 'committed');
COMMIT;
CREATE INDEX value_idx ON t(value);
CREATE TABLE big(id INT PRIMARY KEY, payload LONGTEXT);
INSERT INTO big VALUES (1, REPEAT('x', 1048576));
SQL
TEMP_RESULT="$(sql <<'SQL'
USE hfsdb;
CREATE TEMPORARY TABLE temp_t(value INT);
INSERT INTO temp_t VALUES (1), (2);
SELECT COUNT(*) FROM temp_t;
SQL
)"
[[ "$TEMP_RESULT" == "2" ]] || {
	echo "ERROR: temporary table result mismatch: $TEMP_RESULT" >&2
	exit 1
}
assert_query "SELECT COUNT(*) FROM hfsdb.t" "3"
assert_query "SELECT SUM(id) FROM hfsdb.t" "8"
assert_query "SELECT OCTET_LENGTH(payload) FROM hfsdb.big" "1048576"

echo "-- clean shutdown and restart --"
stop_server
start_server
assert_query "SELECT COUNT(*) FROM hfsdb.t" "3"
assert_query "SELECT OCTET_LENGTH(payload) FROM hfsdb.big" "1048576"

echo "-- SIGKILL with committed and uncommitted work --"
sql -e "INSERT INTO hfsdb.t VALUES (6, 'before-crash');"
(
	sql <<'SQL'
USE hfsdb;
START TRANSACTION;
INSERT INTO t VALUES (7, 'uncommitted');
SELECT SLEEP(60);
SQL
) >"$TX_LOG" 2>&1 &
TX_PID=$!
sleep 2
if ! kill -0 "$TX_PID" 2>/dev/null; then
	echo "ERROR: transaction client exited before SIGKILL" >&2
	tail -40 "$TX_LOG" >&2 || true
	exit 1
fi
kill -KILL -- -"$MYSQLD_PID"
wait "$MYSQLD_PID" 2>/dev/null || true
MYSQLD_PID=""
wait "$TX_PID" 2>/dev/null || true

start_server
assert_query "SELECT COUNT(*) FROM hfsdb.t WHERE id = 6" "1"
assert_query "SELECT COUNT(*) FROM hfsdb.t WHERE id = 7" "0"
assert_query "SELECT COUNT(*) FROM hfsdb.t" "4"
stop_server

echo "-- short concurrent client load --"
if command -v sysbench >/dev/null 2>&1; then
	start_server
	sysbench oltp_read_only --db-driver=mysql \
		--mysql-socket="$SOCK" --mysql-user=root --mysql-db=hfsdb \
		--tables=1 --table-size=100 prepare
	for threads in 4 16; do
		echo "  sysbench threads=$threads"
		sysbench oltp_read_only --db-driver=mysql \
			--mysql-socket="$SOCK" --mysql-user=root --mysql-db=hfsdb \
			--tables=1 --table-size=100 --threads="$threads" \
			--time=60 --report-interval=0 run
	done
	sysbench oltp_read_only --db-driver=mysql \
		--mysql-socket="$SOCK" --mysql-user=root --mysql-db=hfsdb \
		--tables=1 --table-size=100 cleanup
	stop_server
else
	echo "  sysbench is not installed; using mysqlslap fallback" >&2
	start_server
	for threads in 4 16; do
		echo "  mysqlslap concurrency=$threads"
		mysqlslap --protocol=SOCKET --socket="$SOCK" -uroot \
			--concurrency="$threads" --iterations=3 --create-schema=hfsdb \
			--query="SELECT COUNT(*) FROM hfsdb.t" >/dev/null
	done
	stop_server
fi

echo "MYSQL PERSISTENT RECOVERY OK"
