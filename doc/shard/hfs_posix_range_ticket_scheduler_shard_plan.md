# HFS POSIX Range-Ticket Scheduler 优化 Shard 开发计划

> **Shard**：63
> **状态**：已实现并完成 VM 验证（2026-09-10）
> **基线**：`main@3d02fd1`（Shard-62 POSIX Reactor Queue Engine 已完成）
> **范围**：完整 POSIX/preload 数据路径；不重新引入已退役的 public async ABI
> **唯一推荐方案**：用 inode 内部的 range ticket scheduler 替换“持有 range lock 直到 SPDK completion”的同步实现

本轮实现了 ticket admission、依赖等待/唤醒、aligned POSIX read/write 接入、非对齐 RMW
的单请求顺序保护、生命周期统计和 tracepoint；旧 range-lock 路径保留为
`HFS_POSIX_TICKET=0` 的故障回退。控制操作继续使用已有 inode exclusive sync barrier，
不会绕过 `fsync/fdatasync`、truncate、punch 和 metadata transaction 语义。

## 1. 决策结论

后续性能优化只做一件核心事情：

> **把 POSIX I/O 改造成 ticket 化的内部异步状态机：调用线程只负责创建 ticket、提交依赖并等待自己的完成；真正的 DMA、SPDK submit、copy、依赖推进和回收全部由现有 reactor queue 驱动。range lock 不再跨越后端 I/O 生命周期，重叠范围用 ticket 依赖保证顺序。**

这是当前最优解。它保留完整 POSIX 语义，同时直接消除已经量化的第一瓶颈：

1. `rangeGate/rangeStripes` 的 pthread 阻塞和长时间 hold；
2. 一个同步 syscall 把 DMA lease、SPDK completion 和调用线程绑定在一起；
3. 每个 child 单独 wake caller，导致提交窗口无法跨 syscall 持续存在。

本 Shard 不把“继续扩大 queue depth”“继续增加 reactor”“先优化 memcpy”作为独立路线。它们只能作为 ticket scheduler 的容量参数或后续微优化。

## 2. 现状和性能证据

### 2.1 当前基线

Shard-62 已经完成 bounded POSIX reactor queue、per-reactor DMA pool、SPDK message admission 串行化和队列统计；但 NVMe FIO 仍显示：

| 8 KiB `randrw`，32 job | IOPS |
|---|---:|
| HFS 完整 POSIX 路径 | 约 72.0K |
| 原生 SPDK `iodepth=32` | 约 112.2K |
| HFS / SPDK | 约 64.2% |

Queue depth `32/64/128` A/B 只有约 `71.5K～72.7K` IOPS，说明队列容量不是当前第一限制。

### 2.2 归因数据

8 KiB、32 job、5 秒、`HFS_STATS_TIMING=1` 的归因样本：

```text
lock_io_wait_ns              = 30.95 s
lock_io_hold_ns              = 126.82 s
lock_io_contention           = 95,711
io_copy_ops                  = 365,549
io_copy_bytes                = 2.99 GiB
queue_credit_waits           = 0
io_backend_outstanding_peak  = 32
spdk_submit/complete         = 365,705/365,705
```

结论：SPDK 后端已经能保持有效 outstanding=32，credit 没有耗尽；请求主要浪费在 POSIX 线程等待、range 保护和同步 completion 边界。普通 buffer 的必要 copy 是稳定成本，但不是当前第一主因。

### 2.3 当前代码中的根因

现在的 aligned POSIX 路径仍近似如下：

```text
pread/pwrite
  -> inode sync read side
  -> hfsIoRangeLock(window)
  -> acquire DMA lease / classify buffer
  -> hfsIoStartRequest()
  -> reactor -> SPDK bdev
  -> hfsIoWaitRequest()/futex
  -> copy-out、release lease
  -> hfsIoRangeUnlock(window)
```

`hfsIoReadAtLocked()` 和 `hfsIoWriteAtLocked()` 的窗口锁覆盖了整批 child 的 submit 到 completion；只要一个 child 慢，后续与该 stripe 冲突的请求就会在 pthread 锁上排队。当前实现是“锁保护真实 I/O”，而不是“锁保护顺序元数据”。

