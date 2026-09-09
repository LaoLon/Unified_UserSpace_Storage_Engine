# HFS 用户 Buffer Bounce Copy 优化 Shard 计划

> **状态**：shard-54/55 已实现，shard-56 copy/lease lifetime 开发中
> **计划范围**：shard-54 ～ shard-59
> **输入评审**：[`review/hfs_user_buffer_bounce_copy_optimization.md`](../../review/hfs_user_buffer_bounce_copy_optimization.md)
> **当前基线**：`main@4054111`，VM 内 SPDK 使用锁定 commit `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`

## 1. 背景与问题定义

当前 HFS 在普通 POSIX/preload buffer 上不能保证 `spdk_vtophys()` 成功，因此大多数 FIO
和数据库同步 I/O 走 bounce path：

```text
POSIX user buffer
    ├─ DMA probe 失败
    ├─ reactor-local DMA lease
    ├─ write: memcpy(user → DMA)
    ├─ runtime/data-reactor batch
    ├─ SPDK blob I/O
    ├─ completion/waiter
    └─ read: memcpy(DMA → user)
```

同一块真实 NVMe、同一 `randrw 50/50`、`numjobs=32` 对比 `iodepth=32` 的结果为：

| Block size | HFS IOPS | raw SPDK IOPS | HFS/raw | HFS p99 读/写 |
|---:|---:|---:|---:|---:|
| 4 KiB | 46,523 | 117,821 | 39.5% | 2.57 / 6.39 ms |
| 8 KiB | 26,932 | 108,364 | 24.9% | 3.10 / 20.32 ms |
| 16 KiB | 18,957 | 91,602 | 20.7% | 2.02 / 27.92 ms |
| 32 KiB | 17,914 | 72,022 | 24.9% | 0.92 / 13.43 ms |
| 64 KiB | 15,870 | 52,123 | 30.5% | 43.25 / 68.68 ms |

当前不能直接把全部差距归因于 memcpy。需要先回答四个问题：

1. 每个 logical I/O 实际发生了多少次 copy？
2. 每个 logical I/O 实际提交了多少个 SPDK child request？
3. lease、range lock、runtime queue 和 waiter 分别等待了多久？
4. 哪些请求放大是 POSIX/RMW/fsync 必需的，哪些只是实现路径重复？

因此本阶段明确采用“**先 tracepoint，后优化**”策略。没有 trace 证据的优化不得直接合入
性能主线；所有优化必须保留正确性和回滚开关。

## 2. 阶段目标与非目标

### 2.1 阶段目标

- 建立低开销、默认关闭的 request tracepoint，能重建一次 I/O 从 API 入口到 SPDK completion
  的完整时间线。
- 区分 direct、registered、DMA lease bounce、非对齐 RMW、resize、metadata publish 和
  punch zero fallback。
- 先去除普通 bounce request 不必要的 registry lock、过长 lease 生命周期和 pool 队头阻塞。
- 在有明确 buffer 生命周期的情况下扩大 direct-DMA 覆盖面，并让 direct path 使用 bounded
  async window。
- 只消除不影响 POSIX 语义的 copy/请求；RMW、fsync durability barrier 和必要的 size
  publish 必须保留。
- 以真实 NVMe、Malloc/Aio、MySQL/MariaDB、RocksDB 和既有 smoke 形成统一回归证据。

### 2.2 非目标

- 本阶段不实现 mmap、native libaio/io_uring 或多进程共享 HFS mount。
- 不全局替换 libc `malloc()`，不把未知应用的 heap 指针强行当作 SPDK DMA memory。
- 不为了吞吐删除 overlapping write、RMW、truncate、punch 和 fsync 的 POSIX 保护。
- 不在没有 trace 数据的情况下盲目增大 DMA pool、pipeline 或 reactor 数量。

## 3. 总体依赖图

```text
shard-54 Tracepoint / baseline
          │
          ▼
shard-55 Buffer ownership + registry fast path
          │
          ▼
shard-56 Copy/lease lifetime + backpressure
          │
          ▼
shard-57 Registered DMA + direct async window
          │
          ▼
shard-58 RMW/vector/resize/zero request reduction
          │
          ▼
shard-59 NVMe/FIO + database final closeout
```

