#!/bin/bash
# Single-node TiKV + UUSE / PD / TiDB launch skeleton for UUSE Phase 8.
#
# SKELETON ONLY: the UUSE FS selection (config key + --uuse-config) is wired by
# Shard-116. Until the UuseFileSystem shim lands, this script documents the
# shape and the host/path split; it is not a passing path.
#
# Topology (doc/shard/uuse_tidb_shard_plan.md section 0.4):
#   PD + TiDB server: .254; TiKV + UUSE: .253 (Nvme0n1 @ 0000:13:00.0).
# Host paths (logs/status/socket/pid) stay outside UUSE; storage.data-dir goes
# to the UUSE namespace (section 21.3).
set -euo pipefail

TIKV_BIN="${TIKV_BIN:-/home/uuse/tikv-build/release/tikv-server}"
PD_BIN="${PD_BIN:-/home/uuse/tidb-build/pd-server}"
TIDB_BIN="${TIDB_BIN:-/home/uuse/tidb-build/tidb-server}"
ROOT="${ROOT:-/home/uuse/tidb-run}"
UUSE_CONF="${UUSE_CONF:-/root/code/config/uuse_tidb.conf}"

mkdir -p "$ROOT/log" "$ROOT/status"

# TODO(Shard-116): add the UUSE FS selection to tikv.toml once the shim lands,
# e.g. [storage] data-dir = "/uuse/tikv" plus the UuseFileSystem switch and the
# --uuse-config path; only then are the commands below a runnable path.
# TODO(Shard-113): pin PD/TiDB versions and fill the real join addresses.

# 1) PD (on .254)
# "$PD_BIN" --name=pd1 --data-dir="$ROOT/pd" \
#   --client-urls=http://0.0.0.0:2379 --peer-urls=http://0.0.0.0:2380 \
#   --log-file="$ROOT/log/pd.log" &

# 2) TiKV + UUSE (on .253)
# "$TIKV_BIN" --config "$ROOT/tikv.toml" \
#   --log-file "$ROOT/log/tikv.log" \
#   --status-addr 127.0.0.1:20180 \
#   --pd-endpoints 192.168.1.254:2379 &

# 3) TiDB server (on .254)
# "$TIDB_BIN" --store=tikv --path=192.168.1.254:2379 \
#   --log-file="$ROOT/log/tidb.log" --status=10080 --port=4000 &

echo "skeleton: UUSE conf $UUSE_CONF; fill TODOs before running"
