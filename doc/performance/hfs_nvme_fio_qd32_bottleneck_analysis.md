# HFS 与原生 SPDK NVMe FIO 瓶颈分析

> **测试日期**：2026-09-09  
> **测试结果目录**：`/mnt/hfs-data/performance-stage3-nvme-fio-qd32`  
> **分析基线**：当前 `main`（`d8353c3`），VM 内 SPDK commit `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`

## 1. 结论摘要

本轮将并发度对齐为 HFS `numjobs=32` 与原生 SPDK `iodepth=32`，两者使用同一块真实 NVMe：

```text
PCIe controller 0000:0b:00.0
        │
        └── SPDK bdev Nvme0n1
                ├── HFS: LD_PRELOAD → POSIX psync → libhfs → SPDK blobstore
                └── raw: FIO spdk_bdev → SPDK bdev → NVMe
```

该控制器绑定 `uio_pci_generic`，因此本测试使用 SPDK bdev `Nvme0n1`，不是内核路径
`/dev/nvme0n1`。设备、SPDK 和 FIO engine 在两组测试中保持一致。

| Block size | HFS IOPS | raw SPDK IOPS | HFS/raw | HFS 带宽 | raw SPDK 带宽 |
|---:|---:|---:|---:|---:|---:|
| 4 KiB | 46,523 | 117,821 | 39.5% | 181.7 MiB/s | 460.2 MiB/s |
| 8 KiB | 26,932 | 108,364 | 24.9% | 210.4 MiB/s | 846.6 MiB/s |
| 16 KiB | 18,957 | 91,602 | 20.7% | 296.2 MiB/s | 1,431.3 MiB/s |
| 32 KiB | 17,914 | 72,022 | 24.9% | 559.8 MiB/s | 2,250.7 MiB/s |
| 64 KiB | 15,870 | 52,123 | 30.5% | 991.9 MiB/s | 3,257.7 MiB/s |

所有 workload 的错误数为 0，测试结束后没有残留 FIO 进程。与前一轮 HFS QD1 对照相比，
4 KiB HFS 吞吐从约 23.1K 提升到 46.5K IOPS，但仍只有 raw SPDK 的 39.5%。这说明提高
并发能够部分隐藏单请求开销，却不能消除 HFS 数据面额外的复制、锁、排队和同步等待。

当前请求的有效耗时可以近似拆成：

```text
T_HFS = T_user_copy + T_dma_pool + T_range/sync_lock
      + T_runtime_queue + T_SPDK_submit/completion
      + T_metadata_publish + T_waiter_wakeup

T_raw = T_FIO_DMA_buffer + T_SPDK_submit/completion
```

raw SPDK 没有文件语义、inode 元数据、POSIX 锁和 HFS owner waiter，因此差距主要来自
HFS 上层路径，而不是 NVMe 介质本身。以下四个瓶颈是当前最重要的残余成本。

## 2. 瓶颈一：普通用户 buffer 触发 bounce copy

### 2.1 现象

HFS 的同步 preload 路径收到的是 FIO 普通用户地址。只有同时满足“完整 unit 对齐、长度可处理、
`spdk_vtophys()` 能映射整段地址”时，HFS 才能把调用方 buffer 直接交给 SPDK；否则必须从
固定 DMA pool 借出 buffer。

核心判定在 `src/hfs_io.c:884` 的 `hfsIoBufferDmaCapable()` 和 `src/hfs_io.c:892`
的 `hfsIoVectorDmaCapable()`。当前 FIO `psync` 默认 buffer 没有保证为 SPDK DMA memory，
所以普通读写会走 bounce 路径。

### 2.2 实际执行路径

写请求的典型流程是：

1. `hfsIoWriteAt()` 获取 inode synchronization read lock（`src/hfs_io.c:2188`）。
2. `hfsIoWriteAtLocked()` 为每个 pipeline child 从 DMA pool 获取 lease。
3. 在 `src/hfs_io.c:2097-2099` 把用户 buffer `memcpy()` 到 DMA buffer。
4. `hfsIoStartRequest()` 将 DMA buffer 提交到 data reactor/SPDK。
5. completion 后释放 lease；用户写入的可见性还要经过 inode 元数据发布。

读请求反向执行：SPDK 先写入 DMA buffer，然后在 `src/hfs_io.c:1969-1970` 将结果
`memcpy()` 回用户 buffer。非对齐首尾还会额外执行 unit 级 read-modify-write，见
`src/hfs_io.c:1788-1798`。

