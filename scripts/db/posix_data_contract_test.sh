#!/usr/bin/env bash
# Verify truncate zero-fill, write timestamps, and refreshed filesystem capacity.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-posix-data-contract.img}"
RUN_DIR="${4:-/var/tmp/hfs-posix-data-contract}"
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

run_hfs() {
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
		HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" "$@"
}

run_hfs HFS_FORMAT=1 python3 - <<'PYEOF'
import os

root = "/hfs/posix-data-contract"
os.mkdir(root)

# Shrinking to zero must make every later grown byte read as zero.
regrow_path = root + "/regrow.dat"
regrow_size = 16384
fd = os.open(regrow_path, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.write(fd, b"A" * regrow_size) == regrow_size
os.fsync(fd)
os.ftruncate(fd, 0)
os.ftruncate(fd, regrow_size)
assert os.pread(fd, regrow_size, 0) == b"\0" * regrow_size
os.close(fd)

# O_TRUNC follows the same zero-fill rule after a later explicit grow.
truncate_path = root + "/open-trunc.dat"
fd = os.open(truncate_path, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.write(fd, b"T" * regrow_size) == regrow_size
os.close(fd)
fd = os.open(truncate_path, os.O_RDWR | os.O_TRUNC)
os.ftruncate(fd, regrow_size)
assert os.pread(fd, regrow_size, 0) == b"\0" * regrow_size
os.close(fd)

# Preserve bytes before a non-aligned EOF while clearing later cluster-sized data.
cross_path = root + "/cross-cluster.dat"
cross_size = 2 * 1024 * 1024 + 4096
cut = 1024 * 1024 + 17
fd = os.open(cross_path, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.write(fd, b"C" * cross_size) == cross_size
os.ftruncate(fd, cut)
os.ftruncate(fd, cross_size)
cross_data = os.pread(fd, cross_size, 0)
assert cross_data[:cut] == b"C" * cut
assert cross_data[cut:] == b"\0" * (cross_size - cut)
os.close(fd)

# A content overwrite must advance mtime even when the file size is unchanged.
mtime_path = root + "/mtime.dat"
fd = os.open(mtime_path, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.write(fd, b"mtime") == 5
os.utime(mtime_path, ns=(1_000_000_000, 1_000_000_000))
assert os.pwrite(fd, b"M", 0) == 1
os.fsync(fd)
assert os.fstat(fd).st_mtime_ns > 1_000_000_000
os.close(fd)

# Capacity uses data-cluster units and refreshes after allocation and deletion.
before = os.statvfs(root)
assert before.f_frsize >= 4096
assert before.f_blocks * before.f_frsize > 512 * 1024 * 1024
capacity_path = root + "/capacity.dat"
fd = os.open(capacity_path, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.pwrite(fd, b"X", before.f_frsize * 2) == 1
os.fsync(fd)
after_write = os.statvfs(root)
assert after_write.f_blocks == before.f_blocks
assert after_write.f_bfree < before.f_bfree
os.close(fd)
os.unlink(capacity_path)
after_delete = os.statvfs(root)
assert after_delete.f_blocks == before.f_blocks
assert after_delete.f_bfree > after_write.f_bfree

print("POSIX DATA CONTRACT PHASE 1 OK")
PYEOF

# A new process must observe the zeroed ranges and durable timestamp metadata.
run_hfs python3 - <<'PYEOF'
import os

root = "/hfs/posix-data-contract"
for name, size in (("regrow.dat", 16384), ("open-trunc.dat", 16384)):
    fd = os.open(root + "/" + name, os.O_RDONLY)
    assert os.pread(fd, size, 0) == b"\0" * size
    os.close(fd)

cross_size = 2 * 1024 * 1024 + 4096
cut = 1024 * 1024 + 17
fd = os.open(root + "/cross-cluster.dat", os.O_RDONLY)
cross_data = os.pread(fd, cross_size, 0)
assert cross_data[:cut] == b"C" * cut
assert cross_data[cut:] == b"\0" * (cross_size - cut)
os.close(fd)

assert os.stat(root + "/mtime.dat").st_mtime_ns > 1_000_000_000
assert os.statvfs(root).f_blocks * os.statvfs(root).f_frsize > 512 * 1024 * 1024

for name in ("regrow.dat", "open-trunc.dat", "cross-cluster.dat", "mtime.dat"):
    os.unlink(root + "/" + name)
os.rmdir(root)
print("POSIX DATA CONTRACT PERSISTENCE OK")
PYEOF

echo "== POSIX data contract passed =="
