#!/bin/bash
# Run the HFS fio benchmarks and store JSON results (shard-21).
#
# The malloc bdev is process-private, so each fio run formats a fresh
# filesystem; results are therefore per-process numbers, comparable
# between runs but not across processes.
#
# Usage: bash scripts/fio/run_fio.sh [libhfs.so path] [output dir]
set -euo pipefail

LIB="${1:-$PWD/build-cmake/libhfs.so}"
OUT="${2:-$PWD/build/fio_results}"
HFS_LIB="$(realpath "$LIB")"

if [ ! -f "$HFS_LIB" ]; then
	echo "ERROR: libhfs.so not found: $HFS_LIB" >&2
	exit 1
fi

mkdir -p "$OUT"
echo "== HFS fio benchmark =="
echo "  lib:    $HFS_LIB"
echo "  output: $OUT"

echo "-- randrw (4k, iodepth=32) --"
timeout 120 env LD_PRELOAD="$HFS_LIB" \
	fio scripts/fio/hfs_randrw.ini \
	--output="$OUT/randrw.json" --output-format=json

echo "-- seq (128k, iodepth=32) --"
timeout 120 env LD_PRELOAD="$HFS_LIB" \
	fio scripts/fio/hfs_seq.ini \
	--output="$OUT/seq.json" --output-format=json

echo "== done, results in $OUT =="