每个 shard 独立提交，提交后立即合入 `main`；后续 shard 只能基于已合入的提交继续开发。

## 4. shard-54：I/O Tracepoint 与基线冻结

### 4.1 目标

在不改变请求行为的前提下，记录 copy、lease、锁、队列和 SPDK completion 的阶段耗时，
解决“平均 IOPS 下降但不知道是 copy、锁还是 queue”的归因问题。

### 4.2 实现设计

新增 `src/hfs_trace.[ch]`，不直接在热路径写文本文件：

- 默认关闭；只有设置 `HFS_TRACE_PATH` 或 `HFS_TRACE=1` 才启用。
- 每个 caller thread、owner reactor、data reactor 使用固定大小 ring，避免每个事件
  `malloc/free`。
- 事件写入只做 monotonic timestamp、固定字段写入和无阻塞 ring reserve；ring 满时丢弃事件
  并递增 `trace_drop`，禁止阻塞 I/O。
- 退出或显式 flush 时一次性写二进制/TSV；解析工具把 request ID 还原为阶段时间线。
- 记录 numeric ID，不记录用户 buffer 原始地址；inode/blob ID 可按配置脱敏。
- 当前支持 HFS_TRACE_SAMPLE=N；mask/filter 和周期性 flush 留给后续观测工具 shard，
  避免在第一版 tracepoint 中引入复杂配置解析。

建议事件结构：

```c
typedef struct HfsTraceEvent {
    uint64_t timestampNs;
    uint64_t requestId;
    uint64_t inodeId;
    uint64_t offset;
    uint32_t bytes;
    uint32_t units;
    uint32_t queueDepth;
    uint16_t reactorId;
    uint16_t rangeBucketCount;
    uint8_t operation;
    uint8_t phase;
    uint8_t bufferKind;
    uint8_t poolClass;
    int32_t result;
} HfsTraceEvent;
```

### 4.3 必须覆盖的 tracepoint

| 阶段 | 事件 | 目的 |
|---|---|---|
| API | `io_enter` / `io_exit` | logical I/O 总耗时、错误和返回字节数 |
| buffer | `buffer_classify` | DMA probe、registered hit、fallback 原因 |
| pool | `lease_wait_begin/end` | class 等待时间和 active peak |
| pool | `lease_acquire/release` | lease 生命周期与泄漏检查 |
| copy | `copy_in_begin/end` | write user→DMA 字节、耗时 |
| copy | `copy_out_begin/end` | read DMA→user 字节、耗时 |
| lock | `sync_lock_wait/hold` | inode synchronization 等待和持有范围 |
| lock | `range_lock_wait/hold` | bucket/gate 竞争和持有时间 |
| runtime | `enqueue/dequeue` | owner/data reactor queue wait |
| SPDK | `submit/complete` | child request 数、设备 completion latency |
| metadata | `resize/publish/fsync` | 请求放大和 metadata 依赖 |
| parent | `parent_start/child_start/child_done/parent_done` | pipeline fan-out 和 closure |

### 4.4 基线输出

新增解析脚本或工具输出以下指标：

```text
logical_io_count
spdk_child_count
request_amplification = spdk_child_count / logical_io_count
copy_in_bytes / copy_out_bytes / copy_ops
direct_bytes / bounce_bytes / registered_bytes
rmw_read_count / rmw_write_count
resize_count / metadata_publish_count / flush_count
pool_wait_ns / runtime_queue_wait_ns / range_wait_ns / sync_wait_ns
spdk_latency / completion_wakeup_latency
```

### 4.5 验收

- Trace-off 相对当前 stats-off FIO：IOPS 下降不超过 2%，p99 增加不超过 5%。
- Trace-on 在采样 `N=100` 时，4K/64K QD32 记录无 request ID 断裂，`submit=complete`、
  `lease acquire=release`、`parent pending=0` 全部闭合。
