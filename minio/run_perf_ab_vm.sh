#!/bin/bash
# Same-disk ext4 <-> UUSE performance A/B for MinIO (Shard-9.5 curated entry).
# The target NVMe is switched between the kernel nvme driver (ext4 leg) and
# uio (UUSE leg) on the same physical device; warp drives S3 on port 9100.
#
#   MODE=full (default): PUT+GET legs, 8 s each, 2 rounds (formal A/B, gate)
#   MODE=get:            seed PUT 6 s + 3x GET 6 s per leg; round 2 runs UUSE
#                        with UUSE_MINIO_META_CACHE=0 as the cache control
#
# Env: DIST, CONF, DRIVE, BDF, WARP, MNT, ROUNDS, RESTORE=0 to keep the node
#      as a standalone MinIO instead of restoring the cluster.
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DIST=${DIST:-/root/minio-dist}
CONF=${CONF:-/etc/uuse-minio.conf}
DRIVE=${DRIVE:-/root/uuse-data}
BDF=${BDF:-0000:13:00.0}
MNT=${MNT:-/mnt/perf-ext4}
WARP=${WARP:-/usr/local/bin/warp}
ROUNDS=${ROUNDS:-2}
MODE=${MODE:-full}
S3="--host 127.0.0.1:9100 --access-key minioadmin --secret-key minioadmin --no-color"
export MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin MINIO_UPDATE=off MINIO_CI_CD=1
export LD_LIBRARY_PATH="$DIST:/usr/local/lib/uuse"
cd "$DIST" || exit 1

to_nvme() {
  if readlink "/sys/bus/pci/devices/$BDF/driver" >/dev/null 2>&1; then
    echo "$BDF" > "/sys/bus/pci/devices/$BDF/driver/unbind" 2>/dev/null || true
  fi
  sleep 2
  printf '' > "/sys/bus/pci/devices/$BDF/driver_override"
  echo 1 > "/sys/bus/pci/devices/$BDF/remove" 2>/dev/null || true
  sleep 2
  echo 1 > /sys/bus/pci/rescan
  sleep 6
}
to_uio() {
  modprobe uio_pci_generic 2>/dev/null || true
  if readlink "/sys/bus/pci/devices/$BDF/driver" >/dev/null 2>&1; then
    echo "$BDF" > "/sys/bus/pci/devices/$BDF/driver/unbind" 2>/dev/null || true
  fi
  sleep 2
  echo uio_pci_generic > "/sys/bus/pci/devices/$BDF/driver_override"
  echo "$BDF" > /sys/bus/pci/drivers/uio_pci_generic/bind 2>/dev/null || true
  sleep 2
}
# Select the kernel block device by PCI topology, never by nvmeX number.
find_kdev() {
  for d in /sys/block/nvme*; do
    [ -e "$d" ] || continue
    addr=$(readlink -f "$d/device" 2>/dev/null | grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]' | tail -1)
    [ "$addr" = "$BDF" ] && { basename "$d"; return 0; }
  done
  return 1
}
stop_minio() { for pid in $(pgrep -x minio-bin); do kill -9 "$pid" 2>/dev/null || true; done; sleep 1; }