## 3. 目标架构

### 3.1 数据流

```text
POSIX read/write/pread/pwrite
        │
        ├─ 快速取得 open-file/inode 引用
        ├─ 一次 buffer classify；不可 DMA 时分配一次 staging lease
        ├─ ticket admission：短暂 ticketLock，建立 range 依赖
        └─ 提交到 blob/range 对应的 reactor queue
                       │
                bounded in-flight credits
                       │
             batch drain -> SPDK channel/qpair
                       │
        completion -> copy/release -> promote dependents
                       │
              唤醒当前 POSIX caller 并返回
```

调用线程仍同步返回，因而 MySQL/MariaDB/RocksDB 不需要修改；“异步”只存在于 HFS 内部，目标是让多个 POSIX syscall 共用一个持续的 in-flight 窗口。

### 3.2 Ticket 状态机

每一个 logical POSIX I/O 使用一个 `HfsPosixIoTicket`，生命周期固定为：

```text
ALLOCATED
   -> ADMITTED
   -> WAIT_DEPENDENCY ──────┐
   -> READY                 │ dependency count == 0
   -> SUBMITTED             │
   -> COMPLETING            │ SPDK callback / RMW step
   -> DONE / FAILED / CANCELLED
```

ticket 必须拥有完整的生命周期数据，不得再依赖 `HfsFile` 的共享 scratch：

```c
typedef struct HfsPosixIoTicket {
    HfsInodeRuntime *runtime;
    HfsFile *file;
    uint64_t ticketId;
    uint64_t submitSeq;
    uint64_t offset;
    uint64_t length;
    uint32_t operation;
    uint32_t dataReactor;
    uint16_t stripeCount;
    uint16_t pendingDependencies;
    uint32_t pendingChildren;
    uint32_t state;
    int result;
    int error;
    /* DMA lease, RMW stage and caller wake state are ticket-owned. */
} HfsPosixIoTicket;
```

结构体字段以实际代码命名为准；上面的布局只定义所有权和状态边界，不要求按示例顺序实现。

### 3.3 Range 顺序模型

使用现有固定 `HFS_INODE_RANGE_STRIPE_COUNT` 作为保守冲突分片，但不再把 `HfsRangeLock` 持有到 completion：

1. ticket 在 inode 内取得单调递增 `submitSeq`；
2. 按 offset 从小到大计算涉及的 stripe，并在一个短临界区内插入每个 stripe 的 ticket 链；
3. 只为真正冲突的前序 ticket 建立依赖：
   - read/read：可并行；
   - read/write、write/read、write/write：按 `submitSeq` 排序；
   - `truncate`、`ftruncate`、`punch hole`、size publish：对覆盖范围建立写 fence；
4. `pendingDependencies==0` 的 ticket 进入 reactor ready queue；
5. ticket 完成后从所有 stripe 链摘除，递减后继依赖并批量 promote ready ticket；
6. ticket admission 只持有短期 mutex，绝不在该 mutex 或 stripe mutex 中调用 SPDK、等待 condition/futex 或执行用户 copy。

为避免多 stripe 请求形成锁顺序死锁，所有 stripe 链只按升序修改；completion 阶段不得反向获取另一个 inode 的 ticket 锁。需要跨 inode 的 rename 时，沿用现有 metadata transaction 顺序，先获取稳定 inode 顺序，再插入 fence ticket。

### 3.4 POSIX 语义映射

| 语义 | ticket 处理 |
|---|---|
| 同一范围 read-after-write | read 依赖最近未完成的 overlapping write |
| overlapping write | 按 `submitSeq` 串行，完成后才 promote 后继 |
| 不重叠 I/O | 不建立依赖，可在不同 reactor 并行 |
| `O_APPEND` | 保留现有 appendLock；分配最终 offset 后再 admission |
| shared offset | 保留 `HfsOpenFile` 的 offset lock；ticket 只接管数据面生命周期 |
| `ftruncate`/`punch hole` | 作为写 fence，完成后再 publish size/hole 状态 |
| `fsync`/`fdatasync` | 等待目标 data epoch 的 ticket 集合，再由 durability leader 执行一次 metadata/device flush |
| rename/unlink | 继续使用 metadata transaction；对受影响 inode 插入 exclusive fence |
| fork child | 继承现有 owner PID 防护，不得进入父进程 ticket registry |
| stop/close | 停止 admission，排空 ready/submitted ticket；失败或取消必须唤醒所有 POSIX waiter |

