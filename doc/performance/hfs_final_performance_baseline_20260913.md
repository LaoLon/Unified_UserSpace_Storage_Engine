# HFS 最终性能基线

> **基线日期**：2026-09-13  
> **HFS 提交**：`26bd79c`  
> **SPDK**：VM 内 Git checkout，`f0b63ed866f0e4d3c56e73bd4a98d0042956147a`  
> **测试 VM**：Ubuntu，8 vCPU；NVMe PCI BDF `0000:0b:00.0`，SPDK bdev `Nvme0n1`

本文件是当前项目唯一的性能基线。历史 shard 的测试记录已不再作为性能基线；设计取舍、
瓶颈分析和恢复细节分别保留在 `doc/review/`、`doc/shard/` 和 `doc/bug.md`。

## 1. FIO 主基线：HFS POSIX vs raw SPDK

两者使用同一块 VM NVMe。HFS 走完整 POSIX `LD_PRELOAD` 路径，raw SPDK 使用官方
`spdk_bdev` FIO engine；`randrw=50/50`、`O_DIRECT`、256 MiB、每项 10 秒，HFS
`numjobs=32`，raw SPDK `iodepth=32`。正式数字 trace-off，所有样本 `errors=0`，SPDK
submit/complete 闭合。

| block size | HFS total IOPS | raw SPDK total IOPS | HFS/raw | HFS read p50/p99 (µs) | HFS write p50/p99 (µs) |
|---:|---:|---:|---:|---:|---:|
| 4 KiB | 77,249 | 121,911 | 63.4% | 395 / 889 | 399 / 889 |
| 8 KiB | 71,456 | 112,535 | 63.5% | 428 / 954 | 436 / 963 |
| 16 KiB | 61,608 | 94,063 | 65.5% | 502 / 1,090 | 506 / 1,090 |
| 32 KiB | 50,260 | 73,673 | 68.2% | 610 / 1,319 | 619 / 1,319 |
| 64 KiB | 37,623 | 53,026 | 70.9% | 815 / 1,745 | 823 / 1,761 |

原始结果：VM `/mnt/hfs-data/shard65-fio-nvme/summary.tsv`。复现脚本：
`scripts/performance/fio_stage2_compare.sh`，NVMe 配置：`config/hfs_nvme.json`。

## 2. 数据库状态

数据库结果用于确认 POSIX 语义和恢复正确性，不与 FIO 裸 SPDK 吞吐混为同一指标：

- MariaDB/MySQL：初始化、CRUD/DDL、事务提交/回滚、LONGTEXT、临时表、正常重启、
  SIGKILL 恢复及 mysqlslap 4/16 并发全部通过。
- RocksDB：fillseq/readrandom、direct I/O、overwrite、flush/compact、WAL、
  readwhilewriting、SIGKILL 恢复、跨进程 reopen 和 `ldb checkconsistency` 全部通过。
- durability 统计显示实际 mysqlslap 样本的 `durability_batch_max=1`，独立事务仍基本
  各自等待一次真实 device flush；因此 MySQL 写入不能用扩大 group window 或跳过 flush
  伪造为原生性能。

数据库持久化回归产物：VM `/mnt/hfs-data/shard65-db/`。

## 3. 解释与使用规则

1. 后续 FIO 回归必须保持本文件的 NVMe、块大小、并发、读写比例和 trace-off 口径；
   不得用旧的 Malloc/Aio 或 `/dev/sdb1` 数字替代本基线。
2. HFS/raw SPDK 的差值代表完整 POSIX 文件系统层开销，不能直接解释为 NVMe 硬件规格。
3. stats/trace 只用于归因，不纳入吞吐基线；归因时使用
   `HFS_STATS_PATH=/mnt/hfs-data/<run>/stats` 和 `HFS_TRACE_PATH=/mnt/hfs-data/<run>/trace.tsv`。
4. 当前剩余性能主因是 POSIX wrapper、必要 bounce copy、每请求 completion 以及独立
   durability flush；不能通过弱化 `fsync/fdatasync` 语义解决。
