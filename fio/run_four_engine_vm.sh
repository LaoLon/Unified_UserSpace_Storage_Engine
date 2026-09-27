#!/usr/bin/env bash
# Four-engine 4K randrw comparison on one verified NVMe at 64 total
# outstanding I/Os: ext4+libaio, ext4+io_uring, SPDK raw bdev and HFS.
# Shape A keeps the previous matrix convention: 1 job x QD64 (HFS 2 x 32).
# Shape B uses the literal 64 threads x QD1. Three 10 s reps per cell.
set -uo pipefail

FIO="${FIO:-/home/hfs/fio/fio}"
CONFIG="${CONFIG:-/root/code/config/hfs_nvme.conf}"
HFSCTL="${HFSCTL:-/root/code/build-verify/hfsctl}"
HFS_PLUGIN="${HFS_PLUGIN:-/root/code/build-verify/fio-hfs.so}"
LIB_DIR="${LIB_DIR:-/root/code/build-verify}"
RAW_PLUGIN="${RAW_PLUGIN:-/root/code/spdk/build/fio/spdk_bdev}"
RESULT="${RESULT:-/home/hfs/fio-four-engine-20260928}"
MOUNT="${MOUNT:-/mnt/fio-four-engine}"
EXPECTED_SERIAL="${EXPECTED_SERIAL:-VMware NVME_0002}"
RUNS="${RUNS:-3}"
RUNTIME="${RUNTIME:-10}"
BS="${BS:-4k}"
SIZE="${SIZE:-256m}"
BDF="$(awk -F= '/^[[:space:]]*pci_bdf[[:space:]]*=/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$CONFIG")"
DEVICE=""
STAGE=preflight
FAILED=0

test -n "$BDF" && test -d "/sys/bus/pci/devices/$BDF"
test -x "$FIO" && test -x "$HFSCTL"
test -f "$HFS_PLUGIN" && test -f "$RAW_PLUGIN"
test "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)" -ge 1024
test "$(nproc)" -ge 6
test "$SIZE" == 256m
test ! -e "$RESULT"

# Bind only the controller named in the checked-in single-device configuration.
bind_driver() {
  local wanted="$1" current override="/sys/bus/pci/devices/$BDF/driver_override"
  current="$(basename "$(readlink -f "/sys/bus/pci/devices/$BDF/driver")")"
  if [[ "$current" == "$wanted" ]]; then return; fi
  printf '%s\n' "$wanted" > "$override"
  if [[ -e "/sys/bus/pci/devices/$BDF/driver" ]]; then
    printf '%s\n' "$BDF" > "/sys/bus/pci/drivers/$current/unbind"
  fi
  printf '%s\n' "$BDF" > "/sys/bus/pci/drivers/$wanted/bind"
  printf '\n' > "$override"
  udevadm settle
  test "$(basename "$(readlink -f "/sys/bus/pci/devices/$BDF/driver")")" == "$wanted"
}

# Select the namespace by PCI topology and serial, never by nvmeX number.
find_target_device() {
  local block controller_path serial
  for block in /sys/block/nvme*n*; do
    [[ -e "$block" ]] || continue
    controller_path="$(readlink -f "$block/device")"
    [[ "$controller_path" == *"/$BDF/"* ]] || continue
    serial="$(cat "$controller_path/serial")"
    test "$serial" == "$EXPECTED_SERIAL"
    DEVICE="/dev/$(basename "$block")"
    test -b "$DEVICE"
    test "$(blockdev --getsize64 "$DEVICE")" -ge 8589934592
    if lsblk -nr -o MOUNTPOINTS "$DEVICE" | grep -q '[^[:space:]]'; then
      printf 'target NVMe has mounted filesystems: %s\n' "$DEVICE" >&2
      return 1
    fi
    return 0
  done
  printf 'no namespace found under PCI %s\n' "$BDF" >&2
  return 1
}

cleanup() {
  local status=$?
  trap - EXIT
  mountpoint -q "$MOUNT" && umount "$MOUNT" || true
  if [[ -d "/sys/bus/pci/devices/$BDF" ]]; then bind_driver uio_pci_generic || status=1; fi
  printf 'four-engine cleanup: stage=%s exit=%s\n' "$STAGE" "$status"
  exit "$status"
}
trap cleanup EXIT

modprobe nvme
modprobe uio_pci_generic
bind_driver nvme
find_target_device
printf 'target: BDF=%s serial=%s device=%s\n' "$BDF" "$EXPECTED_SERIAL" "$DEVICE"
if [[ "${PREFLIGHT_ONLY:-0}" == 1 ]]; then exit 0; fi

