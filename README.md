# HFS_BASE_SPDK

An experimental filesystem built on SPDK Blobstore. The initial implementation is migrated from
the `hfs` example and uses a malloc bdev configured through JSON.

SPDK official project: <https://github.com/spdk/spdk>

## Checkout

```bash
git clone --recurse-submodules <repository-url>
cd HFS_BASE_SPDK
```

For an existing checkout:

```bash
git submodule update --init --recursive
```

## Build SPDK

`spdk/` is a Git submodule. Build it separately by following the official SPDK setup flow. From the
project root, the usual Linux sequence is:

```bash
git submodule update --init --recursive
cd spdk
sudo ./scripts/pkgdep.sh
./configure
make -j"$(nproc)"
cd ..
```

Keep SPDK's configuration and build in the submodule. Do not run the project application build before
SPDK has been configured and built.

## Build The Filesystem

After SPDK is built, build this project separately:

```bash
make app
```

`make` is an alias for `make app`. The project never rebuilds SPDK implicitly.

Outputs are isolated from the source tree:

```text
build/bin/spdk-hfs
build/obj/hfs.o
```

Run with the default configuration:

```bash
make run
```

Override the configuration file at build time when needed:

```bash
make CONFIG=/absolute/path/to/hfs.json
```

Generate a clangd compilation database with Bear after both SPDK configuration and build:

```bash
make compile-commands
```

## LD_PRELOAD Usage

The interceptor library (`libhfs.so`) exposes an HFS namespace under a mount prefix
(default `/hfs/`). Programs that operate on paths under the prefix are routed to the
HFS filesystem; everything else passes through to the kernel.

```bash
# Build the library (cmake-based build also produces build-cmake/libhfs.so)
make lib

# Route a single process's file operations to HFS
LD_PRELOAD=$PWD/build/lib/libhfs.so python3 - <<'EOF'
import os
os.mkdir('/hfs/d')
fd = os.open('/hfs/d/f', os.O_CREAT | os.O_RDWR, 0o644)
os.write(fd, b'hello')
os.close(fd)
print(os.listdir('/hfs/d'))
EOF

# Custom mount prefix
HFS_MOUNT_PREFIX=/data LD_PRELOAD=$PWD/build/lib/libhfs.so <program>
```

Known limitations:

- External shell commands (`mkdir`, `cat`, `dd`, ...) run in fork+exec children that
  inherit `_HFS_INIT_DONE` and fall through to the kernel. Use single-process
  workloads (python, C programs) or run everything in one shell built-in.
- The default malloc bdev is process-private: data does not survive across processes.

Run the interceptor smoke test (assertions run in a single python process):

```bash
bash scripts/preload_smoke.sh build/lib/libhfs.so
```