因此，一次普通 aligned FIO I/O 至少增加一次 memcpy；读写各自都增加一个内存带宽消耗，
并且 copy 发生在 I/O completion 之前/之后的关键路径上。raw SPDK FIO engine 则直接使用
可 DMA 的 buffer，不需要这两次用户态拷贝。

### 2.3 为什么 QD32 仍然受影响

4 KiB QD32 时，单次 copy 的绝对字节数不大，但请求数达到数万 IOPS，copy、cache miss、
线程调度和 pool lease 管理会叠加到每个 I/O。64 KiB 时单请求 copy 成本更大，且单个
request 会占用更大的 DMA class，进一步放大 pool 压力。

当前已经提供 registered DMA API 和 direct async path，但这只覆盖显式注册或已能通过
`spdk_vtophys()` 的 buffer；普通 POSIX/FIO buffer 仍会 fallback 到 bounce path。因而本轮
测试测到的是兼容性最强、也是最常用的 fallback 路径，而不是 registered-DMA 最优路径。

### 2.4 优化方向

**P0：扩大真正的 direct DMA 覆盖面**

- 为 preload/FIO 路径提供可选的 DMA-aware allocator，或在应用初始化时注册稳定的大块 buffer。
- 让 aligned full-unit `pread/pwrite` 在 buffer 可映射时保持异步 outstanding，不因为同步 API
  立刻退回单请求等待。
- 对 direct path 增加统计：命中率、`spdk_vtophys` 失败原因、direct/fallback 字节比例。

**P1：降低 fallback copy 成本**

- 保持 per-reactor pool，避免跨 reactor 借还 buffer。
- 合并连续 child 的 copy，使用更大的连续 DMA chunk；在不改变 POSIX 原子性的前提下减少
  `memcpy()` 次数。
- 对 4 KiB 小请求评估非临时写、批量 copy 或 cache-line 对齐，避免为了少量数据触发更重的
  1 MiB class。

**验收指标**

- 普通 malloc buffer 与 registered-DMA buffer 分别跑 QD1/QD32。
- 记录 `io_copy_bytes`、`io_copy_ops`、`io_direct_bytes`、direct 命中率。
- direct 命中率提高时，HFS/raw IOPS 比例应提升，而 p99 不应由 pool 等待恶化。

## 3. 瓶颈二：同步 waiter 和 owner/data reactor 排队

### 3.1 现象

HFS 对外暴露的是同步 POSIX 读写。虽然内部有 data reactor、batch dispatcher 和 async
libhfs，但 `read()`/`write()` 必须在返回前确认 SPDK completion。一个同步请求因此要经历：

```text
调用线程
  └─ hfsIoStartRequest()
       └─ batch queue / spdk_thread_send_msg()
            └─ data reactor 执行 hfsIoStart()
                 └─ SPDK blob I/O
                      └─ hfsIoDone()
                           └─ futex/cond completion
                                └─ 调用线程醒来
```

`src/hfs_io.c:1118` 的 `hfsIoWaitRequest()` 最终调用 `hfsRequestWaitResult()`；
`src/hfs.c:193-203` 再通过 `hfsRuntimeCompletionWait()` 阻塞调用线程。raw SPDK FIO 则由
FIO engine 直接维护 outstanding queue，不为每个请求经过 HFS 的 POSIX waiter。

### 3.2 队列和同步的代码证据

- `src/hfs_io.c:704-735`：请求被包装成 `HfsIoRequest`，再通过 owner/data batch dispatcher
  投递。
- `src/hfs_runtime.c:193-293`：owner/data reactor 批量取出消息并调用 SPDK start callback。
- `src/hfs_runtime.c:803-871`：每个 data reactor 仍需要 mutex、batch queue 和一次通知。
- `src/hfs_runtime.c:873-944`：owner-side batch 只合并通知，不能消除每个 request 的 callback、
  completion 和 buffer 生命周期。
- `src/hfs.c:105-203`：completion 状态写入、唤醒、超时和错误转换都位于同步路径。

batch dispatcher 已修复“每个 I/O 一次跨线程通知”的结构性问题，但它只减少通知次数，不能
把同步 POSIX 调用变成 raw SPDK 的异步 outstanding queue。QD32 下，32 个 FIO worker 会同时
把请求推向有限的 reactor 和 DMA pipeline；一旦 reactor 或 pool 排队，所有等待该请求的
worker 都会把排队时间反映为 p99。

### 3.3 QD32 数据如何证明这一点

