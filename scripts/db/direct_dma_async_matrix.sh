#!/usr/bin/env bash
# Measure registered-DMA async throughput while varying the outstanding window.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_nvme.json}"
BDEV="${HFS_DIRECT_DMA_BDEV_NAME:-Nvme0n1}"
OUT="${3:-/var/tmp/hfs-direct-dma-matrix-20260909}"
REACTORS="${HFS_DIRECT_DMA_REACTORS:-4}"
BS="${HFS_DIRECT_DMA_BS:-65536}"
DEPTHS_TEXT="${HFS_DIRECT_DMA_DEPTHS:-1 2 4 8 16 32}"
ROUNDS="${HFS_DIRECT_DMA_ROUNDS:-16}"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [spdk config] [output dir]" >&2
	exit 2
fi
if [[ ! "$REACTORS" =~ ^[1-8]$ ]]; then
	echo "HFS_DIRECT_DMA_REACTORS must be between 1 and 8" >&2
	exit 2
fi

rm -rf "$OUT"
mkdir -p "$OUT"
{
	echo "timestamp=$(date -Is)"
	echo "hostname=$(hostname)"
	echo "kernel=$(uname -srmo)"
	echo "workload=registered_dma_async"
	echo "hfs_lib=$(realpath "$LIB")"
	echo "spdk_config=$(realpath "$CONFIG")"
	echo "bdev=$BDEV"
	echo "reactors=$REACTORS"
	echo "request_bytes=$BS"
	echo "depths=$DEPTHS_TEXT"
	echo "rounds=$ROUNDS"
	echo "hfs_commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "spdk_commit=$(git -C "$ROOT/spdk" rev-parse HEAD 2>/dev/null || echo unknown)"
} > "$OUT/environment.txt"

printf 'depth\trounds\twrite_iops\tread_iops\twrite_ms\tread_ms\twrite_errors\tread_errors\tio_direct_ops\tio_copy_ops\tbackend_peak\tbackend_outstanding\tqueue_peak\tasync_submit\tasync_complete\n' > "$OUT/summary.tsv"

for depth in $DEPTHS_TEXT; do
	RUN_DIR="$OUT/depth-$depth"
	mkdir -p "$RUN_DIR"
	STATS_PATH="$RUN_DIR/stats"
	HFS_CONFIG_PATH="$CONFIG" HFS_BDEV_NAME="$BDEV" HFS_FORMAT=1 \
	HFS_AUTO_MKDIRS=/direct-dma HFS_DATA_REACTORS="$REACTORS" \
	HFS_DATA_STRIPE_KIB=64 HFS_RUNTIME_BATCH=1 HFS_STATS_PATH="$STATS_PATH" \
	LD_PRELOAD="$LIB" \
	python3 - "$depth" "$BS" "$ROUNDS" "$RUN_DIR" <<'PYEOF'
import ctypes
import json
import os
import pathlib
import sys
import time

depth = int(sys.argv[1])
request_bytes = int(sys.argv[2])
rounds = int(sys.argv[3])
run_dir = pathlib.Path(sys.argv[4])
total_bytes = depth * request_bytes
lib = ctypes.CDLL(os.environ["LD_PRELOAD"], mode=ctypes.RTLD_GLOBAL,
                  use_errno=True)

lib.hfsDmaAlloc.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
lib.hfsDmaAlloc.restype = ctypes.c_void_p
lib.hfsDmaFree.argtypes = [ctypes.c_void_p]
lib.hfsRegisterBuffer.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
lib.hfsRegisterBuffer.restype = ctypes.c_int
lib.hfsUnregisterBuffer.argtypes = [ctypes.c_void_p]
lib.hfsUnregisterBuffer.restype = ctypes.c_int
lib.hfs_lib_async_pwrite.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                     ctypes.c_size_t, ctypes.c_longlong,
                                     ctypes.c_void_p, ctypes.c_void_p,
                                     ctypes.POINTER(ctypes.c_void_p)]
lib.hfs_lib_async_pwrite.restype = ctypes.c_int
lib.hfs_lib_async_pread.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                    ctypes.c_size_t, ctypes.c_longlong,
                                    ctypes.c_void_p, ctypes.c_void_p,
                                    ctypes.POINTER(ctypes.c_void_p)]
lib.hfs_lib_async_pread.restype = ctypes.c_int
lib.hfsAsyncWait.argtypes = [ctypes.c_void_p, ctypes.c_uint]
lib.hfsAsyncWait.restype = ctypes.c_int
lib.hfsAsyncResult.argtypes = [ctypes.c_void_p,
                               ctypes.POINTER(ctypes.c_ssize_t),
                               ctypes.POINTER(ctypes.c_int)]
lib.hfsAsyncResult.restype = ctypes.c_int
lib.hfsAsyncRelease.argtypes = [ctypes.c_void_p]

root = f"/hfs/direct-dma-matrix-{os.getpid()}"
os.mkdir(root)
fd = os.open(f"{root}/data", os.O_CREAT | os.O_RDWR, 0o600)
os.ftruncate(fd, total_bytes)

