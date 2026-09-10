# shard-62：POSIX Reactor Queue Engine 性能优化计划

> **状态**：实现完成（VM 功能/兼容性回归通过；性能门槛部分未达成，剩余瓶颈已量化）  
> **目标**：解决 HFS 完整 POSIX 路径在高并发 FIO/数据库 workload 下的提交串行化  
> **唯一技术选项**：内部 reactor-sharded bounded async submission queue  
> **前置证据**：`doc/review/hfs_performance_bottleneck_reanalysis_2026-09-10.md`  
> **前置代码**：`main@ee6b0c4` 加当前 tracepoint 扩展  
> **SPDK**：VM 内 Git，锁定 `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`

## 1. 方案决策

本 shard 只实施一条路线：**POSIX Reactor Queue Engine**。它是 HFS 内部实现，不增加
用户可见的 async API，不要求 MySQL、MariaDB、RocksDB 或其他 POSIX 应用修改 buffer
分配器。

```text
POSIX syscall
    │
    ├─ fd/open-file description snapshot
    ├─ buffer classify（direct 或一次 staging copy）
    ├─ inode/range ticket
    └─ hash(blobId, range) 选择 data reactor
                              │
             reactor-local bounded submit queue
                              │
                    bounded credits
                              │
              one reactor drain / batch submit
                              │
                   SPDK channel + bdev
                              │
                ticket complete / lease recycle
                              │
                 POSIX caller 同步返回
```

这里的“async”只存在于 POSIX 边界内部：多个 syscall 可以同时处于 queue/in-flight，调用者
仍在自己的 ticket 上等待完整结果。因此 `read/write/pread/pwrite/fsync/fdatasync` 的可见
顺序、错误返回和生命周期语义不改变。

## 2. 为什么这是唯一优选

当前 8 KiB、32 job 的实测结果已经给出明确 A/B：

| data reactor | 总 IOPS | p99 读/写 | DMA wait |
|---:|---:|---:|---:|
| 1 | 66,144 | 1,466 / 1,991 µs | 130,929 |
| 2 | 67,722 | 1,466 / 1,925 µs | 123,266 |
| 4 | **87,580** | **995 / 1,384 µs** | **2,297** |
| 8 | 51,708 | 2,277 / 3,097 µs | 22 |

4 个 reactor 既降低 DMA class 等待，又降低尾延迟；8 个 reactor 则因调度和队列争用
反而退化。由此确定：

- 默认值采用 `min(4, max(1, nproc/2))`，当前 8 vCPU VM 默认 4；
- 每个 reactor 必须拥有独立 queue、SPDK channel 和 DMA pool shard；
- 必须有 bounded credit 和 backpressure，不能无限增加并发；
- 只扩大 pool、只改 metadata、只增加 callback 或只做 copy 优化都不能解决 owner
  串行提交边界，故不作为本 shard 的独立路线。

## 3. 当前问题到目标的映射

| 当前证据 | 根因 | shard-62 的修复 |
|---|---|---|
| `runtime_inflight_peak=1` | owner callback 执行串行 | 多 data reactor + 每 reactor drain |
| `io_pipeline_peak=1` | 每个 POSIX syscall 立即等待 | ticket 与 bounded in-flight window |
| 8/16 KiB `dma_wait` 约 39%～41% | class slot 受单 shard 16 个 buffer 限制 | pool 随 reactor 分片，credit 与 class 绑定 |
| `lock_io` wait 82～108 µs | 保护范围覆盖提交/完成生命周期 | ticket 只保护顺序，I/O 等待移出独占临界区 |
| completion 每 request 唤醒 | 每个 syscall 一个 waiter 边界 | reactor 批量完成、ticket 定向唤醒 |
| metadata lock 平均数百 ns | FIO steady state 非主因 | 保持现有事务，按 dirty epoch 合并，不扩大锁范围 |

## 4. 设计不变量

### 4.1 POSIX 顺序不变量

1. 同一 open-file description 的共享 offset 仍由 `HfsOpenFile` 管理；ticket 不得复制或
   延迟 offset 更新。
