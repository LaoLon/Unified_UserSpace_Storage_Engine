#!/bin/bash
# Build the MinIO frontend on a lab VM (Shard-9.x curated entry):
#   1) UUSE C ABI shim + uusectl (cmake)
#   2) install shim/core libs into /usr/local/lib/uuse (ldconfig)
#   3) apply the MinIO seam patch to the fork (idempotent)
#   4) build minio-bin (cgo, rpath /usr/local/lib/uuse)
#   5) build the helper tools and assemble the deploy dir DIST
#
# Env: UUSE_ROOT (default /root/code), BUILD_DIR (default $UUSE_ROOT/build-minio),
#      FORK_DIR (default /build-nvme/minio-fork), DIST (default $FORK_DIR),
#      PATCH=1 to apply the seam, JOBS.
set -u
UUSE_ROOT=${UUSE_ROOT:-/root/code}
BUILD_DIR=${BUILD_DIR:-$UUSE_ROOT/build-minio}
FORK_DIR=${FORK_DIR:-/build-nvme/minio-fork}
DIST=${DIST:-$FORK_DIR}
PATCH=${PATCH:-1}
JOBS=${JOBS:-4}
TOOLS_DIR=$UUSE_ROOT/minio/tools
GO_PROFILE=${GO_PROFILE:-/etc/profile.d/go-minio.sh}

fail() { echo "$1"; exit 1; }

echo "== 1/5 UUSE shim (cmake in $BUILD_DIR) =="
cmake -S "$UUSE_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
  > /tmp/minio_shim_cfg.log 2>&1 || { tail -30 /tmp/minio_shim_cfg.log; fail CFG-FAIL; }
cmake --build "$BUILD_DIR" -j "$JOBS" \
  --target uuse_minio_shim uuse_minio_shim_smoke uuse_minio_shim_lifecycle uusectl \
  > /tmp/minio_shim_build.log 2>&1 || { tail -40 /tmp/minio_shim_build.log; fail SHIM-BUILD-FAIL; }

echo "== 2/5 install libs to /usr/local/lib/uuse =="
mkdir -p /usr/local/lib/uuse
cp -f "$BUILD_DIR"/libuuse_minio_shim.so /usr/local/lib/uuse/ || fail "missing shim lib"
cp -f "$BUILD_DIR"/libuuse_core.so.2 "$BUILD_DIR"/libuuse_core.so.2.0.0 /usr/local/lib/uuse/ 2>/dev/null || true
ln -sfn libuuse_core.so.2 /usr/local/lib/uuse/libuuse_core.so
echo /usr/local/lib/uuse > /etc/ld.so.conf.d/uuse.conf
ldconfig 2>/dev/null || true

echo "== 3/5 MinIO fork seam ($FORK_DIR) =="
[ -d "$FORK_DIR" ] || fail "no fork dir $FORK_DIR"
if [ "$PATCH" = 1 ]; then
  python3 "$UUSE_ROOT/minio/patches/apply-uuse-seam.py" "$FORK_DIR" || fail PATCH-FAIL
  (cd "$FORK_DIR" && gofmt -w cmd/xl-storage.go cmd/os-instrumented.go cmd/os_unix.go internal/uuseos/*.go) 2>/dev/null || true
else
  echo "PATCH=0: keeping the checkout as-is"
fi

echo "== 4/5 minio-bin =="
[ -r "$GO_PROFILE" ] && source "$GO_PROFILE"
mkdir -p "$DIST"
cd "$FORK_DIR" || fail "cd $FORK_DIR"
CGO_ENABLED=1 go build -p 2 -ldflags '-r /usr/local/lib/uuse' -o minio-bin . \
  > /tmp/minio_bin_build.log 2>&1 || { tail -30 /tmp/minio_bin_build.log; fail MINIO-BUILD-FAIL; }
ls -la "$FORK_DIR/minio-bin"

echo "== 5/5 helper tools =="
# uuseos clients must build inside the fork (import internal/uuseos).
for t in wipe_tool tree_probe poke_tool big_probe; do
  mkdir -p "$FORK_DIR/e2e-$t"
  cp -f "$TOOLS_DIR/$t.go" "$FORK_DIR/e2e-$t/main.go"
  (cd "$FORK_DIR" && CGO_ENABLED=1 go build -o "$FORK_DIR/$t" "./e2e-$t") \
    > "/tmp/minio_tool_$t.log" 2>&1 || { tail -10 "/tmp/minio_tool_$t.log"; fail "TOOL-BUILD-FAIL $t"; }
done
# minio-go clients: matrix / crash / quad / dist.
for t in matrix_client crash_tool quad_client dist_client; do
  mkdir -p "$FORK_DIR/e2e-$t"
  cp -f "$TOOLS_DIR/$t.go" "$FORK_DIR/e2e-$t/main.go"
  (cd "$FORK_DIR" && CGO_ENABLED=1 go build -o "$FORK_DIR/$t" "./e2e-$t") \
    > "/tmp/minio_tool_$t.log" 2>&1 || { tail -10 "/tmp/minio_tool_$t.log"; fail "TOOL-BUILD-FAIL $t"; }
done

echo "== assemble DIST=$DIST =="
cp -f "$BUILD_DIR"/libuuse_minio_shim.so "$DIST/"
cp -f "$BUILD_DIR"/libuuse_core.so.2 "$BUILD_DIR"/libuuse_core.so.2.0.0 "$DIST/" 2>/dev/null || true
cp -f "$BUILD_DIR"/uusectl "$DIST/"
cp -f "$UUSE_ROOT/config/uuse_nvme.conf" "$DIST/uuse_nvme.conf"
for t in wipe_tool tree_probe poke_tool big_probe matrix_client crash_tool quad_client dist_client; do
  # keep the tool names used by the run scripts
  case "$t" in
    wipe_tool)   cp -f "$FORK_DIR/$t" "$DIST/minio-wipe" ;;
    tree_probe)  cp -f "$FORK_DIR/$t" "$DIST/tree-probe" ;;
    poke_tool)   cp -f "$FORK_DIR/$t" "$DIST/s96-poke" ;;
    big_probe)   cp -f "$FORK_DIR/$t" "$DIST/big-probe" ;;
    matrix_client) cp -f "$FORK_DIR/$t" "$DIST/matrix-client" ;;
    crash_tool)  cp -f "$FORK_DIR/$t" "$DIST/crash-tool" ;;
    quad_client) cp -f "$FORK_DIR/$t" "$DIST/quad-client" ;;
    dist_client) cp -f "$FORK_DIR/$t" "$DIST/dist-client" ;;
  esac
done
ls -la "$DIST" | grep -E 'minio-bin|minio-wipe|matrix-client|crash-tool|uusectl|libuuse'
echo MINIO-BUILD-DONE