mkdir -p "$RESULT" "$MOUNT"
{
  date -Is
  printf 'bdf=%s serial=%s device=%s runs=%s seconds=%s size=%s bs=%s\n' \
    "$BDF" "$EXPECTED_SERIAL" "$DEVICE" "$RUNS" "$RUNTIME" "$SIZE" "$BS"
  printf 'cpus=%s hugepages=%s\n' "$(nproc)" \
    "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)"
  "$FIO" --version
  git -C /root/code/spdk rev-parse HEAD
  sha256sum "$CONFIG" "$HFS_PLUGIN" "$RAW_PLUGIN"
  df -h /home/hfs
} > "$RESULT/environment.txt"
printf 'label\tengine\tshape\trep\trc\n' > "$RESULT/cells.tsv"

# Fail a cell unless fio reports zero errors and both read and write I/O.
check_result() {
  python3 - "$1" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as handle:
    result = json.load(handle)
jobs = result["jobs"]
assert jobs and all(job["error"] == 0 for job in jobs), "fio job error"
assert sum(job["read"]["iops"] for job in jobs) > 0, "missing reads"
assert sum(job["write"]["iops"] for job in jobs) > 0, "missing writes"
PY
}

# Run one cell; never abort the whole matrix on a single engine failure.
# Arguments: label engine shape rep prefix fio-args...
run_cell() {
  local label="$1" engine="$2" shape="$3" rep="$4" prefix="$5"; shift 5
  local rc=0
  taskset -c 0-5 "$@" > "$prefix.json" 2> "$prefix.stderr" || rc=$?
  if [[ "$rc" == 0 ]]; then
    check_result "$prefix.json" || rc=$?
  fi
  if [[ "$rc" == 0 && "$engine" == hfs ]]; then
    grep -q 'fio-hfs closure:' "$prefix.stderr" || rc=$?
  fi
  printf '%s\t%s\t%s\t%s\t%s\n' "$label" "$engine" "$shape" "$rep" "$rc" >> "$RESULT/cells.tsv"
  if [[ "$rc" != 0 ]]; then
    FAILED=1
    printf 'CELL FAILED: %s rc=%s\n' "$label" "$rc"
  else
    printf '%s complete\n' "$label"
  fi
}

# ext4 file prefill so reads never hit holes.
prefill_ext4() {
  dd if=/dev/zero of="$MOUNT/fio-four-engine" bs=1M count=256 \
    oflag=direct conv=fsync status=none
}

# --- ext4: libaio + io_uring ---
STAGE=ext4
mkfs.ext4 -F "$DEVICE" > "$RESULT/ext4-format.log" 2>&1 || exit 1
mount -t ext4 -o noatime "$DEVICE" "$MOUNT" || exit 1
prefill_ext4
for rep in $(seq 1 "$RUNS"); do
  run_cell "libaio-a-r$rep" libaio A "$rep" "$RESULT/libaio-a-r$rep" \
    "$FIO" --name=libaio-a --ioengine=libaio --thread=1 \
    --filename="$MOUNT/fio-four-engine" --bs="$BS" --size="$SIZE" \
    --rw=randrw --rwmixread=50 --direct=1 --iodepth=64 --numjobs=1 \
    --group_reporting=1 --time_based=1 --runtime="$RUNTIME" --output-format=json
  run_cell "uring-a-r$rep" uring A "$rep" "$RESULT/uring-a-r$rep" \
    "$FIO" --name=uring-a --ioengine=io_uring --thread=1 \
    --filename="$MOUNT/fio-four-engine" --bs="$BS" --size="$SIZE" \
    --rw=randrw --rwmixread=50 --direct=1 --iodepth=64 --numjobs=1 \
    --group_reporting=1 --time_based=1 --runtime="$RUNTIME" --output-format=json
done
for rep in $(seq 1 "$RUNS"); do
  run_cell "libaio-b-r$rep" libaio B "$rep" "$RESULT/libaio-b-r$rep" \
    "$FIO" --name=libaio-b --ioengine=libaio --thread=1 \
    --filename="$MOUNT/fio-four-engine" --bs="$BS" --size="$SIZE" \
    --rw=randrw --rwmixread=50 --direct=1 --iodepth=1 --numjobs=64 \
    --group_reporting=1 --time_based=1 --runtime="$RUNTIME" --output-format=json
  run_cell "uring-b-r$rep" uring B "$rep" "$RESULT/uring-b-r$rep" \
    "$FIO" --name=uring-b --ioengine=io_uring --thread=1 \
    --filename="$MOUNT/fio-four-engine" --bs="$BS" --size="$SIZE" \
    --rw=randrw --rwmixread=50 --direct=1 --iodepth=1 --numjobs=64 \
    --group_reporting=1 --time_based=1 --runtime="$RUNTIME" --output-format=json
done
umount "$MOUNT"