2. 同一 inode、重叠 range 的 write、truncate、size publish 按 sequence 依次生效；不重叠
   range 才允许并行。
3. read 在其可见 sequence 之前不得完成；read-after-write 和 `O_APPEND` 保持现有语义。
4. `fsync/fdatasync` 只在目标 dirty epoch 的 data ticket 全部完成后提交 metadata/flush。
5. unlink/rename orphan、fd close、fork child 的生命周期规则不改变。

### 4.2 资源不变量

1. 一个 ticket 只能属于一个 reactor queue；SPDK channel 只能由其 owner reactor 使用。
2. queue 深度和 backend in-flight 都有上限；达到 credit 上限时不在 POSIX range lock
   内等待，而是走已存在的 direct-submit 溢出路径，避免锁反转和 busy-spin。
3. staging buffer 在 SPDK completion 和 read copy-out 之前不得归还；write copy-in 只做
   一次，direct buffer 必须完成整个范围的 `spdk_vtophys` 验证。
4. queue close 先拒绝新 ticket，再 drain 已提交 ticket，最后销毁 channel/pool；关闭过程
   可被唤醒但不能释放仍被 callback 使用的内存。
5. ticket completion 先快照结果和 trace 字段，再改变状态、回收资源、唤醒 POSIX caller。

## 5. 模块设计

### 5.1 `src/hfs_runtime.[ch]`：reactor 与 queue

新增或重构以下内部对象：

```c
typedef struct HfsPosixSubmitQueue {
    _Atomic(HfsPosixTicket *) tail;
    HfsPosixTicket *head;
    uint32_t depth;
    uint32_t maxDepth;
    uint32_t reactorId;
    struct spdk_thread *thread;
    struct spdk_io_channel *channel;
} HfsPosixSubmitQueue;
```

实现要求：

- producer 只持有 reactor-local 短 mutex 链接 ticket，consumer 只在对应 reactor 中 drain，
  避免 producer 持有 SPDK channel；进入 SPDK message ring 前再由 runtime 短锁串行化
  多生产者提交，规避 DPDK reservation stall。
- queue 由一次 `spdk_thread_send_msg` 触发 drain，drain 批量取出当时可见的 ticket；
- `batchScheduled`、depth、credit 在 batch mutex 下维护，保留 stop-vs-submit admission
  保护；credit 在 backend callback 完成后归还。
- `HFS_DATA_REACTORS` 仍可显式覆盖默认值，用于回退和实验；默认按 CPU 数计算并限制为 4；
- 为每个 reactor 暴露 queue depth、drain batch、submit、complete、credit wait 统计。

### 5.2 `src/hfs_io.[ch]`：ticket 化请求

新增 `HfsPosixTicket`，至少包含：

```text
requestId / parentId
file / open-file description
inodeId / blobId
offset / bytes / units
operation / buffer kind
range sequence / ordering fence
reactorId / queue pointer
DMA lease / direct buffer reference
submit timestamp / completion timestamp
POSIX result / errno / completed bytes
```

处理流程：

1. POSIX wrapper 在已有 fd 引用保护下创建 ticket，并一次完成 buffer classify。
2. aligned direct buffer 直接挂到 ticket；普通 buffer 从目标 reactor 的 class pool 取得
   staging lease，执行一次 copy-in。
3. 先申请 inode/range sequence，再将 ticket 入对应 queue；不要在 queue 外持有跨请求的
   write lock。
4. reactor drain 按可执行 sequence 批量调用 SPDK；单 ticket callback 不再进行用户态
   syscall 或阻塞等待。
5. completion callback 写入 ticket 结果；读路径在 callback 中或紧邻 caller 侧完成一次
   copy-out，随后释放 lease、递减 credit、唤醒 ticket。
6. POSIX caller 只等待自己的 ticket；批次中的其他 ticket 不得被一个慢 request 拖成
   同一把锁的持有者。

### 5.3 range 与 fd 顺序

- 保留现有 range stripe 的冲突判定和 ascending lock order，改为 ticket reservation；
  queue drain 只消费当前可执行 ticket。
