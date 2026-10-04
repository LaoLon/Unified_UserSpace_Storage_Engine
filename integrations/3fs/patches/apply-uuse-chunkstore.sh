#!/bin/bash
# Install the UUSE chunk-store core (M2.1) into a 3FS checkout and enable the
# smoke target (CMake block appended once).
set -eu
ROOT="${1:-/build/3fs}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../uuse"

cp "$SRC/UuseChunkStore.h" "$ROOT/src/storage/store/UuseChunkStore.h"
cp "$SRC/UuseChunkStore.cc" "$ROOT/src/storage/store/UuseChunkStore.cc"
cp "$SRC/UuseChunkStoreSmoke.cc" "$ROOT/src/storage/store/UuseChunkStoreSmoke.cc"

if ! grep -q 'UUSE chunk store (M2.1 lab target)' "$ROOT/CMakeLists.txt"; then
  cat "$SRC/CMake.uuse-chunkstore.cmake" >> "$ROOT/CMakeLists.txt"
  echo "CMake block appended"
fi
echo "uuse chunkstore installed into $ROOT"
