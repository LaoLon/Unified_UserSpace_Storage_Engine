#!/usr/bin/env bash
# Measure MariaDB read/write QPS on native ext4 and the HFS persistent Aio path.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build-cmake/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-stage2-mysql.img}"
OUT="${4:-$ROOT/build/performance-stage2-mysql}"
COUNT="${HFS_MYSQLSLAP_QUERIES:-2000}"
ITERATIONS="${HFS_MYSQLSLAP_ITERATIONS:-3}"
HFS_BDEV_TYPE="${HFS_MYSQL_BDEV_TYPE:-persistent-aio}"
HFS_BDEV_NAME="${HFS_MYSQL_BDEV_NAME:-Aio0}"
BOOTSTRAP_SIGNAL_TIMEOUT="${HFS_MYSQL_BOOTSTRAP_SIGNAL_TIMEOUT:-0}"
SPDK_PCI_BDF="${HFS_MYSQL_SPDK_PCI_BDF:-}"
HFS_LIB="$(realpath "$LIB")"
HFS_PREFIX="/hfs"
HFS_DATADIR="$HFS_PREFIX/mysql"
HFS_SOCK="$OUT/hfs.sock"
NATIVE_DATADIR="$OUT/native-datadir"
NATIVE_SOCK="$OUT/native.sock"
HFS_PID=""
NATIVE_PID=""

