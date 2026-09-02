#!/bin/bash
# MariaDB smoke test on HFS (shard-24).
#
# mariadbd --initialize-insecure initializes the datadir in-process
# (single process), then mariadbd runs in the foreground as one process
# with threads; the mysql/mysqladmin clients talk over a kernel socket
# and never touch /hfs, so they run without LD_PRELOAD.
#
# Usage: bash scripts/db/mysql_smoke.sh [libhfs.so path] [mount prefix]
set -euo pipefail

LIB="${1:-$PWD/build-cmake/libhfs.so}"
PREFIX="${2:-/hfs}"
HFS_LIB="$(realpath "$LIB")"
DATADIR="$PREFIX/mysql"
SOCK="/tmp/hfs-mysql.sock"
PORT=3307

if [ ! -f "$HFS_LIB" ]; then
	echo "ERROR: libhfs.so not found: $HFS_LIB" >&2
	exit 1
fi

echo "== MariaDB smoke test on HFS =="
echo "  lib:     $HFS_LIB"
echo "  datadir: $DATADIR"

# --- initialize datadir (mariadb-install-db execs mariadbd --bootstrap,
# which inherits LD_PRELOAD and initializes in-process) ---
# HFS_AUTO_MKDIRS makes libhfs create the datadir right after mount,
# since mariadbd expects it to exist and cross-process sharing of the
# malloc bdev is impossible.
echo "-- install-db --"
env LD_PRELOAD="$HFS_LIB" HFS_AUTO_MKDIRS=/mysql \
	mariadb-install-db --datadir="$DATADIR" --user=root \
	--auth-root-authentication-method=normal \
	--log-error=/tmp/hfs-mysqld-init.err 2>&1 | tail -3

# --- start mariadbd in the foreground ---
echo "-- start --"
env LD_PRELOAD="$HFS_LIB" HFS_AUTO_MKDIRS=/mysql \
	mariadbd --datadir="$DATADIR" --socket="$SOCK" --port="$PORT" \
	--bind-address=127.0.0.1 --skip-networking=0 \
	--pid-file=/tmp/hfs-mysqld.pid --log-error=/tmp/hfs-mysqld.err \
	--user=root &
MYSQLD_PID=$!

# --- wait for readiness ---
READY=0
for i in $(seq 1 60); do
	if mysqladmin --socket="$SOCK" -uroot ping >/dev/null 2>&1; then
		READY=1
		break
	fi
	sleep 1
done
if [ "$READY" != 1 ]; then
	echo "ERROR: mariadbd did not become ready" >&2
	tail -20 /tmp/hfs-mysqld.err >&2 || true
	kill "$MYSQLD_PID" 2>/dev/null || true
	exit 1
fi
echo "  ready (pid $MYSQLD_PID)"

# --- create table, insert, query ---
mysql --socket="$SOCK" -uroot -e \
	"create table t(a int); insert into t values(1),(2),(3);" 
RESULT=$(mysql --socket="$SOCK" -uroot -N -e "select count(*), sum(a) from t;")
echo "  query result: $RESULT"
[ "$RESULT" = "3	6" ] || {
	echo "ERROR: unexpected query result: $RESULT" >&2
	kill "$MYSQLD_PID" 2>/dev/null || true
	exit 1
}

# --- shutdown ---
mysqladmin --socket="$SOCK" -uroot shutdown
wait "$MYSQLD_PID" || true
echo "MYSQL SMOKE OK"