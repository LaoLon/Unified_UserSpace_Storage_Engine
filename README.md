# UUSE

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)](https://www.kernel.org/)

**Unified User-space Storage Engine** — a coroutine-first storage engine for NVMe that
runs inside the application process.

UUSE takes exclusive ownership of one or more NVMe devices through SPDK and provides its
own file namespace, extent allocator, write-ahead journal and checkpoints. I/O never
crosses the kernel block layer, a file system, or a page cache. Applications either link
the C++20 engine API (`uuse::Engine`, `uuse::Client`, `uuse::File`) or attach through the
adapters in this repository for FIO, RocksDB, MySQL, TiKV and MinIO.

## Status

UUSE is early-stage research software. Expect breakage.

- Single node, single process, exclusive device ownership.
- Validated on lab virtual machines only; bare-metal results are still pending.
- The C++ API and the on-disk format are not stable.
- Not production software. Do not store data you care about on it.

## Why

- **Remove the kernel from the path.** No syscalls on the I/O path, no block-layer
  queues, no page cache, no file-system journal: the engine submits NVMe commands
  directly and owns the scheduling, ordering and durability semantics.
- **Co-locate with the data engine.** A database, an object store or a file service runs
  in the same process as its storage, so cache, metadata and I/O preparation can share
  the same threads and the same buffers.
- **Measure what you run.** Every operation, barrier and checkpoint can be traced from
  the application call down to the device submission.

## Features

- C++20 coroutine API with single-threaded `Client`/`File` handles and asynchronous
  submission/completion.
- Direct SPDK bdev access over `uio` or `vfio`; device sets with capacity-weighted
  placement and per-device submit/complete accounting.
- File namespace: files, directories, `rename`/`link`/`remove`, preallocation,
  `sync`/`fdatasync`, and crash replay from journal plus checkpoints.
- Shard-coroutine actors with a per-inode dependency scheduler, so independent work runs
  in parallel and conflicting work stays ordered.
- Direct-buffer and bounded bounce-buffer data paths over a shared DMA arena.
- One `.conf` file describes devices, placement, reactors, limits and observability.

## Architecture

```text
FIO / RocksDB / MySQL / TiKV / MinIO / 3FS
                      │
          adapters and C ABI shims
                      │
               uuse::Client
                      │
        bounded MPSC shard ring
                      │
       shard coroutine actor (×N)
                      │
   per-inode dependency scheduler
                      │
       raw-bdev extent planner
                      │
    SPDK channel / NVMe device set
```

The metadata control plane (namespace, journal, checkpoints) is separated from the data
reactors. A process lock prevents two processes from mounting the same device set.

## Getting started

### Requirements

| Item | Requirement |
|---|---|
| OS | Linux, x86-64 |
| Toolchain | CMake ≥ 3.16, GCC ≥ 12 or Clang ≥ 15, C++20 |
| Memory | 1024 × 2 MiB hugepages recommended |
| Device | A dedicated NVMe you can erase; all data on it will be lost |
| Dependencies | Installed by `spdk/scripts/pkgdep.sh` |

### Build

```bash
git clone --recurse-submodules https://github.com/LaoLon/Unified_UserSpace_Storage_Engine.git
cd Unified_UserSpace_Storage_Engine

sudo spdk/scripts/pkgdep.sh
(cd spdk && ./configure && make -j"$(nproc)")

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

This produces `build/libuuse_core.so`, `build/uusectl`, the test binaries and the
optional adapters that are enabled by their CMake variables (`FIO_SOURCE_DIR`,
`ROCKSDB_ROOT`). The `Makefile` offers the same core targets for environments that
prefer `make`.

### Prepare a device

```bash
sudo sysctl -w vm.nr_hugepages=1024
sudo modprobe uio_pci_generic
echo uio_pci_generic | sudo tee /sys/bus/pci/devices/0000:13:00.0/driver_override
echo 0000:13:00.0 | sudo tee /sys/bus/pci/drivers/uio_pci_generic/bind
```

`0000:13:00.0` is the controller used by `config/uuse_nvme.conf`. Replace it in both the
binding commands and the `.conf` for your machine; list BDFs with `lspci -D` rather than
inferring them from `nvmeXn1` numbering. Binding removes the device from the kernel, so
its file systems disappear until you unbind.

### Format and inspect

```bash
build/uusectl --config=config/uuse_nvme.conf validate
build/uusectl --config=config/uuse_nvme.conf format --yes   # destructive
build/uusectl --config=config/uuse_nvme.conf info
build/uusectl --config=config/uuse_nvme.conf check
```

`format` writes the superblock, journal and initial checkpoint; `info` mounts read-only
metadata and prints the layout; `check` scans superblocks, checkpoints and the journal
and reports `corruption=none` when the volume is clean.

### Run a workload

Build the FIO external engine by pointing CMake at an fio source tree, then run it:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFIO_SOURCE_DIR=/path/to/fio-3.42
cmake --build build -j"$(nproc)"

/path/to/fio-3.42/fio --name=uuse-4k \
  --ioengine=external:build/fio-uuse.so \
  --uuse_config=config/uuse_nvme.conf --uuse_async=1 --uuse_client_depth=32 \
  --filename=/demo --rw=randrw --rwmixread=50 --bs=4k --size=256m \
  --direct=1 --iodepth=32 --numjobs=1 --time_based=1 --runtime=10
```

### Tests

```bash
ctest --test-dir build --output-on-failure
```

The suite mixes pure in-memory tests with device tests that need a bound NVMe; the device
tests take a CTest resource lock so they never run concurrently, and they are skipped when
SPDK is unavailable.

## Frontends

| Frontend | Integration | Directory | State |
|---|---|---|---|
| FIO 3.42 | external ioengine (`fio_uuse.cpp`) | [`fio/`](fio/) | working |
| RocksDB 8.9.1 | `FileSystem` adapter (`uuse_filesystem.cpp`) | [`rocksdb/`](rocksdb/) | working |
| MySQL 8.0.46 | InnoDB `os_file` seam, patch stack on a fork | [`mysql/`](mysql/) | rebuild required |
| TiKV 8.5.8 | raft-engine `FileSystem` + RocksDB `Env` seam | [`tikv/`](tikv/) | working |
| MinIO | disk-layer seam plus C ABI shim | [`minio/`](minio/) | working |
| 3FS | chunk-store backend (`UuseChunkStore`) | [`integrations/3fs/`](integrations/3fs/) | experimental |

Each directory holds its own README, patch stack and VM scripts. The MySQL fork must be
rebuilt against the current core before use; the TiKV and MinIO frontends link the
C ABI shims built here.

## Performance

Against the native path on the same device, UUSE is within ~5% of raw SPDK for 4K
`randrw`, and at parity for 3FS chunk writes and MinIO PUT. Application-level results
(MySQL, RocksDB, TiKV) sit between 88% and 110% of the native path: once a database brings
its own cache and durability barrier, the device stops being the bottleneck and the
engine's advantage mostly disappears. Cold random reads are the exception, because the
native synchronous path degrades at high thread counts.

All measurements so far come from 4–6 vCPU virtual machines with one virtual NVMe; no
bare-metal run has been done yet. See [performance-compare.md](performance-compare.md)
for the full matrix, per-workload attribution and the VM-only caveat.

## Repository layout

```text
include/uuse/     public engine headers
src/              core implementation (common, config, core, metadata, runtime, storage)
tests/            CTest suites, including real-device tests
tools/            uusectl, metadata bench, trace dump, lock probe
fio/              FIO external ioengine
rocksdb/          RocksDB FileSystem adapter, bench and inspection tools
mysql/            InnoDB seam patch stack and build scripts
tikv/             TiKV frontend, patches and VM scripts
minio/            MinIO frontend, shim, Go disk layer and patches
integrations/3fs/ 3FS chunk-store backend (experimental)
config/           example .conf files
doc/              design, architecture, reviews, performance baselines
spdk/             SPDK submodule (built in-tree)
```

## Documentation

- [`doc/design/uuse.md`](doc/design/uuse.md) — the source of truth for architecture,
  interfaces, boundaries and acceptance criteria.
- [`doc/Architecture/uuse_architecture.md`](doc/Architecture/uuse_architecture.md) —
  component and data-flow diagrams.
- [`performance-compare.md`](performance-compare.md) plus
  [`doc/performance/`](doc/performance/) — measurement methodology, results and
  attributions.
- [`doc/review/`](doc/review/) — design audits and post-mortems.
- Per-frontend READMEs in `fio/`, `rocksdb/`, `mysql/`, `tikv/`, `minio/`,
  `integrations/3fs/`.

## Limitations and non-goals

- **No POSIX layer.** There is no FUSE mount, no `LD_PRELOAD` shim, no file descriptor
  or `mmap` emulation, and no SPDK Blobstore image. Frontends use their own seams.
- **One process at a time.** The engine owns its device set exclusively; concurrent
  mounts from several processes are rejected by a process lock.
- **No replication.** UUSE is a local engine. 3FS and MinIO provide replication and
  distribution above it.
- **No host page cache.** Reads always reach the device; caching is the frontend's job.
- **Linux and x86-64 only**, because it depends on SPDK and `uio`/`vfio`.
- **Unstable interfaces.** Both the API and the on-disk format may change between
  releases, with no migration path yet.

## Contributing

Issues and pull requests are welcome. Before submitting:

- run `ctest --test-dir build --output-on-failure` and keep it green;
- update [`doc/design/uuse.md`](doc/design/uuse.md) before changing behaviour, since it
  is the reference the reviews are checked against;
- keep commits focused and use conventional-commit subjects (`feat:`, `fix:`, `docs:`,
  `perf:`, `chore:`).

## License

MIT — see [LICENSE](LICENSE). The `spdk/` submodule is distributed under its own
BSD-3-Clause license. The `mysql/`, `tikv/`, `minio/` and `integrations/3fs/` directories
contain patch stacks and adapters only; they do not include upstream sources.
