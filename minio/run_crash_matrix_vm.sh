#!/bin/bash
# Crash/durability matrix on the real UUSE path (Shard-9.3 curated entry):
# reset the volume, start minio-bin, kill -9 mid-write, restart and verify all
# client-ACKed objects (plus a full sweep), then repeat the kill during a
# second write round and sweep again.
#
# Env: DIST, CONF, DRIVE, ROUNDS (default 2), ACK_FILE, WIPE=1 (default).
set -u
DIST=${DIST:-/root/minio-dist}
CONF=${CONF:-/etc/uuse-minio.conf}
DRIVE=${DRIVE:-/root/uuse-data}
ROUNDS=${ROUNDS:-2}
ACK_FILE=${ACK_FILE:-/tmp/minio_crash_ack.txt}
LOG_DIR=${LOG_DIR:-/tmp}

[ -x "$DIST/minio-bin" ] || { echo "no minio-bin in $DIST (run build_minio_vm.sh)"; exit 1; }
export LD_LIBRARY_PATH="$DIST:/usr/local/lib/uuse"
export MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin
export MINIO_UPDATE=off MINIO_CI_CD=1
export MINIO_UUSE_DRIVES="$DRIVE=$CONF"
MATRIX_ENDPOINT=${MATRIX_ENDPOINT:-127.0.0.1:9000}
export MATRIX_ENDPOINT

STOPPED=0
start_server() { # $1 = log file
  cd "$DIST"
  nohup ./minio-bin server "$DRIVE" --address :9000 --console-address :9001 > "$1" 2>&1 &
  for i in $(seq 1 60); do
    curl -sf "http://127.0.0.1:9000/minio/health/live" >/dev/null 2>&1 && return 0
    sleep 1
  done
  echo SERVER-DOWN; tail -8 "$1"; exit 1
}
stop_server() { for pid in $(pgrep -x minio-bin); do kill -9 "$pid" 2>/dev/null || true; done; sleep 2; }
trap 'stop_server' EXIT

if [ "${WIPE:-1}" = 1 ]; then
  echo "== reset =="
  stop_server
  rm -rf "$DRIVE"; mkdir -p "$DRIVE"
  "$DIST/minio-wipe" "$CONF" "$DRIVE" 2>&1 | grep -vE '^uuse:|^\[CPU' | tail -1
fi

start_server "$LOG_DIR/minio_crash_r0.log"
for R in $(seq 1 "$ROUNDS"); do
  echo "== round $R: write then kill -9 mid-flight =="
  rm -f "$ACK_FILE"
  nohup "$DIST/crash-tool" write > "$LOG_DIR/minio_crash_writer_$R.log" 2>&1 &
  writer=$!
  sleep 3
  stop_server
  kill -9 "$writer" 2>/dev/null || true
  wait "$writer" 2>/dev/null || true
  grep '^ACK' "$LOG_DIR/minio_crash_writer_$R.log" | sort > "$ACK_FILE" || true
  echo "acked-before-crash=$(wc -l < "$ACK_FILE")"

  echo "== restart + verify ACKed objects + sweep =="
  start_server "$LOG_DIR/minio_crash_verify_$R.log"
  "$DIST/crash-tool" verify "$ACK_FILE" | tail -1
  "$DIST/crash-tool" sweep | tail -1
done

echo "== graceful stop =="
for pid in $(pgrep -x minio-bin); do kill -TERM "$pid" 2>/dev/null || true; done
sleep 10
echo CRASH-MATRIX-DONE
