# UUSE_BASE_SPDK

> 状态（2026-09-16）：UUSE 正在按 `doc/design/uuse.md` 做 clean-break 重构（C++20
> coroutine-first、raw SPDK bdev、`.conf` 配置）。下方 README 描述的是 UUSE 1.x
> 冻结线（Blobstore/preload）的构建与使用方式，仅用于旧线复现，不代表 UUSE 2.0。

An experimental filesystem built on SPDK Blobstore. The initial implementation is migrated from
the `uuse` example and uses a malloc bdev configured through JSON.

SPDK official project: <https://github.com/spdk/spdk>

## Checkout

```bash
git clone --recurse-submodules <repository-url>
cd UUSE_BASE_SPDK
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
build/bin/spdk-uuse
build/obj/uuse.o
```

Run with the default configuration:

```bash
make run
```

Override the configuration file at build time when needed:

```bash
make CONFIG=/absolute/path/to/uuse.json
```

Generate a clangd compilation database with Bear after both SPDK configuration and build:

```bash
make compile-commands
```

## LD_PRELOAD Usage

The interceptor library (`libuuse.so`) exposes an UUSE namespace under a mount prefix
(default `/uuse/`). Programs that operate on paths under the prefix are routed to the
UUSE filesystem; everything else passes through to the kernel.

```bash
# Build the library (cmake-based build also produces build-cmake/libuuse.so)
make lib

# Format a fresh in-memory backend and route one process's file operations to UUSE
UUSE_FORMAT=1 LD_PRELOAD=$PWD/build/lib/libuuse.so python3 - <<'EOF'
import os
os.mkdir('/uuse/d')
fd = os.open('/uuse/d/f', os.O_CREAT | os.O_RDWR, 0o644)
os.write(fd, b'hello')
os.close(fd)
print(os.listdir('/uuse/d'))
EOF

# Custom mount prefix
UUSE_MOUNT_PREFIX=/data UUSE_FORMAT=1 LD_PRELOAD=$PWD/build/lib/libuuse.so <program>

# Reuse a persistent SPDK bdev (format once, then omit UUSE_FORMAT)
UUSE_CONFIG_PATH=$PWD/config/uuse_db_aio.json UUSE_BDEV_NAME=Aio0 \
	UUSE_FORMAT=1 LD_PRELOAD=$PWD/build/lib/libuuse.so <formatting-program>
UUSE_CONFIG_PATH=$PWD/config/uuse_db_aio.json UUSE_BDEV_NAME=Aio0 \
	LD_PRELOAD=$PWD/build/lib/libuuse.so <program>
```

## Database Validation

The current stage validates a single database process on a persistent SPDK AIO/NVMe backend.
Clients connect to the database through its normal socket or TCP interface; they do not mount UUSE
directly.

| Backend | Status | Validation |
|---|---|---|
| MySQL/MariaDB-compatible server | Supported for the validated POSIX path | `scripts/db/mysql_smoke.sh` covers initialization, SQL CRUD/DDL, transactions, restart, SIGKILL recovery, and concurrent clients |
| RocksDB | Supported for the validated POSIX path | `scripts/db/rocksdb_smoke.sh` covers cross-process reopen, normal/direct I/O, WAL recovery, concurrent read/write, and `ldb checkconsistency` |

Use `scripts/db/database_benchmark.sh` to record the effective backend configuration, RocksDB
throughput, write latency p50/p99, CPU time, RSS, UUSE lock/I/O counters, and error counts. Optional
counters are enabled only when `UUSE_STATS_PATH` (or `UUSE_STATS=1`) is set; the default runtime path
keeps them disabled.

For a repeatable three-round stage result with raw logs, median aggregation, and dispersion data, use
`scripts/db/stage48_matrix.sh`. It validates zero errors and matching SPDK submit/complete counts
before including a round in the summary.

Known limitations:

- External shell commands (`mkdir`, `cat`, `dd`, ...) run in fork+exec children that
  inherit `_UUSE_INIT_DONE` and fall through to the kernel. Use single-process
  workloads (python, C programs) or run everything in one shell built-in.
- The default malloc bdev is process-private: data does not survive across processes.
- Persistent AIO/NVMe bdevs require one explicit `UUSE_FORMAT=1` formatting run; normal runs
  load the existing blobstore and reject an unformatted backend. Only one UUSE process may mount
  a backend at a time; sequential process restart/reopen is supported.
- File `mmap` returns `ENODEV`; native `libaio` and `io_uring` completion queues are not supported.

Run the interceptor smoke test (assertions run in a single python process):

```bash
bash scripts/preload_smoke.sh build/lib/libuuse.so
```