- 同一 inode 的顺序字段由 inode runtime 生成，不使用 wall-clock 作为排序依据。
- fd lookup/reference 在 enqueue 前完成，ticket 持有 `HfsOpenFile` 引用直到 completion；
  close 只摘除 fd slot，不得提前释放 queue 仍引用的 open description。
- 非对齐 RMW 仍按现有首尾 unit 语义执行；RMW ticket 可以拆成内部 child，但对 caller
  仍表现为一个 POSIX operation。

### 5.4 metadata 与 durability

- 普通 aligned write 只更新 runtime dirty epoch，不同步取得全局 metadata write lock。
- resize、truncate、rename、unlink、属性变更生成 metadata ticket，并与 data ticket 建立
  dependency。
- durability coordinator 按 inode/epoch 收集 ticket；一个 leader 执行必要 metadata
  transaction 和设备 flush，其余 waiter 只等待同一 epoch。
- `fsync/fdatasync` 的强弱语义、错误传播和崩溃恢复顺序沿用当前实现；不以“异步”名义
  提前返回未持久化数据。

### 5.5 trace 与 stats

新增/补齐以下字段：

```text
reactor_id
queue_depth
queue_wait_ns
credit_wait_ns
batch_size
ticket_sequence
backend_outstanding
ticket_wake_ns
```

现有 `lock_class`、`wake_reason`、`metadata_op` 继续保留。必须能重建：

```text
POSIX io_enter
  -> ticket enqueue
  -> queue drain
  -> spdk submit
  -> spdk complete
  -> lease recycle / ticket wake
  -> io_exit
```

trace 默认关闭；归因样本优先使用 `HFS_TRACE_SAMPLE=1000` 和短 runtime，避免当前固定
4096-event/thread ring 在 32 job 下溢出。`trace_dropped` 非零时，不能用 trace 计数推导
request amplification。

## 6. 分阶段实施

### shard-62A：基线与开关

- 冻结当前 1/2/4/8 reactor 的 4/8/16/32/64 KiB FIO 结果；
- 新增 `HFS_POSIX_QUEUE=0/1` 开关，默认先保持旧路径，便于逐项 A/B；
- 增加 queue/ticket 生命周期统计和 trace schema 检查；
- 验证现有 POSIX contract、fd lifecycle、metadata transaction、fork 防护。

### shard-62B：默认 4-way reactor queue

- 将默认 data reactor 计算改为 `min(4, max(1, nproc/2))`；
- 为每个 reactor 初始化独立 queue、SPDK channel、DMA pool shard 和 credit；
- 保留 `HFS_DATA_REACTORS=1` 作为回退路径；
- 只改 aligned read/write，RMW、punch、resize 先走旧路径以缩小风险。

### shard-62C：ticket submit/completion

- 把 aligned POSIX I/O 改为 enqueue → batch drain → ticket completion；
- 默认每 reactor 使用有界 credit，初始值不超过 32，按 pool class 动态限制；
- completion 先回收 lease/credit，再定向唤醒 caller；
- 验证 submit/complete、lease acquire/release、ticket pending 闭合。

### shard-62D：ordering 与 metadata dependency

- 接入 inode/range sequence，验证 overlap write、shared offset、`O_APPEND`；
- 将 resize、truncate、rename、unlink 与 data ticket 建立依赖；
- 将 dirty epoch 接入 durability coordinator，验证 fsync/fdatasync 和 crash recovery；
- RMW/vector/zero 请求逐项迁移，任何语义不确定的操作继续走旧实现。

### shard-62E：性能与兼容性收口

- 在同一 NVMe bdev 上跑 HFS 1/2/4/8 reactor 与 raw SPDK 对照；
- 跑 FIO、MySQL/MariaDB、RocksDB 完整矩阵，保留 JSON、stats、trace、CPU/RSS；
- 将 `HFS_POSIX_QUEUE=1` 设为默认；若发现回归，使用 `HFS_POSIX_QUEUE=0` 精确回退并记录；
- 更新 `doc/design/fio_baseline_notes.md`、review 和项目记忆。

## 7. 验收门槛

### 7.1 性能门槛

