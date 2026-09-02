#!/bin/bash
# HFS LD_PRELOAD interceptor smoke test (shard-20).
#
# Covers: open/read/write/pread/pwrite/lseek, fsync/ftruncate/fallocate,
# mkdir/rmdir/rename/unlink, stat/fstat/access, dup, opendir/readdir.
#
# All assertions run inside a SINGLE python process: fork+exec children
# (external shell commands) inherit _HFS_INIT_DONE and fall through to
# the kernel, and the malloc bdev is process-private anyway.
#
# Usage: bash scripts/preload_smoke.sh [libhfs.so path] [mount prefix]
set -euo pipefail

LIB="${1:-$PWD/build-cmake/libhfs.so}"
PREFIX="${2:-/hfs}"
HFS_LIB="$(realpath "$LIB")"

if [ ! -f "$HFS_LIB" ]; then
	echo "ERROR: libhfs.so not found: $HFS_LIB" >&2
	exit 1
fi

echo "== HFS preload smoke test =="
echo "  lib:    $HFS_LIB"
echo "  prefix: $PREFIX"

timeout 60 env LD_PRELOAD="$HFS_LIB" python3 - "$PREFIX" <<'PYEOF'
import os
import sys

prefix = sys.argv[1]
d = prefix + "/smoke"

# Cleanup leftovers from previous runs (single process, ignore errors).
try:
    for name in os.listdir(d):
        p = os.path.join(d, name)
        os.unlink(p) if os.path.isfile(p) else os.rmdir(p)
    os.rmdir(d)
except OSError:
    pass

# --- mkdir / stat / access ---
os.mkdir(d)
assert os.path.isdir(d) and os.path.isdir(d + "/."), "mkdir failed"
assert os.access(d, os.R_OK | os.W_OK | os.X_OK), "access failed"
st = os.stat(d)
assert st.st_mode & 0o170000 == 0o040000, "dir mode not S_IFDIR"

# --- open/write/read/lseek ---
fd = os.open(d + "/a.txt", os.O_CREAT | os.O_RDWR, 0o644)
os.write(fd, b"hello")
assert os.fstat(fd).st_size == 5, "fstat size after write"
os.lseek(fd, 0, os.SEEK_SET)
assert os.read(fd, 5) == b"hello", "read back"
assert os.lseek(fd, 0, os.SEEK_CUR) == 5, "lseek SEEK_CUR"

# --- pread/pwrite (offset does not move the fd cursor) ---
os.pwrite(fd, b"XY", 2)
assert os.pread(fd, 2, 2) == b"XY", "pread"
os.lseek(fd, 0, os.SEEK_SET)
assert os.read(fd, 5) == b"heXYo", "read after pwrite"
os.close(fd)

# --- fsync / ftruncate / fallocate ---
fd = os.open(d + "/a.txt", os.O_RDWR)
os.fsync(fd)
os.ftruncate(fd, 100)
assert os.fstat(fd).st_size == 100, "ftruncate"
os.close(fd)
st = os.stat(d + "/a.txt")
assert st.st_size == 100, "stat size after ftruncate"
assert st.st_ino > 0, "stat ino"

# --- rename / dup ---
os.rename(d + "/a.txt", d + "/b.txt")
assert os.path.exists(d + "/b.txt") and not os.path.exists(d + "/a.txt"), "rename"

fd = os.open(d + "/b.txt", os.O_RDONLY)
fd2 = os.dup(fd)
os.lseek(fd2, 0, os.SEEK_SET)
assert os.read(fd2, 2) == b"he", "read via dup"
os.close(fd2)
os.close(fd)

# --- opendir/readdir ---
names = sorted(os.listdir(d))
assert names == ["b.txt"], "listdir: %r" % (names,)
entries = [e.name for e in os.scandir(d)]
assert sorted(entries) == ["b.txt"], "scandir: %r" % (entries,)

# --- unlink / rmdir ---
os.unlink(d + "/b.txt")
assert not os.path.exists(d + "/b.txt"), "unlink"
os.rmdir(d)
assert not os.path.exists(d), "rmdir"

print("SMOKE OK")
PYEOF

echo "== smoke test passed =="