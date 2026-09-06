#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
"${ROOT_DIR}/scripts/db/metadata_v5_format_test.sh" "${ROOT_DIR}"
echo "METADATA WAL RECOVERY OK"
