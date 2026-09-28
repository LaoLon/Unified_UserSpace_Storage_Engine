#!/usr/bin/env bash
# Compare the same MySQL 8.0 binary on one NVMe: HFS volume vs native ext4
# datadir. Sysbench runs read_write (about 70% read / 30% write), read_only,
# write_only, point_select and an optional mysqlslap seq_scan, at the given
# thread list with full latency histograms. MYSQLD_EXTRA_ARGS can shrink the
# buffer pool and force O_DIRECT so large working sets stay device-bound.
set -euo pipefail

BUILD="${BUILD:-/home/hfs/mysql-build}"
CONFIG="${CONFIG:-/root/code/config/hfs_mysql.conf}"
HFSCTL="${HFSCTL:-/root/code/build-verify/hfsctl}"
RESULT="${RESULT:-/home/hfs/mysql-native-vs-hfs-20260927}"
MOUNT="${MOUNT:-/mnt/mysql-native-vs-hfs-ext4}"
HFS_DATA="${HFS_DATA:-/home/hfs/mysql-native-vs-hfs-hfs-datadir}"
EXPECTED_SERIAL="${EXPECTED_SERIAL:-VMware NVME_0002}"
TABLES="${TABLES:-10}"
TABLE_SIZE="${TABLE_SIZE:-10000}"
DURATION="${DURATION:-10}"
THREADS_LIST="${THREADS_LIST:-16 32 64}"
SCENARIOS="${SCENARIOS:-read_write read_only write_only point_select}"
PHASES="${PHASES:-hfs-1 native-1 hfs-2 native-2}"
# Extra mysqld flags, e.g. "--innodb-buffer-pool-size=64M --innodb-flush-method=O_DIRECT".
MYSQLD_EXTRA_ARGS="${MYSQLD_EXTRA_ARGS:-}"
# seq_scan runs a mysqlslap UNION ALL scan over SEQ_TABLES tables per query.
SEQ_TABLES="${SEQ_TABLES:-4}"
SEQ_QUERIES_PER_THREAD="${SEQ_QUERIES_PER_THREAD:-1}"
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
read -r -a MYSQLD_EXTRA <<< "$MYSQLD_EXTRA_ARGS"

test -n "$BDF" && test -d "/sys/bus/pci/devices/$BDF"
for executable in "$MYSQLD" "$MYSQL" "$MYSQLADMIN" "$MYSQLSLAP" "$HFSCTL"; do
  test -x "$executable" || { printf 'missing: %s\n' "$executable" >&2; exit 2; }
done
command -v sysbench > /dev/null
test "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)" -ge 1024
test "$(nproc)" -ge 6
test "$(df -PB1 /home/hfs | awk 'NR==2 {print $4}')" -ge 4294967296
! pgrep -x mysqld > /dev/null
! pgrep -x hfs_db_bench > /dev/null
test ! -e "$RESULT"
[[ "$THREADS_LIST" =~ ^[0-9[:space:]]+$ ]] || { printf 'bad THREADS_LIST: %s\n' "$THREADS_LIST" >&2; exit 2; }
mkdir -p "$RESULT" "$MOUNT"

export LD_LIBRARY_PATH="/root/code/build-verify:${LD_LIBRARY_PATH:-}"

# Bind only the controller named in the HFS MySQL configuration.
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
  printf 'mysql native-vs-hfs cleanup: stage=%s exit=%s\n' "$STAGE" "$status"
  exit "$status"
}
trap cleanup EXIT

start_server() {
  local mode="$1" run="$2" data="$3"
  if [[ "$mode" == hfs ]]; then
    HFS_MYSQL_CONFIG="$CONFIG" HFS_MYSQL_PREFIX="$HFS_DATA" \
      "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" --datadir="$data" \
      --socket="$SOCKET" --pid-file="$RESULT/mysqld.pid" \
      --log-error="$RESULT/${run}-mysqld.log" --skip-networking \
      --server-id=105 --innodb-flush-log-at-trx-commit=1 \
      --innodb-use-native-aio=OFF --disabled-storage-engines=MyISAM \
      "${MYSQLD_EXTRA[@]}" \
      > "$RESULT/${run}-mysqld.stdout" 2>&1 &
  else
    env -u HFS_MYSQL_CONFIG -u HFS_MYSQL_PREFIX \
      "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" --datadir="$data" \
      --socket="$SOCKET" --pid-file="$RESULT/mysqld.pid" \
      --log-error="$RESULT/${run}-mysqld.log" --skip-networking \
      --server-id=105 --innodb-flush-log-at-trx-commit=1 \
      --innodb-use-native-aio=OFF --disabled-storage-engines=MyISAM \
      "${MYSQLD_EXTRA[@]}" \
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
    --mysql-db=hfs_perf --tables="$TABLES" --table-size="$TABLE_SIZE" "$@"
}

# Map matrix scenario names to the sysbench 1.0 Lua tests.
sysbench_test_for() {
  case "$1" in
    read_write) printf 'oltp_read_write' ;;
    read_only) printf 'oltp_read_only' ;;
    write_only) printf 'oltp_write_only' ;;
    point_select) printf 'oltp_point_select' ;;
    *) return 1 ;;
  esac
}

# seq_scan is a mysqlslap full scan across SEQ_TABLES tables (device-bound
# when the buffer pool is smaller than the scanned data).
SEQ_SCAN_QUERY="SELECT SQL_NO_CACHE SUM(k) FROM sbtest1"
for seq_table in $(seq 2 "$SEQ_TABLES"); do
  SEQ_SCAN_QUERY="$SEQ_SCAN_QUERY UNION ALL SELECT SQL_NO_CACHE SUM(k) FROM sbtest$seq_table"
