# HFS 第二阶段真实 NVMe / native SATA 对比

日期：2026-09-08  
测试目的：纠正上一版 Aio 镜像口径，验证 HFS 直连真实 NVMe 时与 native 文件系统盘的差异。

## 1. 测试口径

| 项目 | HFS | native 对照 |
|---|---|---|
| 数据设备 | SPDK PCIe NVMe `0000:0b:00.0`，对应 `/dev/nvme0n1`，裸盘，不挂 ext4、不经过镜像文件 | `/dev/sdb1`，挂载点 `/mnt/hfs-data`（VMware SATA） |
| FIO 文件 | `/hfs/fio/rw.dat` | `/mnt/hfs-data/fio-native/rw.dat` |
| MySQL 数据目录 | `/hfs/mysql` | `/mnt/hfs-data/performance-stage2-nvme-mysql/native-datadir` |
| RocksDB 数据目录 | `/hfs/rocks/benchmark` | `/mnt/hfs-data/performance-stage2-nvme-rocksdb-native/round-*/db` |
| HFS 配置 | `config/hfs_nvme.json`，bdev `Nvme0n1` | 不加载 `libhfs.so` |
| SPDK | `f0b63ed866f0e4d3c56e73bd4a98d0042956147a` | 不适用 |
| VM | Ubuntu，Linux 6.8.0-139-generic，8 vCPU | 同左 |

`/dev/sda` 仅作为系统盘，没有承载本轮 workload。上一份
[`stage2_baseline.md`](stage2_baseline.md) 是持久化 Aio 镜像落在 `/dev/sdb1` 的旧口径，
不能与本报告的 direct-NVMe 结果直接等价比较。

## 2. FIO

参数：`randrw`、读写 50/50、`psync`、`direct=1`、`iodepth` 由同步引擎决定、256 MiB、
每项 15 秒；HFS 与 native 顺序执行，错误数均为 0。

| BS | HFS 总 IOPS | native 总 IOPS | HFS/native | HFS 总 MiB/s | native 总 MiB/s | HFS p99 读/写 µs | native p99 读/写 µs |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 KiB | 15,908.7 | 7,845.8 | 2.03x | 62.1 | 30.6 | 268/276 | 297/313 |
| 8 KiB | 17,502.6 | 6,902.9 | 2.54x | 136.7 | 53.9 | 247/255 | 317/330 |
| 16 KiB | 16,073.2 | 7,423.8 | 2.16x | 251.1 | 116.0 | 232/241 | 313/322 |
| 32 KiB | 16,424.3 | 7,712.2 | 2.13x | 513.3 | 241.0 | 196/206 | 261/272 |
| 64 KiB | 16,111.7 | 4,512.7 | 3.57x | 1,007.0 | 282.0 | 177/187 | 627/635 |

结论：真实 NVMe 的设备优势清晰可见，HFS 在所有块大小上约为 native SATA 的
2.1–3.6 倍吞吐；HFS 的用户态元数据/同步开销尚未抵消 NVMe 的设备差距。

## 3. MariaDB / MySQL

参数：MariaDB 10.11.14，`mysqlslap` 每 workload 2,000 条查询、3 次迭代；读为
`SUM(value)` 全表范围查询，写为单行 autocommit `UPDATE`；并发 1/4/16。

| workload | 并发 | HFS NVMe QPS | native sdb QPS | HFS/native |
|---|---:|---:|---:|---:|
| read | 1 | 938.5 | 937.6 | 1.00x |
| read | 4 | 3,327.8 | 3,597.1 | 0.93x |
| read | 16 | 4,902.0 | 5,333.3 | 0.92x |
| write | 1 | 1,146.8 | 3,731.3 | 0.31x |
| write | 4 | 2,567.4 | 8,438.8 | 0.30x |
| write | 16 | 9,009.0 | 17,094.0 | 0.53x |

结论：读路径基本接近 native SATA（1 并发持平，4/16 并发约低 7–8%）；写路径仍受
HFS 元数据提交、fsync 和 blob/目录语义成本限制，单并发约为 native 的 30%，16 并发
提升到约 53%，但仍未达到 native。

### 退出兼容性说明

本轮 direct-NVMe bootstrap 在系统表提交后复现已知的
`Signal handler thread did not exit in a timely manner`。为不让退出阶段阻塞性能采集，
脚本检测警告后回收 bootstrap/server 进程组；查询阶段的 12 个 workload 均完成且无
SQL/I/O 错误。该处理不等于修复正常退出，详细记录见 [`doc/bug.md`](../doc/bug.md) 的
条目 44。强制回收 SPDK 进程后会留下 PCI lock，测试器在确认无其他 SPDK peer 后清理它，
否则下一次挂载会得到 `ENODEV`。

## 4. RocksDB

参数：3 轮；`fillseq` 20,000 条、value 256 B；随后 `readwhilewriting` 1/4 线程各 60 秒；
WAL 开启、`mmap_read=0`、`mmap_write=0`。HFS 组的三轮 `errors=0`，SPDK submit/complete
计数一致；native 组 RocksDB error counters 均为 0。

| case | HFS NVMe ops/s（中位数） | native sdb ops/s（中位数） | HFS/native | HFS p50/p99 µs | native p50/p99 µs |
|---|---:|---:|---:|---:|---:|
| fillseq | 5,117 | 310,380 | 0.016x | 152.6 / 828.9 | 2.0 / 5.9 |
| readwhilewriting-1t | 438,619 | 456,538 | 0.961x | 161.4 / 820.4 | 3.7 / 13.2 |
| readwhilewriting-4t | 2,037,147 | 2,084,022 | 0.978x | 229.4 / 1,108.6 | 3.8 / 12.8 |

结论：读写混合持续 workload 的吞吐已经接近 native SATA，但 HFS 延迟明显更高（p99
约高 60–140 倍）；`fillseq` 受大量创建、WAL 和元数据同步影响，吞吐约为 native 的
1/61，是当前最明显的数据库性能瓶颈。

## 5. 性能判断与后续方向

1. 设备层：FIO 证明 SPDK 直连 NVMe 路径有效，HFS 不是被 `/dev/sdb` 的 SATA 带宽
   限制；后续应继续优化 HFS 上层提交路径，而不是把 Aio 镜像结果当成 NVMe 结果。
2. 写放大/同步：MySQL 写 QPS 和 RocksDB `fillseq` 的差距共同指向元数据 image、
   blob 分配、fsync/flush 与目录更新的串行化。应优先做脏页/元数据 page 增量提交、
   group commit 和批量 blob 分配。
3. 延迟：RocksDB 吞吐接近 native 但 p99 高，说明并发度已足够、尾延迟被 owner-thread
   排队、同步等待或每次请求的 wrapper/拷贝放大；应结合 `HFS_STATS_PATH` 中的
   `runtime_queue_wait_ns`、`metadata_sync_count`、`flush_count` 做分位数归因。
4. 退出生命周期：direct-NVMe 下的 MariaDB 正常 shutdown 和 SPDK PCI lock 回收仍需
   产品级修复；当前只能将“单数据库进程、查询阶段正确”作为支持边界，不能宣称正常
   退出完全兼容。

## 6. 原始结果位置

- FIO：`/mnt/hfs-data/performance-stage2-nvme-fio/summary.tsv`
- MySQL：`/mnt/hfs-data/performance-stage2-nvme-mysql/results.tsv`
- HFS RocksDB：`/mnt/hfs-data/performance-stage2-nvme-rocksdb/summary.tsv`
- native RocksDB：`/mnt/hfs-data/performance-stage2-nvme-rocksdb-native/summary.tsv`

