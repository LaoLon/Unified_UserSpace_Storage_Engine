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
	--output="$OUT/randrw.raw" --output-format=json 2>/dev/null
python3 - "$OUT/randrw.raw" <<'PYEOF'
import json
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
data = json.loads(text[text.index("{"):])
for j in data["jobs"]:
    print("randrw err=%s read_iops=%s write_iops=%s read_bw=%s write_bw=%s" % (
        j["error"], j["read"]["iops"], j["write"]["iops"],
        j["read"]["bw_bytes"], j["write"]["bw_bytes"]))
PYEOF

echo "-- seq (128k, iodepth=32) --"
timeout 120 env LD_PRELOAD="$HFS_LIB" \
	fio scripts/fio/hfs_seq.ini \
	--output="$OUT/seq.raw" --output-format=json 2>/dev/null
python3 - "$OUT/seq.raw" <<'PYEOF'
import json
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
data = json.loads(text[text.index("{"):])
for j in data["jobs"]:
    print("seq err=%s read_iops=%s write_iops=%s read_bw=%s write_bw=%s" % (
        j["error"], j["read"]["iops"], j["write"]["iops"],
        j["read"]["bw_bytes"], j["write"]["bw_bytes"]))
PYEOF

echo "== done, results in $OUT =="