# rangeGate 分片锁与提交队列修复验证

**日期**：2026-09-09  
**设备**：SPDK NVMe `Nvme0n1`，PCI `0000:0b:00.0`  
**工作负载**：`randrw` 50/50、256 MiB、15 秒、64 KiB、HFS `numjobs=32`、raw SPDK `iodepth=32`

## 1. 修复目标

上一轮归因测试显示，64 KiB 请求集中经过同一 inode 的 `rangeGate`：

- `range_gate` wait/hold：59,588/59,588；
- contention：53,900（约 90.45%）；
- 平均等待约 7.80 ms，平均持有约 115 us；
- stats-on FIO 只有约 3.7 kIOPS。

同时，`runtime_inflight_peak=1` 容易被误读为“没有异步并发”。该字段实际只表示
队列 dispatcher 正在执行的回调数，不表示 SPDK backend outstanding 深度。

## 2. 代码修复

### 2.1 用 per-inode range stripes 替代热路径 rangeGate

每个 `HfsInodeRuntime` 新增 64 个 `HfsRangeLock`，每个 stripe 覆盖 64 个逻辑 unit：

```text
logical unit / 64 -> stripe [0, 63]
```

同一请求涉及多个 stripe 时按升序获取、按逆序释放，避免死锁；不同 inode 不再共享
全局 256 bucket，也不会因为“大范围请求收集到很多 hash bucket”而升级到 inode 级排他
`rangeGate`。旧 `rangeGate` 保留为兼容字段，但数据 I/O 热路径不再使用它。

读写锁仍保留 writer-priority 的安全性，同时限制 writer 排队后的新读者 burst 为 8，
避免持续读流把写请求长期饿死。

### 2.2 复用现有 native-style submission queue

没有再添加一层重复队列；现有 `hfsRuntimeSubmitBatch()`/
`hfsRuntimeSubmitDataBatch()` 已经提供了需要的提交队列：

```text
POSIX worker
  -> hfsRequestSubmitDataBatch()
  -> per-reactor linked queue
  -> one spdk_thread_send_msg notification
  -> dispatcher drains the batch
  -> spdk_blob_io_*()
  -> completion callback / futex wake
```

本轮新增 `io_backend_outstanding_peak`，在真正调用 `spdk_blob_io_*()` 时加一、在
completion callback 时减一，用于区分 backend outstanding 和 dispatcher callback depth。
同步 POSIX `pread/pwrite` 仍必须等待自己的 completion，这是 POSIX 语义；需要调用方
保持 request window 的，应使用已有 `hfs_lib_async_*`/registered-DMA direct API。

## 3. 验证结果

### 3.1 统计开启的归因样本

| 指标 | 修复后 |
|---|---:|
| FIO total IOPS | 28,271 |
| read/write p99 | 3,293 / 5,407 us |
| `lock_range_gate_wait/hold` | 0 / 0 |
| `lock_io_contention` | 180,979 |
| `runtime_queue_depth_peak` | 26 |
| `runtime_inflight_peak` | 1 |
| `io_backend_outstanding_peak` | 31 |
| SPDK submit/complete | 428,400 / 428,400 |
| errors | 0 |

`io_backend_outstanding_peak=31` 证明 32 个 POSIX worker 已经可以让后端保持接近 QD32
的异步 outstanding；`runtime_inflight_peak=1` 只是单个 dispatcher 回调不重入，不能用来
否定提交队列的并发性。

### 3.2 stats-off 同盘对照

| 路径 | total IOPS | read p99 | write p99 |
|---|---:|---:|---:|
| HFS `psync`, `numjobs=32` | 30,233 | 2,867 us | 4,817 us |
| raw SPDK `iodepth=32` | 35,287 | 2,073 us | 2,073 us |
| HFS/raw | 85.7% | 1.38x | 2.32x |

该轮 HFS 与 raw 使用同一 `Nvme0n1`、同一 64 KiB 50/50 workload；设备状态会造成短样本
波动，因此结果用于确认方向，不替代多轮正式矩阵。

### 3.3 正确性与生命周期

VM 上以下检查通过：

- `make -B app lib -j2`；
- CMake Release；
- request-local fast I/O smoke；
- 32 线程 I/O concurrency matrix，data reactor submit/complete `770/770`；
- 4-reactor multireactor scaling，三个 data reactor 合计 `192/192`；
- registered-DMA/direct-window smoke。

## 4. 当前结论与后续

`rangeGate` 已不再是 64 KiB 热路径的全局排队点，真正 backend queue 也已经存在并可观测。
剩余差距主要来自普通 POSIX buffer 的 bounce copy、64 个 stripe 的同 inode false sharing、
以及同步 API 的调用方等待成本。下一轮应优先：

1. 做 registered-DMA/direct async 的持续并发矩阵，确认去掉 copy 后的上限；
2. 对 `lock_io_contention` 进一步评估 128/256 stripes 或动态区间锁；
3. 以 `hfs_lib_async_*` 与 raw SPDK 做相同 outstanding window 的公平对照，而不是把同步
   POSIX API 强行改成非阻塞。

