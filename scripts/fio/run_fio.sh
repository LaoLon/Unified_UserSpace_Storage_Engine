#!/bin/bash
# Run the HFS fio benchmarks (and an optional ext4 baseline) and store
# JSON results (shard-21/23).
#
# The malloc bdev is process-private, so each HFS fio run formats a
# fresh filesystem; results are per-process numbers, comparable between
# runs but not across processes.
#
# Usage:
#   bash scripts/fio/run_fio.sh [libhfs.so path] [output dir] [ext4 dir]
#   ext4 dir empty/absent -> HFS only; e.g. /tmp/fio-ext4 -> also run
#   the same configs on ext4 for a baseline comparison.
set -euo pipefail

LIB="${1:-$PWD/build-cmake/libhfs.so}"
OUT="${2:-$PWD/build/fio_results}"
EXT4_DIR="${3:-}"
HFS_LIB="$(realpath "$LIB")"

if [ ! -f "$HFS_LIB" ]; then
	echo "ERROR: libhfs.so not found: $HFS_LIB" >&2
	exit 1
fi

mkdir -p "$OUT"
echo "== HFS fio benchmark =="
echo "  lib:    $HFS_LIB"
echo "  output: $OUT"

run_one() {
	local tag="$1" ini="$2" raw="$3" extra=("${@:4}")
	timeout 120 env "${extra[@]}" \
		fio "$ini" --output="$raw" --output-format=json 2>/dev/null
	python3 - "$tag" "$raw" <<'PYEOF'
import json
import sys

tag, path = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8", errors="replace").read()
try:
    data = json.loads(text[text.index("{"):])
except ValueError:
    print("%s raw output missing JSON: %s" % (tag, path))
    sys.exit(1)
for j in data["jobs"]:
    print("%s err=%s read_iops=%s write_iops=%s read_bw=%s write_bw=%s" % (
        tag, j["error"], j["read"]["iops"], j["write"]["iops"],
        j["read"]["bw_bytes"], j["write"]["bw_bytes"]))
PYEOF
}

echo "-- randrw (4k, iodepth=32) --"
run_one hfs-randrw scripts/fio/hfs_randrw.ini "$OUT/randrw.raw" \
	LD_PRELOAD="$HFS_LIB"

echo "-- seq (128k, iodepth=32) --"
run_one hfs-seq scripts/fio/hfs_seq.ini "$OUT/seq.raw" \
	LD_PRELOAD="$HFS_LIB"

if [ -n "$EXT4_DIR" ]; then
	mkdir -p "$EXT4_DIR"
	echo "== ext4 baseline (no LD_PRELOAD) =="
	echo "  dir:    $EXT4_DIR"

	# Job-level filename in the ini wins over a global --filename, so
	# generate ext4-specific ini files with the paths rewritten.
	sed "s#/hfs/fio/#$EXT4_DIR/#" scripts/fio/hfs_randrw.ini \
		> "$OUT/ext4_randrw.ini"
	sed "s#/hfs/fio/#$EXT4_DIR/#" scripts/fio/hfs_seq.ini \
		> "$OUT/ext4_seq.ini"

	echo "-- randrw (4k, iodepth=32) --"
	run_one ext4-randrw "$OUT/ext4_randrw.ini" "$OUT/ext4_randrw.raw"

	echo "-- seq (128k, iodepth=32) --"
	run_one ext4-seq "$OUT/ext4_seq.ini" "$OUT/ext4_seq.raw"
fi

echo "== done, results in $OUT =="