- 普通 aligned bounce、registered direct、非对齐 RMW、首次扩展写、fsync 和 punch 各有最小
  可复现样例。
- 解析结果能明确显示：copy 时间、pool wait、range wait、runtime queue wait 和 SPDK latency
  哪一项主导 p99。

### 4.6 失败止损

如果 trace-off 超过 2% 开销，改为更小的 event、采样或 per-thread aggregation；不得为了
观测把 trace 计时逻辑永久放进 stats-off 热路径。

### 4.7 当前实现记录（shard-54）

- 新增 src/hfs_trace.[ch]：默认关闭，使用 per-thread 固定 ring；设置
  HFS_TRACE_PATH 或 HFS_TRACE=1 后在进程退出时导出 TSV。
- HFS_TRACE_SAMPLE=N 按 parent/request ID 采样；ring 溢出只递增 thread-local dropped
  计数，不阻塞 I/O，也不记录用户 buffer 原始地址。
- 已接入 hfs_io.c 的 logical I/O enter/exit、buffer classify、DMA lease 生命周期、
  copy-in/out、SPDK submit/complete；hfs_runtime.c 已接入 batch enqueue/dequeue。
- direct eligibility 在 API 层计算一次并传入 locked path，避免 trace 分类新增一次
  spdk_vtophys() 探测。
- 新增 scripts/db/io_trace_smoke.sh，复用 request-local fast I/O smoke 并校验 trace
  header、parent/child 阶段、runtime 阶段和 copy-in/out 事件。
- 新增 scripts/db/hfs_trace_summary.py，可输出 logical I/O、child request 放大、copy
  字节、submit/complete 闭合、phase/buffer 分布及阶段 p50。
- VM 验证：make -B app lib、CMake Release、io_trace_smoke.sh 通过；trace-on 样例
  记录 421 条事件、13 类 phase，SPDK submit/complete 与现有数据校验均通过。

## 5. shard-55：Buffer ownership 与 Registry Fast Path

### 5.1 目标

去除 HFS-owned DMA lease 的无意义 registry 查询，并让同一个 caller buffer 的 DMA classification
可复用；不改变 direct buffer 生命周期安全性。

### 5.2 设计

在 `HfsIoRequest` 中增加明确的 buffer kind：

```text
CALLER_BUFFER       caller-owned，可能需要 registered lifetime ref
REGISTERED_BUFFER   caller-owned，已有 registration handle
DMA_LEASE_BUFFER    HFS-owned，lease 已经保证生命周期
VECTOR_BUFFER       iovec 生命周期由 request/调用者共同保证
```

改动要点：

1. `hfsIoAcquire()` 成功后将 request 标记为 `DMA_LEASE_BUFFER`。
2. `hfsIoStartRequest()` 只对 caller/registered buffer 调用 `hfsIoBufferAcquire()`。
3. 增加 `_Atomic uint32_t registeredBufferCount`；count 为 0 时直接跳过全局 registry mutex。
4. 对已注册 buffer 返回 immutable handle，避免每个 child 重新扫描链表。
5. direct、bounce、vector、async 和 shutdown 都复用同一 ownership 规则。

### 5.3 验收

- 普通 bounce request 不再触碰 `g_hfsIoBufferLock`。
- registered buffer 的 unregister 仍在 outstanding request 时返回 `EBUSY`。
- fork child、close、cancel、shutdown 无 use-after-free。
- Trace 显示 aligned bounce 每个 request 只有一次 buffer classify，不重复 probe。
- 4K QD32 CPU 时间和 runtime queue wait 相比 shard-54 基线下降；不得以 p99 回退为代价。

### 5.4 当前实现记录（shard-55）

- HfsIoRequest 在进入 SPDK submit 前先确定 buffer kind；HFS 自有 DMA lease 标记为
  DMA_LEASE，不会调用 caller registry 查询，也不会增加 registry active reference。
- caller/registered buffer 仍通过同一生命周期引用路径；命中 registry 后 request kind
  升级为 REGISTERED，completion 或 submit failure 统一释放引用。
