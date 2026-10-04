#!/bin/bash
# Reproducible MySQL 8.0.46 build for UUSE Phase 7 (run inside the VM).
# Source of truth: doc/shard/uuse_mysql_innodb_shard_plan.md (Shard-99).
#
# Required packages (Ubuntu 24.04): git cmake make ninja-build gcc g++ bison
# pkg-config libssl-dev libncurses-dev libaio-dev libtirpc-dev
set -euo pipefail

SRC="${SRC:-/home/uuse/mysql-fork}"
BUILD="${BUILD:-/home/uuse/mysql-build}"
BOOST_DIR="${BOOST_DIR:-/home/uuse/boost}"
JOBS="${JOBS:-2}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
UUSE_CORE_ROOT="${UUSE_CORE_ROOT:-/root/code/build-verify}"
UUSE_CORE_INCLUDE="${UUSE_CORE_INCLUDE:-/root/code/include}"
PATCH_ROOT="${PATCH_ROOT:-/root/code/mysql/patches}"

if [ ! -d "$SRC/.git" ]; then
  echo "missing MySQL fork at $SRC" >&2
  exit 1
fi

# Apply the ordered patch stack only when the VM fork lacks each layer.
if [ ! -f "$SRC/storage/innobase/os/uuse_vfs.cc" ]; then
  git -C "$SRC" apply --check "$PATCH_ROOT/uuse_seam_shard101b.patch"
  git -C "$SRC" apply "$PATCH_ROOT/uuse_seam_shard101b.patch"
fi
if ! grep -q 'uuse_vfs: native AIO is unsupported' \
     "$SRC/storage/innobase/handler/ha_innodb.cc"; then
  git -C "$SRC" apply --check "$PATCH_ROOT/uuse_shard104_guards.patch"
  git -C "$SRC" apply "$PATCH_ROOT/uuse_shard104_guards.patch"
fi
if ! grep -q 'uuse_vfs: device_io read_ops=' \
     "$SRC/storage/innobase/os/uuse_vfs.cc"; then
  git -C "$SRC" apply --check "$PATCH_ROOT/uuse_shard105_metrics.patch"
  git -C "$SRC" apply "$PATCH_ROOT/uuse_shard105_metrics.patch"
fi
if ! grep -q 'Async fast-path counters' \
     "$SRC/storage/innobase/os/uuse_vfs.cc"; then
  git -C "$SRC" apply --check "$PATCH_ROOT/uuse_seam_shard106_async.patch"
  git -C "$SRC" apply "$PATCH_ROOT/uuse_seam_shard106_async.patch"
fi
if ! grep -q "Lazily create this thread's own client" \
     "$SRC/storage/innobase/os/uuse_vfs.cc"; then
  git -C "$SRC" apply --check "$PATCH_ROOT/uuse_seam_shard108_async_lazy.patch"
  git -C "$SRC" apply "$PATCH_ROOT/uuse_seam_shard108_async_lazy.patch"
fi
if ! grep -q 'AioBatchRequest' \
     "$SRC/storage/innobase/os/uuse_vfs.h"; then
  git -C "$SRC" apply --check "$PATCH_ROOT/uuse_seam_shard109_aio_batch.patch"
  git -C "$SRC" apply "$PATCH_ROOT/uuse_seam_shard109_aio_batch.patch"
fi
if ! grep -q 'uuse_vfs::allocDmaBuffer' \
     "$SRC/storage/innobase/buf/buf0buf.cc"; then
  git -C "$SRC" apply --check "$PATCH_ROOT/uuse_seam_shard110_zero_copy.patch"
  git -C "$SRC" apply "$PATCH_ROOT/uuse_seam_shard110_zero_copy.patch"
fi
if ! grep -q 'Growth writes (redo appends)' \
     "$SRC/storage/innobase/os/uuse_vfs.cc"; then
  git -C "$SRC" apply --check "$PATCH_ROOT/uuse_seam_shard112_async_append.patch"
  git -C "$SRC" apply "$PATCH_ROOT/uuse_seam_shard112_async_append.patch"
fi

cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_INSTALL_PREFIX="$BUILD/install" \
  -DINSTALL_LAYOUT=STANDALONE \
  -DWITH_BOOST="$BOOST_DIR" -DDOWNLOAD_BOOST=1 \
  -DWITH_MYSQLX=OFF \
  -DWITH_UNIT_TESTS=OFF \
  -DWITH_SYSTEMD=OFF \
  -DUUSE_CORE_ROOT="$UUSE_CORE_ROOT" \
  -DUUSE_CORE_INCLUDE="$UUSE_CORE_INCLUDE"

ninja -C "$BUILD" -j "$JOBS" mysqld mysql mysqladmin mysqlslap mysqldump mysqlcheck

ls -l "$BUILD/runtime_output_directory/mysqld" 2>/dev/null || \
  ls -l "$BUILD/bin/mysqld" 2>/dev/null || true
echo "mysql build finished"
