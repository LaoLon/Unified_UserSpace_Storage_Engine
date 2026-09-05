#!/usr/bin/env bash
# Verify that injected request failures reach callers without leaking create state.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-io-error-injection.img}"
RUN_DIR="${4:-/var/tmp/hfs-io-error-injection}"
LOCK_PATH="$RUN_DIR/hfs.lock"
STATE_FILE="$RUN_DIR/expected-free-clusters"

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

run_hfs HFS_TEST_STATE_FILE="$STATE_FILE" HFS_FORMAT=1 python3 - <<'PYEOF'
import errno
import ctypes
import os

root = "/hfs/io-error-injection"
os.mkdir(root)
seed = root + "/seed"
fd = os.open(seed, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
assert os.write(fd, b"seed") == 4
os.close(fd)

def expect_eio(point, operation, phase="HFS_TEST_FAIL_SUBMIT"):
    os.environ[phase] = point
    try:
        operation()
    except OSError as exc:
        assert exc.errno == errno.EIO, (point, exc.errno)
    else:
        raise AssertionError("injected request returned success: " + point)
    finally:
        os.environ.pop(phase, None)

# Submission and owner-thread callback failures must both expose EIO to callers.
expect_eio("blob-open", lambda: os.open(seed, os.O_RDONLY),
           "HFS_TEST_FAIL_COMPLETE")
fd = os.open(seed, os.O_RDWR)
expect_eio("blob-sync", lambda: os.fsync(fd), "HFS_TEST_FAIL_COMPLETE")
os.close(fd)

# close(2) must return the SPDK failure while retiring its placeholder fd.
# The retained blob handle is retried during orderly library teardown.
fd = os.open(seed, os.O_RDONLY)
expect_eio("blob-close", lambda: os.close(fd), "HFS_TEST_FAIL_COMPLETE")

# Data I/O and resize use the same request-private error path. Failed writes
# and growth requests must leave the committed file contents and size intact.
fd = os.open(seed, os.O_RDWR)
expect_eio("blob-read", lambda: os.pread(fd, 4, 0), "HFS_TEST_FAIL_COMPLETE")
expect_eio("blob-write", lambda: os.pwrite(fd, b"FAIL", 0), "HFS_TEST_FAIL_COMPLETE")
expect_eio("blob-resize", lambda: os.pwrite(fd, b"grow", 8 * 1024 * 1024),
           "HFS_TEST_FAIL_COMPLETE")
assert os.pread(fd, 4, 0) == b"seed"
assert os.fstat(fd).st_size == 4
os.close(fd)

# fsync must flush data first and then surface a metadata transaction failure;
# after the fault clears, a subsequent fsync commits that inode metadata and
# performs the post-commit device barrier.
fd = os.open(seed, os.O_RDWR)
assert os.pwrite(fd, b"meta", 0) == 4
expect_eio("metadata-write", lambda: os.fsync(fd), "HFS_TEST_FAIL_COMPLETE")
visible_after_failed_fsync = os.pread(fd, 4, 0)
assert visible_after_failed_fsync == b"meta", visible_after_failed_fsync
os.fsync(fd)

# A post-commit device barrier failure must fail fsync even though the inode
# transaction completed. A retry uses the normal barrier and must succeed.
assert os.pwrite(fd, b"last", 0) == 4
expect_eio("bdev-flush-post-commit", lambda: os.fsync(fd),
           "HFS_TEST_FAIL_COMPLETE")
assert os.pread(fd, 4, 0) == b"last"
os.fsync(fd)
os.close(fd)

# A failed accelerated WRITE_ZEROES completion must not leave a successful
# unmap with unspecified data: fallocate succeeds only after the DMA fallback
# writes observable zeroes over the punched range.
libc = ctypes.CDLL(None, use_errno=True)
libc.fallocate.argtypes = [ctypes.c_int, ctypes.c_int,
                           ctypes.c_longlong, ctypes.c_longlong]
libc.fallocate.restype = ctypes.c_int
punch_path = root + "/punch-fallback"
fd = os.open(punch_path, os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
assert os.write(fd, b"P" * 12288) == 12288
os.environ["HFS_TEST_FAIL_COMPLETE"] = "blob-write-zeroes"
try:
    assert libc.fallocate(fd, 0x03, 4096, 4096) == 0, ctypes.get_errno()
finally:
    os.environ.pop("HFS_TEST_FAIL_COMPLETE", None)
assert os.pread(fd, 12288, 0) == b"P" * 4096 + b"\0" * 4096 + b"P" * 4096
os.close(fd)

# Capacity collection is an owner-thread request too, so it cannot report a
# fabricated successful statvfs result after a callback failure.
expect_eio("capacity", lambda: os.statvfs(root), "HFS_TEST_FAIL_COMPLETE")

completion_failed = root + "/completion-failed"
expect_eio("metadata-write", lambda: os.open(
    completion_failed, os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600),
           "HFS_TEST_FAIL_COMPLETE")
assert not os.path.exists(completion_failed)

# Fail before every metadata-image write after data-blob creation. A failure
# must not leave a directory entry or consume a data cluster after 1,000 tries.
before = os.statvfs(root).f_bfree
for index in range(1000):
    candidate = root + "/failed-{:04d}".format(index)
    expect_eio("metadata-write", lambda candidate=candidate: os.open(
        candidate, os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600))
    assert not os.path.exists(candidate), candidate
after = os.statvfs(root).f_bfree
assert after == before, (before, after)

# Once a namespace transaction is committed, a failed follow-on data blob
# delete cannot resurrect the old name. The unreachable blob is reclaimed by
# the next mount, which is the only point with no live open description.
orphaned = root + "/delete-retry"
before_orphan = os.statvfs(root).f_bfree
fd = os.open(orphaned, os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
assert os.write(fd, b"x" * 4096) == 4096
os.fsync(fd)
os.close(fd)
after_orphan_create = os.statvfs(root).f_bfree
orphan_clusters = before_orphan - after_orphan_create
assert orphan_clusters > 0, (before_orphan, after_orphan_create)
expect_eio("blob-delete", lambda: os.unlink(orphaned))
assert not os.path.exists(orphaned)
assert os.statvfs(root).f_bfree == after_orphan_create

verified = root + "/verified"
fd = os.open(verified, os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
assert os.write(fd, b"verified") == 8
os.fsync(fd)
os.close(fd)

expected_after_reclaim = os.statvfs(root).f_bfree + orphan_clusters
with open(os.environ["HFS_TEST_STATE_FILE"], "w", encoding="ascii") as state:
    state.write(str(expected_after_reclaim))

# A metadata-read setup failure must also be visible and leave no entry.
blocked_open = root + "/blocked-open"
expect_eio("metadata-open", lambda: os.mkdir(blocked_open),
           "HFS_TEST_FAIL_COMPLETE")
print("IO ERROR INJECTION PHASE 1 OK")
PYEOF

# A fresh process must see only the successful metadata transaction.
run_hfs HFS_TEST_STATE_FILE="$STATE_FILE" python3 - <<'PYEOF'
import errno
import os

root = "/hfs/io-error-injection"
assert os.read(os.open(root + "/verified", os.O_RDONLY), 16) == b"verified"
with open(os.environ["HFS_TEST_STATE_FILE"], "r", encoding="ascii") as state:
    expected_free = int(state.read())
assert os.statvfs(root).f_bfree == expected_free
for index in (0, 499, 999):
    try:
        os.stat(root + "/failed-{:04d}".format(index))
    except OSError as exc:
        assert exc.errno == errno.ENOENT
    else:
        raise AssertionError("failed create survived remount")
print("IO ERROR INJECTION PERSISTENCE OK")
PYEOF

echo "== I/O error injection passed =="
