#!/usr/bin/env bash
# Validate RocksDB persistence, cross-process reopen, WAL recovery, and concurrency.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build-cmake/libhfs.so}"
PREFIX="${2:-/hfs}"
CONFIG="${3:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${4:-/var/tmp/hfs-rocksdb.img}"
RUN_DIR="${5:-/var/tmp/hfs-rocksdb}"
HFS_LIB="$(realpath "$LIB")"
DBDIR="$PREFIX/rocks/fill"
LOCK_PATH="$RUN_DIR/hfs.lock"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
NUM="${ROCKSDB_NUM:-20000}"
VALUE_SIZE="${ROCKSDB_VALUE_SIZE:-256}"
DURATION="${ROCKSDB_DURATION_SECONDS:-60}"

if [[ ! -f "$HFS_LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [mount prefix] [config] [image] [run-dir]" >&2
	exit 2
fi
for tool in db_bench ldb; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "ERROR: required RocksDB tool not found: $tool" >&2
		exit 2
	fi
done

mkdir -p "$RUN_DIR"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

# Print the last benchmark result without making grep determine test success.
print_result() {
	local log="$1"
	awk '/^[[:space:]]*(fillseq|readrandom|overwrite|readwhilewriting|compact|levelstats)/ { line = $0 } END { if (line != "") print line }' "$log" || true
}

# Fail on explicit data-path errors that can be hidden in a successful log stream.
check_log() {
	local log="$1"
	if grep -Eiq 'corruption|no such device|segmentation fault|assertion failed|io error' "$log"; then
		echo "ERROR: RocksDB log contains a fatal data-path error: $log" >&2
		tail -80 "$log" >&2 || true
		return 1
	fi
}

# Run one bounded db_bench command and preserve its complete output.
run_bench() {
	local name="$1"
	shift
	local log="$RUN_DIR/${name}.log"
	local format=0
	local rc
	if [[ "$name" == "fillseq" ]]; then
		format=1
	fi
	set +e
	timeout --signal=TERM --kill-after=5s 180s \
		env LD_PRELOAD="$HFS_LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT="$format" \
		HFS_AUTO_MKDIRS=/rocks \
		db_bench "$@" >"$log" 2>&1
	rc=$?
	set -e
	if [[ "$rc" -ne 0 ]]; then
		echo "ERROR: db_bench $name failed with exit code $rc" >&2
		tail -120 "$log" >&2 || true
		return "$rc"
	fi
	check_log "$log"
	print_result "$log"
}

# Run a process that can be killed while RocksDB is writing and reading.
run_kill_recovery() {
	local log="$RUN_DIR/readwhilewriting-kill.log"
	local pid
	local rc
	set +e
	setsid env LD_PRELOAD="$HFS_LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_AUTO_MKDIRS=/rocks \
		db_bench --benchmarks=readwhilewriting --db="$DBDIR" \
			--num="$NUM" --value_size="$VALUE_SIZE" --key_size=16 \
			--use_existing_db=1 --disable_wal=0 --mmap_read=0 --mmap_write=0 \
			--threads=8 --duration=60 --statistics=1 --histogram=1 >"$log" 2>&1 &
	pid=$!
	sleep 5
	if ! kill -0 "$pid" 2>/dev/null; then
		echo "ERROR: readwhilewriting process exited before SIGKILL" >&2
		tail -120 "$log" >&2 || true
		return 1
	fi
	kill -KILL -- -"$pid" 2>/dev/null || true
	wait "$pid"
	rc=$?
	set -e
	if [[ "$rc" -eq 0 ]]; then
		echo "ERROR: SIGKILL workload exited cleanly" >&2
		return 1
	fi
	check_log "$log"
	echo "  SIGKILL workload terminated with expected exit code $rc"
}

echo "== RocksDB persistent HFS smoke test =="
echo "  lib:     $HFS_LIB"
echo "  db:      $DBDIR"
echo "  image:   $IMAGE"

echo "-- fillseq in process A --"
run_bench fillseq \
	--benchmarks=fillseq --db="$DBDIR" --num="$NUM" \
	--value_size="$VALUE_SIZE" --key_size=16 --use_existing_db=0 \
	--disable_wal=0 --sync=1 --mmap_read=0 --mmap_write=0 \
	--statistics=1 --histogram=1

echo "-- reopen and readrandom in process B --"
run_bench readrandom \
	--benchmarks=readrandom --db="$DBDIR" --num="$NUM" \
	--reads="$NUM" --value_size="$VALUE_SIZE" --key_size=16 \
	--use_existing_db=1 --disable_wal=0 --mmap_read=0 --mmap_write=0 \
	--statistics=1 --histogram=1

echo "-- direct I/O reopen and compaction in process B2 --"
run_bench direct_readrandom \
	--benchmarks=readrandom --db="$DBDIR" --num="$NUM" \
	--reads="$NUM" --value_size="$VALUE_SIZE" --key_size=16 \
	--use_existing_db=1 --disable_wal=0 --mmap_read=0 --mmap_write=0 \
	--use_direct_reads=1 --use_direct_io_for_flush_and_compaction=1 \
	--statistics=1 --histogram=1

echo "-- overwrite and flush in process C --"
run_bench overwrite \
	--benchmarks=overwrite --db="$DBDIR" --num="$NUM" \
	--value_size="$VALUE_SIZE" --key_size=16 --use_existing_db=1 \
	--disable_wal=0 --sync=1 --mmap_read=0 --mmap_write=0 \
	--statistics=1 --histogram=1
run_bench flush_compact \
	--benchmarks=flush,compact --db="$DBDIR" --num="$NUM" \
	--value_size="$VALUE_SIZE" --key_size=16 --use_existing_db=1 \
	--disable_wal=0 --mmap_read=0 --mmap_write=0 --statistics=1

echo "-- concurrent readwhilewriting --"
run_bench readwhilewriting \
	--benchmarks=readwhilewriting --db="$DBDIR" --num="$NUM" \
	--value_size="$VALUE_SIZE" --key_size=16 --use_existing_db=1 \
	--disable_wal=0 --mmap_read=0 --mmap_write=0 --threads=8 \
	--duration="$DURATION" --statistics=1 --histogram=1

echo "-- SIGKILL during WAL workload --"
run_kill_recovery

echo "-- reopen after WAL recovery --"
run_bench recovery_readrandom \
	--benchmarks=readrandom --db="$DBDIR" --num="$NUM" \
	--reads="$NUM" --value_size="$VALUE_SIZE" --key_size=16 \
	--use_existing_db=1 --disable_wal=0 --mmap_read=0 --mmap_write=0 \
	--statistics=1 --histogram=1

echo "-- ldb consistency check in process D --"
LDB_LOG="$RUN_DIR/ldb-checkconsistency.log"
set +e
timeout --signal=TERM --kill-after=5s 180s \
	env LD_PRELOAD="$HFS_LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
	HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_AUTO_MKDIRS=/rocks \
	ldb --db="$DBDIR" \
	checkconsistency >"$LDB_LOG" 2>&1
LDB_RC=$?
set -e
if [[ "$LDB_RC" -ne 0 ]]; then
	echo "ERROR: ldb checkconsistency failed with exit code $LDB_RC" >&2
	tail -120 "$LDB_LOG" >&2 || true
	exit "$LDB_RC"
fi
check_log "$LDB_LOG"

echo "ROCKSDB PERSISTENT RECOVERY OK"
