#!/bin/bash
# Reproducible MySQL 8.0.46 build for HFS Phase 7 (run inside the VM).
# Source of truth: doc/shard/hfs2.0_mysql_innodb_shard_plan.md (Shard-99).
#
# Required packages (Ubuntu 24.04): git cmake make ninja-build gcc g++ bison
# pkg-config libssl-dev libncurses-dev libaio-dev libtirpc-dev
set -euo pipefail

SRC="${SRC:-/home/hfs/mysql-fork}"
BUILD="${BUILD:-/home/hfs/mysql-build}"
BOOST_DIR="${BOOST_DIR:-/home/hfs/boost}"
JOBS="${JOBS:-2}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
HFS_CORE_ROOT="${HFS_CORE_ROOT:-/root/code/build-verify}"
HFS_CORE_INCLUDE="${HFS_CORE_INCLUDE:-/root/code/include}"
PATCH_ROOT="${PATCH_ROOT:-/root/code/mysql/patches}"

if [ ! -d "$SRC/.git" ]; then
  echo "missing MySQL fork at $SRC" >&2
  exit 1
fi

# Apply the ordered patch stack only when the VM fork lacks each layer.
if [ ! -f "$SRC/storage/innobase/os/hfs_vfs.cc" ]; then
  git -C "$SRC" apply --check "$PATCH_ROOT/hfs_seam_shard101b.patch"
  git -C "$SRC" apply "$PATCH_ROOT/hfs_seam_shard101b.patch"
fi
if ! grep -q 'hfs_vfs: native AIO is unsupported' \
     "$SRC/storage/innobase/handler/ha_innodb.cc"; then
  git -C "$SRC" apply --check "$PATCH_ROOT/hfs_shard104_guards.patch"
  git -C "$SRC" apply "$PATCH_ROOT/hfs_shard104_guards.patch"
fi
if ! grep -q 'hfs_vfs: device_io read_ops=' \
     "$SRC/storage/innobase/os/hfs_vfs.cc"; then
  git -C "$SRC" apply --check "$PATCH_ROOT/hfs_shard105_metrics.patch"
  git -C "$SRC" apply "$PATCH_ROOT/hfs_shard105_metrics.patch"
fi

cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_INSTALL_PREFIX="$BUILD/install" \
  -DINSTALL_LAYOUT=STANDALONE \
  -DWITH_BOOST="$BOOST_DIR" -DDOWNLOAD_BOOST=1 \
  -DWITH_MYSQLX=OFF \
  -DWITH_UNIT_TESTS=OFF \
  -DWITH_SYSTEMD=OFF \
  -DHFS_CORE_ROOT="$HFS_CORE_ROOT" \
  -DHFS_CORE_INCLUDE="$HFS_CORE_INCLUDE"

ninja -C "$BUILD" -j "$JOBS" mysqld mysql mysqladmin mysqlslap mysqldump mysqlcheck

ls -l "$BUILD/runtime_output_directory/mysqld" 2>/dev/null || \
  ls -l "$BUILD/bin/mysqld" 2>/dev/null || true
echo "mysql build finished"