- 增加原子 registration count：无任何注册 buffer 时直接跳过全局 registry mutex；注册、
  注销和 unmount 清理均在链表锁内更新 count。
- vector request 明确标记为 VECTOR，避免误走 contiguous buffer registry 路径。
- 下一步仍需用显式 DMA buffer 和 QD32 性能数据验证 registered 命中率及 mutex/CPU 降幅。

## 6. shard-56：Copy/Lease 生命周期与 Backpressure

### 6.1 目标

不改变 copy 字节数的前提下，尽早归还 DMA lease，避免 pool 枯竭、range lock 和 waiter 相互
放大；消除“持有锁等待 pool”的队头阻塞。

### 6.2 设计

#### Read path

当前同步 bounce read 在 pipeline 子请求全部完成后才集中执行 DMA→user copy。改为 child
completion 立即执行：

```text
SPDK read complete
    → copy_out(DMA → user)
    → release lease
    → parent pending--
```

父请求仍需等所有 child 完成后才能返回，保证同步 API 语义不变；async path 继续由
registered buffer ref 保证用户内存生命周期。

#### Write path

- 先获取一个 child 的 lease，再 copy_in、submit；
- 不在持有 inode/range reservation 时无限等待下一个 lease；
- pool 不足时进入有界 queue 或返回明确 `EAGAIN`；
- 保留 `HFS_IO_PIPELINE_DEPTH` 上限，避免通过无限 outstanding 掩盖 backpressure。

#### Pool

- 为每个 reactor/class 提供 credit 计数；
- fast path 使用 reactor-local freelist，跨线程释放进入 deferred queue；
- 记录 `wait_ns`、`exhaust`、`active_peak`、`queue_depth_peak`；
- 保留 `HFS_IO_POOL_CLASS_COUNTS` 和 `HFS_IO_POOL_BYTES` 的回滚/限额控制。

### 6.3 验收

- 64 KiB QD32 p99 比 shard-54 至少下降 30%。
- pool 耗尽时无死锁、无静默丢 I/O；错误返回只使用明确 `EAGAIN`/`ENOMEM`。
- trace 中 lease hold time 缩短，且 `lease acquire=release` 闭合。
- `io_copy_bytes` 不得因为提前 release 发生遗漏，数据校验和 read-after-write 全部通过。

### 6.4 当前实现记录（shard-56）

- 同步 read pipeline 新增逐 child 完成处理：等待一个 SPDK child 后立即执行
  DMA→user copy，并释放对应 lease；父请求仍等待所有 child，保持同步 API 语义。
- read child 出错时先释放其 lease，再继续等待其余 child，最终返回首个错误；不会因
  提前释放导致 pending child 使用已回收 buffer。
- write pipeline 和 async read/write 的现有生命周期保持不变；copy 字节数和 request
  fan-out 不变，便于与 shard-55 trace 基线比较。
- VM 已通过 Make/CMake、request-local trace smoke、registered DMA lifetime 和 POSIX
  contract 回归；QD32 NVMe 性能回归待下一步统一执行。

## 7. shard-57：Registered DMA 与 Direct Async Window

### 7.1 目标

让明确拥有稳定生命周期的 buffer 真正绕过 bounce copy，并将 direct path 从“submit 一个
chunk、wait 一个 completion”升级为 bounded outstanding window。

### 7.2 设计

- 保留 `hfsRegisterBuffer()`/`hfsUnregisterBuffer()`，增加可复用 registration handle。
- 新增独立 `hfsDmaAlloc()`/`hfsDmaFree()` ABI，禁止全局重载 libc `malloc()`。
- aligned、full-unit、registered buffer 进入 direct async parent；child 直接引用 caller
  buffer 的对应区间。
- parent depth 由 `HFS_IO_PIPELINE_DEPTH` 控制，completion 汇聚后只做一次 parent publish。
- `readv/writev` 在全部 segment 可 DMA 时构造一个 SPDK vector/SGL；混合 vector 只对非 DMA
  segment fallback，不让整个 vector 退化。
- direct path 仍保留 range/sync correctness guard；不能为了零拷贝删除 overlapping write
  protection。

### 7.3 验收