buffers = []
requests = []
for index in range(depth):
    buffer = lib.hfsDmaAlloc(request_bytes, 4096)
    assert buffer, f"DMA allocation failed at depth {depth} index {index}"
    assert lib.hfsRegisterBuffer(buffer, request_bytes) == 0, (
        "register", index, ctypes.get_errno())
    ctypes.memset(buffer, 0x30 + (index % 40), request_bytes)
    buffers.append(buffer)

def wait_all(expected_bytes, verify=False):
    errors = 0
    for request, index in requests:
        assert lib.hfsAsyncWait(request, 30000) == 0
        result = ctypes.c_ssize_t()
        error = ctypes.c_int()
        assert lib.hfsAsyncResult(request, ctypes.byref(result),
                                  ctypes.byref(error)) == 0
        if result.value != expected_bytes or error.value != 0:
            errors += 1
        if verify and ctypes.string_at(buffers[index], 1) != bytes([0x30 + (index % 40)]):
            errors += 1
        lib.hfsAsyncRelease(request)
    requests.clear()
    return errors

def submit_write():
    for index, buffer in enumerate(buffers):
        request = ctypes.c_void_p()
        offset = index * request_bytes
        rc = lib.hfs_lib_async_pwrite(fd, buffer, request_bytes, offset,
                                      None, None, ctypes.byref(request))
        assert rc == 0 and request.value, ("pwrite", index, rc,
                                             ctypes.get_errno())
        requests.append((request, index))

def submit_read():
    for index, buffer in enumerate(buffers):
        ctypes.memset(buffer, 0, request_bytes)
        request = ctypes.c_void_p()
        offset = index * request_bytes
        rc = lib.hfs_lib_async_pread(fd, buffer, request_bytes, offset,
                                     None, None, ctypes.byref(request))
        assert rc == 0 and request.value, ("pread", index, rc,
                                            ctypes.get_errno())
        requests.append((request, index))

start = time.perf_counter_ns()
write_errors = 0
for _ in range(rounds):
    submit_write()
    write_errors += wait_all(request_bytes)
write_ms = (time.perf_counter_ns() - start) / 1_000_000.0

start = time.perf_counter_ns()
read_errors = 0
for _ in range(rounds):
    submit_read()
    read_errors += wait_all(request_bytes, verify=True)
read_ms = (time.perf_counter_ns() - start) / 1_000_000.0

for buffer in buffers:
    assert lib.hfsUnregisterBuffer(buffer) == 0
    lib.hfsDmaFree(buffer)
os.close(fd)

row = {
    "depth": depth,
    "rounds": rounds,
    "write_ms": write_ms,
    "read_ms": read_ms,
    "write_iops": (depth * rounds) / (write_ms / 1000.0),
    "read_iops": (depth * rounds) / (read_ms / 1000.0),
    "write_errors": write_errors,
    "read_errors": read_errors,
}
(run_dir / "timing.json").write_text(json.dumps(row, indent=2), encoding="utf-8")
print(json.dumps(row, sort_keys=True))
PYEOF

	python3 - "$RUN_DIR/timing.json" "$STATS_PATH" "$OUT/summary.tsv" <<'PYEOF'
import json
import pathlib
import sys

timing = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
stats = {}
for line in pathlib.Path(sys.argv[2]).read_text(encoding="utf-8").splitlines():
    if "=" in line:
        key, value = line.split("=", 1)
        stats[key] = value
row = [
    str(timing["depth"]), str(timing["rounds"]),
    f'{timing["write_iops"]:.2f}',
    f'{timing["read_iops"]:.2f}', f'{timing["write_ms"]:.3f}',
    f'{timing["read_ms"]:.3f}', str(timing["write_errors"]),
    str(timing["read_errors"]), stats.get("io_direct_ops", "0"),
    stats.get("io_copy_ops", "0"),
    stats.get("io_backend_outstanding_peak", "0"),
    stats.get("io_backend_outstanding", "0"),
    stats.get("runtime_queue_depth_peak", "0"),
    stats.get("async_submit", "0"), stats.get("async_complete", "0"),
]
with open(sys.argv[3], "a", encoding="utf-8") as stream:
    stream.write("\t".join(row) + "\n")
print("\t".join(row))
PYEOF
done

python3 - "$OUT/summary.tsv" <<'PYEOF'
import csv
import sys

with open(sys.argv[1], newline="", encoding="utf-8") as stream:
    rows = list(csv.DictReader(stream, delimiter="\t"))
if not rows:
    raise SystemExit("direct-DMA matrix produced no rows")
for row in rows:
    if any(int(row[key]) != 0 for key in ("write_errors", "read_errors")):
        raise SystemExit(f"I/O errors at depth {row['depth']}")
    if int(row["io_copy_ops"]) != 0:
        raise SystemExit(f"bounce copy observed at depth {row['depth']}")
    if int(row["io_direct_ops"]) < 2 * int(row["depth"]) * int(row["rounds"]):
        raise SystemExit(f"direct accounting mismatch at depth {row['depth']}")
    if int(row["backend_outstanding"]) != 0:
        raise SystemExit(f"backend outstanding leak at depth {row['depth']}")
    if int(row["async_submit"]) != int(row["async_complete"]):
        raise SystemExit(f"async accounting mismatch at depth {row['depth']}")
print("DIRECT DMA ASYNC MATRIX OK")
PYEOF
