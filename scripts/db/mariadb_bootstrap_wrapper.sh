#!/usr/bin/env bash
# Run MariaDB bootstrap in one HFS process while the caller stays unpreloaded.
set -euo pipefail

: "${HFS_LIB:?HFS_LIB is required}"
: "${HFS_CONFIG_PATH:?HFS_CONFIG_PATH is required}"
: "${HFS_VIRTUAL_DATADIR:?HFS_VIRTUAL_DATADIR is required}"

MYSQLD_BIN="${HFS_MYSQLD_BIN:-/usr/sbin/mariadbd}"
BOOTSTRAP_ARGS=()
for arg in "$@"; do
	case "$arg" in
		--datadir=*)
			BOOTSTRAP_ARGS+=("--datadir=$HFS_VIRTUAL_DATADIR")
			;;
		*)
			BOOTSTRAP_ARGS+=("$arg")
			;;
	esac
done

exec env LD_PRELOAD="$HFS_LIB" HFS_CONFIG_PATH="$HFS_CONFIG_PATH" \
	HFS_BDEV_NAME="${HFS_BDEV_NAME:-Aio0}" \
	HFS_LOCK_PATH="${HFS_LOCK_PATH:?HFS_LOCK_PATH is required}" \
	"$MYSQLD_BIN" "${BOOTSTRAP_ARGS[@]}"
