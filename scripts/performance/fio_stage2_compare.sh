#!/usr/bin/env bash
# Run the stage-two three-way FIO comparison with stage-one-compatible inputs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build-cmake/libhfs.so}"
OUT="${2:-$ROOT/build/performance-stage2-fio}"
EXT4_DIR="${3:-/var/tmp/hfs-stage2-fio-ext4}"
SPDK_PLUGIN="${4:-/root/code/spdk/build/fio/spdk_bdev}"
SPDK_CONF="${5:-$ROOT/config/hfs.json}"
HFS_CONFIG="${HFS_FIO_CONFIG:-$SPDK_CONF}"
HFS_BDEV="${HFS_FIO_BDEV_NAME:-Malloc0}"
SPDK_BDEV="${HFS_FIO_SPDK_BDEV_NAME:-Malloc0}"
HFS_NUMJOBS="${HFS_FIO_NUMJOBS:-1}"
SIZE="${HFS_FIO_SIZE:-256m}"
RUNTIME="${HFS_FIO_RUNTIME_SECONDS:-15}"
RUN_SPDK_REFERENCE="${HFS_FIO_RUN_SPDK_REFERENCE:-1}"

HFS_LIB="$(realpath "$LIB")"
if [[ ! -f "$HFS_LIB" || ! -f "$SPDK_PLUGIN" || ! -f "$SPDK_CONF" || ! -f "$HFS_CONFIG" ]]; then
	 echo "usage: $0 [libhfs.so] [output dir] [ext4 dir] [spdk plugin] [spdk config]" >&2
	 exit 2
fi
mkdir -p "$OUT" "$EXT4_DIR"
rm -f "$OUT"/*.json "$OUT"/*.log "$OUT/summary.tsv" "$OUT/environment.txt"

{
	echo "timestamp=$(date -Is)"
	echo "hostname=$(hostname)"
	echo "kernel=$(uname -srmo)"
	echo "cpu_count=$(nproc)"
	echo "hfs_lib=$HFS_LIB"
	echo "spdk_plugin=$SPDK_PLUGIN"
	echo "spdk_config=$SPDK_CONF"
	echo "spdk_bdev=$SPDK_BDEV"
	echo "hfs_numjobs=$HFS_NUMJOBS"
	echo "size=$SIZE"
	echo "runtime_seconds=$RUNTIME"
	echo "spdk_reference=$RUN_SPDK_REFERENCE"
	echo "fio=$(fio --version 2>&1 | head -1)"
	echo "hfs_commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "spdk_commit=$(git -C "$(dirname "$(dirname "$SPDK_PLUGIN")")" rev-parse HEAD 2>/dev/null || echo unknown)"
} > "$OUT/environment.txt"

for BS in 4k 8k 16k 32k 64k; do
	echo "== bs=$BS =="
	timeout 300 env LD_PRELOAD="$HFS_LIB" HFS_CONFIG_PATH="$HFS_CONFIG" \
		HFS_BDEV_NAME="$HFS_BDEV" HFS_FORMAT=1 HFS_AUTO_MKDIRS=/fio \
		fio --name=rw --ioengine=psync --thread=1 --numjobs="$HFS_NUMJOBS" --bs="$BS" --size="$SIZE" \
		--rw=randrw --rwmixread=50 --filename=/hfs/fio/rw.dat --direct=1 \
		--group_reporting=1 --time_based=1 --runtime="$RUNTIME" \
		--output="$OUT/hfs_${BS}.log" --output-format=json 2>/dev/null
	timeout 300 fio --name=rw --ioengine=psync --bs="$BS" --size="$SIZE" \
		--rw=randrw --rwmixread=50 --filename="$EXT4_DIR/rw.dat" --direct=1 \
		--group_reporting=1 --time_based=1 --runtime="$RUNTIME" \
		--output="$OUT/ext4_${BS}.log" --output-format=json 2>/dev/null
	if [[ "$RUN_SPDK_REFERENCE" == "1" ]]; then
		timeout 300 env LD_PRELOAD="$SPDK_PLUGIN" \
			fio --name=rw --ioengine=spdk_bdev --thread=1 --spdk_conf="$SPDK_CONF" \
			--filename="$SPDK_BDEV" --bs="$BS" --size="$SIZE" --rw=randrw --rwmixread=50 \
			--direct=1 --iodepth=32 --group_reporting=1 --time_based=1 --runtime="$RUNTIME" \
			--output="$OUT/spdk_${BS}.log" --output-format=json 2>/dev/null
	fi
done

python3 - "$OUT" <<'PYEOF'
import json
import sys
from pathlib import Path

out = Path(sys.argv[1])
rows = []

def load(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    return json.loads(text[text.index("{"):])

layers = ["hfs", "ext4"]
if (out / "spdk_4k.log").exists():
    layers.append("spdk")
for bs in ("4k", "8k", "16k", "32k", "64k"):
    for layer in layers:
        job = load(out / f"{layer}_{bs}.log")["jobs"][0]
        read = job["read"]
        write = job["write"]
        rows.append({
            "bs": bs,
            "layer": layer,
            "read_iops": read["iops"],
            "write_iops": write["iops"],
            "total_iops": read["iops"] + write["iops"],
            "read_bw_mib": read["bw_bytes"] / 1048576,
            "write_bw_mib": write["bw_bytes"] / 1048576,
            "total_bw_mib": (read["bw_bytes"] + write["bw_bytes"]) / 1048576,
            "read_p50_us": read["clat_ns"]["percentile"].get("50.000000", 0) / 1000,
            "read_p99_us": read["clat_ns"]["percentile"].get("99.000000", 0) / 1000,
            "write_p50_us": write["clat_ns"]["percentile"].get("50.000000", 0) / 1000,
            "write_p99_us": write["clat_ns"]["percentile"].get("99.000000", 0) / 1000,
            "errors": read.get("io_u_error", 0) + write.get("io_u_error", 0),
        })

columns = list(rows[0])
with (out / "summary.tsv").open("w", encoding="utf-8") as stream:
    stream.write("\t".join(columns) + "\n")
    for row in rows:
        stream.write("\t".join(str(row[column]) for column in columns) + "\n")

print("bs\tlayer\ttotal_iops\ttotal_bw_mib\tread_p50_us\tread_p99_us\twrite_p50_us\twrite_p99_us\terrors")
for row in rows:
    print("{bs}\t{layer}\t{total_iops:.2f}\t{total_bw_mib:.2f}\t{read_p50_us:.2f}\t{read_p99_us:.2f}\t{write_p50_us:.2f}\t{write_p99_us:.2f}\t{errors}".format(**row))
PYEOF

echo "FIO STAGE2 OK"
