# HFS 第二阶段性能报告

> **测试日期**：2026-09-08
>
> **测试目标**：验证 HFS 直连真实 NVMe 时，与 native 文件系统路径的性能差异。
>
> **对比原则**：HFS 使用 `/dev/nvme0n1`；native FIO/MySQL/RocksDB 使用 `/dev/sdb1`。

## 一页结论

| 结论项 | 结果 | 判断 |
|---|---:|---|
| FIO 随机混合吞吐 | HFS 为 native 的 **2.03–3.57 倍** | SPDK 直连 NVMe 路径有效 |
| MySQL 读 QPS | HFS 为 native 的 **0.92–1.00 倍** | 读路径基本接近 native |
| MySQL 写 QPS | HFS 为 native 的 **0.30–0.53 倍** | 元数据提交和同步开销明显 |
| RocksDB 持续混合吞吐 | HFS 为 native 的 **0.961–0.978 倍** | 吞吐接近，但尾延迟较高 |
| RocksDB `fillseq` | HFS 为 native 的 **约 1/61** | 创建、WAL 和元数据同步是主要瓶颈 |
| 数据完整性 | 所有 workload 错误数为 0 | 本轮结果有效 |

最重要的结论是：**HFS 的设备路径已经切换到真实 NVMe，FIO 结果证明设备层没有走 `/dev/sdb`。当前主要差距集中在写入提交、元数据同步和尾延迟，而不是 NVMe 设备带宽。**

## 1. 测试拓扑

```text
HFS 应用
  └─ LD_PRELOAD / libhfs.so
      └─ HFS + SPDK
          └─ PCIe NVMe 0000:0b:00.0
              └─ /dev/nvme0n1（裸盘，不挂 ext4、不经过镜像文件）

native 应用
  └─ libc / ext4
      └─ /dev/sdb1
          └─ /mnt/hfs-data（VMware SATA）

/dev/sda：仅作为 Ubuntu 系统盘，不承载本轮 workload
```

| 项目 | HFS 路径 | native 对照路径 |
|---|---|---|
| FIO 文件 | `/hfs/fio/rw.dat` | `/mnt/hfs-data/fio-native/rw.dat` |
| MySQL 数据目录 | `/hfs/mysql` | `/mnt/hfs-data/performance-stage2-nvme-mysql/native-datadir` |
| RocksDB 数据目录 | `/hfs/rocks/benchmark` | `/mnt/hfs-data/performance-stage2-nvme-rocksdb-native/round-*/db` |
| HFS 配置 | `config/hfs_nvme.json`，bdev `Nvme0n1` | 不加载 `libhfs.so` |
| SPDK commit | `f0b63ed866f0e4d3c56e73bd4a98d0042956147a` | 不适用 |
| VM | Ubuntu，Linux 6.8.0-139-generic，8 vCPU | 同左 |

> **口径说明**：`doc/performance/stage2_baseline.md` 是旧的持久化 Aio 镜像测试，镜像位于 `/dev/sdb1`。旧报告与本报告的 direct-NVMe 结果不能直接等价比较。

## 2. 测试方法

| 应用 | 工作负载 | 参数 | 验证方式 |
|---|---|---|---|
| FIO | `randrw`，读写 50/50 | `psync`、`direct=1`、256 MiB、每项 15 秒 | HFS/native 顺序执行，错误数为 0 |
| MariaDB | 读：`SUM(value)`；写：单行 autocommit `UPDATE` | 2,000 queries、3 次迭代，并发 1/4/16 | 12 个 workload 全部完成，无 SQL/I/O 错误 |
| RocksDB | `fillseq`、`readwhilewriting` | 20,000 条、value 256 B、WAL 开启、mmap 关闭、3 轮 | HFS `errors=0`，SPDK submit/complete 一致；native error counters 为 0 |

## 3. FIO 结果

### 3.1 数据

| 块大小 | HFS IOPS | native IOPS | HFS/native | HFS MiB/s | native MiB/s | HFS p99 读/写（µs） | native p99 读/写（µs） |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 KiB | 15,908.7 | 7,845.8 | **2.03x** | 62.1 | 30.6 | 268 / 276 | 297 / 313 |
| 8 KiB | 17,502.6 | 6,902.9 | **2.54x** | 136.7 | 53.9 | 247 / 255 | 317 / 330 |
| 16 KiB | 16,073.2 | 7,423.8 | **2.16x** | 251.1 | 116.0 | 232 / 241 | 313 / 322 |
| 32 KiB | 16,424.3 | 7,712.2 | **2.13x** | 513.3 | 241.0 | 196 / 206 | 261 / 272 |
| 64 KiB | 16,111.7 | 4,512.7 | **3.57x** | 1,007.0 | 282.0 | 177 / 187 | 627 / 635 |

### 3.2 判断

- HFS 在所有块大小上都高于 native SATA，吞吐优势为 **2.1–3.6 倍**。
- 64 KiB 时 native 吞吐明显下降，NVMe 设备优势更加突出。
- HFS p99 也低于 native，说明本轮 FIO 中用户态 HFS 开销尚未抵消 NVMe 的设备差距。

