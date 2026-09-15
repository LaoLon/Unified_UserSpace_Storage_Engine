#!/usr/bin/env bash
# Verify the POSIX namespace, metadata, capacity, hint, and mmap contracts.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-posix-contract.img}"
RUN_DIR="${4:-/var/tmp/hfs-posix-contract}"
LOCK_PATH="$RUN_DIR/hfs.lock"
BDEV_NAME="${HFS_BDEV_NAME:-Aio0}"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [config] [image] [run-dir]" >&2
	exit 2
fi

mkdir -p "$RUN_DIR"
if [[ "$BDEV_NAME" == "Aio0" ]]; then
	EFFECTIVE_CONFIG="$RUN_DIR/hfs_db_aio.json"
	sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
		"$CONFIG" > "$EFFECTIVE_CONFIG"
	HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G
else
	# Use the supplied real-device configuration without creating a file-backed bdev.
	EFFECTIVE_CONFIG="$CONFIG"
fi

env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" \
	HFS_BDEV_NAME="$BDEV_NAME" HFS_LOCK_PATH="$LOCK_PATH" HFS_FORMAT=1 \
	python3 - <<'PYEOF'
import ctypes
import errno
import os

root = "/hfs/posix-contract"
os.mkdir(root)
os.mkdir(root + "/nested")
dirfd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)

# openat/mkdirat/fstatat/renameat/unlinkat must keep HFS fd-relative paths.
fd = os.open("file", os.O_CREAT | os.O_RDWR, 0o640, dir_fd=dirfd)
assert os.write(fd, b"contract") == 8
stat_result = os.stat("file", dir_fd=dirfd)
assert stat_result.st_size == 8
assert stat_result.st_mode & 0o777 == 0o640
os.chmod("file", 0o600, dir_fd=dirfd)
assert os.fstat(fd).st_mode & 0o777 == 0o600
os.chown("file", os.getuid(), os.getgid(), dir_fd=dirfd)
os.utime("file", (123.0, 456.0), dir_fd=dirfd)
assert int(os.stat("file", dir_fd=dirfd).st_mtime) == 456
os.rename("file", "renamed", src_dir_fd=dirfd, dst_dir_fd=dirfd)
assert os.stat("renamed", dir_fd=dirfd).st_size == 8
os.unlink("renamed", dir_fd=dirfd)
try:
    os.stat("renamed", dir_fd=dirfd)
except OSError as exc:
    assert exc.errno == errno.ENOENT
else:
    raise AssertionError("unlinkat did not remove the file")

# fstatat's AT_EMPTY_PATH path is required by several database probes.
empty_fd = os.open("empty", os.O_CREAT | os.O_RDWR, 0o600, dir_fd=dirfd)
libc = ctypes.CDLL(None, use_errno=True)
libc.fstatat.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int]
libc.fstatat.restype = ctypes.c_int
stat_buf = ctypes.create_string_buffer(256)
at_empty_path = 0x1000
assert libc.fstatat(empty_fd, b"", stat_buf, at_empty_path) == 0

# statx/statfs/statvfs and fstatfs/fstatvfs must report a usable HFS mount.
libc.statx.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                       ctypes.c_uint, ctypes.c_void_p]
libc.statx.restype = ctypes.c_int
statx_buf = ctypes.create_string_buffer(256)
statx_rc = libc.statx(-100, b"/hfs/posix-contract/empty", 0, 0xFFF, statx_buf)
assert statx_rc == 0
assert int.from_bytes(statx_buf.raw[0:4], "little") != 0

libc.statfs.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
libc.statfs.restype = ctypes.c_int
libc.fstatfs.argtypes = [ctypes.c_int, ctypes.c_void_p]
libc.fstatfs.restype = ctypes.c_int
libc.statvfs.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
libc.statvfs.restype = ctypes.c_int
libc.fstatvfs.argtypes = [ctypes.c_int, ctypes.c_void_p]
libc.fstatvfs.restype = ctypes.c_int
fs_buf = ctypes.create_string_buffer(256)
vfs_buf = ctypes.create_string_buffer(256)
assert libc.statfs(b"/hfs/posix-contract", fs_buf) == 0
assert libc.fstatfs(dirfd, fs_buf) == 0
assert libc.statvfs(b"/hfs/posix-contract", vfs_buf) == 0
assert libc.fstatvfs(dirfd, vfs_buf) == 0
assert os.statvfs(root).f_bsize > 0

# Valid advisory hints are no-ops; invalid values return the POSIX error code.
libc.posix_fadvise.argtypes = [ctypes.c_int, ctypes.c_long, ctypes.c_long, ctypes.c_int]
libc.posix_fadvise.restype = ctypes.c_int
assert libc.posix_fadvise(empty_fd, 0, 0, os.POSIX_FADV_SEQUENTIAL) == 0
assert libc.posix_fadvise(empty_fd, 0, 0, 9999) == errno.EINVAL

# HFS fds deliberately reject mmap instead of exposing the /dev/null placeholder.
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.mmap.restype = ctypes.c_void_p
mapped = libc.mmap(None, 4096, 1, 2, empty_fd, 0)
assert mapped == ctypes.c_void_p(-1).value
assert ctypes.get_errno() == errno.ENODEV

# chmod/fchown/futimens on the open description remain valid after rename.
assert libc.fchmod(empty_fd, 0o644) == 0
assert libc.fchown(empty_fd, os.getuid(), os.getgid()) == 0
libc.futimens.argtypes = [ctypes.c_int, ctypes.c_void_p]
libc.futimens.restype = ctypes.c_int
assert libc.futimens(empty_fd, None) == 0

os.unlink("empty", dir_fd=dirfd)
os.close(empty_fd)
os.close(fd)
os.close(dirfd)
os.rmdir(root + "/nested")
os.rmdir(root)

print("POSIX CONTRACT OK")
PYEOF

echo "== POSIX contract passed =="
