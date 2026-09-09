#!/usr/bin/env bash
# Run the shard-59 performance closeout with explicit backend skip reporting.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"
CONFIG="${2:-$ROOT/config/hfs_db_aio.json}"
IMAGE="${3:-/var/tmp/hfs-shard59-aio.img}"
OUT="${4:-/var/tmp/hfs-shard59-performance}"
ROUNDS="${HFS_SHARD59_ROUNDS:-3}"
DURATION="${HFS_SHARD59_DURATION_SECONDS:-60}"
FIO_RUNTIME="${HFS_SHARD59_FIO_RUNTIME_SECONDS:-15}"
FIO_SIZE="${HFS_SHARD59_FIO_SIZE:-256m}"
DB_NUM="${HFS_SHARD59_DB_NUM:-20000}"
MYSQL_QUERIES="${HFS_SHARD59_MYSQL_QUERIES:-2000}"
MYSQL_ITERATIONS="${HFS_SHARD59_MYSQL_ITERATIONS:-3}"
SPDK_PLUGIN="${HFS_SHARD59_SPDK_PLUGIN:-$ROOT/spdk/build/fio/spdk_bdev}"
NVME_CONFIG="${HFS_SHARD59_NVME_CONFIG:-$ROOT/config/hfs_nvme.json}"

if [[ ! -f "$LIB" || ! -f "$CONFIG" ]]; then
	echo "usage: $0 [libhfs.so] [aio-config] [image] [output dir]" >&2
	exit 2
fi
if ! [[ "$ROUNDS" =~ ^[1-9][0-9]*$ && "$DURATION" =~ ^[1-9][0-9]*$ ]]; then
	echo "rounds and duration must be positive integers" >&2
	exit 2
fi

mkdir -p "$OUT"
LIB="$(realpath "$LIB")"
EFFECTIVE_CONFIG="$OUT/hfs_db_aio.json"
sed "s#\"filename\": \"[^\"]*\"#\"filename\": \"${IMAGE}\"#" \
	"$CONFIG" > "$EFFECTIVE_CONFIG"

# Older VM copies may retain Windows line endings; normalize only the helpers
# used by this run so bash does not parse `pipefail\r` as an option name.
for helper in \
	 scripts/performance/fio_stage2_compare.sh \
	 scripts/performance/mysql_stage2_benchmark.sh \
	 scripts/db/mariadb_bootstrap_wrapper.sh \
	 scripts/db/stage48_matrix.sh \
	scripts/db/database_benchmark.sh \
	scripts/db/prepare_hfs_db_bdev.sh; do
	sed -i 's/\r$//' "$ROOT/$helper"
done
HFS_RECREATE=1 bash "$ROOT/scripts/db/prepare_hfs_db_bdev.sh" "$IMAGE" 1G

