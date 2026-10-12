#!/bin/bash
# Single-node MinIO acceptance on one lab VM (Shard-9.2/9.3 curated entry):
# device prep (hugepages + uio), format/wipe the UUSE volume, start minio-bin
# on :9000 and run the functional matrix against it.
#
# Env: DIST (runtime dir from build_minio_vm.sh), CONF, DRIVE, BDF,
#      MATRIX_ENDPOINT (default 127.0.0.1:9000), SKIP_FORMAT=1 to keep data.
set -u
DIST=${DIST:-/root/minio-dist}
CONF=${CONF:-/etc/uuse-minio.conf}
DRIVE=${DRIVE:-/root/uuse-data}
BDF=${BDF:-0000:13:00.0}
MATRIX_ENDPOINT=${MATRIX_ENDPOINT:-127.0.0.1:9000}
LOG=${LOG:-/tmp/minio_single_node.log}
GO_PROFILE=${GO_PROFILE:-/etc/profile.d/go-minio.sh}

[ -x "$DIST/minio-bin" ] || { echo "no minio-bin in $DIST (run build_minio_vm.sh)"; exit 1; }
export LD_LIBRARY_PATH="$DIST:/usr/local/lib/uuse"
export MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin
export MINIO_UPDATE=off MINIO_CI_CD=1
[ -r "$GO_PROFILE" ] && source "$GO_PROFILE"

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

echo "== conf =="
[ -f "$CONF" ] || cp "$DIST/uuse_nvme.conf" "$CONF"
grep -E 'pci_bdf|bdev_name' "$CONF" | head -4

if [ "${SKIP_FORMAT:-0}" != 1 ]; then
  echo "== format + wipe =="
  "$DIST/uusectl" --config="$CONF" format --yes > /tmp/minio_format.log 2>&1 || { tail -5 /tmp/minio_format.log; exit 1; }
  rm -rf "$DRIVE"; mkdir -p "$DRIVE"
  "$DIST/minio-wipe" "$CONF" "$DRIVE" 2>&1 | grep -vE '^uuse:|^\[CPU' | tail -1
fi

echo "== start minio-bin (single node) =="
for pid in $(pgrep -x minio-bin); do kill -9 "$pid" 2>/dev/null || true; done
sleep 1
export MINIO_UUSE_DRIVES="$DRIVE=$CONF"
cd "$DIST"
nohup ./minio-bin server "$DRIVE" --address :9000 --console-address :9001 > "$LOG" 2>&1 &
for i in $(seq 1 60); do
  curl -sf "http://127.0.0.1:9000/minio/health/live" >/dev/null 2>&1 && break
  sleep 1
done
pgrep -x minio-bin >/dev/null && echo NODE-UP || { echo NODE-DOWN; tail -12 "$LOG"; exit 1; }
echo "uuse-mounts=$(grep -c 'uuse:.*mounted' "$LOG")"

echo "== functional matrix ($MATRIX_ENDPOINT) =="
MATRIX_ENDPOINT="$MATRIX_ENDPOINT" "$DIST/matrix-client" 2>&1 | tee /tmp/minio_single_matrix.log | tail -6
grep -q 'MATRIX-DONE pass=.* fail=0' /tmp/minio_single_matrix.log \
  && echo SINGLE-NODE-PASS || { echo SINGLE-NODE-FAIL; exit 1; }
