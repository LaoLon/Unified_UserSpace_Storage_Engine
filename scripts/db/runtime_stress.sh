#!/usr/bin/env bash
# Verify owner-thread completion delivery under concurrent application load.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-runtime-stress.img}"
RUN_DIR="${4:-/var/tmp/hfs-runtime-stress}"
THREADS="${HFS_STRESS_THREADS:-32}"
DURATION="${HFS_STRESS_SECONDS:-60}"
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
	env LD_PRELOAD="$LIB" HFS_CONFIG_PATH="$EFFECTIVE_CONFIG" HFS_BDEV_NAME=Aio0 \
		HFS_LOCK_PATH="$LOCK_PATH" "$@"
}

echo "== HFS runtime owner-thread stress =="
echo "  threads: $THREADS"
echo "  seconds: $DURATION"

RUNTIME_LOG="$RUN_DIR/runtime.log"
if ! run_hfs HFS_FORMAT=1 HFS_STRESS_THREADS="$THREADS" HFS_STRESS_SECONDS="$DURATION" \
	timeout "$((DURATION + 15))" python3 - <<'PYEOF' 2>"$RUNTIME_LOG"
import os
import threading
import time

path = "/hfs/runtime-stress.dat"
threads = int(os.environ["HFS_STRESS_THREADS"])
duration = float(os.environ["HFS_STRESS_SECONDS"])
block_size = 4096
file_size = threads * block_size
errors = []
errors_lock = threading.Lock()
start_barrier = threading.Barrier(threads)
stop = threading.Event()

fd = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o600)
try:
    assert os.pwrite(fd, b"\0" * file_size, 0) == file_size
    os.fsync(fd)
finally:
    os.close(fd)

def worker(worker_id):
    offset = worker_id * block_size
    payload = bytes([worker_id & 0xff]) * block_size
    fd = -1
    try:
        start_barrier.wait()
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline and not stop.is_set():
            fd = os.open(path, os.O_RDWR)
            data = os.pread(fd, block_size, offset)
            assert len(data) == block_size
            assert os.pwrite(fd, payload, offset) == block_size
            os.fsync(fd)
            os.close(fd)
            fd = -1
    except BaseException as exc:
        with errors_lock:
            errors.append((worker_id, repr(exc)))
        stop.set()
    finally:
        if fd >= 0:
            os.close(fd)

workers = [threading.Thread(target=worker, args=(i,)) for i in range(threads)]
for thread in workers:
    thread.start()
for thread in workers:
    thread.join()

if errors:
    raise AssertionError("worker failures: %r" % errors)

print("RUNTIME STRESS OK")
PYEOF

then
	cat "$RUNTIME_LOG" >&2
	exit 1
fi

if grep -q "SPDK completion arrived outside owner thread" "$RUNTIME_LOG"; then
	echo "ERROR: completion callback escaped the owner thread" >&2
	cat "$RUNTIME_LOG" >&2
	exit 1
fi

echo "== runtime stress passed =="
