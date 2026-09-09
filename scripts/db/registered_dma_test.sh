#!/usr/bin/env bash
# Verify registered DMA ranges, duplicate detection, and async lifetime closure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-registered-dma.img}"
RUN_DIR="${4:-/var/tmp/hfs-registered-dma}"
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

env LD_PRELOAD="$LIB" HFS_LIBHFS_PATH="$LIB" \
	HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" HFS_BDEV_NAME=Aio0 \
	HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 HFS_DATA_REACTORS=4 \
	HFS_DATA_STRIPE_KIB=64 HFS_RUNTIME_BATCH=1 \
python3 <<'PYEOF'
import ctypes
import errno
import os

lib = ctypes.CDLL(os.environ["HFS_LIBHFS_PATH"], mode=ctypes.RTLD_GLOBAL,
                  use_errno=True)
lib.hfsRegisterBuffer.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
lib.hfsRegisterBuffer.restype = ctypes.c_int
lib.hfsUnregisterBuffer.argtypes = [ctypes.c_void_p]
lib.hfsUnregisterBuffer.restype = ctypes.c_int
lib.spdk_malloc.argtypes = [ctypes.c_size_t, ctypes.c_size_t,
                            ctypes.c_void_p, ctypes.c_int, ctypes.c_uint]
lib.spdk_malloc.restype = ctypes.c_void_p
lib.spdk_free.argtypes = [ctypes.c_void_p]
lib.hfs_lib_async_pwrite.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                     ctypes.c_size_t, ctypes.c_longlong,
                                     ctypes.c_void_p, ctypes.c_void_p,
                                     ctypes.POINTER(ctypes.c_void_p)]
lib.hfsAsyncWait.argtypes = [ctypes.c_void_p, ctypes.c_uint]
lib.hfsAsyncResult.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ssize_t),
                               ctypes.POINTER(ctypes.c_int)]
lib.hfsAsyncRelease.argtypes = [ctypes.c_void_p]

root = "/hfs/registered-dma"
os.mkdir(root)
fd = os.open(f"{root}/data", os.O_CREAT | os.O_RDWR, 0o600)
size = 512 * 1024
os.ftruncate(fd, size)

heap = ctypes.create_string_buffer(4096)
assert lib.hfsRegisterBuffer(heap, len(heap)) == -1
assert ctypes.get_errno() == errno.EINVAL

buffer = lib.spdk_malloc(size, 4096, None, -1, 1)
assert buffer, "SPDK DMA allocation failed"
ctypes.memset(buffer, 0x5A, size)
register_rc = lib.hfsRegisterBuffer(buffer, size)
assert register_rc == 0
assert lib.hfsRegisterBuffer(buffer, size) == -1
assert ctypes.get_errno() == errno.EEXIST

request = ctypes.c_void_p()
rc = lib.hfs_lib_async_pwrite(fd, buffer, size, 0, None, None,
                              ctypes.byref(request))
assert rc == 0 and request.value, (rc, ctypes.get_errno())
unregister_rc = lib.hfsUnregisterBuffer(buffer)
if unregister_rc != 0:
    assert ctypes.get_errno() == errno.EBUSY
assert lib.hfsAsyncWait(request, 30000) == 0
result = ctypes.c_ssize_t()
error = ctypes.c_int()
assert lib.hfsAsyncResult(request, ctypes.byref(result), ctypes.byref(error)) == 0
assert result.value == size and error.value == 0, (result.value, error.value)
lib.hfsAsyncRelease(request)
unregister_rc = lib.hfsUnregisterBuffer(buffer)
assert unregister_rc == 0
assert lib.hfsUnregisterBuffer(buffer) == -1
assert ctypes.get_errno() == errno.ENOENT
lib.spdk_free(buffer)
assert os.pread(fd, 4096, 0) == bytes([0x5A]) * 4096
os.close(fd)
print("REGISTERED DMA IO OK direct=1 duplicate=EEXIST lifetime=closed")
PYEOF
