#!/usr/bin/env bash
# Run the five-pair 8j4 UUSE/raw SPDK FIO gate on the selected test NVMe.
set -euo pipefail

FIO="${FIO:-/home/uuse/fio/fio}"
RAW_PLUGIN="${RAW_PLUGIN:-/root/code/spdk/build/fio/spdk_bdev}"
UUSE_PLUGIN="${UUSE_PLUGIN:-/root/code/build-verify/fio-uuse.so}"
UUSECTL="${UUSECTL:-/root/code/build-verify/uusectl}"
UUSE_LIB_DIR="${UUSE_LIB_DIR:-/root/code/build-verify}"
CONFIG="${CONFIG:-/root/code/config/uuse_nvme.conf}"
RESULT="${RESULT:-/home/uuse/mysql-shard105-gate}"
SPDK_CONF="$RESULT/spdk-nvme.json"
mkdir -p "$RESULT"

# A raw run overwrites the first 256 MiB, so check the bound test controller.
BDF="$(awk -F= '/^[[:space:]]*pci_bdf[[:space:]]*=/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$CONFIG")"
test -n "$BDF"
test "$(basename "$(readlink -f "/sys/bus/pci/devices/$BDF/driver")")" == uio_pci_generic
test "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)" -ge 1024
test -x "$FIO" && test -f "$RAW_PLUGIN" && test -f "$UUSE_PLUGIN"
/root/code/spdk/scripts/gen_nvme.sh --json-with-subsystems > "$SPDK_CONF"
grep -q "$BDF" "$SPDK_CONF"

for run in 1 2 3 4 5; do
  taskset -c 0-5 env LD_PRELOAD="$RAW_PLUGIN" "$FIO" \
    --name=raw --ioengine=spdk_bdev --thread=1 \
    --spdk_conf="$SPDK_CONF" --filename=Nvme0n1 \
    --bs=4k --size=256m --rw=randrw --rwmixread=50 --direct=1 \
    --iodepth=32 --numjobs=1 --group_reporting=1 \
    --time_based=1 --runtime=10 --output-format=json \
    > "$RESULT/raw-${run}.json" 2> "$RESULT/raw-${run}.stderr"
  "$UUSECTL" --config="$CONFIG" format --yes \
    > "$RESULT/format-${run}.log" 2>&1
  LD_LIBRARY_PATH="$UUSE_LIB_DIR" taskset -c 0-5 "$FIO" \
    --name=uuse --ioengine="external:$UUSE_PLUGIN" --thread=1 \
    --uuse_config="$CONFIG" --uuse_async=1 --uuse_client_depth=32 \
    --filename=/fio-gate --numjobs=8 --iodepth=4 \
    --bs=4k --size=256m --rw=randrw --rwmixread=50 --direct=1 \
    --group_reporting=1 --time_based=1 --runtime=10 --output-format=json \
    > "$RESULT/uuse-${run}.json" 2> "$RESULT/uuse-${run}.stderr"
  grep -q 'fio-uuse closure:' "$RESULT/uuse-${run}.stderr"
  printf 'gate pair %s complete\n' "$run"
done
