#!/bin/bash
# Shard-120 same-disk TiKV performance A/B: both sides on the target NVMe
# (0000:13:00.0). UUSE side: bound to uio + uusectl format. Host side: bound to
# the kernel nvme driver, mkfs.ext4, mounted at /mnt/tikv-host-target.
# Interleaved rounds; sysbench oltp_point_select + oltp_read_write.
set -u
export LD_LIBRARY_PATH=/root/code/build-uuse
BASE=/build/tidb
UUSECTL=/root/code/build-uuse/uusectl
CONF=/root/code/config/uuse_nvme.conf
TIKV=/build/tikv-build/release/tikv-server
PD=$BASE/bin/pd-server
TIDB=$BASE/bin/tidb-server
MYSQL=/home/uuse/mysql-build/runtime_output_directory/mysql
SYSBENCH=/usr/bin/sysbench
BDF=0000:13:00.0
MOUNT=/mnt/tikv-host-target
ROUNDS=${ROUNDS:-5}
BENCH_TIME=${BENCH_TIME:-60}
THREADS=${THREADS:-8}
TABLES=8
TABLE_SIZE=20000
R=$BASE/perf-ab-target-$(date +%Y%m%d-%H%M%S)
mkdir -p "$R"
echo "R=$R"

# Host-ext4 side on the target device (mounted at $MOUNT).
mkdir -p $BASE/logs/rocksdb
cat >$BASE/tikv-host-target.toml <<'EOF'
[server]
addr = "127.0.0.1:20160"
status-addr = "127.0.0.1:20180"

[storage]
data-dir = "/mnt/tikv-host-target/tikv-data"
reserve-space = "0GB"
reserve-raft-space = "0GB"

[raft-engine]
dir = "/mnt/tikv-host-target/tikv-data/raft-engine"

[log]
level = "info"
file.filename = "/build/tidb/logs/tikv-host-target.log"

[rocksdb]
info-log-dir = "/build/tidb/logs/rocksdb"

[raftdb]
info-log-dir = "/build/tidb/logs/rocksdb"
EOF

