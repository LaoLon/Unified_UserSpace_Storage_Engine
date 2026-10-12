# UUSE (unified user-space storage engine) 2.0

UUSE 2.0 是一个 **C++20 coroutine-first、直接管理 SPDK bdev** 的单进程用户态存储引擎。
前端（FIO / RocksDB / MySQL / TiKV / MinIO）与 core 在同一进程内，或通过 fork seam 的
C ABI shim 接入；core 在 raw NVMe 上自建文件 namespace、extent 分配、journal/checkpoint
与崩溃恢复，**不经过 Linux 文件系统、FUSE、POSIX wrapper 或 SPDK Blobstore**。

> **命名与状态**：项目于 2026-10-04 由 **HFS** 更名为 **UUSE**；`doc/` 下的历史评审与
> 基线文档保留 HFS 时期名称与证据路径。当前定位是**单节点实验室验证口径**（单应用进程、
> 单 Engine、独占 `.conf` 声明的 device set），能力边界与停止条件以
> `doc/design/uuse.md` §3/§17 为准。
>
> HFS 1.x（Blobstore / LD_PRELOAD 线）已冻结为本地 `src_legacy_frozen/`：不进入 Git、
> 不进入构建、只作考古参考。

## 特性

支持：

- C++20 coroutine-first 的 core API（`uuse::Engine` / `uuse::Client` / `uuse::File`）；
- 直接 SPDK bdev/NVMe 访问（uio/vfio），多设备 device set 与容量加权 placement；
- 文件 namespace、目录、rename/link/remove、extent 分配、`sync`/`fdatasync` 与 crash replay；
- direct buffer 与 bounded bounce buffer 两条数据路径、共享 DMA arena；
- 前端接入：FIO 3.42 external ioengine、RocksDB 8.9.1 `FileSystem` adapter、
  MySQL 8.0.46 InnoDB `os_file` seam、TiKV v8.5.8、MinIO S3（盘层 seam）、
  3FS chunk-store（实验）；
- 单一 `.conf` 配置入口（`config/uuse_nvme.conf` 为单盘正式口径）。

不提供（永久删除）：

- `LD_PRELOAD`、POSIX fd/stdio wrapper、FUSE、通用 `mmap`；
- SPDK Blobstore 镜像与旧 async/registered-DMA ABI；
- 多进程共享挂载、daemon、跨进程锁；fork child 直接调用 core 返回 `ForkedProcess`；
- 旧 HFS 1.x 镜像的在线兼容/自动升级；
- 通用 POSIX 兼容层，包括 MySQL/TiKV/MinIO 的 POSIX 接入（只支持各自 fork 的 seam）。

## 架构（概览）

```text
Frontend（FIO / RocksDB adapter / MySQL seam / TiKV seam / MinIO seam）
  → uuse::Client → Task / Operation
  → bounded MPSC shard ring
  → shard coroutine actor（×R）
  → 唯一 inode dependency scheduler
  → raw-bdev extent planner
  → SPDK channel / NVMe device set
  → completion event → frontend callback 或 poll
```

- 单进程、单 Engine；Engine 独占 device set，进程锁防止重复挂载；
- metadata control plane（namespace / journal / checkpoint）与 data reactor（shard actor）分离；
- 结构细节、不变量与 ABI 见 `doc/design/uuse.md`；图解见
  `doc/Architecture/uuse_architecture.md`。

## 构建（Linux，建议在 VM 内进行）

1. SPDK（Git submodule，独立构建）：

```bash
git clone --recurse-submodules <repository-url>
cd uuse
cd spdk
sudo ./scripts/pkgdep.sh        # 首次安装依赖
./configure
make -j"$(nproc)"
cd ..
```

2. UUSE core 与 `uusectl`（CMake 或 Makefile 二选一）：

```bash
# CMake（推荐；同时注册 CTest 与可选前端目标）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# 或 Makefile（SPDK 存在时自动启用 runtime）
make app        # build/lib/libuuse_core.so + build/bin/uusectl
```

可选前端目标（CMake cache 变量指向对应源码/安装目录后自动启用）：

```bash
cmake -S . -B build \
  -DFIO_SOURCE_DIR=/path/to/fio-3.42 \
  -DROCKSDB_ROOT=/path/to/rocksdb-8.9.1-install
cmake --build build -j"$(nproc)"   # 产出 fio-uuse.so、uuse_rocksdb、uuse_db_bench、uuse_ldb 等
```

MySQL / TiKV / MinIO / 3FS 的前端不在本仓库直接编译，各自打补丁栈到对应上游 fork，
入口见 `mysql/`、`tikv/`、`minio/`、`integrations/3fs/` 下的 README 与脚本。

## 使用

```bash
# 目标 NVMe 按 .conf 绑定 uio/vfio（默认 hugepages=1024）
# 以下为 CMake 布局；Makefile 构建的产物在 build/bin/ 下
build/uusectl --config=config/uuse_nvme.conf format --yes   # 破坏性格式化
build/uusectl --config=config/uuse_nvme.conf check          # superblock/checkpoint/journal 扫描
build/uusectl --config=config/uuse_nvme.conf info           # 卷布局
build/uusectl --config=config/uuse_nvme.conf validate       # 仅校验 .conf
```