### 3.5 普通 buffer 和 DMA buffer

本 Shard 不放宽普通 POSIX buffer 的生命周期约束：

- 可验证 DMA-capable 且对齐的 buffer 直接挂到 ticket，`io_copy_ops=0`；
- 普通 buffer 只允许一次 write copy-in 或 read copy-out，staging lease 由 ticket 持有到对应 child completion；
- copy 必须在 reactor completion 状态已经稳定后进行，不能在 ticket admission 锁内做；
- 非对齐请求作为一个不可拆分的 RMW ticket，内部状态为 `READ_UNIT -> MODIFY -> WRITE_UNIT`，不会把 edge unit 暴露给后继 ticket。

因此本 Shard 先解决“copy 被长锁和同步等待放大”的问题，copy-elision 只作为后续可测量优化，不改变普通 POSIX 语义。

## 4. 实现范围和代码改动

### 4.1 `src/hfs_inode_runtime.[ch]`

新增 inode-local ticket scheduler：

- `ticketLock`、`nextTicketSeq`、`nextTicketId`；
- 每个 stripe 的有序 ticket 链或等价的 bounded conflict queue；
- ticket pool、ready list、completed/promotion list；
- `hfsInodeTicketAdmit()`、`hfsInodeTicketPromote()`、`hfsInodeTicketComplete()`、`hfsInodeTicketCancel()`；
- inode 销毁前保证 ticket list 为空，禁止 runtime teardown 与 outstanding ticket 并行。

实现要求：

- admission/promotion 的临界区只修改链表和计数；
- 依赖计数使用无符号饱和检查，禁止下溢；
- 同一 ticket 可能覆盖多个 stripe，但完成/取消只能触发一次最终 wake；
- pool 达到上限时返回 `EAGAIN` 或在 bounded wait 上阻塞，不能无界 malloc。

### 4.2 `src/hfs_io.[ch]`

把当前“locked function + wait pipeline”拆成 ticket 驱动的阶段函数：

- `hfsIoTicketCreate()`：校验参数、捕获 buffer ownership、分配 lease；
- `hfsIoTicketSubmit()`：建立依赖并送入 runtime queue；
- `hfsIoTicketStartChild()`：在 reactor 上提交一个 SPDK child；
- `hfsIoTicketCompleteChild()`：记录结果、copy/release、推进 RMW；
- `hfsIoTicketFinish()`：从 stripe 链摘除、更新 logical size/mtime、唤醒 caller；
- `hfsIoTicketWait()`：只等待自己的 completion，不持有 range lock。

第一阶段先迁移 aligned read/write；第二阶段迁移非对齐 RMW、vector 和 punch；旧 `hfsIoRangeLock()` 路径保留为可回退实现，直到所有语义测试通过。

### 4.3 `src/hfs_runtime.[ch]` 和 `src/hfs_runtime.c`

复用 Shard-62 的 per-reactor bounded queue，不再新增第二套提交队列：

- ready ticket 只通过现有 `hfsRuntimeSubmitDataBatch()` 入队；
- dispatcher 每轮从 ticket ready list 取不超过当前 credit 的 ticket；
- 同一 reactor 一次 drain 可提交多个不同 inode/range ticket；
- queue full 时 ticket 保留在 ready list，caller 进入有界等待，不 busy-spin；
- completion 只向 ticket waiter 发送一次 wake，禁止每个内部 child 再创建独立 POSIX waiter。

### 4.4 `src/hfs_stats.[ch]` 与 `src/hfs_trace.[ch]`

新增默认关闭的低成本统计和 tracepoint：

