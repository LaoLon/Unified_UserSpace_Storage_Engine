# HFS 数据库 POSIX 兼容路径优化 Shard 开发说明书

> **状态**：待开发
> **Shard**：shard-61
> **目标对象**：MySQL/MariaDB、RocksDB 以及不能修改 I/O API 的普通 POSIX 应用
> **唯一技术路线**：POSIX Compatibility Engine（无锁 DMA pool 快速获取 + reactor 批量提交
> + 延迟元数据发布 + fdatasync/fsync group commit）
> **当前基线**：`main@d4d85c0`，VM 内 SPDK 使用锁定 commit
> `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`

## 1. 方案决策

数据库应用必须继续使用 `open/read/write/pread/pwrite/fsync/fdatasync/fallocate/rename`
等完整 POSIX 语义，不能要求它们改用 `hfsDmaAlloc()` 或 HFS async API。因此本 shard 不再
尝试把普通用户 buffer 包装成伪异步零拷贝，而是在同步 POSIX 边界内建立一条专用的
**POSIX Compatibility Engine**：

```text
POSIX syscall
    │
    ├─ 直接使用可验证 DMA buffer（若 spdk_vtophys 成功）
    └─ 普通 buffer：无锁 fast-acquire DMA staging
          │
          ├─ 单次必要 copy
          ├─ data-reactor batch submit
          ├─ futex completion wait
          └─ read copy-out / write return

写入完成后只更新 runtime dirty epoch；
fdatasync/fsync 由 durability coordinator 做有界 group commit。
```

该路线的边界非常明确：普通 POSIX buffer 的 copy 在无法被 SPDK 直接寻址时是必要成本，
本 shard 只消除 copy 之外的锁、队列、请求通知、metadata 和 barrier 冗余，不改变 POSIX
可见顺序和错误语义。

## 2. 为什么不使用其他做法

- 不修改 MySQL/RocksDB 的 buffer 分配器；数据库的 buffer pool/WAL cache 与 HFS cache
  重复，透明加一层 page cache 会增加内存占用和一致性风险。
- 不把同步 `pread/pwrite` 包装成 callback + condition variable；该原型在 NVMe A/B 中比
  当前同步 bounce 路径慢约 15%～30%。
- 不做 write-behind 后台写回；普通 `write()` 的可见顺序、read-after-write、错误返回和
  `fsync` 边界会变得难以证明。
- 不删除 RMW、size publish、range protection 或设备 flush；这些是 POSIX/数据库恢复所需
  的语义，而不是冗余请求。

## 3. 当前瓶颈证据

当前普通路径已经完成 data-reactor batch dispatcher 和延迟 mtime/ctime 更新，但数据库
工作负载仍承受以下成本：

1. 每次 bounce I/O 都要在 reactor-local DMA pool 上取得/释放 lease，并在 pool mutex 下
   维护 free list；高并发时会把调用线程阻塞在 pool condition 上。
2. 每个同步 syscall 都独立经过 inode runtime、range stripe、reactor batch 和一次
   completion wait；不能跨 syscall 共享提交上下文。
3. `fdatasync` 当前等同于 `fsync`，会走更强的 metadata/barrier 顺序；RocksDB WAL 和
   MySQL redo log 会频繁触发该路径。
4. 多个并发数据库线程各自调用 `fsync` 时，可能重复 drain、metadata commit 和 device
   flush；同一 durable epoch 可以由一个 leader 合并完成。
5. 非对齐或未预分配范围仍需要 RMW/resize；这些必须保留，但可通过已有 logical EOF、
   fallocate fast-hit 和请求统计避免在 steady state 重复进入慢路径。

## 4. POSIX Compatibility Engine 设计

### 4.1 数据 I/O：无锁 DMA pool fast path

保留现有 reactor-local DMA pool 的容量和 backpressure 语义，改造每个 pool class 的空闲
slot 获取方式：

- 每个 class 使用最多 32 个 slot 的 32-bit `freeMask`；bit=1 表示 slot 可用。
- uncontended acquire 使用 CAS 清除最低可用 bit，release 使用 atomic OR 归还 bit，
  不进入 `pthread_mutex`。
- pool closing、active lease 计数和等待条件仍由慢路径 mutex/condition 保护；关闭时先
  禁止新 acquire，再等待 active lease 为零，避免 fast path 与 SPDK free 竞态。
- freeMask 没有可用 bit 时才进入现有 condition wait；`EAGAIN`、`ESHUTDOWN` 和错误注入
  语义保持不变。
- 统计新增 `dma_fast_acquire`、`dma_slow_wait`、`dma_fast_release`、`dma_free_mask_peak`，
  使 pool 锁竞争与真实设备延迟分离。

