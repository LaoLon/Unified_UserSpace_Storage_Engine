#!/usr/bin/env bash
# Verify bounded asynchronous positioned I/O, vector I/O, completion, and cancellation.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-async.img}"
RUN_DIR="${4:-/var/tmp/hfs-async}"
LOCK_PATH="$RUN_DIR/hfs.lock"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

STATS_PATH="$RUN_DIR/async.stats"
env LD_PRELOAD="$LIB" HFS_LIBHFS_PATH="$LIB" HFS_STATS_PATH="$STATS_PATH" \
	HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
	HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 \
	HFS_ASYNC_WORKERS=4 HFS_ASYNC_QUEUE_DEPTH=64 python3 - <<'PYEOF'
import ctypes
import errno
import os
import threading

root = "/hfs/async"
os.mkdir(root)
path = root + "/parallel.dat"
fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)

lib = ctypes.CDLL(os.environ["HFS_LIBHFS_PATH"], mode=ctypes.RTLD_GLOBAL)
Request = ctypes.c_void_p
Callback = ctypes.CFUNCTYPE(None, Request, ctypes.c_ssize_t,
                            ctypes.c_int, ctypes.c_void_p)

lib.hfs_lib_async_pwrite.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                     ctypes.c_size_t, ctypes.c_longlong,
                                     Callback, ctypes.c_void_p,
                                     ctypes.POINTER(Request)]
lib.hfs_lib_async_pread.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                    ctypes.c_size_t, ctypes.c_longlong,
                                    Callback, ctypes.c_void_p,
                                    ctypes.POINTER(Request)]
lib.hfs_lib_async_pwritev.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                      ctypes.c_int, ctypes.c_longlong,
                                      Callback, ctypes.c_void_p,
                                      ctypes.POINTER(Request)]
lib.hfs_lib_async_fsync.argtypes = [ctypes.c_int, Callback, ctypes.c_void_p,
                                    ctypes.POINTER(Request)]
lib.hfsAsyncWait.argtypes = [Request, ctypes.c_uint]
lib.hfsAsyncResult.argtypes = [Request, ctypes.POINTER(ctypes.c_ssize_t),
                               ctypes.POINTER(ctypes.c_int)]
lib.hfsAsyncCancel.argtypes = [Request]
lib.hfsAsyncRelease.argtypes = [Request]

lock = threading.Lock()
callbacks = []
callbacks_ready = threading.Event()

def completed(request, result, error, context):
    with lock:
        callbacks.append((int(result), int(error)))
        if len(callbacks) >= 16:
            callbacks_ready.set()

callback = Callback(completed)
requests = []
buffers = []
unit = 4096
for index in range(16):
    payload = ctypes.create_string_buffer(bytes([65 + index]) * unit)
    request = Request()
    rc = lib.hfs_lib_async_pwrite(fd, payload, unit, index * unit,
                                  callback, None, ctypes.byref(request))
    assert rc == 0, ("async pwrite", rc, ctypes.get_errno())
    buffers.append(payload)
    requests.append(request)

for request in requests:
    assert lib.hfsAsyncWait(request, 10000) == 0
    result = ctypes.c_ssize_t()
    error = ctypes.c_int()
    assert lib.hfsAsyncResult(request, ctypes.byref(result), ctypes.byref(error)) == 0
    assert result.value == unit and error.value == 0, (result.value, error.value)
    lib.hfsAsyncRelease(request)

assert callbacks_ready.wait(5), callbacks
assert len(callbacks) >= 16, callbacks
data = os.pread(fd, unit * 16, 0)
assert len(data) == unit * 16
for index in range(16):
    assert data[index * unit:(index + 1) * unit] == bytes([65 + index]) * unit

# Exercise vector submission while retaining caller-owned iovec storage.
left = ctypes.create_string_buffer(b"vector-left")
right = ctypes.create_string_buffer(b"vector-right")
class IOVec(ctypes.Structure):
    _fields_ = [("iov_base", ctypes.c_void_p), ("iov_len", ctypes.c_size_t)]
iov = (IOVec * 2)(IOVec(ctypes.cast(left, ctypes.c_void_p), len(left.value)),
                   IOVec(ctypes.cast(right, ctypes.c_void_p), len(right.value)))
request = Request()
assert lib.hfs_lib_async_pwritev(fd, iov, 2, unit * 16, callback, None,
                                 ctypes.byref(request)) == 0
assert lib.hfsAsyncWait(request, 10000) == 0
result = ctypes.c_ssize_t()
error = ctypes.c_int()
assert lib.hfsAsyncResult(request, ctypes.byref(result), ctypes.byref(error)) == 0
assert result.value == len(left.value) + len(right.value) and error.value == 0
lib.hfsAsyncRelease(request)

# Cancellation suppresses callback delivery but does not free an in-flight request.
cancelled = Request()
payload = ctypes.create_string_buffer(b"cancelled")
assert lib.hfs_lib_async_pwrite(fd, payload, len(payload.value), unit * 17,
                                callback, None, ctypes.byref(cancelled)) == 0
assert lib.hfsAsyncCancel(cancelled) == 0
assert lib.hfsAsyncWait(cancelled, 10000) == 0
result = ctypes.c_ssize_t()
error = ctypes.c_int()
assert lib.hfsAsyncResult(cancelled, ctypes.byref(result), ctypes.byref(error)) == 0
assert error.value in (0, errno.ECANCELED), error.value
lib.hfsAsyncRelease(cancelled)

sync_request = Request()
assert lib.hfs_lib_async_fsync(fd, callback, None, ctypes.byref(sync_request)) == 0
assert lib.hfsAsyncWait(sync_request, 10000) == 0
assert lib.hfsAsyncResult(sync_request, ctypes.byref(result), ctypes.byref(error)) == 0
assert error.value == 0, error.value
lib.hfsAsyncRelease(sync_request)
os.close(fd)
print("ASYNC IO OK callbacks=%d" % len(callbacks))
PYEOF

async_submit="$(awk -F= '$1 == "async_submit" {print $2}' "$STATS_PATH")"
async_complete="$(awk -F= '$1 == "async_complete" {print $2}' "$STATS_PATH")"
async_peak="$(awk -F= '$1 == "async_queue_depth_peak" {print $2}' "$STATS_PATH")"
if [[ -z "$async_submit" || "$async_submit" != "$async_complete" || "${async_peak:-0}" -lt 2 ]]; then
	echo "async executor accounting failed: submit=$async_submit complete=$async_complete peak=$async_peak" >&2
	exit 1
fi

echo "== async I/O smoke passed =="
