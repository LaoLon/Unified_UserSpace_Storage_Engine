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