if [[ ! -f "$HFS_LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [output dir]" >&2
	exit 2
fi
mkdir -p "$OUT"
rm -f "$OUT/results.tsv" "$OUT"/*.log "$HFS_SOCK" "$NATIVE_SOCK"
EFFECTIVE_CONFIG="$OUT/hfs_db_aio.json"
if [[ "$HFS_BDEV_TYPE" == "direct-nvme" ]]; then
	EFFECTIVE_CONFIG="$CONFIG"
else
	sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" "$CONFIG" > "$EFFECTIVE_CONFIG"
	HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G
fi

cleanup() {
	if [[ -n "$HFS_PID" ]]; then
		if ! timeout 10 mysqladmin --protocol=SOCKET --socket="$HFS_SOCK" -uroot shutdown >/dev/null 2>&1; then
			kill -TERM -- -"$HFS_PID" 2>/dev/null || kill -TERM "$HFS_PID" 2>/dev/null || true
			sleep 1
			kill -KILL -- -"$HFS_PID" 2>/dev/null || true
		fi
		wait "$HFS_PID" 2>/dev/null || true
	fi
	if [[ -n "$NATIVE_PID" ]]; then
		mysqladmin --protocol=SOCKET --socket="$NATIVE_SOCK" -uroot shutdown >/dev/null 2>&1 || kill "$NATIVE_PID" 2>/dev/null || true
		wait "$NATIVE_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

wait_ready() {
	local socket="$1" pid="$2" log="$3"
	for _ in $(seq 1 90); do
		if mysqladmin --protocol=SOCKET --socket="$socket" -uroot ping >/dev/null 2>&1; then
			return 0
		fi
		if ! kill -0 "$pid" 2>/dev/null; then
			tail -80 "$log" >&2 || true
			return 1
		fi
		sleep 1
	done
	tail -80 "$log" >&2 || true
	return 1
}

start_hfs() {
	rm -f "$HFS_SOCK"
	setsid env LD_PRELOAD="$HFS_LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME="$HFS_BDEV_NAME" HFS_LOCK_PATH="$OUT/hfs.lock" HFS_AUTO_MKDIRS=/mysql \
		mariadbd --datadir="$HFS_DATADIR" --socket="$HFS_SOCK" --port=3307 \
			--bind-address=127.0.0.1 --skip-networking=0 --pid-file="$OUT/hfs.pid" \
			--log-error="$OUT/hfs-server.log" --console --user=root --skip-name-resolve \
			--innodb-use-native-aio=0 --innodb-flush-method=fsync >"$OUT/hfs-server.log" 2>&1 &
	HFS_PID=$!
	wait_ready "$HFS_SOCK" "$HFS_PID" "$OUT/hfs-server.log"
}

start_native() {
	rm -f "$NATIVE_SOCK"
	setsid env -u LD_PRELOAD -u HFS_CONFIG_PATH -u HFS_BDEV_NAME -u HFS_LOCK_PATH \
		mariadbd --datadir="$NATIVE_DATADIR" --socket="$NATIVE_SOCK" --port=3308 \
			--bind-address=127.0.0.1 --skip-networking=0 --pid-file="$OUT/native.pid" \
			--log-error="$OUT/native-server.log" --console --user=root --skip-name-resolve \
			--innodb-use-native-aio=0 --innodb-flush-method=fsync >"$OUT/native-server.log" 2>&1 &
	NATIVE_PID=$!
	wait_ready "$NATIVE_SOCK" "$NATIVE_PID" "$OUT/native-server.log"
}

seed_db() {
	local socket="$1"
	mysql --protocol=SOCKET --socket="$socket" -uroot <<'SQL'
CREATE DATABASE bench;
CREATE TABLE bench.t(id INT PRIMARY KEY, value INT NOT NULL) ENGINE=InnoDB;
SQL
	python3 - "$OUT/seed.sql" <<'PYEOF'
import sys
path = sys.argv[1]
with open(path, "w", encoding="utf-8") as stream:
    for base in range(1, 10001, 100):
        values = ",".join(f"({item},0)" for item in range(base, min(base + 100, 10001)))
        stream.write(f"INSERT INTO bench.t(id,value) VALUES {values};\n")
PYEOF
	mysql --protocol=SOCKET --socket="$socket" -uroot < "$OUT/seed.sql"
}

run_case() {
	local layer="$1" socket="$2" workload="$3" threads="$4" output avg qps
	output="$OUT/${layer}_${workload}_${threads}.log"
	if [[ "$workload" == "read" ]]; then
		query="SELECT SUM(value) FROM bench.t WHERE id BETWEEN 1 AND 10000"
	else
		query="UPDATE bench.t SET value=value+1 WHERE id=1"
	fi
	mysqlslap --protocol=SOCKET --socket="$socket" -uroot \
		--concurrency="$threads" --iterations="$ITERATIONS" \
		--number-of-queries="$COUNT" --create-schema=bench \
		--query="$query" >"$output" 2>&1
	avg="$(awk '/Average number of seconds to run all queries:/ {print $(NF-1); exit}' "$output")"
	qps="$(python3 - "$COUNT" "$avg" <<'PYEOF'
import sys
count = float(sys.argv[1])
seconds = float(sys.argv[2])
print(f"{count / seconds:.6f}")
PYEOF
)"
	printf "%s\t%s\t%s\t%s\t%s\n" "$layer" "$workload" "$threads" "$avg" "$qps" >> "$OUT/results.tsv"
}

printf "layer\tworkload\tconcurrency\tavg_seconds\tqps\n" > "$OUT/results.tsv"
{
	echo "timestamp=$(date -Is)"
	echo "hostname=$(hostname)"
	echo "hfs_lib=$HFS_LIB"
	echo "config=$EFFECTIVE_CONFIG"
	echo "backend=$HFS_BDEV_TYPE"
	echo "bdev_name=$HFS_BDEV_NAME"
	echo "bootstrap_signal_timeout=$BOOTSTRAP_SIGNAL_TIMEOUT"
	echo "lock_wait_seconds=${HFS_MYSQL_LOCK_WAIT_SECONDS:-15}"
	echo "spdk_pci_bdf=$SPDK_PCI_BDF"
	echo "image=$IMAGE"
	echo "queries=$COUNT"
	echo "iterations=$ITERATIONS"
	echo "workload=read:SUM range; write:single-row autocommit UPDATE"
} > "$OUT/environment.txt"

echo "== MariaDB stage-two native ext4 =="
rm -rf -- "$NATIVE_DATADIR"
mkdir -p "$NATIVE_DATADIR"
mariadb-install-db --datadir="$NATIVE_DATADIR" --auth-root-authentication-method=normal \
	--skip-test-db --skip-name-resolve --innodb-use-native-aio=0 \
	--innodb-flush-method=fsync >"$OUT/native-install.log" 2>&1
start_native
seed_db "$NATIVE_SOCK"
for threads in 1 4 16; do
	run_case native "$NATIVE_SOCK" read "$threads"
	run_case native "$NATIVE_SOCK" write "$threads"
done
mysqladmin --protocol=SOCKET --socket="$NATIVE_SOCK" -uroot shutdown >/dev/null
wait "$NATIVE_PID" 2>/dev/null || true
NATIVE_PID=""

echo "== MariaDB stage-two HFS backend=$HFS_BDEV_TYPE =="
set +e
setsid env -u LD_PRELOAD MYSQLD_BOOTSTRAP="$ROOT/scripts/db/mariadb_bootstrap_wrapper.sh" \
	HFS_LIB="$HFS_LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" HFS_BDEV_NAME="$HFS_BDEV_NAME" \
	HFS_LOCK_PATH="$OUT/hfs.lock" HFS_FORMAT=1 HFS_AUTO_MKDIRS=/mysql \
	HFS_VIRTUAL_DATADIR="$HFS_DATADIR" HFS_MYSQLD_BIN="$(command -v mariadbd)" \
	mariadb-install-db --datadir="$OUT/datadir-shadow" --auth-root-authentication-method=normal \
		--skip-test-db --skip-name-resolve --innodb-use-native-aio=0 \
		--innodb-flush-method=fsync >"$OUT/hfs-install.log" 2>&1 &
bootstrap_pid=$!
bootstrap_forced_stop=0
if [[ "$BOOTSTRAP_SIGNAL_TIMEOUT" == "1" ]]; then
	for _ in $(seq 1 180); do
		if grep -q 'Signal handler thread did not exit in a timely manner' "$OUT/hfs-install.log" 2>/dev/null; then
			# MariaDB has already committed the system tables; stop its known shutdown hang.
			kill -TERM -- -"$bootstrap_pid" 2>/dev/null || true
			sleep 1
			kill -KILL -- -"$bootstrap_pid" 2>/dev/null || true
			bootstrap_forced_stop=1
			break
		fi
		if ! kill -0 "$bootstrap_pid" 2>/dev/null; then
			break
		fi
		sleep 1
	done
fi
set +e
wait "$bootstrap_pid"
bootstrap_rc=$?
set -e
if [[ "$bootstrap_forced_stop" == "1" ]]; then
	bootstrap_rc=0
	if [[ -n "$SPDK_PCI_BDF" ]]; then
		# A SIGKILLed SPDK owner cannot unlink its PCI lock; no peer may be active here.
		rm -f -- "/var/tmp/spdk_pci_lock_${SPDK_PCI_BDF}"
	fi
fi
set -e
if [[ "$bootstrap_rc" -ne 0 ]]; then
	tail -120 "$OUT/hfs-install.log" >&2
	exit "$bootstrap_rc"
fi

# Wait until every bootstrap child releases the shared backend lock.
lock_wait_seconds="${HFS_MYSQL_LOCK_WAIT_SECONDS:-15}"
lock_available=0
for _ in $(seq 1 "$lock_wait_seconds"); do
	if flock -n "$OUT/hfs.lock" -c true 2>/dev/null; then
		lock_available=1
		break
	fi
	sleep 1
done
if [[ "$lock_available" != "1" ]]; then
	echo "ERROR: HFS backend lock remained busy after bootstrap" >&2
	exit 1
fi
start_hfs
seed_db "$HFS_SOCK"
for threads in 1 4 16; do
	run_case hfs "$HFS_SOCK" read "$threads"
	run_case hfs "$HFS_SOCK" write "$threads"
done

echo "layer\tworkload\tconcurrency\tavg_seconds\tqps"
tail -n +2 "$OUT/results.tsv"
echo "MYSQL STAGE2 OK"
