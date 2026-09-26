#!/usr/bin/env bash
# Benchmark ext4/libaio, raw SPDK, and HFS on one verified NVMe controller.
set -euo pipefail

FIO="${FIO:-/home/hfs/fio/fio}"
CONFIG="${CONFIG:-/root/code/config/hfs_nvme.conf}"
HFSCTL="${HFSCTL:-/root/code/build-verify/hfsctl}"
HFS_PLUGIN="${HFS_PLUGIN:-/root/code/build-verify/fio-hfs.so}"
HFS_LIB_DIR="${HFS_LIB_DIR:-/root/code/build-verify}"
RAW_PLUGIN="${RAW_PLUGIN:-/root/code/spdk/build/fio/spdk_bdev}"
RESULT="${RESULT:-/home/hfs/fio-three-way-20260925}"
MOUNT="${MOUNT:-/mnt/hfs-fio-ext4}"
EXPECTED_SERIAL="${EXPECTED_SERIAL:-VMware NVME_0002}"
RUNS="${RUNS:-3}"
RUNTIME_SECONDS="${RUNTIME_SECONDS:-10}"
SIZE="${SIZE:-256m}"
BDF="$(awk -F= '/^[[:space:]]*pci_bdf[[:space:]]*=/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$CONFIG")"
DEVICE=""
STAGE="preflight"

test -n "$BDF" && test -d "/sys/bus/pci/devices/$BDF"
test -x "$FIO" && test -x "$HFSCTL"
test -f "$HFS_PLUGIN" && test -f "$RAW_PLUGIN"
test "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)" -ge 1024
test "$(nproc)" -ge 6
test "$RUNS" -ge 1 && test "$RUNTIME_SECONDS" -ge 1
test "$SIZE" == 256m

# Bind only the BDF named by the checked-in single-device configuration.
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
    [[ "$serial" == "$EXPECTED_SERIAL" ]] || {
      printf 'unexpected NVMe serial for %s: %s\n' "$BDF" "$serial" >&2
      return 1
    }
    DEVICE="/dev/$(basename "$block")"
    test -b "$DEVICE"
    test "$(blockdev --getsize64 "$DEVICE")" -ge $((2 * 1024 * 1024 * 1024))
    if lsblk -nr -o MOUNTPOINTS "$DEVICE" | grep -q '[^[:space:]]'; then
      printf 'target NVMe has mounted filesystems: %s\n' "$DEVICE" >&2
      return 1
    fi
    return 0
  done
  printf 'no namespace found under PCI %s\n' "$BDF" >&2
  return 1
}

# Restore SPDK ownership of the target on both success and failure.
cleanup() {
  local status=$?
  trap - EXIT
  if mountpoint -q "$MOUNT"; then umount "$MOUNT" || status=1; fi
  if [[ -d "/sys/bus/pci/devices/$BDF" ]]; then
    bind_driver uio_pci_generic || status=1
  fi
  printf 'matrix cleanup: stage=%s exit=%s\n' "$STAGE" "$status"
  exit "$status"
}
trap cleanup EXIT

modprobe nvme
modprobe uio_pci_generic
bind_driver nvme
find_target_device
printf 'target: BDF=%s serial=%s device=%s\n' "$BDF" "$EXPECTED_SERIAL" "$DEVICE"
if [[ "${PREFLIGHT_ONLY:-0}" == 1 ]]; then exit 0; fi
test "${ALLOW_FORMAT:-0}" == 1 || {
  printf 'set ALLOW_FORMAT=1 to overwrite the verified test NVMe\n' >&2
  exit 2
}
if [[ "${RESUME_SPDK_HFS:-0}" == 1 || "${RECHECK_EXT4:-0}" == 1 ]]; then
  test -d "$RESULT"
  for bs in 4k 16k 64k; do
    for qd in 32 64; do
      for rep in $(seq 1 "$RUNS"); do
        test -s "$RESULT/ext4-${bs}-q${qd}-r${rep}.json"
      done
    done
  done
else
  test ! -e "$RESULT" || {
    printf 'result directory already exists: %s\n' "$RESULT" >&2
    exit 2
  }
fi
mkdir -p "$RESULT" "$MOUNT"
{
  date -Is
  printf 'bdf=%s serial=%s device=%s runs=%s seconds=%s size=%s\n' \
    "$BDF" "$EXPECTED_SERIAL" "$DEVICE" "$RUNS" "$RUNTIME_SECONDS" "$SIZE"
  printf 'cpus=%s hugepages=%s\n' "$(nproc)" \
    "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)"
  "$FIO" --version
  git -C /root/code/spdk rev-parse HEAD
  sha256sum "$CONFIG" "$HFS_PLUGIN" "$RAW_PLUGIN"
  df -h /home/hfs
} > "$RESULT/environment.txt"

# Fail immediately if fio reports an engine error or no mixed I/O.
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

# Fill the measured file before timed randrw so ext4 cannot read holes.
prefill_ext4() {
  dd if=/dev/zero of="$MOUNT/fio-matrix" bs=1M count=256 \
    oflag=direct conv=fsync status=none
}