done

# Run the scenario matrix once on the live server of one phase.
measure() {
  local run="$1"
  sql -e 'CREATE DATABASE IF NOT EXISTS hfs_perf'
  sysbench_mysql oltp_read_write prepare > "$RESULT/${run}-prepare.log" 2>&1
  sql -e "SHOW VARIABLES LIKE 'innodb_flush_log_at_trx_commit'" \
    > "$RESULT/${run}-durability.tsv"
  ps -L -p "$SERVER_PID" -o tid,psr,pcpu,comm > "$RESULT/${run}-cpu-map.tsv"
  printf 'run\tscenario\tthreads\tduration_s\tfsync_before\tfsync_after\tlog\n' \
    > "$RESULT/${run}-results.tsv"
  for scenario in $SCENARIOS; do
    for threads in $THREADS_LIST; do
      local log="$RESULT/${run}-${scenario}-t${threads}.log"
      local fsync_before fsync_after
      fsync_before="$(sql -e "SHOW GLOBAL STATUS LIKE 'Innodb_os_log_fsyncs'" | awk '{print $2}')"
      if [[ "$scenario" == seq_scan ]]; then
        "$MYSQLSLAP" --no-defaults --protocol=socket --socket="$SOCKET" -uroot \
          --create-schema=hfs_perf --query="$SEQ_SCAN_QUERY" \
          --concurrency="$threads" \
          --number-of-queries=$((threads * SEQ_QUERIES_PER_THREAD)) \
          --iterations=1 > "$log" 2>&1
      else
        local test_name
        test_name="$(sysbench_test_for "$scenario")"
        sysbench_mysql --threads="$threads" --time="$DURATION" --percentile=99 \
          --histogram=on "$test_name" run > "$log" 2>&1
      fi
      fsync_after="$(sql -e "SHOW GLOBAL STATUS LIKE 'Innodb_os_log_fsyncs'" | awk '{print $2}')"
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$run" "$scenario" "$threads" "$DURATION" "$fsync_before" "$fsync_after" \
        "$(basename "$log")" >> "$RESULT/${run}-results.tsv"
      printf '%s %s t%s complete\n' "$run" "$scenario" "$threads"
    done
  done
}

{
  date -Is
  printf 'config=%s\n' "$CONFIG"
  printf 'mount=%s hfs_data=%s\n' "$MOUNT" "$HFS_DATA"
  printf 'tables=%s table_size=%s duration_s=%s threads=%s scenarios=%s\n' \
    "$TABLES" "$TABLE_SIZE" "$DURATION" "$THREADS_LIST" "$SCENARIOS"
  printf 'mysqld_extra_args=%s\n' "$MYSQLD_EXTRA_ARGS"
  printf 'seq_tables=%s seq_queries_per_thread=%s\n' "$SEQ_TABLES" "$SEQ_QUERIES_PER_THREAD"
  printf 'cpus=%s hugepages=%s\n' "$(nproc)" \
    "$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)"
  free -h
  df -h /home/hfs
  sha256sum "$CONFIG" "$MYSQLD" "$HFSCTL"
  "$MYSQLD" --version
  sysbench --version
} > "$RESULT/environment.txt"

# Interleave modes so session drift is visible; each phase rebuilds its datadir.
for run in $PHASES; do
  mode="${run%-*}"
  if [[ "$mode" == native ]]; then
    STAGE="native-setup-$run"
    bind_driver nvme
    find_target_device
    mkfs.ext4 -F "$DEVICE" > "$RESULT/${run}-ext4-format.log" 2>&1
    mount -t ext4 -o noatime "$DEVICE" "$MOUNT"
    native_data="$MOUNT/datadir"
    STAGE="native-init-$run"
    env -u HFS_MYSQL_CONFIG -u HFS_MYSQL_PREFIX \
      "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" \
      --datadir="$native_data" --initialize-insecure \
      --innodb-use-native-aio=OFF --disabled-storage-engines=MyISAM \
      "${MYSQLD_EXTRA[@]}" \
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
    STAGE="hfs-setup-$run"
    bind_driver uio_pci_generic
    "$HFSCTL" --config="$CONFIG" format --yes > "$RESULT/${run}-hfs-format.log" 2>&1
    rm -rf "$HFS_DATA"
    mkdir -p "$HFS_DATA"
    STAGE="hfs-init-$run"
    HFS_MYSQL_CONFIG="$CONFIG" HFS_MYSQL_PREFIX="$HFS_DATA" \
      "$MYSQLD" --no-defaults --user=root --basedir="$BUILD" --datadir="$HFS_DATA" \
      --initialize-insecure --innodb-use-native-aio=OFF \
      --disabled-storage-engines=MyISAM \
      "${MYSQLD_EXTRA[@]}" \
      --log-error="$RESULT/${run}-initialize.log" \
      > "$RESULT/${run}-initialize.stdout" 2>&1
    STAGE="hfs-run-$run"
    start_server hfs "$run" "$HFS_DATA"
    measure "$run"
    stop_server
    STAGE="hfs-check-$run"
    "$HFSCTL" --config="$CONFIG" check > "$RESULT/${run}-hfs-check.log" 2>&1
    grep -q 'corruption=none' "$RESULT/${run}-hfs-check.log"
  fi
  printf '%s complete\n' "$run"
done
STAGE=complete
