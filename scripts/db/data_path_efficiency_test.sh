#!/usr/bin/env bash
# Exercise DMA size classes, bounce accounting, and vector fallback correctness.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-data-efficiency.img}"
RUN_DIR="${4:-/var/tmp/hfs-data-efficiency}"
LOCK_PATH="$RUN_DIR/hfs.lock"
STATS_PATH="$RUN_DIR/data-efficiency.stats"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi
mkdir -p "$RUN_DIR"
EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

env LD_PRELOAD="$LIB" HFS_LIBHFS_PATH="$LIB" HFS_STATS_PATH="$STATS_PATH" \
	HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" HFS_BDEV_NAME=Aio0 \
	HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 HFS_RUNTIME_BATCH=1 \
python3 - <<'PYEOF'
import ctypes
import os

root = "/hfs/efficiency"
os.mkdir(root)
fd = os.open(f"{root}/data", os.O_CREAT | os.O_RDWR, 0o600)
os.ftruncate(fd, 2 * 1024 * 1024)
for index, size in enumerate((4 * 1024, 16 * 1024, 64 * 1024, 1024 * 1024)):
    payload = bytes([65 + index]) * size
    offset = index * 4096
    assert os.pwrite(fd, payload, offset) == size
    assert os.pread(fd, size, offset) == payload

# Exercise a DMA-addressable caller buffer for direct and native vector paths.
hfs = ctypes.CDLL(os.environ["HFS_LIBHFS_PATH"], mode=ctypes.RTLD_GLOBAL)
hfs.spdk_malloc.argtypes = [ctypes.c_size_t, ctypes.c_size_t,
                            ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
hfs.spdk_malloc.restype = ctypes.c_void_p
hfs.spdk_free.argtypes = [ctypes.c_void_p]
dma_size = 64 * 1024
dma_ptr = hfs.spdk_malloc(dma_size, 4096, None, 0xFFFFFFFF, 1)
assert dma_ptr
dma_buffer = (ctypes.c_ubyte * dma_size).from_address(dma_ptr)
for index in range(dma_size):
    dma_buffer[index] = 0x5A
assert os.pwrite(fd, dma_buffer, 0) == dma_size
assert os.preadv(fd, [dma_buffer], 0) == dma_size
hfs.spdk_free(dma_ptr)

# Keep vector semantics covered even when the caller buffers are not DMA memory.
parts = [b"vector-a" * 512, b"vector-b" * 512]
assert os.pwritev(fd, parts, 0) == sum(map(len, parts))
read_parts = [bytearray(len(part)) for part in parts]
assert os.preadv(fd, read_parts, 0) == sum(map(len, parts))
assert bytes(read_parts[0]) == parts[0] and bytes(read_parts[1]) == parts[1]
os.close(fd)
print("DATA PATH EFFICIENCY IO OK")
PYEOF

for class_id in 0 1 2 3; do
	value="$(awk -F= -v key="dma_pool_class_${class_id}" '$1 == key {print $2}' "$STATS_PATH")"
	if [[ -z "${value:-}" || "$value" -eq 0 ]]; then
		echo "DMA size class ${class_id} was not exercised" >&2
		exit 1
	fi
done
submit="$(awk -F= '$1 == "spdk_submit" {print $2}' "$STATS_PATH")"
complete="$(awk -F= '$1 == "spdk_complete" {print $2}' "$STATS_PATH")"
copy_bytes="$(awk -F= '$1 == "io_copy_bytes" {print $2}' "$STATS_PATH")"
vector_ops="$(awk -F= '$1 == "io_vector_ops" {print $2}' "$STATS_PATH")"
direct_ops="$(awk -F= '$1 == "io_direct_ops" {print $2}' "$STATS_PATH")"
echo "dma classes: $(grep '^dma_pool_class_' "$STATS_PATH" | tr '\n' ' ')"
echo "spdk_submit=${submit:-0} spdk_complete=${complete:-0} copy_bytes=${copy_bytes:-0} direct_ops=${direct_ops:-0} vector_ops=${vector_ops:-0}"
if [[ "${submit:-0}" != "${complete:-0}" || "${submit:-0}" -eq 0 ]]; then
	echo "SPDK accounting mismatch" >&2
	exit 1
fi
if [[ "${direct_ops:-0}" -eq 0 || "${vector_ops:-0}" -eq 0 ]]; then
	echo "DMA direct/vector path was not exercised" >&2
	exit 1
fi
echo "DATA PATH EFFICIENCY OK"