| Block size | HFS p99 读 | HFS p99 写 | raw SPDK p99 读 | raw SPDK p99 写 |
|---:|---:|---:|---:|---:|
| 4 KiB | 2.57 ms | 6.39 ms | 0.63 ms | 0.64 ms |
| 8 KiB | 3.10 ms | 20.32 ms | 0.67 ms | 0.68 ms |
| 16 KiB | 2.02 ms | 27.92 ms | 0.75 ms | 0.76 ms |
| 32 KiB | 0.92 ms | 13.43 ms | 0.86 ms | 0.86 ms |
| 64 KiB | 43.25 ms | 68.68 ms | 1.11 ms | 1.11 ms |

raw SPDK 的 p99 也会随着队列深度上升，但仍在约 0.6～1.1 ms；HFS 在 64 KiB 时出现
43～69 ms 的尾延迟，说明排队和资源等待远大于 NVMe completion 本身。

### 3.4 优化方向

**P0：将兼容层和高性能层明确分离**

- 对数据库、高吞吐 FIO 等调用方，优先使用 `hfs_lib_async_*`，让 worker 不等待 SPDK child。
- 为 preload 增加可控的 per-fd/per-process outstanding window，在仍满足 POSIX 返回语义的前提下，
  让多个请求在一次同步调用批次中并行飞行。
- 把 completion 线程唤醒从“每 child 一次”优化为“batch/parent 一次”，减少 futex 竞争。

**P1：继续减少跨线程提交成本**

- 让提交队列具备 per-reactor single-producer/single-consumer 快路径，降低 batch mutex 竞争。
- 统计并拆分 `runtime_queue_wait_ns`、`runtime_owner_ns`、`data_reactor_queue_wait_ns` 和
  `completion_wakeup_ns`，避免只看总 p99。
- 对同一 reactor 的连续 request 批量调用 SPDK submit，completion 只推进 parent state machine。

**验收指标**

- QD1 与 QD32 分别测同步 preload、async libhfs、raw SPDK。
- 在 QD32 下，HFS 的 queue wait 不应随 block size 非线性增长；64 KiB p99 目标先降到 10 ms
  以下，再逐步接近 raw SPDK 的 5 倍以内。
- 所有 `submit`/`complete`、async parent/child 和 worker 引用必须严格闭合。

## 4. 瓶颈三：range lock 和 inode synchronization 的锁持有范围

### 4.1 锁的目的与代价

HFS 必须保证同一文件的并发写、RMW、truncate、punch hole 和 fsync 之间的 POSIX 语义，
因此不能像 raw SPDK 一样只提交一个无文件语义的 block request。当前数据面至少有两层同步：

1. `HfsInodeRuntime.syncLock`：把普通 I/O 与 truncate/fsync/resize 等 inode 级操作隔离。
2. `fs->rangeLocks[256]` 与每 inode `rangeGate`：保护 byte range，避免重叠 unit 的 lost update。

`src/hfs_io.c:327-338` 获取 inode read side；`src/hfs_io.c:182-238` 根据 unit 哈希收集
range bucket；当一个大范围覆盖至少四分之一 bucket 时，改用 `rangeGate` 独占锁。

### 4.2 当前同步路径的锁生命周期

- `hfsIoReadAt()` / `hfsIoWriteAt()` 先获取 inode `syncLock`，见 `src/hfs_io.c:2148-2220`。
- 非对齐请求在整个 RMW 期间持有 range guard。
- 对齐 bounce pipeline 按 window 获取 range reservation，提交 child、等待 completion、复制
  数据后才释放，见 `src/hfs_io.c:1916-1975` 和 `src/hfs_io.c:2048-2134`。
- direct async 使用 `hfsIoRangeTryLock()`，完成回调 `hfsIoDirectAsyncDone()` 才释放 guard，
  见 `src/hfs_io.c:976-1020`。

这套设计首先保证正确性，但锁的持有时间包含了跨线程投递、DMA pool 获取、SPDK completion
和用户 buffer copy。对于不重叠的请求，逻辑上它们本可以并行；当不同 logical unit 哈希
到同一 bucket 时，还会出现哈希冲突造成的伪竞争。

### 4.3 QD32 下的表现

HFS 在 4 KiB QD32 还有 46.5K IOPS，说明小范围请求尚能通过 256 个 bucket 和短 reservation
扩展；但 64 KiB 请求的 p99 突然升至 43～69 ms，说明一个请求覆盖更多 unit 后：

