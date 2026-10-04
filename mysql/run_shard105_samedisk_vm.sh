#!/usr/bin/env bash
# Compare the same MySQL binary on one NVMe: UUSE volume vs native ext4 datadir.
# Each phase rebuilds its datadir on the verified target device; per-run logs
# capture sysbench read/read-write, mysqlslap, durability counters and CPU maps.
set -euo pipefail

BUILD="${BUILD:-/home/uuse/mysql-build}"
CONFIG="${CONFIG:-/root/code/config/uuse_mysql.conf}"
UUSECTL="${UUSECTL:-/root/code/build-verify/uusectl}"
RESULT="${RESULT:-/home/uuse/mysql-samedisk-20260926}"
MOUNT="${MOUNT:-/mnt/mysql-samedisk-ext4}"
UUSE_DATA="${UUSE_DATA:-/home/uuse/mysql-samedisk-uuse-datadir}"
EXPECTED_SERIAL="${EXPECTED_SERIAL:-VMware NVME_0002}"
BIN="$BUILD/runtime_output_directory"
MYSQLD="$BIN/mysqld"
MYSQL="$BIN/mysql"
MYSQLADMIN="$BIN/mysqladmin"
MYSQLSLAP="$BIN/mysqlslap"
SOCKET="$RESULT/mysql.sock"
SERVER_PID=""
DEVICE=""
BDF="$(awk -F= '/^[[:space:]]*pci_bdf[[:space:]]*=/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$CONFIG")"
STAGE=preflight

test -n "$BDF" && test -d "/sys/bus/pci/devices/$BDF"
for executable in "$MYSQLD" "$MYSQL" "$MYSQLADMIN" "$MYSQLSLAP" "$UUSECTL"; do
  test -x "$executable" || { printf 'missing: %s\n' "$executable" >&2; exit 2; }
done
command -v sysbench > /dev/null
test "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)" -ge 1024
test "$(nproc)" -ge 6
test "$(df -PB1 /home/uuse | awk 'NR==2 {print $4}')" -ge 4294967296
! pgrep -x mysqld > /dev/null
! pgrep -x uuse_db_bench > /dev/null
mkdir -p "$RESULT" "$MOUNT"
if [[ "${PREFLIGHT_ONLY:-0}" == 1 ]]; then
  printf 'preflight ok: BDF=%s\n' "$BDF"
  exit 0
fi

export LD_LIBRARY_PATH="/root/code/build-verify:${LD_LIBRARY_PATH:-}"

# Bind only the controller named in the UUSE MySQL configuration.
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

# Resolve the target namespace by PCI topology and serial, never its nvme index.
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
  printf 'target NVMe not found under %s\n' "$BDF" >&2
  return 1
}

sql() {
  "$MYSQL" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
    --batch --skip-column-names "$@"
}

stop_server() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    "$MYSQLADMIN" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
      shutdown > "$RESULT/shutdown.log" 2>&1 || true
    wait "$SERVER_PID" || true
  fi
  SERVER_PID=""
}

# Release the server, the ext4 mount and the SPDK binding on every exit.
cleanup() {
  local status=$?
  trap - EXIT
  stop_server || status=1
  if mountpoint -q "$MOUNT"; then umount "$MOUNT" || status=1; fi
  if [[ -d "/sys/bus/pci/devices/$BDF" ]]; then
    bind_driver uio_pci_generic || status=1
  fi
  printf 'mysql same-disk cleanup: stage=%s exit=%s\n' "$STAGE" "$status"
  exit "$status"
}
trap cleanup EXIT

start_server() {
  local mode="$1" run="$2" data="$3"
  if [[ "$mode" == uuse ]]; then
    data="$UUSE_DATA"
    UUSE_MYSQL_CONFIG="$CONFIG" UUSE_MYSQL_PREFIX="$UUSE_DATA" \
      "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" --datadir="$data" \
      --socket="$SOCKET" --pid-file="$RESULT/mysqld.pid" \
      --log-error="$RESULT/${run}-mysqld.log" --skip-networking \
      --server-id=105 --innodb-flush-log-at-trx-commit=1 \
      --innodb-use-native-aio=OFF --disabled-storage-engines=MyISAM \
      > "$RESULT/${run}-mysqld.stdout" 2>&1 &
  else
    env -u UUSE_MYSQL_CONFIG -u UUSE_MYSQL_PREFIX \
      "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" --datadir="$data" \
      --socket="$SOCKET" --pid-file="$RESULT/mysqld.pid" \
      --log-error="$RESULT/${run}-mysqld.log" --skip-networking \
      --server-id=105 --innodb-flush-log-at-trx-commit=1 \
      --innodb-use-native-aio=OFF --disabled-storage-engines=MyISAM \
      > "$RESULT/${run}-mysqld.stdout" 2>&1 &
  fi
  SERVER_PID=$!
  for _ in $(seq 1 120); do
    if sql -e 'SELECT 1' > /dev/null 2>&1; then return; fi
    kill -0 "$SERVER_PID" 2>/dev/null || {
      tail -n 40 "$RESULT/${run}-mysqld.log" >&2
      return 1
    }
    sleep 1
  done
  tail -n 40 "$RESULT/${run}-mysqld.log" >&2
  return 1
}

