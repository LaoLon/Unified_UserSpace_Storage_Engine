#!/bin/bash
# Reproducible MySQL 8.0.46 build for HFS Phase 7 (run inside the VM).
# Source of truth: doc/shard/hfs2.0_mysql_innodb_shard_plan.md (Shard-99).
#
# Required packages (Ubuntu 24.04): git cmake make ninja-build gcc g++ bison
# pkg-config libssl-dev libncurses-dev libaio-dev libtirpc-dev
set -euo pipefail

SRC="${SRC:-/mnt/hfs-data/mysql-fork}"
BUILD="${BUILD:-/mnt/hfs-data/mysql-build}"
BOOST_DIR="${BOOST_DIR:-/mnt/hfs-data/boost}"
JOBS="${JOBS:-8}"

if [ ! -d "$SRC/.git" ]; then
  echo "missing MySQL fork at $SRC" >&2
  exit 1
fi

cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$BUILD/install" \
  -DINSTALL_LAYOUT=STANDALONE \
  -DWITH_BOOST="$BOOST_DIR" -DDOWNLOAD_BOOST=1 \
  -DWITH_MYSQLX=OFF \
  -DWITH_UNIT_TESTS=OFF \
  -DWITH_SYSTEMD=OFF

ninja -C "$BUILD" -j "$JOBS" mysqld

ls -l "$BUILD/runtime_output_directory/mysqld" 2>/dev/null || \
  ls -l "$BUILD/bin/mysqld" 2>/dev/null || true
echo "mysql build finished"
