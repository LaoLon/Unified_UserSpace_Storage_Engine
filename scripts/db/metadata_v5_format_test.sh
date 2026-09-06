#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
BUILD_DIR="${ROOT_DIR}/build/bin"
TEST_BIN="${BUILD_DIR}/metadata_v5_format_test"

mkdir -p "${BUILD_DIR}"
cc -std=gnu11 -O2 -Wall -Wextra -Werror \
  -I"${ROOT_DIR}/src" -I"${ROOT_DIR}/spdk/include" \
  -I"${ROOT_DIR}/spdk/dpdk/include" \
  "${ROOT_DIR}/src/hfs_meta_v5.c" \
  "${ROOT_DIR}/scripts/db/metadata_v5_format_test.c" \
  -o "${TEST_BIN}"
"${TEST_BIN}"
echo "METADATA V5 FORMAT OK"
