#!/usr/bin/env bash
# Verify metadata snapshots, cwd state, directory handles, and open flags.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-shard29.img}"
RUN_DIR="${4:-/var/tmp/hfs-shard29}"
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
import ctypes
import errno
import os
import threading

root = "/hfs/shard29"
os.mkdir(root)

# Relative paths use the complete HFS cwd, including relative chdir updates.
os.chdir(root)
os.mkdir("nested")
os.chdir("nested")
fd = os.open("relative.dat", os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
assert os.write(fd, b"relative") == 8
os.close(fd)
os.chdir("..")
assert os.stat("nested/relative.dat").st_size == 8

# fchdir and openat must use an HFS directory descriptor as their base.
dir_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
assert os.fstat(dir_fd).st_size == 0
os.fchdir(dir_fd)
at_fd = os.open("openat.dat", os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600,
                dir_fd=dir_fd)
os.write(at_fd, b"openat")
os.close(at_fd)

# O_CREAT|O_EXCL rejects an existing inode.
try:
    os.open("openat.dat", os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
except OSError as exc:
    assert exc.errno == errno.EEXIST, exc
else:
    raise AssertionError("O_EXCL unexpectedly opened an existing file")

# O_DIRECTORY rejects regular files with the POSIX directory error.
try:
    os.open("openat.dat", os.O_RDONLY | os.O_DIRECTORY)
except OSError as exc:
    assert exc.errno == errno.ENOTDIR, exc
else:
    raise AssertionError("O_DIRECTORY unexpectedly opened a regular file")

# O_APPEND selects EOF for each independent open-file description.
append_path = "append.dat"
append_fd = os.open(append_path, os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o600)
assert os.write(append_fd, b"first") == 5
append_fd2 = os.open(append_path, os.O_WRONLY | os.O_APPEND)
assert os.write(append_fd2, b"-second") == 7
os.close(append_fd2)
os.lseek(append_fd, 0, os.SEEK_SET)
assert os.read(append_fd, 64) == b"first-second"
os.close(append_fd)

# fdopendir consumes the HFS directory fd and closes it with closedir.
fd_for_dir = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
libc = ctypes.CDLL(None, use_errno=True)
libc.fdopendir.argtypes = [ctypes.c_int]
libc.fdopendir.restype = ctypes.c_void_p
libc.readdir.argtypes = [ctypes.c_void_p]
libc.readdir.restype = ctypes.c_void_p
libc.closedir.argtypes = [ctypes.c_void_p]
libc.closedir.restype = ctypes.c_int
dirp = libc.fdopendir(fd_for_dir)
assert dirp, ctypes.get_errno()
assert libc.readdir(dirp)
assert libc.closedir(dirp) == 0

# Independent metadata operations must remain coherent under contention.
errors = []

def worker(index):
    try:
        for sequence in range(40):
            path = f"worker-{index}-{sequence}"
            renamed = path + ".renamed"
            local_fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
            assert os.write(local_fd, b"payload") == 7
            assert os.fstat(local_fd).st_size == 7
            os.close(local_fd)
            os.rename(path, renamed)
            assert os.stat(renamed).st_size == 7
            with os.scandir(".") as entries:
                for entry in entries:
                    entry.is_file(follow_symlinks=False)
            os.unlink(renamed)
    except BaseException as exc:
        errors.append((index, repr(exc)))

threads = [threading.Thread(target=worker, args=(index,)) for index in range(16)]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
assert not errors, errors

os.close(dir_fd)
print("METADATA PATH CONCURRENCY OK")
PYEOF

echo "== metadata/path concurrency smoke passed =="
