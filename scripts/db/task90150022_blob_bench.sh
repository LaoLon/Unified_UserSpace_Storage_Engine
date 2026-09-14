#!/usr/bin/env bash
# Build and run the standalone SPDK Blobstore layer benchmark for P0-2 A2.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONFIG="${1:-$ROOT/config/hfs_nvme.json}"
BS="${TASK_BLOB_BS:-4096}"
RW="${TASK_BLOB_RW:-randread}"
QD="${TASK_BLOB_QD:-32}"
SECONDS_TO_RUN="${TASK_BLOB_SECONDS:-5}"

make -C "$ROOT/scripts/db/task90150022_blob_bench"
env TASK_BLOB_BS="$BS" TASK_BLOB_RW="$RW" TASK_BLOB_QD="$QD" \
	TASK_BLOB_SECONDS="$SECONDS_TO_RUN" \
	"$ROOT/scripts/db/task90150022_blob_bench/task90150022_blob_bench" "$CONFIG"