run_leg_full() { # $1=variant $2=round: PUT then GET, 8 s each
  local V=$1 R=$2 PF=/tmp/ab_${V}_${R}_put.log GF=/tmp/ab_${V}_${R}_get.log
  $WARP put $S3 --bucket ab-$V --duration 8s --concurrent 16 --obj.size 1MiB --noclear > "$PF" 2>&1
  $WARP get $S3 --bucket ab-$V --list-existing --noclear --duration 8s --concurrent 16 > "$GF" 2>&1
  echo "[$V r$R] PUT : $(grep -E 'Average' "$PF" | head -1 | sed 's/^ *//')"
  echo "[$V r$R] GET : $(grep -E 'Average' "$GF" | head -1 | sed 's/^ *//')"
  echo "[$V r$R] err : put=$(grep -oE '[0-9]+ errors' "$PF" | head -1) get=$(grep -oE '[0-9]+ errors' "$GF" | head -1)"
}
run_leg_get() { # $1=variant $2=round: seed PUT 6 s + 3x GET 6 s
  local V=$1 R=$2 PF=/tmp/abg_${V}_${R}_seed.log
  $WARP put $S3 --bucket abg-$V --duration 6s --concurrent 16 --obj.size 1MiB --noclear > "$PF" 2>&1
  echo "[$V r$R] SEED : $(grep -E 'Average' "$PF" | head -1 | sed 's/^ *//')"
  for i in 1 2 3; do
    local GF=/tmp/abg_${V}_${R}_get$i.log
    $WARP get $S3 --bucket abg-$V --list-existing --noclear --duration 6s --concurrent 16 > "$GF" 2>&1
    echo "[$V r$R] GET$i : $(grep -E 'Average' "$GF" | head -1 | sed 's/^ *//')"
    echo "[$V r$R] err  : $(grep -oE '[0-9]+ errors' "$GF" | head -1)"
  done
}

for R in $(seq 1 "$ROUNDS"); do
  echo "===== ROUND $R : EXT4 ====="
  stop_minio
  umount "$MNT" 2>/dev/null || true
  to_nvme
  KDEV=$(find_kdev) || { echo "no kdev for $BDF"; exit 1; }
  mkfs.ext4 -F -q -L perfa "/dev/$KDEV"
  mkdir -p "$MNT"; mount "/dev/$KDEV" "$MNT" || { echo MOUNT-FAIL; exit 1; }
  mkdir -p "$MNT/data"; df -h "$MNT" | tail -1
  nohup ./minio-bin server "$MNT/data" --address :9100 --console-address :9101 > "/tmp/ab_ext4_$R.log" 2>&1 &
  sleep 18
  if [ "$MODE" = get ]; then run_leg_get ext4 "$R"; else run_leg_full ext4 "$R"; fi

  echo "===== ROUND $R : UUSE $([ "$MODE" = get ] && { [ "$R" -eq 1 ] && echo '(cache on)' || echo '(cache off)'; }) ====="
  stop_minio
  umount "$MNT" 2>/dev/null || true
  sleep 1
  to_uio
  readlink "/sys/bus/pci/devices/$BDF/driver" | xargs -r basename
  ./uusectl --config="$CONF" format --yes > "/tmp/ab_fmt_$R.log" 2>&1
  rm -rf "$DRIVE"; mkdir -p "$DRIVE"
  export MINIO_UUSE_DRIVES="$DRIVE=$CONF"
  if [ "$MODE" = get ] && [ "$R" -gt 1 ]; then
    export UUSE_MINIO_META_CACHE=0
  else
    unset UUSE_MINIO_META_CACHE 2>/dev/null || true
  fi
  nohup ./minio-bin server "$DRIVE" --address :9100 --console-address :9101 > "/tmp/ab_uuse_$R.log" 2>&1 &
  sleep 18
  echo "uuse-mounts=$(grep -c 'uuse:.*mounted' "/tmp/ab_uuse_$R.log")"
  if [ "$MODE" = get ]; then run_leg_get uuse "$R"; else run_leg_full uuse "$R"; fi
done

if [ "${RESTORE:-1}" = 1 ]; then
  echo "===== restore cluster node ====="
  stop_minio
  rm -rf "$DRIVE"; mkdir -p "$DRIVE"
  "$DIST/minio-wipe" "$CONF" "$DRIVE" 2>&1 | grep -vE '^uuse:|^\[CPU' | tail -1
  DIST="$DIST" CONF="$CONF" DRIVE="$DRIVE" bash "$SCRIPT_DIR/run_cluster_vm.sh" up 2>&1 | tail -1
fi
echo PERF-AB-DONE