## 4. MariaDB / MySQL 结果

### 4.1 QPS 数据

| 工作负载 | 并发 | HFS NVMe QPS | native sdb QPS | HFS/native |
|---|---:|---:|---:|---:|
| read | 1 | 938.5 | 937.6 | **1.00x** |
| read | 4 | 3,327.8 | 3,597.1 | **0.93x** |
| read | 16 | 4,902.0 | 5,333.3 | **0.92x** |
| write | 1 | 1,146.8 | 3,731.3 | **0.31x** |
| write | 4 | 2,567.4 | 8,438.8 | **0.30x** |
| write | 16 | 9,009.0 | 17,094.0 | **0.53x** |

### 4.2 判断

- **读路径**：1 并发基本持平，4/16 并发仅低 7–8%。
- **写路径**：单并发约为 native 的 30%，16 并发提升到 53%，但仍有明显差距。
- 主要怀疑点是 HFS 元数据提交、`fsync`/flush、blob 分配和目录更新的串行化。

### 4.3 已知退出问题

本轮 direct-NVMe bootstrap 在系统表提交后复现：

```text
Signal handler thread did not exit in a timely manner
```

为避免退出阶段阻塞性能采集，测试脚本在检测到该提示后回收 bootstrap/server 进程组。查询阶段的 12 个 workload 均完成，且没有 SQL/I/O 错误。

这不等于正常退出问题已经修复：

1. 强制回收 SPDK 进程后可能遗留 `/var/tmp/spdk_pci_lock_<BDF>`。
2. 测试器只有在确认没有其他 SPDK peer 后才清理该 lock。
3. 未清理时，下一次挂载可能返回 `ENODEV`。

详细记录见 [`doc/bug.md`](../doc/bug.md) 的条目 44。当前支持边界应表述为：**单数据库进程、查询阶段正确；正常 shutdown 仍待修复。**

## 5. RocksDB 结果

### 5.1 数据

| 场景 | HFS NVMe ops/s | native sdb ops/s | HFS/native | HFS p50 / p99（µs） | native p50 / p99（µs） |
|---|---:|---:|---:|---:|---:|
| `fillseq` | 5,117 | 310,380 | **0.016x** | 152.6 / 828.9 | 2.0 / 5.9 |
| `readwhilewriting-1t` | 438,619 | 456,538 | **0.961x** | 161.4 / 820.4 | 3.7 / 13.2 |
| `readwhilewriting-4t` | 2,037,147 | 2,084,022 | **0.978x** | 229.4 / 1,108.6 | 3.8 / 12.8 |

### 5.2 判断

- `readwhilewriting` 的吞吐已达到 native 的 96.1–97.8%。
- HFS p99 延迟约高 **60–140 倍**，说明当前主要问题是尾延迟，而不是并发吞吐不足。
- `fillseq` 只有 native 的约 1/61，是本轮最明显的数据库瓶颈，集中体现了创建、WAL 和元数据同步成本。

## 6. 问题定位与优化优先级

| 优先级 | 问题 | 证据 | 建议方向 |
|---|---|---|---|
| P0 | direct-NVMe 正常退出和 PCI lock 回收 | MariaDB 出现 signal-handler 超时；强制回收后可能遗留 lock | 修复 SPDK owner 生命周期、信号线程退出和 lock 清理协议 |
| P1 | 小写入同步路径慢 | MySQL 写 QPS 仅为 native 的 0.30–0.53 倍 | 脏页/元数据增量提交、group commit、批量 blob 分配 |
| P1 | 创建/WAL/元数据提交放大 | RocksDB `fillseq` 约慢 61 倍 | 合并元数据事务、减少整 blob image 写入、批量目录更新 |
| P1 | 尾延迟高 | RocksDB p99 高 native 约 60–140 倍 | 拆分 owner-thread 排队、同步等待和 wrapper/copy 延迟；结合统计计数做分位数归因 |
| P2 | 结果可观测性不足 | 当前主要依赖整体 QPS 和 p99 | 默认保留低开销计数，按请求阶段记录 queue wait、metadata sync、flush |

建议重点观察 `HFS_STATS_PATH` 中的：

```text
runtime_queue_wait_ns
metadata_sync_count
flush_count
```

## 7. 原始结果与复现入口

### 7.1 原始结果

- FIO：`/mnt/hfs-data/performance-stage2-nvme-fio/summary.tsv`
- MySQL：`/mnt/hfs-data/performance-stage2-nvme-mysql/results.tsv`
- HFS RocksDB：`/mnt/hfs-data/performance-stage2-nvme-rocksdb/summary.tsv`
- native RocksDB：`/mnt/hfs-data/performance-stage2-nvme-rocksdb-native/summary.tsv`

### 7.2 相关配置和脚本

- NVMe SPDK 配置：`config/hfs_nvme.json`
- FIO 对比脚本：`scripts/performance/fio_stage2_compare.sh`
- MySQL 对比脚本：`scripts/performance/mysql_stage2_benchmark.sh`
- RocksDB benchmark：`scripts/db/database_benchmark.sh`