- registered aligned I/O：`copy_ops=0`、`copy_bytes=0`，SPDK child 数与 direct logical
  request 数一致。
- registered QD32 吞吐显著高于普通 malloc bounce；p99 不因 direct window 失控而上升。
- duplicate registration、partial range、unregister while active、fork、close、cancel、
  shutdown 全部通过。
- 不宣称普通 heap、mmap、native io_uring 自动零拷贝。

## 8. shard-58：冗余请求与请求放大收敛

### 8.1 目标

在不破坏 POSIX 语义的情况下，消除可避免的 RMW、vector 拆分、首次扩容和 punch zero 请求。

### 8.2 设计项

#### A. 非对齐 edge RMW 合并

- 将请求拆分为 `edge prefix / full middle / edge suffix`；
- 同一 async window 中对相同 unit 的相邻更新合并成一次 read-modify-write；
- 同一 unit 的并发 RMW 必须串行化，防止 lost update；
- 同步 `write()` 返回前不得把仍未提交的数据留在 write-behind buffer 中。

#### B. Vector partial fallback

- 对 DMA-capable segment 使用原生 `spdk_blob_io_readv/writev`；
- 非 DMA segment 单独走 bounce，不把整个 iovec 拆成多个同步 `hfsReadAt()/hfsWriteAt()`；
- trace 记录 vector logical request、segment 数和实际 child 数。

#### C. Resize/preallocation

- `fallocate/posix_fallocate` 预分配已知工作集，避免随机写首次跨 cluster 时触发扩容；
- `hfsResizeTo()` 保留容量检查和并发保护；
- trace 区分 fast check 命中与真正的 cluster allocation request。

#### D. Punch capability

- 通过 bdev 能力查询判断 unmap 后读取是否保证 zero；
- 能力可靠时 aligned punch 只提交 unmap；
- 能力未知时保留 write-zero fallback；edge unit 继续 RMW zero。

### 8.3 验收

- aligned FIO 的 `request_amplification` 接近 1.0。
- 非对齐单边请求的额外 read/write 数与 RMW 语义一致，没有重复 edge request。
- vector 的 logical/child 比例下降，数据校验和错误注入通过。
- preallocated workload 的 resize request 从 steady-state 热路径消失。
- punch 后读零、fsync、SIGKILL recovery 和旧 metadata 格式兼容不回退。

## 9. shard-59：真实 NVMe 与数据库性能收口

### 9.1 测试矩阵

```text
backend:     SPDK NVMe Nvme0n1 / 持久化 Aio / Malloc
workload:    randread / randwrite / randrw(50/50) / sequential
block size:  4K / 8K / 16K / 32K / 64K / 128K
concurrency: QD1 / QD4 / QD16 / QD32，sync preload 与 async libhfs 分开
buffer:      ordinary malloc / registered DMA / hfsDmaAlloc
reactor:     1 / 2 / 4 / 8，记录 CPU affinity
database:    MariaDB/MySQL CRUD/WAL/fsync，RocksDB fillseq/readrandom/WAL/compact
```

每个组合至少三轮，保存原始 trace、FIO JSON、stats、CPU/RSS 和错误日志；汇总使用 median、
min、max、p50/p99/p99.9，不能只保留最好一轮。

### 9.2 最终验收门槛

- trace-off 回归 IOPS 不超过当前基线 5% 下降。
- aligned registered DMA：copy 为 0，SPDK submit/complete、lease acquire/release 闭合。
- 普通 malloc aligned bounce：每个方向最多一次 copy，不出现额外 registry lock 热路径。
- 4 KiB QD32 HFS/raw SPDK 比例从当前 39.5% 提升到 50% 以上，或有设备饱和/CPU profile
  证明无法继续提升。
- 64 KiB QD32 p99 从当前 43/69 ms 至少下降 30%。
- MySQL/MariaDB、RocksDB、metadata transaction、POSIX contract、fork/cancel/shutdown 和
  SIGKILL recovery 全部通过，errors=0。