- 需要收集更多 bucket，冲突概率上升；
- 多个 child 被同一个 window reservation 串住；
- 写请求在释放 range guard 前还要完成 DMA copy 和 SPDK completion；
- 任何慢请求都会阻塞同一 inode/stripe 的后续 worker。

大范围 `rangeGate` 已避免“收集全部 256 个全局 bucket”造成 mount 级阻塞，但它仍然是单 inode
级排他门，不能把同一 inode 的不重叠大范围 I/O 并行化。

### 4.4 优化方向

**P0：分离 reservation 与 completion 生命周期**

- 对 aligned、full-unit、已确认不重叠的 I/O，只在提交前保留短 reservation；提交到 SPDK 后
  立即释放可安全释放的锁。
- 对同一 unit、非对齐 RMW、truncate、punch hole 保留 completion 级独占锁，不能为了吞吐破坏
  lost-update 和零填充语义。
- 为请求记录 `[firstUnit,lastUnit]`，只锁实际逻辑范围，不让 64 KiB/1 MiB 请求过度放大锁集合。

**P1：从全局哈希 bucket 迁移到 per-inode stripe**

- 每个 inode 建立固定数量的 stripe lock，stripe 由 logical unit 选择。
- 大范围请求按 stripe 分段提交，而不是持有一个覆盖整个 inode 的 gate。
- 保留全局 bucket 仅用于兼容性/过渡，逐步减少跨 inode 的哈希碰撞。

**验收指标**

- 同一 inode 不重叠 aligned write 的 lock hold time 和 contention 显著下降。
- overlapping write、RMW、truncate、punch hole 的数据校验保持不变。
- 64 KiB QD32 p99 先下降 30%，并且不以增加 lost update、脏读或 completion 泄漏为代价。

## 5. 瓶颈四：固定 DMA pool、pipeline window 和 backpressure

### 5.1 当前资源模型

当前每个 data reactor 有四个固定 DMA class，定义在 `src/hfs_io.c:77-85`：

| class | 单 buffer | 默认 buffer 数 | 典型请求 |
|---:|---:|---:|---|
| 0 | 4 KiB | 32 | 4 KiB I/O |
| 1 | 16 KiB | 16 | 8/16 KiB I/O |
| 2 | 64 KiB | 32 | 32/64 KiB I/O |
| 3 | 1 MiB | 8 | 大块 pipeline |

申请路径 `hfsIoAcquire()`（`src/hfs_io.c:509-555`）在 class 没有空闲 lease 时通过条件变量
等待；异步路径 `hfsIoTryAcquire()` 则返回 `EAGAIN`。释放时需要重新获取该 shard 的 mutex
并广播等待者（`src/hfs_io.c:581-616`）。

pool 已经按 data reactor 分片，消除了早期全局 pool 锁；但它仍是有界资源，且每个 request
在 SPDK completion 前一直占用一个 lease。默认 pipeline depth 为 8，最大 16；32 个 FIO job
同时进入时，实际可用 lease 数、pipeline window 和 data reactor 数共同决定有效 outstanding
数量。

### 5.2 为什么 64 KiB 更容易放大尾延迟

64 KiB 请求使用 64 KiB class。每个 child 持有一个 lease，若其对应 reactor 上的 32 个 lease
都被占用，后续请求只能：

1. 在 `pthread_cond_wait()` 上睡眠；
2. 等待前一批请求完成并释放 lease；
3. 重新获得 lock 后复制用户数据，再提交 SPDK。

这产生典型的 head-of-line blocking：慢的一个 child 占住 class slot，后面的 request 即使
访问不重叠的 LBA，也可能因为同一个 reactor/class 没有 buffer 而等待。64 KiB QD32 的 p99
读/写达到 43/69 ms，正是 pool、range reservation 和同步 waiter 叠加后的表现。

### 5.3 优化方向

**P0：credit-based、有界且可观测的 backpressure**

- 在进入 range lock 和 runtime queue 前先预留 DMA credit，无法预留时返回明确 `EAGAIN` 或
  进入独立的有界 request queue。
- 不要在持有 inode/range lock 时无限等待 `pthread_cond_wait()`；否则 pool 枯竭会把锁竞争
  扩散到所有线程。
- 为每个 reactor/class 暴露 `active_peak`、`wait_count`、`wait_ns`、`exhaust_count` 和
  `queue_depth_peak`。

**P1：按负载动态调节 class 和 pipeline**