该改造不改变 buffer 的所有权：同步函数返回前 lease 仍归当前请求持有，读 copy-out 完成
后才能 release；只是把无竞争的 mutex 往返变成原子位操作。

### 4.2 数据 I/O：同步请求保持单一等待边界

普通 POSIX 路径继续使用当前 `HfsIoRequest` + `HfsRequestResult` 的 request-local 状态，
不引入新的异步 callback 包装：

- aligned 小请求：一个 DMA lease、一个 SPDK logical request、一次必要 copy；
- aligned 大请求：沿用 bounded pipeline，按已有 `HFS_IO_PIPELINE_DEPTH` 提交 child，
  仍只在整个 POSIX syscall 返回前完成 copy/lease release；
- 非对齐请求：只对首尾 unit 做 RMW，中间完整 unit 不重复读；
- data reactor 继续使用 batch dispatcher，避免每个 child 单独 `spdk_thread_send_msg`；
- completion 使用已有 futex，禁止在普通路径新增 per-request pthread cond/malloc。

### 4.3 元数据：写入与 timestamp 解耦

沿用当前 runtime 的 `logicalSize`、`pendingMtimeNs`、`pendingCtimeNs` 和 dirty inode 集合：

- 不扩展文件容量的 aligned write 只更新 runtime dirty epoch，不获取全局 metadata write lock；
- 只有扩展写、truncate、rename、unlink、属性变更需要进入 metadata transaction；
- `stat/fstat` 在 checkpoint 前叠加 pending timestamp，保证进程内观察到完整 POSIX 结果；
- `close()` 不作为 durability acknowledgement，仍由显式 fsync/fdatasync 决定持久化边界。

### 4.4 Durability coordinator：fdatasync/fsync group commit

新增 mount-local `HfsDurabilityCoordinator`，用 epoch 合并并发同步请求：

```text
write/pwrite 完成
      │
      └─ 标记 inode dirtyEpoch=N

多个线程调用 fdatasync/fsync
      │
      ├─ 加入同一个 targetEpoch=N
      ├─ 一个线程成为 leader
      ├─ leader drain 该 epoch 的数据请求
      ├─ 提交必要的 data/blob metadata
      ├─ 提交 inode size/intent/stable image
      ├─ 执行设备 barrier
      └─ 广播同一结果给所有 waiter
```

规则：

- `fsync(fd)` 必须覆盖该 fd 在调用前已成功返回的写入，并可同时使其他已进入同一 epoch
  的 dirty inode durable；这是当前模型已经允许的更强语义。
- `fdatasync(fd)` 新增 data-only 语义：刷新数据、分配/size 等读取数据所需 metadata，
  不为未改变 size 的文件强制提交 mtime/ctime；若 size 变化，size 仍必须进入事务。
- 一个 epoch 只允许一个 leader 执行 drain/commit/barrier；其他 caller 等待同一个 futex
  结果，不重复执行 flush。
- metadata intent/stable/clear 顺序和错误恢复协议不变；任何阶段失败，epoch 中所有 waiter
  都得到 `EIO`，dirty 状态保留以便后续 fsync 重试。
- 当前两道 barrier 的正确顺序先保持不变；只有在故障注入和 crash recovery 证明安全后，
  才能合并冗余 data sync/barrier。
- `fsync`、`fdatasync`、目录 fsync 和 close/shutdown 的生命周期必须共用 coordinator drain，
  防止 owner/blob 在 waiter 仍存在时卸载。

## 5. 实现分解

### 5.1 shard-61.1：pool lock-free fast acquire

- 改造 `HfsIoPoolClass` 的 free list 为原子 `freeMask`。
- 增加 acquire/release fast path 和 closing/active 的安全协议。
- 保留 `HFS_IO_POOL_CLASS_COUNTS`、`HFS_IO_POOL_BYTES` 配置和 pool wait 回退。
- 新增 pool contention smoke，覆盖 1/4/16/32/64 个 POSIX 线程、4/16/64 KiB。

### 5.2 shard-61.2：POSIX request hot-path 收口

- 审计 aligned/unaligned/vector/large request 的 copy、lease、range 和 completion 生命周期。
- 确保 aligned 4/16 KiB 数据库 I/O 不创建额外 async request/context，不进入
  `HFS_POSIX_DIRECT_ASYNC` bridge。
- 为每个 logical POSIX request 输出 `logical_ops`、`spdk_submits`、`spdk_completes`、
  `copy_in/out`、`rmw_reads/writes` 和 `resize_requests`。
- 对 preallocated steady-state workload 验证 resize fast-hit，不得把必要的首次分配当成
  可删除请求。

### 5.3 shard-61.3：durability coordinator 和 fdatasync fast path

