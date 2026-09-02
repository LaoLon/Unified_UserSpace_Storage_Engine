#!/bin/bash
# RocksDB db_bench smoke test on HFS (shard-25).
#
# db_bench runs as a single process (plus its own threads), which fits
# the current HFS single-process model. The DB directory is created
# via HFS_AUTO_MKDIRS (RocksDB expects it to exist and the malloc bdev
# is process-private).
#
# Usage: bash scripts/db/rocksdb_smoke.sh [libhfs.so path] [mount prefix]
set -euo pipefail

LIB="${1:-$PWD/build-cmake/libhfs.so}"
PREFIX="${2:-/hfs}"
HFS_LIB="$(realpath "$LIB")"
DBDIR="$PREFIX/rocks"

if [ ! -f "$HFS_LIB" ]; then
	echo "ERROR: libhfs.so not found: $HFS_LIB" >&2
	exit 1
fi

echo "== RocksDB db_bench smoke test on HFS =="
echo "  lib: $HFS_LIB"
echo "  db:  $DBDIR"

# fillseq: sequential key/value insert (write-heavy)
echo "-- fillseq --"
timeout 180 env LD_PRELOAD="$HFS_LIB" HFS_AUTO_MKDIRS=/rocks \
	db_bench --benchmarks=fillseq --db="$DBDIR/fill" \
	--num=100000 --value_size=100 --disable_wal=0 \
	--statistics 2>&1 | grep -E 'fillseq.*ops/sec|Error|error' | head -3

# readrandom: random point lookups (read-heavy)
echo "-- readrandom --"
timeout 180 env LD_PRELOAD="$HFS_LIB" HFS_AUTO_MKDIRS=/rocks \
	db_bench --benchmarks=readrandom --db="$DBDIR/fill" \
	--num=100000 --value_size=100 --reads=100000 \
	--statistics 2>&1 | grep -E 'readrandom.*ops/sec|Error|error' | head -3

# overwrite: update existing keys
echo "-- overwrite --"
timeout 180 env LD_PRELOAD="$HFS_LIB" HFS_AUTO_MKDIRS=/rocks \
	db_bench --benchmarks=overwrite --db="$DBDIR/fill" \
	--num=100000 --value_size=100 \
	--statistics 2>&1 | grep -E 'overwrite.*ops/sec|Error|error' | head -3

echo "ROCKSDB SMOKE OK"