- 64 KiB class 根据 reactor 数和有效 QD 动态增加到 32～64 个，但受 `HFS_IO_POOL_BYTES`
  总内存上限约束。
- 当 `dma_wait_ns` 或 p99 超过阈值时，自动缩短 `HFS_IO_PIPELINE_DEPTH`；资源充足且 queue
  wait 低时再扩大 window。
- 对大请求使用一次更大的 DMA lease，避免拆成过多 64 KiB child；对小请求禁止误用 1 MiB
  class，保持当前 size-class 选择策略。

**验收指标**

- 64 KiB QD32 p99 降低至少 30%。
- pool 耗尽时无死锁、无静默丢 I/O；错误路径只产生明确 `EAGAIN`/`ENOMEM`。
- `lease acquire/release`、SPDK submit/complete、parent/child completion 数量严格相等。

## 6. 交叉放大项：metadata publish 和文件语义

上述四类瓶颈不是完全独立的。写请求完成后，HFS 还必须维护 inode size、mtime、ctime、dirty
状态和 fsync 的持久化顺序。当前代码已经把非扩展写的大部分 timestamp 更新改为 deferred
overlay：`src/hfs_lib.c:555-577` 的 `hfsPublishWriteEndFast()` 只更新 runtime 原子状态；
只有扩展写、truncate 或 fsync 才需要进入 metadata lock/commit 路径。

这一优化已消除“每次写都获取全局 metadata write lock”的主要问题，但它仍然带来两个残余
成本：

1. 新文件或扩展写需要先确认 blob capacity，再发布 size；
2. fsync 必须先完成数据，再提交 metadata transaction，最后执行 bdev flush，不能像 raw
   SPDK FIO 一样只统计 data completion。

因此，metadata publish 应与数据面统计分开，不应把所有 HFS p99 都简单归因于 NVMe latency。
数据库 fillseq/WAL、短写和频繁 fsync 会比本轮纯 FIO 更容易放大这一项。

## 7. 优先级与实施顺序

| 优先级 | 工作项 | 目标 | 主要风险 |
|---|---|---|---|
| P0 | 普通 buffer direct-DMA 覆盖与命中率统计 | 消除读写双向 memcpy | DMA 生命周期、fork/close 竞态 |
| P0 | pool credit 与锁外 backpressure | 消除 64 KiB pool 队头阻塞 | 资源不足时 POSIX 错误语义 |
| P1 | completion/owner queue 批量化 | 降低 waiter、futex 和跨线程通知成本 | shutdown 与 callback 顺序 |
| P1 | aligned I/O 短 reservation | 降低 range lock hold time | overlapping write 原子性 |
| P1 | per-inode stripe lock | 降低哈希碰撞和大范围 gate 排队 | 锁顺序、truncate/punch 交叉 |
| P2 | metadata group commit / preallocation | 降低数据库短写和 WAL 放大 | fsync durability 语义 |

建议每完成一个工作项都保留以下固定矩阵：

```text
buffer: malloc / registered DMA
qd:     1 / 32
bs:     4K / 8K / 16K / 32K / 64K
rw:     randread / randwrite / randrw(50/50)
stats:  off（主结果）/ on（归因结果）
check:  errors、SPDK submit=complete、lease acquire=release
```

主结果必须使用相同 NVMe、相同 FIO binary、相同 runtime、相同 CPU/reactor 配置；统计开启
只用于归因，不能与 stats-off 的吞吐直接混比。

## 8. 当前判断

HFS 已完成 data reactor、batch dispatcher、per-reactor DMA pool、延迟 metadata publish、
短 range reservation 和 registered-DMA direct path 等结构性修复；本轮结果证明这些修复
有效地把 4 KiB QD1 到 QD32 的吞吐提高了一部分，但当前通用 POSIX/FIO buffer 仍走兼容性
bounce 路径，且同步 API 仍需要等待 HFS runtime/SPDK completion。

因此，当前最有价值的下一步不是继续调整 NVMe 参数，而是：

1. 先测量并扩大普通 buffer 的 direct-DMA 命中率；
2. 在锁外建立有界 DMA credit/backpressure；
3. 再缩短 aligned I/O 的 lock 和 completion 生命周期；
4. 最后用数据库 WAL/fsync 负载验证 metadata group commit 是否兑现收益。

这四步完成后，才有理由期待 HFS 在同一真实 NVMe 上接近 raw SPDK 的吞吐，同时保持当前
POSIX、崩溃恢复和数据库兼容性边界。
