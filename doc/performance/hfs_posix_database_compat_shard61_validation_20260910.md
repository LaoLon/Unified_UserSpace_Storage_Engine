# HFS POSIX 数据库兼容路径 shard-61 验证记录

日期：2026-09-10  
代码：`main@afa0976`（本地实现尚未提交时的工作树验证）  
VM：Ubuntu，SPDK 由 VM 内 Git checkout 维护；本轮 correctness 使用持久化 Aio 后端。

## 实现内容

本 shard 面向不能修改 I/O API 的 MySQL/MariaDB、RocksDB 和普通 POSIX 应用，保持完整
同步 POSIX 语义，优化 syscall 内部的冗余成本：

- DMA pool class 改为 32-bit `freeMask`，无竞争 acquire/release 不再进入 pthread mutex；
  pool 耗尽仍使用 condition wait。
- acquire 全生命周期使用 closing-bit API gate，避免 slow waiter 与 teardown 并发导致
  use-after-free。
- 同 inode 的并发 `fsync`/`fdatasync` 通过 durability epoch coordinator 合并，由一个
  leader 执行 drain/commit/barrier，waiter 复用结果。
- `fdatasync` 对 regular file 的已落数据且无 size/extent dirty 时只执行 data barrier，
  不强制提交未影响数据可见性的时间戳。
- 统计增加 `dma_fast_acquire`、`durability_leader`、`durability_waiter` 和
  `fdatasync_fast`。

## VM 构建

```text
make -B app lib                         PASS
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j2           PASS
```

构建仅有已有的 `_GNU_SOURCE` 和 preload wrapper 声明告警，没有错误。

## Correctness 回归

| 测试 | 结果 | 关键证据 |
| --- | --- | --- |
| `dma_pool_backpressure_test.sh` | PASS | 128 acquire/128 release，active_peak=4 |
| `fdatasync_smoke.sh` | PASS | `fdatasync_fast=8`，leader/waiter 均被触发 |
| `metadata_group_commit_test.sh` | PASS | `batch_max=6`，`metadata_sync_count=1` |
| `inode_io_concurrency_test.sh` | PASS | 多 inode/range positioned I/O 无数据错误 |
| `runtime_stress.sh` | PASS | 32 线程 × 5 秒 |
| `posix_data_contract_test.sh` | PASS | 读后写、持久化和 size 语义通过 |
| `io_fast_path_smoke.sh` | PASS | request-local fast I/O 通过 |
| `metadata_transaction_smoke.sh` | PASS | 三个 intent 边界恢复通过 |
| `io_error_injection_test.sh` / `error_bdev_injection_test.sh` | PASS | I/O/flush 错误传播通过 |
| `durability_crash_recovery_test.sh` | PASS | fsync step 1/2/3 恢复通过 |
| `rocksdb_smoke.sh` | PASS | 20,000 条、WAL、direct I/O、flush/compact、SIGKILL、ldb check |

RocksDB 短基准输出：

```text
fillseq       872 ops/s（20,000 条）
readrandom    111,926 ops/s
direct read   159,201 ops/s
overwrite     902 ops/s
readwhilewriting 4,298,323 ops/s（8 threads，30 s）
```

这些数字用于确认语义和路径未退化，不作为 NVMe 最终性能门槛；Aio 后端与 NVMe 结果需
在同一盘、同一 FIO 参数下单独比较。

## MariaDB 对照结论

`scripts/db/mysql_smoke.sh` 在 bootstrap 结束阶段出现：

```text
Signal handler thread did not exit in a timely manner.
```

改动后的 `build/lib/libhfs.so` 和同一 VM、同一 SPDK 下由远端基线 `a414d49` 构建的旧
`libhfs.so` 均在相同阶段超时（40 秒 bounded run），说明这是当前 VM/MariaDB 信号线程
退出环境问题，而不是 shard-61 的差异回归。遗留的 `mariadbd` 进程已按显式 PID 清理。
环境修复后可直接重跑原脚本完成数据库端到端验收。

## NVMe FIO 快速矩阵

为确认本轮 fast acquire 改动没有只改善 Aio 回归，VM 使用同一 NVMe 控制器
`0000:0b:00.0`，`Nvme0n1`，FIO `randrw` 50/50、direct=1、64 MiB、每项 5 秒。以下是
单队列（HFS `numjobs=1` / SPDK `iodepth=1`）和高并发（HFS `numjobs=32` / SPDK
`iodepth=32`）的总 IOPS：

| bs | qd=1 HFS | qd=1 SPDK | HFS/SPDK | qd=32 HFS | qd=32 SPDK | HFS/SPDK |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 KiB | 22,209 | 32,108 | 69.2% | 65,818 | 116,819 | 56.3% |
| 8 KiB | 22,057 | 30,112 | 73.2% | 63,309 | 108,639 | 58.3% |
| 16 KiB | 20,705 | 28,664 | 72.2% | 56,521 | 89,108 | 63.4% |
| 32 KiB | 19,347 | 26,282 | 73.6% | 51,082 | 70,511 | 72.4% |
| 64 KiB | 17,521 | 23,456 | 74.7% | 40,864 | 49,107 | 83.2% |

全部 workload `errors=0`。相对 shard-59 的同口径趋势，8/16 KiB 高并发比例明显回升，
但仍存在同步 POSIX 的固定入口、bounce copy、range/owner 调度和单请求完成等待；该
矩阵用于归因和回归，不宣称已达到“透明零拷贝”或原生 SPDK 等效并发。

## 结论

shard-61 的 POSIX 兼容路径代码、关闭生命周期和 durability 优化已完成并通过构建与低层
回归；RocksDB 端到端已通过。MariaDB 仅剩环境阻塞，不应在当前 VM 状态下宣称通过。
