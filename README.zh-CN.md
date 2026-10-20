# UUSE

[English](README.md) | **简体中文**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)](https://www.kernel.org/)

**Unified User-space Storage Engine** — 运行在应用进程内、面向 NVMe 的 coroutine-first
存储引擎。

UUSE 通过 SPDK 独占一块或多块 NVMe，自行实现文件命名空间、extent 分配器、预写日志与
checkpoint。I/O 不经过内核块层、文件系统和页缓存。应用可以直接链接 C++20 引擎 API
（`uuse::Engine`、`uuse::Client`、`uuse::File`），也可以通过本仓库的适配层接入
FIO、RocksDB、MySQL、TiKV 与 MinIO。

## 状态

UUSE 处于早期科研阶段，预计会有破坏性变更。

- 单机、单进程、独占设备；
- 目前只在实验虚拟机上验证过，裸机数据待补；
- C++ API 与磁盘格式尚不稳定；
- 不是生产软件，不要存放重要数据。

## 设计动机

- **把内核移出 I/O 路径。** I/O 路径上没有系统调用、没有块层队列、没有页缓存、没有
  文件系统日志：引擎直接向 NVMe 下发命令，并自行负责调度、顺序与持久化语义。
- **与数据引擎同进程。** 数据库、对象存储或文件服务与它的存储跑在同一进程里，缓存、
  元数据与 I/O 准备可以共享同一批线程和缓冲区。
- **可测量。** 每个操作、屏障和 checkpoint 都能从应用调用一路追踪到设备提交。

## 特性

- C++20 coroutine API，单线程 `Client`/`File` 句柄，异步提交与完成。
- 通过 `uio`/`vfio` 直接访问 SPDK bdev；device set 支持按容量加权放置，并逐盘统计
  submit/complete。
- 文件命名空间：文件、目录、`rename`/`link`/`remove`、预分配、`sync`/`fdatasync`，
  以及基于 journal + checkpoint 的崩溃重放。
- shard 协程 actor 与按 inode 的依赖调度器：无关工作并行，冲突工作保持顺序。
- direct buffer 与有界 bounce buffer 两条数据路径，共享 DMA arena。
- 单个 `.conf` 描述设备、放置、reactor、限额与可观测性。

## 架构

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

元数据控制面（命名空间、journal、checkpoint）与数据 reactor 分离。进程锁保证同一
device set 不会被两个进程同时挂载。

## 快速开始

### 环境要求

| 项 | 要求 |
|---|---|
| 操作系统 | Linux，x86-64 |
| 工具链 | CMake ≥ 3.16，GCC ≥ 12 或 Clang ≥ 15，C++20 |
| 内存 | 建议 1024 个 2 MiB 大页 |
| 设备 | 一块可以整盘擦除的 NVMe；盘上数据会全部丢失 |
| 依赖 | 由 `spdk/scripts/pkgdep.sh` 安装 |

### 构建

```bash
git clone --recurse-submodules https://github.com/LaoLon/Unified_UserSpace_Storage_Engine.git
cd Unified_UserSpace_Storage_Engine

sudo spdk/scripts/pkgdep.sh
(cd spdk && ./configure && make -j"$(nproc)")

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

产物包括 `build/libuuse_core.so`、`build/uusectl`、测试二进制，以及由
`FIO_SOURCE_DIR`、`ROCKSDB_ROOT` 两个 CMake 变量启用的可选适配层。`Makefile`
提供同样的 core 目标，供偏好 `make` 的环境使用。

### 准备设备

```bash
sudo sysctl -w vm.nr_hugepages=1024
sudo modprobe uio_pci_generic
echo uio_pci_generic | sudo tee /sys/bus/pci/devices/0000:13:00.0/driver_override
echo 0000:13:00.0 | sudo tee /sys/bus/pci/drivers/uio_pci_generic/bind
```

`0000:13:00.0` 是 `config/uuse_nvme.conf` 使用的控制器。换机器时，绑定命令和 `.conf`
都要改；BDF 用 `lspci -D` 查，不要根据 `nvmeXn1` 编号推断。绑定会把设备从内核摘下，
原有文件系统在解绑前不可见。

### 格式化与检查

```bash
build/uusectl --config=config/uuse_nvme.conf validate
build/uusectl --config=config/uuse_nvme.conf format --yes   # 破坏性操作
build/uusectl --config=config/uuse_nvme.conf info
build/uusectl --config=config/uuse_nvme.conf check
```

`format` 写入超级块、journal 与初始 checkpoint；`info` 以只读方式挂载元数据并打印卷
布局；`check` 扫描超级块、checkpoint 与 journal，卷干净时输出 `corruption=none`。

### 跑一个负载

把 CMake 指向 fio 源码树即可构建 FIO external engine：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFIO_SOURCE_DIR=/path/to/fio-3.42
cmake --build build -j"$(nproc)"

/path/to/fio-3.42/fio --name=uuse-4k \
  --ioengine=external:build/fio-uuse.so \
  --uuse_config=config/uuse_nvme.conf --uuse_async=1 --uuse_client_depth=32 \
  --filename=/demo --rw=randrw --rwmixread=50 --bs=4k --size=256m \
  --direct=1 --iodepth=32 --numjobs=1 --time_based=1 --runtime=10
```

### 测试

```bash
ctest --test-dir build --output-on-failure
```

测试集混合了纯内存测试和需要独占 NVMe 的设备测试；设备测试通过 CTest 资源锁串行执行，
SPDK 不可用时会自动跳过。

## 前端

| 前端 | 接入方式 | 目录 | 状态 |
|---|---|---|---|
| FIO 3.42 | external ioengine（`fio_uuse.cpp`） | [`fio/`](fio/) | 可用 |
| RocksDB 8.9.1 | `FileSystem` 适配（`uuse_filesystem.cpp`） | [`rocksdb/`](rocksdb/) | 可用 |
| MySQL 8.0.46 | InnoDB `os_file` seam，fork 上的补丁栈 | [`mysql/`](mysql/) | 需重建 |
| TiKV 8.5.8 | raft-engine `FileSystem` + RocksDB `Env` seam | [`tikv/`](tikv/) | 可用 |
| MinIO | 盘层 seam 加 C ABI shim | [`minio/`](minio/) | 可用 |
| 3FS | chunk-store 后端（`UuseChunkStore`） | [`integrations/3fs/`](integrations/3fs/) | 实验性 |

每个目录都有自己的 README、补丁栈和 VM 脚本。MySQL fork 需要先按当前 core 重建；
TiKV 与 MinIO 前端链接本仓库构建的 C ABI shim。

## 性能

同盘对照下，4K `randrw` 时 UUSE 与裸 SPDK 相差约 5% 以内，3FS chunk 写与 MinIO PUT
基本持平。应用层结果（MySQL、RocksDB、TiKV）落在原生路径的 88%–110%：一旦数据库自带
缓存和持久化屏障，设备不再是瓶颈，引擎的优势基本消失；冷随机读是例外，因为原生的
同步读路径在高并发下会退化。

目前所有数据都来自 4–6 vCPU、单块虚拟 NVMe 的虚拟机，尚未做裸机验证。完整矩阵、
逐负载归因与 VM 口径说明见 [performance-compare.md](performance-compare.md)。

## 仓库结构

```text
include/uuse/     引擎公共头文件
src/              core 实现（common、config、core、metadata、runtime、storage）
tests/            CTest 测试集，含真实设备测试
tools/            uusectl、元数据基准、trace 导出、锁探针
fio/              FIO external ioengine
rocksdb/          RocksDB FileSystem 适配、基准与检查工具
mysql/            InnoDB seam 补丁栈与构建脚本
tikv/             TiKV 前端、补丁与 VM 脚本
minio/            MinIO 前端、shim、Go 盘层与补丁
integrations/3fs/ 3FS chunk-store 后端（实验性）
config/           示例 .conf
doc/              设计、架构、评审、性能基线
spdk/             SPDK 子模块（仓内构建）
```

## 文档

- [`doc/design/uuse.md`](doc/design/uuse.md) —— 架构、接口、边界与验收标准的唯一事实源。
- [`doc/Architecture/uuse_architecture.md`](doc/Architecture/uuse_architecture.md) ——
  组件与数据流图。
- [`performance-compare.md`](performance-compare.md) 与
  [`doc/performance/`](doc/performance/) —— 测量方法、结果与归因。
- [`doc/review/`](doc/review/) —— 设计审计与专项复盘。
- 各前端 README：`fio/`、`rocksdb/`、`mysql/`、`tikv/`、`minio/`、`integrations/3fs/`。

## 限制与非目标

- **不提供 POSIX 层。** 没有 FUSE 挂载、没有 `LD_PRELOAD` shim、没有 fd/`mmap`
  模拟，也没有 SPDK Blobstore 镜像；各前端走自己的 seam。
- **同一时间只允许一个进程。** 引擎独占其 device set，多进程并发挂载会被进程锁拒绝。
- **不做复制。** UUSE 是本地引擎，复制与分布式由 3FS、MinIO 等上层负责。
- **没有宿主页缓存。** 读请求总是落到设备，缓存由前端自己负责。
- **仅 Linux x86-64**，依赖 SPDK 与 `uio`/`vfio`。
- **接口不稳定。** API 与磁盘格式都可能变化，目前没有迁移路径。

## 参与贡献

欢迎提 issue 和 PR。提交前请：

- 跑 `ctest --test-dir build --output-on-failure`，保持全绿；
- 修改行为前先更新 [`doc/design/uuse.md`](doc/design/uuse.md)，评审以它为准；
- 提交保持聚焦，subject 使用 conventional commits（`feat:`、`fix:`、`docs:`、
  `perf:`、`chore:`）。

## 许可证

MIT，见 [LICENSE](LICENSE)。`spdk/` 子模块按其自身的 BSD-3-Clause 许可分发；
`mysql/`、`tikv/`、`minio/`、`integrations/3fs/` 只包含补丁栈和适配代码，不包含上游源码。
