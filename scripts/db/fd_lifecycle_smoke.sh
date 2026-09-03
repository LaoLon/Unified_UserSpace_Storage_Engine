#!/usr/bin/env bash
# Verify fd slot reuse, shared dup offsets, and unlink-open lifetime semantics.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-fd-lifecycle.img}"
RUN_DIR="${4:-/var/tmp/hfs-fd-lifecycle}"
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

env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
	HFS_BDEV_NAME=Aio0 HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 \
	python3 - <<'PYEOF'
import errno
import fcntl
import os
import threading

root = "/hfs/fd-lifecycle"
try:
    os.rmdir(root)
except OSError:
    pass
os.mkdir(root)

# dup shares the same open-file description and therefore the file offset.
path = root + "/shared"
fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
assert os.write(fd, b"abcdef") == 6
assert os.lseek(fd, 0, os.SEEK_SET) == 0
dup_fd = os.dup(fd)
first = os.read(fd, 2)
second = os.read(dup_fd, 2)
assert first == b"ab", ("first read", first)
assert second == b"cd", ("shared offset read", second)
os.close(dup_fd)

# F_DUPFD_CLOEXEC and dup2 must share the same offset and status flags.
assert fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDWR
fcntl_dup = fcntl.fcntl(fd, getattr(fcntl, "F_DUPFD_CLOEXEC", fcntl.F_DUPFD), 0)
assert os.read(fcntl_dup, 2) == b"ef"
os.close(fcntl_dup)
assert os.lseek(fd, 0, os.SEEK_SET) == 0
dup2_fd = os.dup2(fd, fd + 20)
assert os.read(dup2_fd, 2) == b"ab"
os.close(dup2_fd)
os.close(fd)

# Closing a slot must permit immediate reuse without stale HFS routing.
fd = os.open(path, os.O_RDONLY)
old_number = fd
os.close(fd)
fd = os.open(root + "/reused", os.O_CREAT | os.O_RDWR, 0o600)
assert fd == old_number
assert os.write(fd, b"reuse") == 5
os.close(fd)

# An unlinked file stays available through its old open description.
orphan_path = root + "/orphan"
fd = os.open(orphan_path, os.O_CREAT | os.O_RDWR, 0o600)
assert os.write(fd, b"orphan-data") == 11
os.fsync(fd)
os.unlink(orphan_path)
assert not os.path.exists(orphan_path)
try:
    os.open(orphan_path, os.O_RDONLY)
except OSError as exc:
    assert exc.errno == errno.ENOENT
else:
    raise AssertionError("unlinked path unexpectedly reopened")
assert os.pread(fd, 11, 0) == b"orphan-data"
os.close(fd)

# Renaming over an open destination keeps the old description alive.
rename_dst = root + "/rename-dst"
rename_src = root + "/rename-src"
old_fd = os.open(rename_dst, os.O_CREAT | os.O_RDWR, 0o600)
assert os.write(old_fd, b"old-data") == 8
new_fd = os.open(rename_src, os.O_CREAT | os.O_RDWR, 0o600)
assert os.write(new_fd, b"new-data") == 8
os.fsync(old_fd)
os.fsync(new_fd)
os.rename(rename_src, rename_dst)
assert os.pread(old_fd, 8, 0) == b"old-data"
os.close(new_fd)
reopened_fd = os.open(rename_dst, os.O_RDONLY)
assert os.read(reopened_fd, 8) == b"new-data"
os.close(reopened_fd)
os.close(old_fd)

# Renaming a path onto itself is a no-op and must retain its blob.
same_path = root + "/same"
same_fd = os.open(same_path, os.O_CREAT | os.O_RDWR, 0o600)
assert os.write(same_fd, b"same-data") == 9
os.rename(same_path, same_path)
assert os.pread(same_fd, 9, 0) == b"same-data"
os.close(same_fd)
os.unlink(same_path)

# Independent descriptions can perform concurrent positioned I/O safely.
parallel_path = root + "/parallel"
fd = os.open(parallel_path, os.O_CREAT | os.O_RDWR, 0o600)
assert os.pwrite(fd, b"\0" * (8 * 4096), 0) == 8 * 4096
os.close(fd)
errors = []

def worker(index):
    local_fd = -1
    try:
        local_fd = os.open(parallel_path, os.O_RDWR)
        payload = bytes([index]) * 4096
        offset = index * 4096
        assert os.pwrite(local_fd, payload, offset) == len(payload)
        assert os.pread(local_fd, len(payload), offset) == payload
    except BaseException as exc:
        errors.append((index, repr(exc)))
    finally:
        if local_fd >= 0:
            os.close(local_fd)

threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
assert not errors, errors

print("FD LIFECYCLE OK")
PYEOF

echo "== fd lifecycle smoke passed =="