# --- SPDK raw bdev ---
STAGE=spdk-raw
bind_driver uio_pci_generic
/root/code/spdk/scripts/gen_nvme.sh --json-with-subsystems > "$RESULT/spdk-nvme.json"
grep -q "$BDF" "$RESULT/spdk-nvme.json" || exit 1
for rep in $(seq 1 "$RUNS"); do
  prefix="$RESULT/raw-a-r$rep"
  taskset -c 0-5 env LD_PRELOAD="$RAW_PLUGIN" "$FIO" \
    --name=raw-prep --ioengine=spdk_bdev --thread=1 \
    --spdk_conf="$RESULT/spdk-nvme.json" --filename=Nvme0n1 \
    --rw=write --bs=1m --size="$SIZE" --direct=1 --iodepth=32 --numjobs=1 \
    --output-format=json > "$prefix.prep.json" 2> "$prefix.prep.stderr"
  run_cell "raw-a-r$rep" raw A "$rep" "$prefix" \
    env LD_PRELOAD="$RAW_PLUGIN" "$FIO" --name=raw-a --ioengine=spdk_bdev --thread=1 \
    --spdk_conf="$RESULT/spdk-nvme.json" --filename=Nvme0n1 \
    --bs="$BS" --size="$SIZE" --rw=randrw --rwmixread=50 --direct=1 \
    --iodepth=64 --numjobs=1 --group_reporting=1 --time_based=1 \
    --runtime="$RUNTIME" --output-format=json
  prefix="$RESULT/raw-b-r$rep"
  taskset -c 0-5 env LD_PRELOAD="$RAW_PLUGIN" "$FIO" \
    --name=raw-prep --ioengine=spdk_bdev --thread=1 \
    --spdk_conf="$RESULT/spdk-nvme.json" --filename=Nvme0n1 \
    --rw=write --bs=1m --size="$SIZE" --direct=1 --iodepth=32 --numjobs=1 \
    --output-format=json > "$prefix.prep.json" 2> "$prefix.prep.stderr"
  run_cell "raw-b-r$rep" raw B "$rep" "$prefix" \
    env LD_PRELOAD="$RAW_PLUGIN" "$FIO" --name=raw-b --ioengine=spdk_bdev --thread=1 \
    --spdk_conf="$RESULT/spdk-nvme.json" --filename=Nvme0n1 \
    --bs="$BS" --size="$SIZE" --rw=randrw --rwmixread=50 --direct=1 \
    --iodepth=1 --numjobs=64 --group_reporting=1 --time_based=1 \
    --runtime="$RUNTIME" --output-format=json
done

# --- HFS external engine ---
STAGE=hfs
for rep in $(seq 1 "$RUNS"); do
  "$HFSCTL" --config="$CONFIG" format --yes > "$RESULT/hfs-r$rep-format.log" 2>&1
  prefix="$RESULT/hfs-a-r$rep"
  LD_LIBRARY_PATH="$LIB_DIR" taskset -c 0-5 "$FIO" \
    --name=hfs-prep --ioengine="external:$HFS_PLUGIN" --thread=1 \
    --hfs_config="$CONFIG" --hfs_async=1 --hfs_client_depth=32 \
    --filename=/fio-four-engine --rw=write --bs=64k --size="$SIZE" \
    --direct=1 --iodepth=4 --numjobs=1 --output-format=json \
    > "$prefix.prep.json" 2> "$prefix.prep.stderr"
  run_cell "hfs-a-r$rep" hfs A "$rep" "$prefix" \
    env LD_LIBRARY_PATH="$LIB_DIR" "$FIO" --name=hfs-a \
    --ioengine="external:$HFS_PLUGIN" --thread=1 \
    --hfs_config="$CONFIG" --hfs_async=1 --hfs_client_depth=32 \
    --filename=/fio-four-engine --bs="$BS" --size="$SIZE" \
    --rw=randrw --rwmixread=50 --direct=1 --iodepth=32 --numjobs=2 \
    --group_reporting=1 --time_based=1 --runtime="$RUNTIME" --output-format=json
  prefix="$RESULT/hfs-b-r$rep"
  run_cell "hfs-b-r$rep" hfs B "$rep" "$prefix" \
    env LD_LIBRARY_PATH="$LIB_DIR" "$FIO" --name=hfs-b \
    --ioengine="external:$HFS_PLUGIN" --thread=1 \
    --hfs_config="$CONFIG" --hfs_async=1 --hfs_client_depth=32 \
    --filename=/fio-four-engine --bs="$BS" --size="$SIZE" \
    --rw=randrw --rwmixread=50 --direct=1 --iodepth=1 --numjobs=64 \
    --group_reporting=1 --time_based=1 --runtime="$RUNTIME" --output-format=json
done
"$HFSCTL" --config="$CONFIG" check > "$RESULT/hfs-check.log" 2>&1
grep -q 'corruption=none' "$RESULT/hfs-check.log" || FAILED=1

STAGE=complete
exit "$FAILED"