| 统计 | 用途 |
|---|---|
| `ticket_admit/ready/submit/complete/cancel` | 生命周期闭合 |
| `ticket_dependency_wait_ns` | 区分范围顺序等待和后端延迟 |
| `ticket_ready_queue_depth/peak` | 观察 reactor admission |
| `ticket_promote_batch` | 观察完成批量化 |
| `ticket_rmw_steps` | 统计非对齐请求放大 |
| `ticket_waiter_wake` | 检查 POSIX wake 是否按 logical I/O 发生 |
| `ticket_pool_wait/reject` | 检查 bounded backpressure |

trace 事件至少覆盖：

```text
TICKET_ADMIT -> TICKET_DEP_WAIT -> TICKET_READY
             -> TICKET_SUBMIT -> CHILD_COMPLETE
             -> TICKET_PROMOTE -> TICKET_WAKE
```

每个事件带 `ticketId/inodeId/submitSeq/reactorId/stripeMask/operation`；正式性能测试关闭 trace，归因测试用采样 trace。

### 4.5 `src/hfs_lib.c`、`src/hfs_file.c` 和 metadata 路径

- `fsync/fdatasync` 等待 ticket epoch，不再扫描或等待无关的未来请求；
- `hfs_file` 的 size publish 必须发生在成功 ticket completion 之后；
- append、truncate、punch 和 rename 的现有 metadata lock 顺序不能改变；
- ticket wake 前必须完成 pending mtime/ctime 的可见性更新；
- 任意 error/cancel 路径都要释放 lease、ticket 引用和 open-file 引用。

## 5. 分阶段开发步骤

### Shard-63.1：观测和 shadow admission

1. 加入 ticket 结构、生命周期统计和 tracepoint；
2. 在旧 range-lock 路径旁建立只记录依赖的 shadow graph，不改变提交顺序；
3. 用 overlap/read-after-write smoke 校验 shadow 依赖与实际 range lock 冲突集合一致；
4. 增加 ticket pool 上限和 stop/fork 清理检查。

完成条件：统计无下溢，`ticket_admit == ticket_complete + ticket_pending`，shadow 模式无功能回归。

### Shard-63.2：aligned POSIX read/write ticket path

1. 迁移 aligned bounce read/write；
2. 迁移 aligned DMA-capable direct read/write；
3. 使用现有 reactor queue 执行多 ticket submit；
4. 移除 aligned steady-state 对 `hfsIoRangeLock()` 的长 hold，仅保留 ticket dependency；
5. 保留 `HFS_POSIX_TICKET=0` 回退旧路径，默认先以 `HFS_POSIX_TICKET=1` 在 VM 验证。

完成条件：8 KiB 32-job FIO 中 `lock_io_hold_ns` 不再覆盖 backend latency，ticket pending 在退出时为 0，submit/complete 和 DMA acquire/release 严格闭合。

### Shard-63.3：RMW、vector 和控制操作收口

1. 把非对齐 read/write 迁移成单 ticket 的内部 RMW 状态机；
2. readv/writev 通过同一 ticket 完成 buffer segment 管理，禁止逐 iovec 产生同步 syscall；
3. punch/truncate/size publish 使用 exclusive fence ticket；
4. fsync/fdatasync 接入 data epoch 和已有 durability leader；
5. 关闭默认旧路径，只保留环境变量回退用于故障定位。

### Shard-63.4：数据库和性能门禁

1. 运行完整 Make/CMake、POSIX contract、fd lifecycle、metadata transaction、IO trace 和 runtime stress；
2. 运行 MariaDB/MySQL persistent recovery 和 RocksDB WAL/compaction/recovery；
3. 在同一 SPDK NVMe bdev 上执行 HFS 与 raw SPDK FIO，固定 `numjobs=32`、`iodepth=32`、`randrw 50/50`；
4. 更新 `doc/design/fio_baseline_notes.md`、`doc/review/` 和本 Shard 的实测结果；
5. 通过门禁后将 `HFS_POSIX_TICKET=1` 固化为默认配置。

## 6. 验收标准

### 6.1 功能和正确性

- 现有所有 smoke 和数据库验收继续通过，错误数为 0；
- 同一 fd shared offset、`O_APPEND`、overlap write、read-after-write、truncate、punch、rename 和 unlink-open 语义不回退；
- `fsync/fdatasync` 在 SIGKILL 恢复后保持已返回成功的数据和 size；
- fork child 不得使用父进程 ticket registry；
- stop、close、错误和超时路径无 ticket、lease、open-file 引用泄漏。