在当前 8 vCPU VM、同一 NVMe、32 job、5 秒口径下：

- 8 KiB 总 IOPS ≥ 82k，且至少比当前 1 reactor 提升 25%；
- 8 KiB p99 读 ≤ 1.2 ms、写 ≤ 1.6 ms；
- 16 KiB 总 IOPS 比当前 1 reactor 提升 ≥20%，写 p99 不高于 2.18 ms；
- 4/32/64 KiB 相对当前 stats-off 基线不得下降超过 5%；
- `dma_wait_ns`、`queue_wait_ns`、`lock_io_wait_ns` 必须按 reactor 导出，不能以总数掩盖
  单 shard 饥饿；
- 默认 4 reactor 不得出现 8 reactor 样本中的吞吐退化，CPU 使用率和 RSS 需记录。

### 7.2 POSIX 与数据库门槛

- `posix_contract_test.sh`、fd lifecycle、metadata transaction、persistent bdev smoke 全部
  通过；
- MariaDB/MySQL：初始化、CRUD/DDL、事务、LONGTEXT、临时表、重启、SIGKILL 恢复和并发
  客户端全通过；
- RocksDB：fillseq/readrandom/overwrite/readwhilewriting、WAL、flush/compact、SIGKILL
  recovery、跨进程 reopen、`ldb checkconsistency` 全通过；
- 所有 workload `errors=0`，`spdk_submit=spdk_complete`，ticket pending=0。

### 7.3 可观测性门槛

- trace-off 是正式性能数字；trace-on 只用于归因；
- 短 trace 样本 `trace_dropped=0`，能关联 enqueue、drain、submit、complete、wake；
- lock/metadata/thread wake 字段与当前项目记忆中的解析命令兼容；
- 对每个失败或超时样本保留原始 FIO JSON、stats、trace 和 stderr。

## 8. 风险与回退

| 风险 | 保护措施 |
|---|---|
| queue 改变 write/read 顺序 | inode/range sequence；先迁移 aligned I/O，再迁移 RMW/vector |
| ticket 提前释放 fd/lease | ticket 持有 open-file 引用；completion 最后释放资源 |
| reactor 数过多导致调度退化 | 默认上限 4；保留 `HFS_DATA_REACTORS` 和 `HFS_POSIX_QUEUE=0` |
| metadata 提前提交破坏恢复 | durability 只等待 dirty epoch，保留 intent→stable→clear 顺序 |
| trace 造成额外瓶颈 | 默认关闭、采样、固定 ring；`trace_dropped` 非零不做精确计数 |
| SPDK callback 与 caller 竞态 | callback 先快照，再 state transition、recycle、wake；加入生命周期 smoke |

## 9. Shard-62 实现与验证结果

### 9.1 已实现

- `HFS_POSIX_QUEUE`（默认 `1`）和 `HFS_POSIX_QUEUE_DEPTH`（默认 32，范围 4～256）；
- 未显式设置 `HFS_DATA_REACTORS` 时按 `min(4, max(1, nproc/2))` 创建 data reactor，
  reactor 0 保留为 metadata/owner；8 vCPU VM 实际为 4 个 data reactor；
- reactor-local batch queue、queue depth/in-flight credit、drain batch 统计，以及
  completion 归还 credit；
- 所有 SPDK owner/data/batch/stop 消息经 `hfsRuntimeSendMessage()` 短锁串行提交，
  修复 DPDK 多生产者 ring reservation stall；queue 满时 direct-submit 溢出，避免
  在 range lock 持有期间等待；
- queue、DMA、SPDK submit/complete 及 trace 的关联字段；统计和 trace 默认关闭。

### 9.2 VM 验证

在 VM 内使用原生 Git 获取的 SPDK（锁定 `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`）完成：

- `make -B app lib -j2`、CMake Release：通过；
- data reactor stripe（5 total/4 data）：138 submit/138 complete，4 个 data reactor
  均有实际负载；
