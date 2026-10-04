#!/usr/bin/env bash
# Compare 32-worker RocksDB writes and random reads on one verified NVMe,
# then value-verify every key through the same read path. Includes UUSE
# adapter call-pattern counters (UUSE_ROCKSDB_IO_STATS=1) for cross-checks.
set -euo pipefail

CONFIG="${CONFIG:-/root/code/config/uuse_nvme.conf}"
BENCH="${BENCH:-/root/code/build-verify/uuse_db_bench}"
UUSECTL="${UUSECTL:-/root/code/build-verify/uusectl}"
RESULT="${RESULT:-/home/uuse/rocksdb-rw32-cache1g-20260925}"
MOUNT="${MOUNT:-/mnt/uuse-rocksdb-ext4}"
EXPECTED_SERIAL="${EXPECTED_SERIAL:-VMware NVME_0002}"
BDF="$(awk -F= '/^[[:space:]]*pci_bdf[[:space:]]*=/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$CONFIG")"
DEVICE=""
STAGE=preflight

test -n "$BDF" && test -d "/sys/bus/pci/devices/$BDF"
test -x "$BENCH" && test -x "$UUSECTL"
test "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)" -ge 1024
test "$(nproc)" -ge 6
test "$(df -PB1 /home/uuse | awk 'NR==2 {print $4}')" -ge 4294967296
! pgrep -x uuse_db_bench >/dev/null
! pgrep -x mysqld >/dev/null
test ! -e "$RESULT"

# Bind only the controller named in the UUSE configuration.
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

# Resolve the namespace from sysfs topology and serial, never its nvme index.
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
      printf 'target NVMe is mounted: %s\n' "$DEVICE" >&2
      return 1
    fi
    return 0
  done
  printf 'target NVMe not found under %s\n' "$BDF" >&2
  return 1
}

# Release the ext4 mount and return the device to SPDK on every exit.
cleanup() {
  local status=$?
  trap - EXIT
  if mountpoint -q "$MOUNT"; then umount "$MOUNT" || status=1; fi
  if [[ -d "/sys/bus/pci/devices/$BDF" ]]; then
    bind_driver uio_pci_generic || status=1
  fi
  printf 'rocksdb cleanup: stage=%s exit=%s\n' "$STAGE" "$status"
  exit "$status"
}
trap cleanup EXIT

modprobe nvme
modprobe uio_pci_generic
bind_driver nvme
find_target_device
printf 'target: BDF=%s serial=%s device=%s\n' "$BDF" "$EXPECTED_SERIAL" "$DEVICE"
if [[ "${PREFLIGHT_ONLY:-0}" == 1 ]]; then exit 0; fi
test "${ALLOW_FORMAT:-0}" == 1

mkdir -p "$RESULT" "$MOUNT"
{
  date -Is
  printf 'BDF=%s serial=%s device=%s\n' "$BDF" "$EXPECTED_SERIAL" "$DEVICE"
  printf 'threads=32 cache_bytes=1073741824 num_per_thread=40000 total_keys=1280000 key_size=16 value_size=1024\n'
  printf 'compression=none direct_reads=1 direct_flush_compaction=1 WAL=enabled sync_per_put=0\n'
  printf 'worker_cpus=0,1,2,3,4,5 block_size=4096 io_stats=1 overwrite_verify=1\n'
  printf 'cpus=%s hugepages=%s\n' "$(nproc)" "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)"
  grep -m1 'model name' /proc/cpuinfo
  free -h
  df -h /home/uuse
  git -C /root/code/spdk rev-parse HEAD
  sha256sum "$CONFIG" "$BENCH"
  ldd "$BENCH" | grep rocksdb || true
} > "$RESULT/environment.txt"

# Use identical RocksDB options and fresh databases in both modes.
run_bench() {
  local mode="$1" db="$2"
  local args=(--env="$mode" --db="$db" --benchmarks=fillseq,readrandom
    --threads=32 --num=40000 --key_size=16 --value_size=1024
    --worker_cpus=0,1,2,3,4,5 --cache_size=1073741824
    --block_size=4096 --compression=none --use_direct_reads=1
    --use_direct_io_for_flush_and_compaction=1)
  if [[ "$mode" == uuse ]]; then
    args+=(--uuse-config="$CONFIG")
    UUSE_ROCKSDB_IO_STATS=1 LD_LIBRARY_PATH=/root/code/build-verify taskset -c 0-5 "$BENCH" "${args[@]}" \
      > "$RESULT/$mode.stdout" 2> "$RESULT/$mode.stderr"
  else
    LD_LIBRARY_PATH=/root/code/build-verify taskset -c 0-5 "$BENCH" "${args[@]}" \
      > "$RESULT/$mode.stdout" 2> "$RESULT/$mode.stderr"
  fi
  grep -q '^fillseq ' "$RESULT/$mode.stdout"
  grep -q '^readrandom r1 ' "$RESULT/$mode.stdout"
  ! grep -Eiq '(^|[[:space:]])(error|failed|corruption)(:|[[:space:]])' "$RESULT/$mode.stderr"
  if [[ "$mode" == uuse ]]; then
    grep -q 'uuse-rocksdb io stats:' "$RESULT/$mode.stderr"
  fi
  printf '%s complete\n' "$mode"
}

# Reopens the same DB and Get-verifies every key through the read path.
verify_bench() {
  local mode="$1" db="$2"
  local args=(--env="$mode" --db="$db" --benchmarks=overwrite
    --threads=32 --num=40000 --key_size=16 --value_size=1024
    --worker_cpus=0,1,2,3,4,5 --cache_size=1073741824
    --block_size=4096 --compression=none --use_direct_reads=1
    --use_direct_io_for_flush_and_compaction=1)
  local status=0
  if [[ "$mode" == uuse ]]; then
    args+=(--uuse-config="$CONFIG")
    UUSE_ROCKSDB_IO_STATS=1 LD_LIBRARY_PATH=/root/code/build-verify taskset -c 0-5 "$BENCH" "${args[@]}" \
      > "$RESULT/$mode-verify.stdout" 2> "$RESULT/$mode-verify.stderr" || status=$?
  else
    LD_LIBRARY_PATH=/root/code/build-verify taskset -c 0-5 "$BENCH" "${args[@]}" \
      > "$RESULT/$mode-verify.stdout" 2> "$RESULT/$mode-verify.stderr" || status=$?
  fi
  test "$status" -eq 0
  grep -q '^overwrite ' "$RESULT/$mode-verify.stdout"
  ! grep -Eiq 'verification failed|Put failed|Get failed' "$RESULT/$mode-verify.stderr"
  if [[ "$mode" == uuse ]]; then
    grep -q 'uuse-rocksdb io stats:' "$RESULT/$mode-verify.stderr"
  fi
  printf '%s overwrite verification complete\n' "$mode"
}

STAGE=ext4
mkfs.ext4 -F "$DEVICE" > "$RESULT/ext4-format.log" 2>&1
mount -t ext4 -o noatime "$DEVICE" "$MOUNT"
run_bench native "$MOUNT/db"
sync
du -sh "$MOUNT/db" > "$RESULT/native-size.txt"
verify_bench native "$MOUNT/db"
umount "$MOUNT"

STAGE=uuse
bind_driver uio_pci_generic
"$UUSECTL" --config="$CONFIG" format --yes > "$RESULT/uuse-format.log" 2>&1
run_bench uuse /rocksdb-rw32-cache1g
verify_bench uuse /rocksdb-rw32-cache1g
"$UUSECTL" --config="$CONFIG" check > "$RESULT/uuse-check.log" 2>&1
STAGE=complete