### 6.2 生命周期不变量

```text
ticket_admit = ticket_complete + ticket_pending
ticket_pending(exit) = 0
spdk_submit = spdk_complete
dma_acquire = dma_release
ticket_wake <= ticket_complete
```

任意失败路径都必须满足上述等式或给出明确的 rejected/cancelled 计数，不得静默丢请求。

### 6.3 性能门禁

在 8 vCPU VM、同一 NVMe bdev、stats-off 的正式 FIO 口径下：

| 指标 | 门禁 |
|---|---:|
| 8 KiB、32 job HFS IOPS | `>=82K`，且比 Shard-62 提升至少 25% |
| 8 KiB 读/写 p99 | 读 `<=1.2 ms`，写 `<=1.6 ms` |
| 16 KiB、32 job | 比当前基线提升至少 20% |
| 4/32/64 KiB | 不得比当前 stats-off 基线下降超过 5% |
| range lock hold | aligned steady-state 不得覆盖 SPDK completion；旧 `lock_io_hold_ns` 显著下降 |
| ticket dependency wait | 单独统计，不得伪装成 `lock_io_wait_ns` |
| backend outstanding | QD32 稳定接近 32，submit/complete 闭合 |

如果 IOPS 未达到门禁，不得用“queue depth 已扩大”或“copy 已减少”宣称完成；必须根据 ticket wait、reactor queue wait、DMA wait 和 backend latency 重新归因。

### 6.4 资源上限

- ticket pool、ready queue、每 reactor credit 都是有界的；
- steady-state 不允许每个 child `malloc/free`；
- queue 满时只能 bounded wait 或返回 `EAGAIN`，禁止无限制增长；
- 8 vCPU 默认仍使用 4 个 data reactor，不能用增加 reactor 数掩盖 ticket 调度问题。

## 7. 测试脚本和文档交付物

新增或更新：

- `scripts/db/posix_range_ticket_test.sh`：依赖顺序、重叠读写、RMW、取消和生命周期；
- `scripts/performance/fio_stage2_compare.sh`：增加 ticket 统计摘要和固定 NVMe 口径；
- `scripts/db/hfs_trace_summary.py`：解析 ticket admission/promotion/wake 链；
- `doc/design/fio_baseline_notes.md`：记录正式前后对照；
- `doc/review/hfs_performance_bottleneck_reanalysis_2026-09-10.md`：追加 Shard-63 归因；
- `doc/bug.md`：只在发现真实语义或生命周期 bug 时记录现象、根因和修复。

## 8. 明确不做的事情

本 Shard 不做以下会改变方向的工作：

1. 不重新恢复 public `hfs_lib_async_*`、registered-DMA ring 或新 async ABI；
2. 不把普通 POSIX buffer 强行当作 DMA-capable，避免悬空用户指针和生命周期错误；
3. 不先扩大 DMA pool、queue depth 或 reactor 数并把容量变化当成架构优化；
4. 不为 FIO 牺牲 `fsync/fdatasync`、O_APPEND、overlap write 或 RMW 语义；
5. 不在没有 ticket dependency 证据前大规模重写 metadata/WAL。

## 9. 回退与风险控制

- 开发期间保留 `HFS_POSIX_TICKET=0`，出现语义或生命周期回归时回到 Shard-62 路径；
- ticket admission 与旧 range lock 可在一个构建中并存，但同一请求只能选择一条路径；
- 任何异常完成都必须先将 ticket 标记为终态，再唤醒 POSIX caller，防止 caller 观察到半更新的 size/metadata；
- 只有通过全量数据库恢复和 FIO 性能门禁后，才移除旧路径的默认可见性；旧实现可暂时保留为诊断回退。

## 10. 最终结论

Shard-62 已解决“没有 bounded POSIX queue”的结构问题，但当前数据表明 HFS 仍把 range protection 持有到了后端 completion，导致 queue 已经有能力而请求仍被同步锁边界截断。下一步唯一值得投入的是 **POSIX 内部 range-ticket scheduler + reactor async state machine**：短临界区只记录顺序，后端 I/O 在 ticket 状态机中并行，完成后再推进依赖和唤醒调用者。