sqlq() { "$MYSQL" --no-defaults -h127.0.0.1 -P4000 -uroot "$@" 2>&1; }
wait_pd() { for i in $(seq 1 30); do curl -s http://127.0.0.1:2379/pd/api/v1/health >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }
wait_tikv() { for i in $(seq 1 120); do curl -s http://127.0.0.1:20180/status >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }
wait_tidb() { for i in $(seq 1 60); do curl -s http://127.0.0.1:10080/status >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }

stop_stack() {
  pkill -9 -f tikv-server 2>/dev/null || true
  pkill -9 -f 'tidb-server --store=tikv' 2>/dev/null || true
  pkill -9 -f 'pd-server --name=pd1' 2>/dev/null || true
  sleep 3
}

bind_uio() {
  local D=/sys/bus/pci/devices/$BDF cur
  echo uio_pci_generic > "$D/driver_override"
  if [ -e "$D/driver" ]; then
    cur=$(basename "$(readlink -f "$D/driver")")
    [ "$cur" = uio_pci_generic ] || echo "$BDF" > "/sys/bus/pci/drivers/$cur/unbind"
  fi
  [ -e "/sys/bus/pci/drivers/uio_pci_generic/$BDF" ] || \
    echo "$BDF" > /sys/bus/pci/drivers/uio_pci_generic/bind
  # Clear the override so a later kernel-nvme bind is not forced back to uio.
  printf '\n' > "$D/driver_override"
  echo "uio bound: $(readlink -f /sys/bus/pci/devices/$BDF/driver | xargs basename)"
}

# Binds the kernel nvme driver, formats ext4 on the namespace and mounts it.
bind_host_target() {
  local D=/sys/bus/pci/devices/$BDF cur dev
  if [ -e "$D/driver" ]; then
    cur=$(basename "$(readlink -f "$D/driver")")
    [ "$cur" = nvme ] || echo "$BDF" > "/sys/bus/pci/drivers/$cur/unbind"
  fi
  printf '\n' > "$D/driver_override"
  echo "$BDF" > /sys/bus/pci/drivers/nvme/bind 2>/dev/null || true
  for i in $(seq 1 20); do
    dev=""
    for b in /sys/block/nvme*n*; do
      [ -e "$b" ] || continue
      case "$(readlink -f "$b/device")" in
        *"$BDF"*) dev="/dev/$(basename "$b")" ;;
      esac
    done
    [ -n "$dev" ] && break
    sleep 1
  done
  [ -n "$dev" ] || { echo "HOST_DEVICE_NOT_FOUND"; exit 1; }
  echo "host target device: $dev"
  umount "$MOUNT" 2>/dev/null || true
  mkfs.ext4 -F -q "$dev" || { echo "MKFS_FAIL"; exit 1; }
  mkdir -p "$MOUNT"
  mount "$dev" "$MOUNT" || { echo "MOUNT_FAIL"; exit 1; }
}

unbind_host_target() {
  umount "$MOUNT" 2>/dev/null || true
  if [ -e "/sys/bus/pci/devices/$BDF/driver" ]; then
    local cur
    cur=$(basename "$(readlink -f /sys/bus/pci/devices/$BDF/driver)")
    [ "$cur" = nvme ] && echo "$BDF" > /sys/bus/pci/drivers/nvme/unbind || true
  fi
}

start_stack() {
  local side="$1" cfg pd_dir
  stop_stack
  if [ "$side" = uuse ]; then
    bind_uio
    cfg=$BASE/tikv-uuse-both.toml
    pd_dir=$BASE/pd-ab-t-uuse
    rm -rf "$pd_dir" $BASE/tikv-data
    mkdir -p $BASE/tikv-data
    "$UUSECTL" --config="$CONF" format --yes >"$R/format-$side.log" 2>&1 || { echo "FORMAT_FAIL"; exit 1; }
  else
    unbind_host_target 2>/dev/null || true
    bind_host_target
    cfg=$BASE/tikv-host-target.toml
    pd_dir=$BASE/pd-ab-t-host
    rm -rf "$pd_dir" "$MOUNT/tikv-data"
    mkdir -p "$MOUNT/tikv-data"
  fi
  setsid nohup "$PD" --name=pd1 --data-dir="$pd_dir" \
    --client-urls=http://127.0.0.1:2379 --peer-urls=http://127.0.0.1:2380 \
    --log-file=$BASE/logs/pd.log >$BASE/logs/pd.stdout 2>&1 &
  wait_pd || { echo "PD_FAIL"; exit 1; }
  setsid nohup "$TIKV" --config "$cfg" --pd-endpoints 127.0.0.1:2379 >>$BASE/logs/tikv.stdout 2>&1 &
  wait_tikv || { echo "TIKV_FAIL"; exit 1; }
  setsid nohup "$TIDB" --store=tikv --path=127.0.0.1:2379 --host=127.0.0.1 -P 4000 --status=10080 \
    --log-file=$BASE/logs/tidb.log >>$BASE/logs/tidb.stdout 2>&1 &
  wait_tidb || { echo "TIDB_FAIL"; exit 1; }
  sqlq -e "SET GLOBAL tidb_ddl_enable_fast_reorg = OFF; SET GLOBAL tidb_enable_dist_task = OFF;" >/dev/null
}

sysbench_common() {
  "$SYSBENCH" --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=4000 \
    --mysql-user=root --mysql-db=sbtest --tables=$TABLES --table-size=$TABLE_SIZE \
    --threads=$THREADS --time=$BENCH_TIME --percentile=99 --report-interval=0 "$@"
}

run_side() {
  local side="$1" round="$2" t0 t1
  echo "== side=$side round=$round $(date -u +%H:%M:%S) =="
  t0=$(date +%s)
  start_stack "$side"
  t1=$(date +%s); echo "$side stack_up_s=$((t1 - t0))" >>"$R/times.txt"
  sqlq -e "DROP DATABASE IF EXISTS sbtest; CREATE DATABASE sbtest;" >/dev/null
  t0=$(date +%s)
  sysbench_common oltp_common prepare >"$R/$side-r$round-prepare.txt" 2>&1 || { echo "PREPARE_FAIL"; exit 1; }
  t1=$(date +%s); echo "$side prepare_s=$((t1 - t0))" >>"$R/times.txt"
  local iostat_pid mpstat_pid
  iostat -x 1 $((BENCH_TIME + 2)) >"$R/$side-r$round-iostat.txt" 2>&1 &
  iostat_pid=$!
  mpstat -P ALL 1 $((BENCH_TIME + 2)) >"$R/$side-r$round-mpstat.txt" 2>&1 &
  mpstat_pid=$!
  sysbench_common oltp_point_select run >"$R/$side-r$round-point_select.txt" 2>&1
  wait $iostat_pid $mpstat_pid 2>/dev/null || true
  iostat -x 1 $((BENCH_TIME + 2)) >"$R/$side-r$round-iostat-rw.txt" 2>&1 &
  iostat_pid=$!
  mpstat -P ALL 1 $((BENCH_TIME + 2)) >"$R/$side-r$round-mpstat-rw.txt" 2>&1 &
  mpstat_pid=$!
  sysbench_common oltp_read_write run >"$R/$side-r$round-read_write.txt" 2>&1
  wait $iostat_pid $mpstat_pid 2>/dev/null || true
  curl -s http://127.0.0.1:20180/metrics >"$R/$side-r$round-metrics.txt" 2>/dev/null || true
  echo "== done side=$side round=$round =="
}

for round in $(seq 1 "$ROUNDS"); do
  if [ $((round % 2)) = 1 ]; then
    run_side uuse "$round"
    run_side host "$round"
  else
    run_side host "$round"
    run_side uuse "$round"
  fi
done

stop_stack
# Restore the uio binding/mount state so later UUSE tooling (uusectl format,
# matrix runs) finds the device as expected.
umount "$MOUNT" 2>/dev/null || true
bind_uio 2>/dev/null || true
cd /build
python3 /build/uuse_perf_ab_summary.py "$R" | tee "$R/summary.txt" || true
echo "PERF_AB_TARGET_DONE"
