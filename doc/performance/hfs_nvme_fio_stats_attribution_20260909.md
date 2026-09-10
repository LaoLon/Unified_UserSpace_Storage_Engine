# NVMe FIO 统计归因测试

**日期**：2026-09-09  
**设备**：SPDK NVMe `Nvme0n1`，PCI `0000:0b:00.0`，`uio_pci_generic`  
**工作负载**：`randrw` 50/50，256 MiB，15 秒，4/8/16/32/64 KiB，HFS `numjobs=32`，raw SPDK `iodepth=32`  
**注意**：HFS 开启 `HFS_STATS_PATH` 与 `HFS_STATS_TIMING=1`，raw SPDK 未开启 HFS 统计；本轮 IOPS 仅用于定位，不与 stats-off 性能表直接比较。

## 归因结果

| bs | HFS IOPS | raw SPDK IOPS | HFS p99 / SPDK p99 (µs) | copy ops / direct ops | runtime queue peak / inflight | I/O lock wait / hold (ms，累计) | DMA wait (ms，累计) |
|---|---:|---:|---:|---:|---:|---:|---:|
| 4K | 37,962 | 92,799 | 6,914 / 963 | 635,040 / 0 | 11 / 1 | 392,940 / 1,056,060 | 0.0 |
| 8K | 22,212 | 88,892 | 17,695 / 864 | 366,019 / 0 | 15 / 1 | 522,266 / 612,590 | 21.6 |
| 16K | 14,641 | 73,702 | 28,705 / 1,057 | 236,049 / 0 | 17 / 1 | 648,446 / 400,870 | 208.1 |
| 32K | 9,794 | 57,883 | 36,962 / 1,335 | 155,135 / 0 | 25 / 1 | 616,393 / 320,098 | 0.0 |
| 64K | 3,705 | 37,734 | 41,681 / 2,179 | 59,691 / 0 | 5 / 1 | 0.0002 / 131.5 | 0.0 |

累计锁时间是 32 个 FIO worker 的总和，不应直接相加成墙钟耗时。

## 明确结论

### 1. 普通 FIO buffer 全部走 bounce copy

五种块大小均满足：

```text
io_direct_ops = 0
io_copy_ops   = io_read_ops + io_write_ops
```

例如 4 KiB 共 635,040 个逻辑 I/O，产生 635,040 次 copy、约 2.60 GiB copy bytes；64 KiB 共 59,691 个逻辑 I/O，产生 59,691 次 copy、约 3.91 GiB copy bytes。当前 FIO 没有使用 registered-DMA/direct async 优化路径。

### 2. 同步等待限制了有效并发

所有块大小的 `runtime_inflight_peak=1`，但 runtime queue peak 达到 11～25。说明每个 POSIX `psync` 调用仍在等待自己的 completion，32 个 worker 主要表现为排队，而不是像 raw SPDK 一样维持真正的异步 outstanding window。

### 3. 4K～32K 的主要成本是 I/O 锁竞争

`lock_io_wait_ns` 和 `lock_io_contention` 在 4K～32K 显著增长，且 32K 的 runtime queue peak 达到 25。metadata 不是本轮主因：每个进程只有 5 次 metadata transaction、16 次 metadata sync 和 2 次 flush，说明延迟 metadata publish 已经生效。

### 4. DMA pool 不是当前唯一主因

DMA 等待只在 8K、16K 出现，且没有 `dma_exhaust`。因此直接把 pool 数量加大，不能解决 4K～32K 的主要差距；应先缩短锁和同步 waiter 生命周期。

### 5. 64K 统计存在盲区（已补齐）

64K 样本的 `lock_io_wait_ns` 近似为零并不代表没有锁等待。大范围请求会直接走 per-inode `rangeGate`，
旧统计只包围 bucket lock，没有包围 gate lock。该缺口已在本轮补齐；修复后的同口径 64K 样本见
`doc/performance/hfs_range_gate_registered_direct_20260909.md`，记录到 `range_gate` 累计 wait/hold。

### 6. 请求放大不是主因

五种块大小均满足 `spdk_submit≈spdk_complete≈logical_io_count`，request amplification 约为 1；当前随机 aligned FIO 没有明显 RMW 或 vector 请求放大。

## 下一步

1. 使用 SPDK DMA allocator 或 registered buffer 建立持续 direct-DMA 并发基准，对比 `copy_ops=0` 时的收益。
2. 将同步 preload 与 `hfs_lib_async_*` 分开测量，确认 waiter/queue 是吞吐上限还是仅影响尾延迟。
3. 对 4K/32K 优先缩短 aligned I/O 的 lock 生命周期，并验证 overlapping write、RMW、truncate 语义不回退。

## 原始结果

VM 原始目录：`/var/tmp/hfs-fio-nvme-stats-20260909/`。每个块大小包含 HFS/raw SPDK JSON、stderr 和 HFS stats 快照。

## 后续修复结果

本报告的原始 64K 样本是在 `rangeGate` 分片修复前采集的；修复后的同口径结果和提交
队列分析见 [`hfs_range_stripe_submission_queue_fix_20260909.md`](hfs_range_stripe_submission_queue_fix_20260909.md)。
修复后 `lock_range_gate_*` 已归零，新增的 `io_backend_outstanding_peak=31` 表明：
同步 POSIX 调用仍等待各自 completion，但后端已通过 batch submission queue 保持真实
并发 outstanding。
