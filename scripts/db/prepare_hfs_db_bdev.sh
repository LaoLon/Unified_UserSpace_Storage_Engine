#!/usr/bin/env bash
# Prepare a persistent file-backed bdev image for database tests.
set -euo pipefail

IMAGE="${1:-/var/tmp/hfs-db.img}"
SIZE="${2:-8G}"

if [[ -e "$IMAGE" ]]; then
	if [[ "${HFS_RECREATE:-0}" != "1" ]]; then
		echo "image already exists: $IMAGE (set HFS_RECREATE=1 to replace it)" >&2
		exit 1
	fi
	rm -f -- "$IMAGE"
fi

mkdir -p "$(dirname "$IMAGE")"
truncate -s "$SIZE" "$IMAGE"
chmod 0600 "$IMAGE"
echo "prepared image: $IMAGE"
stat --format='size=%s bytes mode=%a' "$IMAGE"