- 新增 `src/hfs_durability_coordinator.[ch]`，实现 epoch、leader、waiter 和失败传播。
- 将 `hfs_lib_fsync` 接入 coordinator，保证同一 epoch 只做一次 metadata drain/commit。
- 新增独立 `hfs_lib_fdatasync` 内部实现和 preload 路由，正确处理 size/extent metadata。
- 增加并发 fsync、fdatasync、跨文件 dirty inode、metadata failure、flush failure、
  crash-step 1/2/3 和后续 retry 测试。

### 5.4 shard-61.4：数据库回归和 NVMe 性能收口

- MySQL/MariaDB：redo log、`O_DIRECT` 16 KiB data page、频繁 fsync、事务提交、并发 client。
- RocksDB：WAL fdatasync、SST 顺序写、flush/compact、direct I/O、SIGKILL recovery。
- FIO：普通 POSIX `psync`，4/8/16/32/64 KiB，numjobs 1/4/16/32；单独记录 pool
  fast/slow、range wait/hold、metadata epoch wait 和 barrier 次数。
- NVMe 对照必须使用同一 `Nvme0n1`；Malloc/Aio 只作为 correctness/回归后端。

## 6. 验收标准

### 6.1 语义和可靠性

- MySQL/MariaDB 与 RocksDB 的既有 CRUD、WAL、事务、flush/compact、重启和 SIGKILL recovery
  全部通过，errors=0。
- `fsync` 和 `fdatasync` 的故障注入均返回正确 errno；失败 epoch 不清除 dirty 状态，
  下一次 barrier 可以重试成功。
- 跨文件 fsync、目录 fsync、truncate/rename/unlink、append 和 concurrent positioned I/O
  不出现数据丢失、旧数据覆盖或死锁。
- pool fast path、lease、SPDK submit/complete、metadata epoch waiter 全部闭合；卸载无
  open blob、UAF 或 shutdown 超时。

### 6.2 性能

- uncontended pool acquire/release 不再进入 pthread mutex；slow wait 只在 class exhausted
  时出现。
- 普通 aligned POSIX I/O 每个方向最多一次必要 copy；`copy_ops` 不因本 shard 增加。
- 4/16 KiB QD32 FIO 相比当前 POSIX 基线吞吐提升至少 15%，p99 不恶化；若设备或锁仍主导，
  必须由统计给出可复现的剩余原因。
- 8/16 KiB 数据库 WAL/写入场景的 metadata lock 和 barrier 次数显著低于逐请求路径；
  group commit waiter 数与 leader 数可核对。
- `fdatasync` 频繁 workload 的 p99 至少下降 20%，且 crash/recovery 证据完整。

## 7. 明确不改变的产品边界

- 普通 POSIX buffer 仍可能发生用户 buffer↔DMA staging copy；本 shard 不承诺透明零拷贝。
- mmap、native libaio/io_uring、多进程共享 HFS mount 仍不在支持范围。
- 高性能应用继续使用 shard-60 的 DMA async ring；数据库兼容路径不调用该 API。
- `HFS_POSIX_DIRECT_ASYNC` 继续默认关闭，不作为数据库性能方案。

## 8. 观测字段和回滚

新增或统一以下统计字段：

```text
posix_ops, posix_block_ns, posix_completion_wait_ns
dma_fast_acquire, dma_slow_wait, dma_fast_release, dma_active_peak
copy_in_bytes, copy_out_bytes, copy_ops
range_wait_ns, range_hold_ns, runtime_queue_wait_ns
resize_checks, resize_fast_hits, resize_requests
metadata_epoch_requested, metadata_epoch_leader, metadata_epoch_waiters
fdatasync_count, fsync_count, device_flush_count, durability_errors
```

回滚策略：

- `HFS_POSIX_COMPAT_FAST=0` 回退 pool 原始 mutex acquire；
- `HFS_DURABILITY_GROUP_COMMIT=0` 回退当前已验证的同步 durability 顺序；
- `HFS_FDATASYNC_DATA_ONLY=0` 让 fdatasync 继续使用当前等价 fsync 语义；
- 任何错误注入、crash recovery 或数据库一致性回退都关闭新增开关，不影响普通 POSIX
  正确性路径。

## 9. 完成定义

shard-61 只有在以下条件全部满足后才能标记完成：

1. pool lock-free fast path、POSIX request 统计、durability coordinator 和 fdatasync 路由
   已合入 `main`。
2. VM 上 Make/CMake、POSIX contract、metadata transaction、error injection、MariaDB/
   RocksDB recovery 和关闭路径全部通过。
3. NVMe FIO 与数据库矩阵达到性能门槛，原始 JSON、stats、trace 和环境信息已保存。
4. `doc/bug.md`、`doc/design/fio_baseline_notes.md`、durability 文档和本说明书已同步。
5. 独立提交后在 `main` 上复核 Git 状态、VM 构建和回归结果。