sysbench_mysql() {
  sysbench --db-driver=mysql --mysql-user=root --mysql-socket="$SOCKET" \
    --mysql-db=uuse_perf --tables=1 --table-size=10000 "$@"
}

# Run the identical measurement set on the live server of one phase.
measure() {
  local run="$1"
  sql -e 'CREATE DATABASE IF NOT EXISTS uuse_perf'
  sysbench_mysql oltp_read_write prepare > "$RESULT/${run}-prepare.log" 2>&1
  sql -e "SHOW VARIABLES LIKE 'innodb_flush_log_at_trx_commit';
    SHOW GLOBAL STATUS LIKE 'Innodb_os_log_fsyncs'" \
    > "$RESULT/${run}-durability-before.tsv"
  ps -L -p "$SERVER_PID" -o tid,psr,pcpu,comm > "$RESULT/${run}-cpu-map.tsv"
  "$MYSQLSLAP" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
    --create-schema=uuse_perf --query='SELECT k FROM sbtest1 WHERE id=1' \
    --concurrency=8 --number-of-queries=10000 --iterations=3 \
    > "$RESULT/${run}-mysqlslap-read.log" 2>&1
  "$MYSQLSLAP" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
    --create-schema=uuse_perf \
    --query='UPDATE sbtest1 SET k=k+1 WHERE id=FLOOR(RAND()*10000)+1' \
    --concurrency=8 --number-of-queries=3000 --iterations=3 \
    > "$RESULT/${run}-mysqlslap-write.log" 2>&1
  sysbench_mysql --threads=8 --time=10 --percentile=99 --histogram=on \
    oltp_read_only run > "$RESULT/${run}-sysbench-read.log" 2>&1
  sysbench_mysql --threads=8 --time=10 --percentile=99 --histogram=on \
    oltp_read_write run > "$RESULT/${run}-sysbench-write.log" 2>&1
  sql -e "SHOW GLOBAL STATUS LIKE 'Innodb_os_log_fsyncs'" \
    > "$RESULT/${run}-durability-after.tsv"
}

{
  date -Is
  printf 'config=%s\n' "$CONFIG"
  printf 'mount=%s uuse_data=%s\n' "$MOUNT" "$UUSE_DATA"
  printf 'cpus=%s hugepages=%s\n' "$(nproc)" \
    "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)"
  free -h
  df -h /home/uuse
  sha256sum "$CONFIG" "$MYSQLD" "$UUSECTL"
  "$MYSQLD" --version
  sysbench --version
} > "$RESULT/environment.txt"

# Interleave modes so session drift is visible; each phase rebuilds its datadir.
for run in uuse-1 native-1 uuse-2 native-2; do
  mode="${run%-*}"
  if [[ "$mode" == native ]]; then
    STAGE="native-setup-$run"
    bind_driver nvme
    find_target_device
    mkfs.ext4 -F "$DEVICE" > "$RESULT/${run}-ext4-format.log" 2>&1
    mount -t ext4 -o noatime "$DEVICE" "$MOUNT"
    native_data="$MOUNT/datadir"
    STAGE="native-init-$run"
    env -u UUSE_MYSQL_CONFIG -u UUSE_MYSQL_PREFIX \
      "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" \
      --datadir="$native_data" --initialize-insecure \
      --innodb-use-native-aio=OFF --disabled-storage-engines=MyISAM \
      --log-error="$RESULT/${run}-initialize.log" \
      > "$RESULT/${run}-initialize.stdout" 2>&1
    STAGE="native-run-$run"
    start_server native "$run" "$native_data"
    measure "$run"
    stop_server
    sync
    du -sh "$native_data" > "$RESULT/${run}-datadir-size.txt"
    umount "$MOUNT"
  else
    STAGE="uuse-setup-$run"
    bind_driver uio_pci_generic
    "$UUSECTL" --config="$CONFIG" format --yes > "$RESULT/${run}-uuse-format.log" 2>&1
    rm -rf "$UUSE_DATA"
    mkdir -p "$UUSE_DATA"
    STAGE="uuse-init-$run"
    UUSE_MYSQL_CONFIG="$CONFIG" UUSE_MYSQL_PREFIX="$UUSE_DATA" \
      "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" --datadir="$UUSE_DATA" \
      --initialize-insecure --innodb-use-native-aio=OFF \
      --disabled-storage-engines=MyISAM \
      --log-error="$RESULT/${run}-initialize.log" \
      > "$RESULT/${run}-initialize.stdout" 2>&1
    STAGE="uuse-run-$run"
    start_server uuse "$run" "$UUSE_DATA"
    measure "$run"
    stop_server
    STAGE="uuse-check-$run"
    "$UUSECTL" --config="$CONFIG" check > "$RESULT/${run}-uuse-check.log" 2>&1
    grep -q 'corruption=none' "$RESULT/${run}-uuse-check.log"
  fi
  printf '%s complete\n' "$run"
done
STAGE=complete