`config/uuse_nvme.conf` 面向当前 VM 的单盘口径（`Nvme0n1`，PCI `0000:13:00.0`）；
更换机器时必须按 sysfs 拓扑核对并更新 `pci_bdf`，不能凭内核 `nvmeXn1` 编号推断。
`config/*.json` 为 HFS 1.x 历史材料，不参与 UUSE 2.0 配置。

测试：

```bash
ctest --test-dir build --output-on-failure    # SPDK 就绪时 29 项（含 8 项真实设备测试）
```

设备测试独占同一块 NVMe，CMake 已用 `RESOURCE_LOCK` 串行化；SPDK 缺失时 core 仍可编译，
设备测试自动跳过。

## 前端与验收状态

| 前端 | 接入方式 | 入口 | 基线/验收 |
|---|---|---|---|
| FIO 3.42 | external ioengine（`fio/fio_uuse.cpp`） | `fio/run_three_way_matrix_vm.sh`、`fio/run_four_engine_vm.sh` | `doc/performance/fio_baseline.md` |
| RocksDB 8.9.1 | `FileSystem` adapter（`rocksdb/uuse_filesystem.cpp`） | `rocksdb/run_native_vs_uuse_vm.sh`、`uuse_db_bench`、`uuse_ldb` | `doc/performance/rocksdb_baseline.md` |
| MySQL 8.0.46 InnoDB | fork `os_file` seam（`mysql/patches/`） | `mysql/build_mysql_vm.sh`、`mysql/run_native_vs_uuse_sysbench_vm.sh` | `doc/performance/mysql_baseline.md` |
| TiKV v8.5.8 | raft-engine `FileSystem` + RocksDB Env seam（`tikv/`） | `tikv/run_*.sh`、`tikv/patches/` | `tikv/README.md`、`doc/performance/uuse_tidb_baseline_20261009.md` |
| MinIO S3 | 盘层 seam + C ABI shim（`minio/`） | `minio/run_*.sh` | `minio/README.md`、`doc/performance/minio_baseline.md` |
| 3FS chunk-store（实验） | 本地引擎适配（`integrations/3fs/`） | `integrations/3fs/README.md` | 未列入 §3.1 支持边界 |

已完成的阶段计划（Shard-68~121）与各阶段验收记录见 `doc/shard/`、
`doc/review/`；当前 Bug 记录见 `doc/bug.md`。

## 目录结构

```text
include/uuse/       core 公共头文件（ABI 3）
src/                core 实现：common / config / core / metadata / runtime / storage
tests/              CTest 结构测试与真实设备测试
tools/              uusectl、uuse_meta_bench、uuse_trace_dump、uuse_lock_probe 等
fio/                FIO external ioengine 与 VM 矩阵脚本
rocksdb/            RocksDB FileSystem adapter、基准与一致性工具
mysql/              MySQL InnoDB seam 补丁栈与 VM 构建/基准脚本
tikv/               TiKV frontend：C ABI shim、补丁栈、spike、VM 脚本
minio/              MinIO frontend：C ABI shim、Go 盘层、seam 补丁、VM 脚本
integrations/3fs/   3FS chunk-store 适配（实验）
config/             `.conf` 配置（正式口径）与历史 `.json`
doc/                design（事实源）/ Architecture / review / performance / shard / bug
spdk/               SPDK git submodule（VM 内构建）
temp_scripts/       任务临时目录（Git 忽略）
src_legacy_frozen/  HFS 1.x 冻结副本（Git 忽略，本地参考）
```

## 文档索引

- `doc/design/uuse.md` — **当前唯一事实源**（架构、边界、接口、配置、验收、阶段计划）；
- `doc/design/uuse_minio_frontend.md`、`doc/design/uuse_3fs_*.md` — 各前端设计；
- `doc/performance/` — FIO / RocksDB / MySQL / MinIO / TiDB 基线
  （`fio_baseline.md`、`rocksdb_baseline.md`、`mysql_baseline.md`、`minio_baseline.md`、
  `uuse_tidb_baseline_20261009.md`）与 MinIO 归因（`minio_shard95_attribution.md`）；
- `doc/review/` — 设计审计与专项评审（含 TiKV / MinIO 接入点审计）；
- `doc/shard/` — Phase 开发计划（Shard-68~121）；
- `doc/bug.md` — 当前 Bug 记录；
- `tikv/README.md`、`tikv/patches/README.md`、`minio/README.md`、
  `integrations/3fs/README.md`、`rocksdb/`、`mysql/`、`fio/` — 各集成实际入口。

## 已知边界（摘要）

- 单进程单挂载：同一 device set 同时只允许一个 UUSE 进程；
- MinIO GET 吞吐与 ext4 的差距已按归因边界冻结（PUT 与 GET p99 达标），见
  `doc/performance/minio_baseline.md`、`minio/README.md`；
- TiKV `kill -9` 未 sync 尾部存在 ≤5 ms 丢失窗口（优雅退出 RPO=0），见 `tikv/README.md` §6；
- MySQL fork 停留在 HFS 时代状态，同盘冒烟未重跑（边界记录在 `tikv/README.md`）；
- `doc/bug.md` #47：同进程 remount 后数据路径读挂起（跨进程恢复不受影响），待处理。

## 许可

MIT，见 [LICENSE](LICENSE)。`spdk/` 为独立 submodule，按其自身许可证（BSD-3-Clause）分发；
`mysql/`、`tikv/`、`minio/`、`integrations/3fs/` 只包含补丁与适配代码，不包含对应上游源码。
