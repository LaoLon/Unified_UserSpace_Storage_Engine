#!/bin/bash
# Bitrot injection on ONE cluster node (Shard-9.6 curated entry): stop the
# local minio-bin, flip bytes inside one shard file of an object via the poke
# tool, restart the node, then read the object through READ_ENDPOINT (another
# node) to check EC reconstruction (1 shard) or fail-closed (3 shards).
#
# Every injected node must be handled separately; run this script on each node
# whose shard should be corrupted. Example for 3-of-4 corruption:
#   NODES="..." bash run_bitrot_probe_vm.sh            # on .250
#   NODES="..." REL=... bash run_bitrot_probe_vm.sh    # on .251, .252
#
# Env: DIST, CONF, DRIVE, OBJ (default /crash/obj-000.bin), REL (auto-detect),
#      OFFSET (default 65536), LEN (default 4096), BYTE (default 165),
#      READ_ENDPOINT (another node:9000), ACK_FILE (optional verify list),
#      CLEANUP=1 to remove the test object through READ_ENDPOINT afterwards.
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DIST=${DIST:-/root/minio-dist}
CONF=${CONF:-/etc/uuse-minio.conf}
DRIVE=${DRIVE:-/root/uuse-data}
OBJ=${OBJ:-/crash/obj-000.bin}
OFFSET=${OFFSET:-65536}
LEN=${LEN:-4096}
BYTE=${BYTE:-165}
export LD_LIBRARY_PATH="$DIST:/usr/local/lib/uuse"

[ -x "$DIST/s96-poke" ] && [ -x "$DIST/tree-probe" ] || { echo "no tools in $DIST (run build_minio_vm.sh)"; exit 1; }

echo "== stop local minio-bin =="
for pid in $(pgrep -x minio-bin); do kill -9 "$pid" 2>/dev/null || true; done
sleep 2

echo "== locate shard file =="
"$DIST/tree-probe" "$CONF" "$DRIVE" "$OBJ" 2>&1 | grep -vE '^uuse:|^\[CPU' | head -8
if [ -z "${REL:-}" ]; then
  SUB=$("$DIST/tree-probe" "$CONF" "$DRIVE" "$OBJ" 2>/dev/null | awk '/^d /{print $2; exit}')
  [ -n "$SUB" ] || { echo "part directory not found under $OBJ"; exit 1; }
  REL="$OBJ/$SUB/part.1"
fi
echo "poke path: $REL"

echo "== poke =="
"$DIST/s96-poke" "$CONF" "$DRIVE" "$REL" "$OFFSET" "$LEN" "$BYTE" 2>&1 | grep -vE '^uuse:|^\[CPU' || exit 1

echo "== restart this node =="
DIST="$DIST" CONF="$CONF" DRIVE="$DRIVE" bash "$SCRIPT_DIR/run_cluster_vm.sh" up 2>&1 | tail -1

if [ -n "${READ_ENDPOINT:-}" ]; then
  echo "== verify via $READ_ENDPOINT =="
  export MATRIX_ENDPOINT="$READ_ENDPOINT"
  if [ -n "${ACK_FILE:-}" ]; then
    "$DIST/crash-tool" verify "$ACK_FILE" | tail -1
  else
    "$DIST/crash-tool" sweep | tail -1
  fi
fi
if [ "${CLEANUP:-0}" = 1 ] && [ -n "${READ_ENDPOINT:-}" ]; then
  MATRIX_ENDPOINT="$READ_ENDPOINT" "$DIST/crash-tool" rm obj-000.bin | tail -1
fi
echo BITROT-PROBE-DONE
