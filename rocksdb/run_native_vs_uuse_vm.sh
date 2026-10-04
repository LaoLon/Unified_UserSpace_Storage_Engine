#!/usr/bin/env bash
# Compare native ext4 RocksDB against UUSE RocksDB on one verified NVMe at
# threads=32 and threads=64. Covers sequential/random reads, sequential/random
# writes, an update-heavy overwrite with value verification, and a
# read-while-writing mix. Each mode/thread pair uses a fresh device state.
set -euo pipefail

CONFIG="${CONFIG:-/root/code/config/uuse_nvme.conf}"
BENCH="${BENCH:-/root/code/build-verify/uuse_db_bench}"
UUSECTL="${UUSECTL:-/root/code/build-verify/uusectl}"
LIB_DIR="${LIB_DIR:-/root/code/build-verify}"
RESULT="${RESULT:-/home/uuse/rocksdb-native-vs-uuse-20260927}"
MOUNT="${MOUNT:-/mnt/uuse-rocksdb-native-vs-uuse}"
EXPECTED_SERIAL="${EXPECTED_SERIAL:-VMware NVME_0002}"
THREADS_LIST="${THREADS_LIST:-32 64}"
TOTAL_KEYS="${TOTAL_KEYS:-1280000}"
CACHE_BYTES="${CACHE_BYTES:-1073741824}"
BDF="$(awk -F= '/^[[:space:]]*pci_bdf[[:space:]]*=/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$CONFIG")"
DEVICE=""
STAGE=preflight

test -n "$BDF" && test -d "/sys/bus/pci/devices/$BDF"
test -x "$BENCH" && test -x "$UUSECTL"
test "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)" -ge 1024
test "$(nproc)" -ge 6
test "$(df -PB1 /home/uuse | awk 'NR==2 {print $4}')" -ge 4294967296
! pgrep -x uuse_db_bench > /dev/null
! pgrep -x mysqld > /dev/null
test ! -e "$RESULT"
test -z "${THREADS_LIST//[0-9 ]/}"

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
  printf 'rocksdb native-vs-uuse cleanup: stage=%s exit=%s\n' "$STAGE" "$status"
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
  printf 'threads=%s total_keys=%s cache_bytes=%s key_size=16 value_size=1024\n' \
    "$THREADS_LIST" "$TOTAL_KEYS" "$CACHE_BYTES"
  printf 'compression=none direct_reads=1 direct_flush_compaction=1 block_size=4096\n'
  printf 'wal=enabled sync_per_put=0 worker_cpus=0,1,2,3,4,5 io_stats_uuse=1\n'
  printf 'cpus=%s hugepages=%s\n' "$(nproc)" "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)"
  free -h
  df -h /home/uuse
  git -C /root/code/spdk rev-parse HEAD
  sha256sum "$CONFIG" "$BENCH" "$UUSECTL"
  ldd "$BENCH" | grep rocksdb || true
} > "$RESULT/environment.txt"

# Fail fast on stderr errors from the RocksDB run.
check_stderr() {
  ! grep -Eiq '(^|[[:space:]])(error|failed|corruption)(:|[[:space:]])' "$1"
}

# Run one benchmark phase in one mode and keep stdout/stderr for parsing.
# Arguments: mode threads phase db benchmarks
run_phase() {
  local mode="$1" threads="$2" phase="$3" db="$4" benchmarks="$5"
  local num=$((TOTAL_KEYS / threads))
  local args=(--env="$mode" --db="$db" --benchmarks="$benchmarks"
    --threads="$threads" --num="$num" --key_size=16 --value_size=1024
    --worker_cpus=0,1,2,3,4,5 --cache_size="$CACHE_BYTES"
    --block_size=4096 --compression=none --use_direct_reads=1
    --use_direct_io_for_flush_and_compaction=1)
  local prefix="$RESULT/${mode}-s${threads}-${phase}"
  if [[ "$mode" == uuse ]]; then
    args+=(--uuse-config="$CONFIG")
    UUSE_ROCKSDB_IO_STATS=1 LD_LIBRARY_PATH="$LIB_DIR" taskset -c 0-5 \
      "$BENCH" "${args[@]}" > "$prefix.stdout" 2> "$prefix.stderr"
  else
    LD_LIBRARY_PATH="$LIB_DIR" taskset -c 0-5 \
      "$BENCH" "${args[@]}" > "$prefix.stdout" 2> "$prefix.stderr"
  fi
  for name in ${benchmarks//,/ }; do
    if [[ "$name" == "readseq" || "$name" == "readrandom" ]]; then
      grep -q "^${name} r1" "$prefix.stdout"
    else
      grep -q "^${name}[[:space:]:]" "$prefix.stdout"
    fi
  done
  check_stderr "$prefix.stderr"
  if [[ "$mode" == uuse ]]; then
    grep -q 'uuse-rocksdb io stats:' "$prefix.stderr"
  fi
  printf '%s s%s %s complete: %s\n' "$mode" "$threads" "$phase" "$benchmarks"
}

# One native ext4 phase (fresh mkfs + mount) with a single live DB.
# Arguments: threads phase db benchmarks
run_native_phase() {
  local threads="$1" phase="$2" db="$3" benchmarks="$4"
  STAGE="native-s${threads}-${phase}"
  bind_driver nvme
  find_target_device
  mkfs.ext4 -F "$DEVICE" > "$RESULT/native-s${threads}-${phase}-ext4-format.log" 2>&1
  mount -t ext4 -o noatime "$DEVICE" "$MOUNT"
  run_phase native "$threads" "$phase" "$db" "$benchmarks"
  sync
  du -sh "$db" > "$RESULT/native-s${threads}-${phase}-size.txt"
  umount "$MOUNT"
}

# One UUSE phase on a freshly formatted namespace with a single live DB.
# Arguments: threads phase db benchmarks
run_uuse_phase() {
  local threads="$1" phase="$2" db="$3" benchmarks="$4"
  STAGE="uuse-s${threads}-${phase}"
  bind_driver uio_pci_generic
  "$UUSECTL" --config="$CONFIG" format --yes > "$RESULT/uuse-s${threads}-${phase}-format.log" 2>&1
  run_phase uuse "$threads" "$phase" "$db" "$benchmarks"
  "$UUSECTL" --config="$CONFIG" check > "$RESULT/uuse-s${threads}-${phase}-check.log" 2>&1
  grep -q 'corruption=none' "$RESULT/uuse-s${threads}-${phase}-check.log"
}

# Interleave native and UUSE per thread count so device drift is visible.
# Two phases per pair: a fresh device state per phase keeps only one 1280000-key
# database (plus its compaction debt) live at a time; two concurrent DBs
# exhausted the allocatable extent pool on 2026-09-27.
for threads in $THREADS_LIST; do
  run_native_phase "$threads" seq "$MOUNT/db-s${threads}-seq" fillseq,readseq,readrandom
  run_native_phase "$threads" rnd "$MOUNT/db-s${threads}-rnd" fillrandom,overwrite,readwhilewriting
  run_uuse_phase "$threads" seq "/rocksdb-s${threads}-seq" fillseq,readseq,readrandom
  run_uuse_phase "$threads" rnd "/rocksdb-s${threads}-rnd" fillrandom,overwrite,readwhilewriting
done
STAGE=complete