- 更新 `doc/bug.md`、`doc/design/fio_baseline_notes.md`、`doc/design/hfs_async_model.md`、
  本计划各 shard 状态和最终产品边界。

## 10. 统一观测字段与报告格式

每个 shard 的专项脚本必须输出以下字段，避免不同阶段使用不可比较的数字：

```text
commit, spdk_commit, bdev, pci_bdf, backend
workload, rw, bs, size, runtime, qd, numjobs, reactor_count, cpu_affinity
buffer_kind, direct_ops, bounce_ops, copy_in_bytes, copy_out_bytes, copy_ops
logical_ops, spdk_submits, spdk_completes, request_amplification
rmw_reads, rmw_writes, resize_requests, metadata_publishes, flushes
dma_wait_ns, dma_exhaust, dma_active_peak
runtime_queue_wait_ns, range_wait_ns, sync_wait_ns
iops, bandwidth, p50_us, p99_us, p99_9_us, errors, rejects
```

报告必须明确：

- stats-off 是主性能口径；
- trace-on 只用于归因，不能直接与 stats-off 吞吐混比；
- raw SPDK 与 HFS 使用同一 NVMe bdev；
- 普通 POSIX buffer、registered DMA 和 HFS DMA allocator 必须分开报告；
- RMW、fsync、resize、punch zero 等必要请求不能被计入“无效重复请求”。

## 11. 风险、回滚与完成清单

### 11.1 风险

- trace ring 设计错误会把观测本身变成新的 lock/copy 瓶颈；
- 提前 release lease 可能导致异步 read copy 使用已归还 buffer；
- direct DMA lifetime 错误可能在 close/unregister/fork 时产生 use-after-free；
- RMW 合并可能引入 lost update、旧数据覆盖和 fsync 顺序错误；
- 增大 pool/window 可能只提升平均吞吐，却恶化 p99、RSS 或 NUMA locality。

### 11.2 回滚开关

- `HFS_TRACE=0`：关闭 tracepoint；
- `HFS_IO_PIPELINE_DEPTH`：限制 outstanding window；
- `HFS_IO_POOL_CLASS_COUNTS` / `HFS_IO_POOL_BYTES`：恢复保守 pool 上限；
- `HFS_DATA_REACTORS=1`：回退单 reactor；
- `HFS_RUNTIME_BATCH=0`：仅用于诊断旧逐消息路径；
- direct DMA 和 RMW merge 必须有独立 feature flag，异常时回到当前 bounce fallback。

### 11.3 每个 shard 完成前必须确认

- [ ] `git status`、`git log --stat` 已核对，没有覆盖用户改动。
- [ ] 本地修改后才同步 VM；SPDK 仍只在 VM 内通过 Git 管理。
- [ ] Make 强制构建和 CMake Release 构建通过。
- [ ] 专项测试、旧 smoke、持久化、数据库和错误注入回归通过。
- [ ] trace-off/trace-on 开销、原始结果和环境信息已保存。
- [ ] request、SPDK completion、DMA lease、parent/child、open-file ref 全部闭合。
- [ ] 新发现 bug 立即更新 `doc/bug.md`。
- [ ] shard 文档、性能基线和产品支持边界同步更新。
- [ ] 独立提交后合入 `main`，在 `main` 上复核 commit、工作树和 VM 状态。

## 12. 预期最终状态

完成 shard-54～59 后，HFS 应能够明确回答每个 I/O 的成本来源，并形成两条清晰路径：

1. **高性能路径**：registered/HFS DMA buffer、aligned direct async、bounded outstanding、
   vector/SGL，一次 logical I/O 尽量对应一次 SPDK request，copy 为零。
2. **兼容路径**：普通 POSIX buffer 保留 bounce fallback，但 registry、lease、copy、锁和
   request 生命周期均有明确上限和可观测等待，不产生无意义的重复请求。

最终目标不是为了追求单一 FIO 数字而删除 POSIX 语义，而是在 trace 证据基础上，把必要的
RMW/metadata/fsync 成本与实现冗余分开，使 HFS 在真实 NVMe 上的吞吐和尾延迟都可解释、可
回归、可持续优化。
