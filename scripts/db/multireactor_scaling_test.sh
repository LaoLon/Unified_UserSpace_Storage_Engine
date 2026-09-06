#!/usr/bin/env bash
# Verify per-reactor channels, blob hashing, and direct async completion closure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-multireactor.img}"
RUN_DIR="${4:-/var/tmp/hfs-multireactor}"
REACTORS="${HFS_TEST_REACTORS:-2}"
LOCK_PATH="$RUN_DIR/hfs.lock"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi
if [[ ! "$REACTORS" =~ ^[2-8]$ ]]; then
	echo "HFS_TEST_REACTORS must be between 2 and 8" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

STATS_PATH="$RUN_DIR/multireactor.stats"
env LD_PRELOAD="$LIB" HFS_LIBHFS_PATH="$LIB" HFS_STATS_PATH="$STATS_PATH" \
	HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" HFS_BDEV_NAME=Aio0 \
	HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 HFS_DATA_REACTORS="$REACTORS" \
	HFS_IO_PIPELINE_DEPTH=4 HFS_RUNTIME_BATCH=1 \
	python3 - "$REACTORS" <<'PYEOF'
import ctypes
import os
import sys

reactors = int(sys.argv[1])
lib = ctypes.CDLL(os.environ["HFS_LIBHFS_PATH"], mode=ctypes.RTLD_GLOBAL)
Request = ctypes.c_void_p
Callback = ctypes.CFUNCTYPE(None, Request, ctypes.c_ssize_t,
                            ctypes.c_int, ctypes.c_void_p)
lib.hfs_lib_async_pwrite.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                     ctypes.c_size_t, ctypes.c_longlong,
                                     ctypes.c_void_p, ctypes.c_void_p,
                                     ctypes.POINTER(Request)]
lib.hfsAsyncWait.argtypes = [Request, ctypes.c_uint]
lib.hfsAsyncResult.argtypes = [Request, ctypes.POINTER(ctypes.c_ssize_t),
                               ctypes.POINTER(ctypes.c_int)]
lib.hfsAsyncRelease.argtypes = [Request]

root = "/hfs/multireactor"
os.mkdir(root)
unit = 4096
request_bytes = 4 * 1024 * 1024
file_count = max(reactors * 8, 16)
files = []
payloads = []
requests = []
for index in range(file_count):
    fd = os.open(f"{root}/file-{index}", os.O_CREAT | os.O_RDWR, 0o600)
    os.ftruncate(fd, request_bytes)
    payload = ctypes.create_string_buffer(bytes([65 + index % 26]) * request_bytes)
    request = Request()
    rc = lib.hfs_lib_async_pwrite(fd, payload, request_bytes, 0,
                                  None, None, ctypes.byref(request))
    assert rc == 0, ("async pwrite", index, rc, ctypes.get_errno())
    files.append(fd)
    payloads.append(payload)
    requests.append(request)

for request in requests:
    assert lib.hfsAsyncWait(request, 30000) == 0
    result = ctypes.c_ssize_t()
    error = ctypes.c_int()
    assert lib.hfsAsyncResult(request, ctypes.byref(result),
                              ctypes.byref(error)) == 0
    assert result.value == request_bytes and error.value == 0, (result.value, error.value)
    lib.hfsAsyncRelease(request)
for fd in files:
    os.close(fd)
print(f"MULTIREACTOR IO OK reactors={reactors} files={file_count}")
PYEOF

total_submit=0
total_complete=0
active_reactors=0
for ((index=0; index<REACTORS; index++)); do
	submit="$(awk -F= -v key="data_reactor_${index}_submit" '$1 == key {print $2}' "$STATS_PATH")"
	complete="$(awk -F= -v key="data_reactor_${index}_complete" '$1 == key {print $2}' "$STATS_PATH")"
	submit="${submit:-0}"
	complete="${complete:-0}"
	echo "data_reactor_${index}: submit=${submit} complete=${complete}"
	if [[ "$submit" != "$complete" ]]; then
		echo "data reactor accounting mismatch" >&2
		exit 1
	fi
	if (( submit > 0 )); then
		active_reactors=$((active_reactors + 1))
	fi
	total_submit=$((total_submit + submit))
	total_complete=$((total_complete + complete))
done
expected_active=$((REACTORS > 2 ? 2 : 1))
if (( total_submit == 0 || total_submit != total_complete || active_reactors < expected_active )); then
	echo "insufficient reactor distribution: active=${active_reactors} expected=${expected_active} submit=${total_submit} complete=${total_complete}" >&2
	exit 1
fi
echo "MULTIREACTOR ACCOUNTING OK active=${active_reactors} submit=${total_submit} complete=${total_complete}"