- DMA pool backpressure（queue depth=4）：128 acquire/128 release，active peak≤4；
- 16 线程 runtime stress、POSIX contract、64×250 fd churn、metadata transaction：通过；
- IO trace：1621 records、27 phases；
- POSIX queue engine smoke：64 个并发请求，`submit=188`、`complete=188`，depth=4
  时各 reactor 峰值不超过 3，`errors=0`；
- `HFS_POSIX_QUEUE=0` 回退的 POSIX contract：通过；
- MariaDB/MySQL 完整初始化、CRUD/DDL、事务、重启、SIGKILL 恢复和 4/16 并发：通过；
- RocksDB 20,000 条、WAL、direct I/O、flush/compact、readwhilewriting、SIGKILL recovery
  和 `ldb checkconsistency`：通过；
- 所有统计样本 `spdk_submit=spdk_complete`、`errors=0`。

### 9.3 NVMe 性能结果与未达成项

在同一 `Nvme0n1` 上，HFS 完整 POSIX 路径使用 32 个 fio jobs，原生 SPDK 使用
`iodepth=32`，`randrw` 50/50、64 MiB、5 秒。总 IOPS 为：4K `79,525/123,356`、
8K `72,004/112,179`、16K `61,768/97,294`、32K `49,537/74,459`、64K
`36,331/53,068`（HFS/raw SPDK），错误均为 0；HFS/raw 比例为 64.1%～68.4%。
完整表格和 p50/p99 见 `doc/design/fio_baseline_notes.md`，原始结果在 VM 的
`/mnt/hfs-data/shard62-fio-nvme-20260910/`。

计划中的 8K ≥82K IOPS 和 8K 写 p99 ≤1.6ms 门槛未达成，因此不能宣称本 shard 已经
消除全部性能差距。队列 depth A/B（32/64/128）均约 71.5～72.7K IOPS，说明简单扩大
队列不是解法。stats-on 归因样本显示：`lock_io_wait_ns≈30.95s`、`lock_io_hold_ns≈126.82s`、
`lock_io_contention=95,711`，每个普通 POSIX 请求均产生一次 bounce copy；queue credit
wait 为 0，`io_backend_outstanding_peak=32`。剩余工作应进入后续“POSIX range ticket +
copy-elision/批量同步等待”优化，不应通过继续扩大 queue depth 或 reactor 数量解决。

### 9.4 重要兼容性决策

当前 shard 不改变已有 POSIX range-lock 顺序和 metadata transaction 语义。由于这些锁
可能跨越同步 syscall 的整个等待区间，queue credit 满时采用 direct-submit 溢出而不是
阻塞 producer；后续若迁移为完全 ticketized range reservation，必须保持同一锁序并重新
验证 overlap write、O_APPEND、fsync 和 crash recovery。

## 10. 完成定义

以下条件用于区分“实现交付”和“性能收口”：

1. 默认 4-way reactor queue 已实现，且 queue/credit/submit/complete 统计闭合；
2. 1/2/4/8 reactor A/B 结果和 CPU/RSS/queue 统计已保存；
3. 普通 POSIX、MariaDB/MySQL、RocksDB 和崩溃恢复全部通过；
4. trace 能解释 queue、lock、DMA、metadata、completion 的 p99 贡献，且短样本
  `trace_dropped=0`；
5. `HFS_POSIX_QUEUE=0` 回退可用，文档、项目记忆和 bug 记录已同步；
6. 独立提交完成后立即合入 `main`，并在 `main` 上确认 VM 回归状态。

以上 1～5 已满足，故 Shard-62 的代码/兼容性实现交付；旧版“必须达到 82K”属于
性能收口目标，当前明确标记为未达成并转入后续 shard。

## 11. 不在本 shard 做的事情

- 不重新引入已退役的公开 `hfs_lib_async_*`、registered-DMA 或 DMA allocator ABI；
- 不要求数据库改变 malloc、WAL 或 buffer pool；
- 不用 write-behind、提前返回或删除 fsync barrier 换取表面吞吐；
- 不把单纯扩大 DMA pool、增加 8+ reactor、扩大 lock-free ring 当成完整优化；
- 不宣称多进程共享挂载、mmap、native libaio/io_uring 已因本 shard 获得支持。
