#!/usr/bin/env bash
# Verify that the optional request trace records the copy and queue stages.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-trace-smoke.img}"
RUN_DIR="${4:-/var/tmp/hfs-trace-smoke}"
TRACE_PATH="$RUN_DIR/io-trace.tsv"

mkdir -p "$RUN_DIR"
rm -f -- "$TRACE_PATH"
HFS_TRACE_PATH="$TRACE_PATH" \
	bash "$ROOT/scripts/db/io_fast_path_smoke.sh" "$LIB" "$CONFIG" "$IMAGE" "$RUN_DIR"

TRACE_PATH="$TRACE_PATH" python3 - <<'PYEOF'
import os
from pathlib import Path

path = Path(os.environ["TRACE_PATH"])
lines = path.read_text(encoding="utf-8").splitlines()
if not lines or not lines[0].startswith("sequence\ttid\ttimestamp_ns\tphase"):
    raise SystemExit("trace header missing")
phases = {line.split("\t", 4)[3] for line in lines[1:] if not line.startswith("#")}
required = {"io_enter", "io_exit", "buffer_classify",
            "spdk_submit", "spdk_complete", "runtime_enqueue",
            "runtime_dequeue"}
missing = required - phases
if missing:
    raise SystemExit(f"trace phases missing: {sorted(missing)}")
if not any("copy_in_begin" in line for line in lines):
    raise SystemExit("trace did not record copy-in")
if not any("copy_out_begin" in line for line in lines):
    raise SystemExit("trace did not record copy-out")
print(f"IO TRACE OK: {len(lines) - 1} records, phases={len(phases)}")
PYEOF