这条路线能同时覆盖当前最大的锁等待、completion 边界和 lease 持有放大，又不改变完整 POSIX 语义；copy-elision、metadata 合并和更细的 NUMA 调度都应建立在它完成并通过门禁之后。

## 11. Shard-63 实施与 VM 验证结果

### 11.1 代码交付

- 新增 `src/hfs_ticket.[ch]`：bounded inode-local ticket admission，默认深度 64，支持
  read/read 并行、overlap write/read/write 依赖、promotion、waiter wake 和 stop 清理；
- `src/hfs_io.c` 的 POSIX read/write 默认走 ticket，aligned steady-state 不再持有
  `hfsIoRangeLock()` 到 SPDK completion；`HFS_POSIX_TICKET=0` 保留旧路径；
- `src/hfs_inode_runtime.[ch]` 增加 ticket lock/condition/list 和 teardown 广播；
- `hfs_stats`/`hfs_trace` 增加 ticket admission、dependency wait、ready、promotion、wake
  统计与事件；新增 `scripts/db/posix_range_ticket_test.sh`；
- 修复 ticket init/admit 错误路径的未初始化读取和 waiter mutex/condition 泄漏。

### 11.2 功能和数据库验证

VM 使用原生 Git 获取的 SPDK `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`；Make/CMake
Release、runtime stress、POSIX contract、fd lifecycle、metadata transaction、IO trace、
queue engine、data reactor stripe、DMA backpressure 全部通过。专项 ticket 测试结果：

```text
ticket_admit=366 ticket_ready=366 ticket_complete=366
ticket_pending_peak=16 dependency_waits=237
spdk_submit=922 spdk_complete=922 errors=0
```

MariaDB/MySQL 的初始化、CRUD/DDL、事务、正常重启、SIGKILL recovery、4/16 并发客户端
全部通过；RocksDB 的 fillseq/readrandom/direct I/O/overwrite/flush/compact、60 秒
readwhilewriting、WAL SIGKILL recovery 和 `ldb checkconsistency` 全部通过。

### 11.3 NVMe FIO 门禁

同一 SPDK NVMe bdev `Nvme0n1`、`randrw=50/50`、O_DIRECT、64 MiB、HFS `numjobs=32`、
raw SPDK `iodepth=32`、`HFS_DATA_REACTORS=4`：

| bs | HFS IOPS | raw SPDK IOPS | HFS/raw | HFS read p99 / write p99 (µs) |
|---:|---:|---:|---:|---:|
| 4K | 83,959 | 123,325 | 68.1% | 889 / 897 |
| 8K | 80,604 | 111,129 | 72.5% | 987 / 1,004 |
| 16K | 75,120 | 94,309 | 79.7% | 954 / 979 |
| 32K | 59,571 | 73,771 | 80.8% | 1,122 / 1,155 |
| 64K | 44,311 | 52,712 | 84.1% | 1,516 / 1,565 |

正式 8K 10 秒重复三次为 `88,403/88,577/88,629 IOPS`，p99 读写均约 `0.83–0.85 ms`，
因此通过 `>=82K` 门禁；5 秒全矩阵用于块大小横向回归，短样本受 NVMe 波动影响。stats-on
归因样本中 `lock_io_wait_count=64`、`lock_io_hold_count=8`、`lock_io_hold_ns≈248 ms`，
`ticket_dependency_wait_ns≈263 ms`、`io_backend_outstanding_peak=32`、SPDK
submit/complete=`443010/443010`，说明旧 range lock 已不再覆盖稳态后端 completion，剩余
差距主要来自普通 POSIX buffer bounce copy、fd/metadata/线程同步和每请求 completion。

本 Shard 的 POSIX 兼容路径和数据库支持口径不变：多进程共享挂载、mmap、native
libaio/io_uring 仍不支持；下一阶段应在 ticket 证据基础上优化普通 buffer 的 copy 与
调用方提交批量化，而不是继续无上限增加 reactor 或 queue depth。
