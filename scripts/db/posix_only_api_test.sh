#!/usr/bin/env bash
# Verify that the public HFS surface contains only the POSIX/preload entry points.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="${1:-$ROOT/build/lib/libhfs.so}"

if [[ ! -f "$LIB" ]]; then
	echo "usage: $0 [libhfs.so]" >&2
	exit 2
fi

# Retired async and registered-DMA symbols must not be visible from the shared library.
if nm -D "$LIB" 2>/dev/null | grep -Eq 'hfs_lib_async_|hfsAsync|hfs(Register|Unregister|Dma)'; then
	echo "ERROR: retired async/DMA symbols are exported" >&2
	exit 1
fi

# Deleted implementation and dedicated test entry points must stay absent.
for path in \
	"$ROOT/src/hfs_async.c" \
	"$ROOT/src/hfs_async.h" \
	"$ROOT/scripts/db/async_io_test.sh" \
	"$ROOT/scripts/db/direct_dma_async_matrix.sh" \
	"$ROOT/scripts/db/multireactor_scaling_test.sh" \
	"$ROOT/scripts/db/registered_direct_window_test.sh" \
	"$ROOT/scripts/db/registered_dma_test.sh"; do
	if [[ -e "$path" ]]; then
		echo "ERROR: retired path remains: $path" >&2
		exit 1
	fi
done

echo "POSIX-ONLY API OK"