{
	echo "timestamp=$(date -Is)"
	echo "hostname=$(hostname)"
	echo "kernel=$(uname -srmo)"
	echo "cpu_count=$(nproc)"
	echo "hfs_lib=$LIB"
	echo "hfs_commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "spdk_commit=$(git -C "$ROOT/spdk" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "fio=$(fio --version 2>&1 | head -1 || true)"
	echo "db_bench=$(db_bench --version 2>&1 | head -1 || true)"
	echo "rounds=$ROUNDS"
	echo "duration_seconds=$DURATION"
	echo "fio_runtime_seconds=$FIO_RUNTIME"
	echo "fio_size=$FIO_SIZE"
	echo "db_num=$DB_NUM"
	echo "mysql_queries=$MYSQL_QUERIES"
	echo "mysql_iterations=$MYSQL_ITERATIONS"
	echo "aio_image=$IMAGE"
	echo "nvme_device=$(compgen -G '/dev/nvme*' | tr '\n' ' ' || true)"
	echo "spdk_fio_plugin=$SPDK_PLUGIN"
} > "$OUT/environment.txt"

echo "== shard-59 Aio FIO/native comparison =="
for round in $(seq 1 "$ROUNDS"); do
	echo "== FIO round $round/$ROUNDS =="
	HFS_FIO_CONFIG="$EFFECTIVE_CONFIG" HFS_FIO_BDEV_NAME=Aio0 \
		HFS_FIO_SIZE="$FIO_SIZE" HFS_FIO_RUNTIME_SECONDS="$FIO_RUNTIME" \
		HFS_FIO_RUN_SPDK_REFERENCE=0 \
		bash "$ROOT/scripts/performance/fio_stage2_compare.sh" \
		"$LIB" "$OUT/fio-aio/round-$round" "$OUT/fio-native/round-$round" \
		/bin/true "$EFFECTIVE_CONFIG"
done

echo "== shard-59 RocksDB matrix =="
HFS_STAGE48_ROUNDS="$ROUNDS" HFS_STAGE48_DURATION_SECONDS="$DURATION" \
	HFS_BENCH_NUM="$DB_NUM" HFS_BENCH_BDEV_TYPE=persistent-aio \
	bash "$ROOT/scripts/db/stage48_matrix.sh" \
	"$LIB" /hfs "$EFFECTIVE_CONFIG" "$IMAGE" "$OUT/rocksdb"

echo "== shard-59 MariaDB/MySQL matrix =="
for round in $(seq 1 "$ROUNDS"); do
	echo "== MySQL round $round/$ROUNDS =="
	HFS_MYSQLSLAP_QUERIES="$MYSQL_QUERIES" \
	HFS_MYSQLSLAP_ITERATIONS="$MYSQL_ITERATIONS" \
	HFS_MYSQL_BDEV_TYPE=persistent-aio \
	HFS_MYSQL_BOOTSTRAP_SIGNAL_TIMEOUT=1 \
	bash "$ROOT/scripts/performance/mysql_stage2_benchmark.sh" \
		"$LIB" "$EFFECTIVE_CONFIG" "$IMAGE" "$OUT/mysql/round-$round"
done

NVME_REASON=""
if [[ -f "$NVME_CONFIG" && -f "$SPDK_PLUGIN" && -n "$(compgen -G '/dev/nvme*' || true)" ]]; then
	echo "== shard-59 direct NVMe FIO comparison =="
	for round in $(seq 1 "$ROUNDS"); do
		echo "== direct NVMe FIO round $round/$ROUNDS =="
		HFS_FIO_CONFIG="$NVME_CONFIG" HFS_FIO_BDEV_NAME=Nvme0n1 \
			HFS_FIO_SIZE="$FIO_SIZE" HFS_FIO_RUNTIME_SECONDS="$FIO_RUNTIME" \
			HFS_FIO_RUN_SPDK_REFERENCE=1 \
			bash "$ROOT/scripts/performance/fio_stage2_compare.sh" \
			"$LIB" "$OUT/fio-nvme/round-$round" \
			"$OUT/fio-native-nvme/round-$round" "$SPDK_PLUGIN" "$NVME_CONFIG"
	done
	NVME_STATUS="PASS"
else
	NVME_STATUS="SKIP"
	if [[ ! -f "$NVME_CONFIG" ]]; then NVME_REASON+="nvme_config_missing; "; fi
	if [[ ! -f "$SPDK_PLUGIN" ]]; then NVME_REASON+="spdk_fio_plugin_missing; "; fi
	if [[ -z "$(compgen -G '/dev/nvme*' || true)" ]]; then NVME_REASON+="no_nvme_device; "; fi
	echo "SKIP direct NVMe: $NVME_REASON"
fi

cat > "$OUT/closeout.md" <<EOF
# shard-59 performance closeout

- commit: $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)
- SPDK commit: $(git -C "$ROOT/spdk" rev-parse HEAD 2>/dev/null || echo unknown)
- Aio matrix: PASS ($OUT/fio-aio/round-1..$ROUNDS/summary.tsv)
- RocksDB matrix: PASS ($OUT/rocksdb/summary.tsv)
- MariaDB/MySQL matrix: PASS ($OUT/mysql/round-1..$ROUNDS/results.tsv)
- direct NVMe matrix: $NVME_STATUS
- NVMe note: ${NVME_REASON:-direct NVMe FIO completed}

Stats-off is the primary performance measure. HFS and native FIO use the same
VM and the native path is outside LD_PRELOAD; missing NVMe/SPDK FIO prerequisites
are reported as SKIP rather than substituted with Aio or Malloc numbers.
EOF

echo "SHARD59 PERFORMANCE MATRIX OK"
echo "report=$OUT/closeout.md"
