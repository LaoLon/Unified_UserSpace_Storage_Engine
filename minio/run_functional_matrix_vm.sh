#!/bin/bash
# Run the MinIO functional matrix against a running endpoint (single node or
# any cluster node), using the helper tools built by build_minio_vm.sh:
#   MODE=matrix (default) -> matrix_client, ENDPOINT via MATRIX_ENDPOINT
#   MODE=quad              -> quad_client, QUAD_ENDPOINTS (comma separated)
#   MODE=dist              -> dist_client, DIST_ENDPOINTS (comma separated)
#
# Env: DIST, ENDPOINT (default first NODES host:9000), LOG.
set -u
DIST=${DIST:-/root/minio-dist}
NODES=${NODES:-"192.168.1.250 192.168.1.251 192.168.1.252 192.168.1.253"}
first_node=$(echo "$NODES" | awk '{print $1}')
ENDPOINT=${ENDPOINT:-$first_node:9000}
MODE=${MODE:-matrix}
LOG=${LOG:-/tmp/minio_matrix_${MODE}.log}

[ -x "$DIST/matrix-client" ] || { echo "no tools in $DIST (run build_minio_vm.sh)"; exit 1; }

case "$MODE" in
  matrix)
    MATRIX_ENDPOINT="$ENDPOINT" "$DIST/matrix-client" 2>&1 | tee "$LOG" | tail -8
    grep -q 'MATRIX-DONE pass=.* fail=0' "$LOG" && echo FUNCTIONAL-MATRIX-PASS || { echo FUNCTIONAL-MATRIX-FAIL; exit 1; }
    ;;
  quad)
    list=""
    for n in $NODES; do list="$list,$n:9000"; done
    QUAD_ENDPOINTS="${QUAD_ENDPOINTS:-${list#,}}" "$DIST/quad-client" 2>&1 | tee "$LOG"
    grep -q 'QUAD-CROSS-PASS' "$LOG" && echo QUAD-PASS || { echo QUAD-FAIL; exit 1; }
    ;;
  dist)
    list=""
    for n in $NODES; do list="$list,$n:9000"; done
    DIST_ENDPOINTS="${DIST_ENDPOINTS:-${list#,}}" "$DIST/dist-client" 2>&1 | tee "$LOG"
    grep -q 'DIST-CROSS-PASS' "$LOG" && echo DIST-PASS || { echo DIST-FAIL; exit 1; }
    ;;
  *)
    echo "usage: run_functional_matrix_vm.sh  (MODE=matrix|quad|dist)"; exit 2 ;;
esac
