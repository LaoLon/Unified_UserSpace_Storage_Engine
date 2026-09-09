#!/usr/bin/env bash
# Verify a registered DMA buffer uses a bounded direct child window.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-registered-window.img}"
RUN_DIR="${4:-/var/tmp/hfs-registered-window}"
LOCK_PATH="$RUN_DIR/hfs.lock"
TRACE_PATH="$RUN_DIR/io-trace.tsv"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

env LD_PRELOAD="$LIB" HFS_LIBHFS_PATH="$LIB" \
	HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" HFS_BDEV_NAME=Aio0 \
	HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 HFS_DATA_REACTORS=4 \
	HFS_DATA_STRIPE_KIB=64 HFS_RUNTIME_BATCH=1 HFS_IO_MAX_DIRECT_UNITS=64 \
	HFS_TRACE=1 \
	HFS_TRACE_PATH="$TRACE_PATH" \
python3 - <<'PYEOF'
import ctypes
import os

lib = ctypes.CDLL(os.environ["HFS_LIBHFS_PATH"], mode=ctypes.RTLD_GLOBAL,
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
lib.hfs_lib_async_pread.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                    ctypes.c_size_t, ctypes.c_longlong,
                                    ctypes.c_void_p, ctypes.c_void_p,
                                    ctypes.POINTER(ctypes.c_void_p)]
lib.hfsAsyncWait.argtypes = [ctypes.c_void_p, ctypes.c_uint]
lib.hfsAsyncResult.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ssize_t),
                               ctypes.POINTER(ctypes.c_int)]
lib.hfsAsyncRelease.argtypes = [ctypes.c_void_p]

root = f"/hfs/registered-window-{os.getpid()}"
os.mkdir(root)
fd = os.open(f"{root}/data", os.O_CREAT | os.O_RDWR, 0o600)
size = 512 * 1024
os.ftruncate(fd, size)

write_buffer = lib.hfsDmaAlloc(size, 4096)
read_buffer = lib.hfsDmaAlloc(size, 4096)
assert write_buffer and read_buffer, "HFS DMA allocation failed"
register_rc = lib.hfsRegisterBuffer(write_buffer, size)
assert register_rc == 0, ("write register", register_rc, ctypes.get_errno(),
                          os.strerror(ctypes.get_errno()))
register_rc = lib.hfsRegisterBuffer(read_buffer, size)
assert register_rc == 0, ("read register", register_rc, ctypes.get_errno(),
                          os.strerror(ctypes.get_errno()))
ctypes.memset(write_buffer, 0x6B, size)
ctypes.memset(read_buffer, 0, size)

def wait_request(request):
    wait_rc = lib.hfsAsyncWait(request, 30000)
    assert request.value and wait_rc == 0
    result = ctypes.c_ssize_t()
    error = ctypes.c_int()
    assert lib.hfsAsyncResult(request, ctypes.byref(result),
                              ctypes.byref(error)) == 0
    assert result.value == size and error.value == 0, (result.value, error.value)
    lib.hfsAsyncRelease(request)

request = ctypes.c_void_p()
assert lib.hfs_lib_async_pwrite(fd, write_buffer, size, 0, None, None,
                                ctypes.byref(request)) == 0
assert lib.hfsUnregisterBuffer(write_buffer) != 0
wait_request(request)
assert lib.hfsUnregisterBuffer(write_buffer) == 0
request = ctypes.c_void_p()
assert lib.hfs_lib_async_pread(fd, read_buffer, size, 0, None, None,
                               ctypes.byref(request)) == 0
wait_request(request)
assert ctypes.string_at(read_buffer, 1) == b"k"
assert lib.hfsUnregisterBuffer(read_buffer) == 0
lib.hfsDmaFree(write_buffer)
lib.hfsDmaFree(read_buffer)
os.close(fd)
print("REGISTERED DIRECT WINDOW OK bytes=524288 copy_ops=0 registered=1")
PYEOF

python3 "$ROOT/scripts/db/hfs_trace_summary.py" "$TRACE_PATH"
python3 - "$TRACE_PATH" <<'PYEOF'
import csv
import sys

with open(sys.argv[1], newline="", encoding="utf-8") as stream:
    rows = [row for row in csv.DictReader(
        (line for line in stream if not line.startswith("#")), delimiter="\t")]
registered = [row for row in rows if row["buffer_kind"] == "registered"]
copies = [row for row in rows if row["phase"] in ("copy_in_begin", "copy_out_begin")]
children = [row for row in registered if row["phase"] == "spdk_submit"]
if len(children) < 2 or copies:
    raise SystemExit(f"direct window trace mismatch: registered_children={len(children)} copies={len(copies)}")
print(f"DIRECT WINDOW TRACE OK registered_children={len(children)} copies={len(copies)}")
PYEOF
