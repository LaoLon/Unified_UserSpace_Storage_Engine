#!/usr/bin/env bash
# Build and run the standalone SPDK bdev layer benchmark for P0-2 A1.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
CONFIG=${1:-"$ROOT/config/hfs_nvme.json"}

make -C "$ROOT/scripts/db/task90150022_bdev_bench"
exec "$ROOT/scripts/db/task90150022_bdev_bench/task90150022_bdev_bench" "$CONFIG"