if [[ "${RECHECK_EXT4:-0}" == 1 ]]; then
  STAGE="ext4-recheck"
  mkfs.ext4 -F "$DEVICE" > "$RESULT/ext4-recheck-format.log" 2>&1
  mount -t ext4 -o noatime "$DEVICE" "$MOUNT"
  prefill_ext4
  for bs in 4k 16k 64k; do
    for qd in 32 64; do
      for rep in $(seq 1 "$RUNS"); do
        output="$RESULT/ext4-recheck-${bs}-q${qd}-r${rep}"
        taskset -c 0-5 "$FIO" --name=ext4 --ioengine=libaio --thread=1 \
          --filename="$MOUNT/fio-matrix" --bs="$bs" --size="$SIZE" \
          --rw=randrw --rwmixread=50 --direct=1 --iodepth="$qd" \
          --numjobs=1 --group_reporting=1 --time_based=1 --runtime="$RUNTIME_SECONDS" \
          --output-format=json > "$output.json" 2> "$output.stderr"
        check_result "$output.json"
        printf 'ext4 recheck %s q%s rep%s complete\n' "$bs" "$qd" "$rep"
      done
    done
  done
  umount "$MOUNT"
  STAGE="complete"
  exit 0
fi

if [[ "${RESUME_SPDK_HFS:-0}" != 1 ]]; then
  STAGE="ext4"
  mkfs.ext4 -F "$DEVICE" > "$RESULT/ext4-format.log" 2>&1
  mount -t ext4 -o noatime "$DEVICE" "$MOUNT"
  prefill_ext4
  for bs in 4k 16k 64k; do
    for qd in 32 64; do
      for rep in $(seq 1 "$RUNS"); do
        output="$RESULT/ext4-${bs}-q${qd}-r${rep}"
        taskset -c 0-5 "$FIO" --name=ext4 --ioengine=libaio --thread=1 \
          --filename="$MOUNT/fio-matrix" --bs="$bs" --size="$SIZE" \
          --rw=randrw --rwmixread=50 --direct=1 --iodepth="$qd" \
          --numjobs=1 --group_reporting=1 --time_based=1 --runtime="$RUNTIME_SECONDS" \
          --output-format=json > "$output.json" 2> "$output.stderr"
        check_result "$output.json"
        printf 'ext4 %s q%s rep%s complete\n' "$bs" "$qd" "$rep"
      done
    done
  done
  umount "$MOUNT"
fi
bind_driver uio_pci_generic
/root/code/spdk/scripts/gen_nvme.sh --json-with-subsystems > "$RESULT/spdk-nvme.json"
grep -q "$BDF" "$RESULT/spdk-nvme.json"

# Pair raw and HFS measurements at the same total outstanding request count.
# HFS uses one 32-deep queue and adds a second queue only above QD32.
STAGE="spdk-hfs"
for bs in 4k 16k 64k; do
  for qd in 32 64; do
    if (( qd <= 32 )); then
      num_jobs=1
      per_job=$qd
    else
      num_jobs=$((qd / 32))
      per_job=32
    fi
    for rep in $(seq 1 "$RUNS"); do
      prefix="$RESULT/raw-${bs}-q${qd}-r${rep}"
      taskset -c 0-5 env LD_PRELOAD="$RAW_PLUGIN" "$FIO" \
        --name=raw-prep --ioengine=spdk_bdev --thread=1 \
        --spdk_conf="$RESULT/spdk-nvme.json" --filename=Nvme0n1 \
        --rw=write --bs=1m --size="$SIZE" --direct=1 --iodepth=32 \
        --numjobs=1 --output-format=json \
        > "$prefix.prep.json" 2> "$prefix.prep.stderr"
      check_result_prep="$prefix.prep.json"
      python3 - "$check_result_prep" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as handle:
    assert all(job["error"] == 0 for job in json.load(handle)["jobs"])
PY
      taskset -c 0-5 env LD_PRELOAD="$RAW_PLUGIN" "$FIO" \
        --name=raw --ioengine=spdk_bdev --thread=1 \
        --spdk_conf="$RESULT/spdk-nvme.json" --filename=Nvme0n1 \
        --bs="$bs" --size="$SIZE" --rw=randrw --rwmixread=50 \
        --direct=1 --iodepth="$qd" --numjobs=1 --group_reporting=1 \
        --time_based=1 --runtime="$RUNTIME_SECONDS" --output-format=json \
        > "$prefix.json" 2> "$prefix.stderr"
      check_result "$prefix.json"

      prefix="$RESULT/hfs-${bs}-q${qd}-r${rep}"
      "$HFSCTL" --config="$CONFIG" format --yes \
        > "$prefix.format.log" 2>&1
      LD_LIBRARY_PATH="$HFS_LIB_DIR" taskset -c 0-5 "$FIO" \
        --name=hfs-prep --ioengine="external:$HFS_PLUGIN" --thread=1 \
        --hfs_config="$CONFIG" --hfs_async=1 --hfs_client_depth=32 \
        --filename=/fio-matrix --rw=write --bs=64k --size="$SIZE" \
        --direct=1 --iodepth=4 --numjobs=1 --output-format=json \
        > "$prefix.prep.json" 2> "$prefix.prep.stderr"
      python3 - "$prefix.prep.json" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as handle:
    assert all(job["error"] == 0 for job in json.load(handle)["jobs"])
PY
      LD_LIBRARY_PATH="$HFS_LIB_DIR" taskset -c 0-5 "$FIO" \
        --name=hfs --ioengine="external:$HFS_PLUGIN" --thread=1 \
        --hfs_config="$CONFIG" --hfs_async=1 --hfs_client_depth=32 \
        --filename=/fio-matrix --bs="$bs" --size="$SIZE" \
        --rw=randrw --rwmixread=50 --direct=1 --iodepth="$per_job" \
        --numjobs="$num_jobs" --group_reporting=1 --time_based=1 --runtime="$RUNTIME_SECONDS" \
        --output-format=json > "$prefix.json" 2> "$prefix.stderr"
      check_result "$prefix.json"
      grep -q 'fio-hfs closure:' "$prefix.stderr"
      printf 'raw+hfs %s q%s rep%s complete\n' "$bs" "$qd" "$rep"
    done
  done
done
STAGE="complete"
