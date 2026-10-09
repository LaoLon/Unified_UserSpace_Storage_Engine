#!/bin/bash
# Reproducible TiKV build for UUSE Phase 8 (run inside the VM).
# Source of truth: doc/shard/uuse_tidb_shard_plan.md (Shard-113) and
# doc/design/uuse.md section 21.
#
# Gate 0 freezes the exact TiKV tag/commit; this script refuses to run without
# TIKV_REF so the base cannot drift silently.
#
# Required packages (Ubuntu 26.04): git cmake make ninja-build gcc g++
#   libssl-dev pkg-config, plus the frozen Rust toolchain (rustc/cargo).
set -euo pipefail

TIKV_REF="${TIKV_REF:?set TIKV_REF to the frozen TiKV tag/commit (Gate 0)}"
SRC="${SRC:-/home/uuse/tikv-fork}"
BUILD="${BUILD:-/home/uuse/tikv-build}"
JOBS="${JOBS:-2}"
PATCH_ROOT="${PATCH_ROOT:-/root/code/tikv/patches}"
TIKV_REPO="${TIKV_REPO:-https://github.com/tikv/tikv.git}"

if ! command -v cargo >/dev/null || ! command -v rustc >/dev/null; then
  echo "missing rustc/cargo; install the frozen Rust toolchain first" >&2
  exit 1
fi

if [ ! -d "$SRC/.git" ]; then
  git clone "$TIKV_REPO" "$SRC"
fi
git -C "$SRC" fetch --tags origin
git -C "$SRC" checkout --detach "$TIKV_REF"
git -C "$SRC" submodule update --init --recursive

# Apply the ordered UUSE patch stack only when each layer is missing.
if [ -d "$PATCH_ROOT" ]; then
  for patch in "$PATCH_ROOT"/*.patch; do
    [ -e "$patch" ] || continue
    if git -C "$SRC" apply --check "$patch" 2>/dev/null; then
      git -C "$SRC" apply "$patch"
      echo "applied: $patch"
    elif git -C "$SRC" apply --reverse --check "$patch" 2>/dev/null; then
      echo "already applied: $patch"
    else
      echo "patch does not apply: $patch" >&2
      exit 1
    fi
  done
fi

mkdir -p "$BUILD"
cd "$SRC"
CARGO_TARGET_DIR="$BUILD" cargo build --release --bin tikv-server --jobs "$JOBS"
echo "tikv-server: $BUILD/release/tikv-server"
