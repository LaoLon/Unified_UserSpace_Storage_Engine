#!/bin/bash
# 1GB random read/write benchmark: HFS (LD_PRELOAD) vs ext4 baseline vs
# raw SPDK bdev (spdk_bdev fio plugin), block sizes 4k..64k.
#
# Usage:
#   bash scripts/fio/run_randrw_1g.sh [libhfs.so path] [output dir] [ext4 dir]
set -euo pipefail

LIB="${1:-$PWD/build-cmake/libhfs.so}"
OUT="${2:-$PWD/build/fio_1g_results}"
EXT4_DIR="${3:-/tmp/fio-ext4-1g}"
HFS_LIB="$(realpath "$LIB")"
SPDK_PLUGIN="${4:-/root/code/spdk/build/fio/spdk_bdev}"
SPDK_CONF="${5:-$PWD/config/hfs.json}"

if [ ! -f "$HFS_LIB" ]; then
	echo "ERROR: libhfs.so not found: $HFS_LIB" >&2
	exit 1
fi

mkdir -p "$OUT" "$EXT4_DIR"
echo "== 1GB random rw: HFS vs ext4 vs SPDK bdev (bs 4k..64k) =="
echo "  lib:    $HFS_LIB"
echo "  output: $OUT"

for BS in 4k 8k 16k 32k 64k; do
	echo "-- bs=$BS --"
	# HFS (single process, fresh filesystem per run)
	timeout 300 env LD_PRELOAD="$HFS_LIB" HFS_FORMAT=1 HFS_AUTO_MKDIRS=/fio \
		fio --name=rw --ioengine=psync --bs="$BS" --size=1g \
		--rw=randrw --rwmixread=50 --filename=/hfs/fio/rw.dat \
		--direct=1 --group_reporting=1 --time_based=1 --runtime=15 \
		--output="$OUT/hfs_${BS}.raw" --output-format=json 2>/dev/null
	# ext4 (no LD_PRELOAD)
	timeout 300 fio --name=rw --ioengine=psync --bs="$BS" --size=1g \
		--rw=randrw --rwmixread=50 --filename="$EXT4_DIR/rw.dat" \
		--direct=1 --group_reporting=1 --time_based=1 --runtime=15 \
		--output="$OUT/ext4_${BS}.raw" --output-format=json 2>/dev/null
	# raw SPDK bdev (spdk_bdev fio plugin, no filesystem layer)
	timeout 300 env LD_PRELOAD="$SPDK_PLUGIN" \
		fio --name=rw --ioengine=spdk_bdev --thread=1 \
		--spdk_conf="$SPDK_CONF" --filename=Malloc0 --bs="$BS" --size=1g \
		--rw=randrw --rwmixread=50 --direct=1 --iodepth=32 \
		--group_reporting=1 --time_based=1 --runtime=15 \
		--output="$OUT/spdk_${BS}.raw" --output-format=json 2>/dev/null
done

echo ""
echo "== summary =="
python3 - "$OUT" <<'PYEOF'
import json
import sys

out = sys.argv[1]
print("%-5s %-14s %-14s %-14s %-6s" % (
    "bs", "HFS IOPS", "ext4 IOPS", "spdk IOPS", "HFS/spdk"))

def load(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    return json.loads(text[text.index("{"):])

for bs in ("4k", "8k", "16k", "32k", "64k"):
    h = load("%s/hfs_%s.raw" % (out, bs))["jobs"][0]
    e = load("%s/ext4_%s.raw" % (out, bs))["jobs"][0]
    s = load("%s/spdk_%s.raw" % (out, bs))["jobs"][0]
    hi = (h["read"]["iops"] + h["write"]["iops"]) / 2
    ei = (e["read"]["iops"] + e["write"]["iops"]) / 2
    si = (s["read"]["iops"] + s["write"]["iops"]) / 2
    print("%-5s %-14.0f %-14.0f %-14.0f %-6.2f" % (
        bs, hi, ei, si, (hi / si) if si else 0))
PYEOF

echo "== done, results in $OUT =="
