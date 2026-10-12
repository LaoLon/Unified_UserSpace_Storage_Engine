#!/bin/bash
# MinIO cluster helper (Shard-9.4 curated entry). Run on EACH server node with
# the same NODES list; every node must use the same MINIO_* values and the same
# CONF path (MinIO rejects mixed configs), and the local DRIVE path is used in
# every endpoint URL.
#
#   NODES="192.168.1.250 192.168.1.251 192.168.1.252 192.168.1.253" \
#     bash run_cluster_vm.sh up            # stop, bind uio, start this node
#   ... run_cluster_vm.sh status | wipe | down
#
# Env: DIST, CONF (default /etc/uuse-minio.conf), DRIVE, BDF, NODES, PORT.
set -u
DIST=${DIST:-/root/minio-dist}
CONF=${CONF:-/etc/uuse-minio.conf}
DRIVE=${DRIVE:-/root/uuse-data}
BDF=${BDF:-0000:13:00.0}
NODES=${NODES:-"192.168.1.250 192.168.1.251 192.168.1.252 192.168.1.253"}
PORT=${PORT:-9000}
ACTION=${1:-status}
LOG=${LOG:-/tmp/minio_cluster_4n.log}

[ -x "$DIST/minio-bin" ] || { echo "no minio-bin in $DIST (run build_minio_vm.sh)"; exit 1; }
export LD_LIBRARY_PATH="$DIST:/usr/local/lib/uuse"
export MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin
export MINIO_UPDATE=off MINIO_CI_CD=1

endpoints=""
for n in $NODES; do endpoints="$endpoints http://$n:$PORT$DRIVE"; done

case "$ACTION" in
  up)
    echo "== hugepages =="
    echo 'vm.nr_hugepages=1024' > /etc/sysctl.d/99-zz-uuse-hugepages.conf
    sysctl -p /etc/sysctl.d/99-zz-uuse-hugepages.conf >/dev/null 2>&1 || sysctl -w vm.nr_hugepages=1024 >/dev/null
    echo "nr_hugepages=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)"

    echo "== bind uio =="
    modprobe uio_pci_generic 2>/dev/null || true
    cur=$(readlink /sys/bus/pci/devices/$BDF/driver 2>/dev/null | xargs -r basename)
    if [ "$cur" != "uio_pci_generic" ]; then
      [ -e /sys/bus/pci/devices/$BDF/driver/unbind ] && echo "$BDF" > /sys/bus/pci/devices/$BDF/driver/unbind || true
      echo uio_pci_generic > /sys/bus/pci/devices/$BDF/driver_override 2>/dev/null || true
      echo "$BDF" > /sys/bus/pci/drivers/uio_pci_generic/bind || { echo BIND-FAIL; exit 1; }
    fi
    readlink /sys/bus/pci/devices/$BDF/driver | xargs basename

    [ -f "$CONF" ] || cp "$DIST/uuse_nvme.conf" "$CONF"
    mkdir -p "$DRIVE"
    export MINIO_UUSE_DRIVES="$DRIVE=$CONF"
    for pid in $(pgrep -x minio-bin); do kill -9 "$pid" 2>/dev/null || true; done
    sleep 1
    cd "$DIST"
    # shellcheck disable=SC2086
    nohup ./minio-bin server --address ":$PORT" --console-address :9001 $endpoints > "$LOG" 2>&1 &
    sleep 25
    pgrep -x minio-bin >/dev/null && echo NODE-UP || { echo NODE-DOWN; tail -8 "$LOG"; exit 1; }
    ;;
  status)
    pgrep -x minio-bin >/dev/null && echo "minio-bin: up" || echo "minio-bin: down"
    echo "initialized=$(grep -c 'All MinIO sub-systems initialized' "$LOG" 2>/dev/null || echo 0)"
    echo "uuse-mounts=$(grep -c 'uuse:.*mounted' "$LOG" 2>/dev/null || echo 0)"
    ;;
  wipe)
    # Full reset before a cluster rebuild: stop with -9, drop the namespace and
    # let minio-wipe pre-create the .minio.sys tree durably.
    for pid in $(pgrep -x minio-bin); do kill -9 "$pid" 2>/dev/null || true; done
    sleep 2
    rm -rf "$DRIVE"; mkdir -p "$DRIVE"
    "$DIST/minio-wipe" "$CONF" "$DRIVE" 2>&1 | grep -vE '^uuse:|^\[CPU' | tail -1
    ;;
  down)
    for pid in $(pgrep -x minio-bin); do kill -TERM "$pid" 2>/dev/null || true; done
    for i in $(seq 1 30); do pgrep -x minio-bin >/dev/null || break; sleep 1; done
    pgrep -x minio-bin >/dev/null && { echo "still running, SIGKILL"; pkill -9 -x minio-bin; } || echo NODE-DOWN
    ;;
  *)
    echo "usage: run_cluster_vm.sh up|status|wipe|down  (NODES=... on every node)"; exit 2 ;;
esac
