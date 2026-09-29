# HFS 2.0 架构与性能优化点 Review

> 日期：2026-09-28
> 评审对象：当前工作区 `src/`、`rocksdb/`、`fio/`、`src/runtime/`、`src/metadata/`（HEAD `d84fb0f`）
> 事实源：`doc/design/hfs2.0.md`（本文不替代其接口/边界/验收约束；涉及设计变更的条目必须先更新该文档）
> 测量依据：`doc/performance/fio_baseline.md`、`doc/performance/rocksdb_baseline.md`、
> `doc/performance/mysql_baseline.md`（均为 2026-09-27/28 会话，6 vCPU VM、`VMware NVME_0002`）；
> 历史根因材料 `doc/performance/rocksdb_readrandom_rootcause_20260922.md`
> 关联：`doc/Architecture/hfs2.0_architecture.md`（仅作实现现状补充）、`doc/bug.md`
>
> 2026-09-28 复核修订：本文已按 `doc/design/hfs2.0.md` 与当前源码逐项复核。
> “代码中存在额外复杂度”只证明优化机会，不等同于已经证明它是当前性能差距的主因；
> 未经同会话 A/B 或 trace/perf 归因的收益均按假设处理。

## 1. 范围与结论摘要

### 1.1 评审范围

- 目标：在"追求极致性能、追求最低时间复杂度"的前提下，对 HFS 2.0 当前整体架构（前端 → Client → ring →
  shard actor → planner/DMA → bdev → completion → frontend）做一次完整走查，列出仍可优化的点、
  现状复杂度、目标复杂度、风险与验证方式。
- 不包含：不修改设计契约；不宣称任何未实现的性能目标；不覆盖多 NUMA、recovery 完整矩阵、
  MySQL native AIO（Phase 7 二期）等已知未闭合边界。

### 1.2 结论先行

1. **已实现的数据主路径在少线程深队列形状下基本成立，但不能据此断言整体架构已经闭合**。
   per-shard actor + bounded MPSC ring + callback 状态机在 1–2 job 深队列形状下达到
   SPDK raw 的 95–103%（4K QD32/64、16K、64K，`fio_baseline.md` §3），说明该口径下
   设备提交、planner、ring、completion 没有显著固定回退。当前实现仍有三项设计符合性缺口：
   namespace mutation 尚未收敛到 metadata actor；同步 DataPath 仍逐 I/O 发送 SPDK message；
   scheduler 的 interval tree 未平衡且 completion 扫描 pending FIFO。
2. **可确认的复杂度风险集中在四处**，但其中哪些主导当前 gate 仍需 A/B 证明：
   - **每请求固定成本**：全局 `std::mutex`（OperationPool、DmaArena、namespace）、O(N) 线性扫描
     （StatePool、backend slot、FIO pending、DmaArena `ownsRange`、`InodeExtentMap::lookup`）、
     4.6 KB/请求的无用清零、每 API 一次 `getpid()` syscall、shared_ptr 原子计数、前端每次提交的
     堆分配与 `getenv`。
   - **多 client/多前端线程形状下的共享结构争用**：8j4 FIO gate ≈0.764（`fio_baseline.md` §6 引用），
     64 线程 × QD1 只有 2×32 形状的 56%（`fio_baseline.md` §8.2），MySQL 64 线程写 84.8–93.6%
     （`mysql_baseline.md` §9.1/§12.1）。同一个每请求常数被前端线程数放大。
   - **bounce 拷贝与全局 epoch/全量快照**：设备读全部经 arena bounce 再 copy-out
     （`rocksdb_readrandom_rootcause_20260922.md` §16.3：`read_direct_user_children=0`，
     copy-out ≈4.1 µs/read）；`logicalEpoch` 是全局的，任何文件增长都会让所有文件快照失效；
     checkpoint 持 namespace 全局锁做 O(全量) 序列化。
   - **durability 与 scheduler 的隐藏 O(N)**：`publishDurableEpoch()` 每次 metadata epoch
     发布都扫描全部 inode；checkpoint 路径的 `prepareDurableEpoch()` 还复制全部 durable size。
     scheduler 使用未平衡 BST，最坏不是设计声称的 `O(log A + K)`，完成路径还会重扫 pending FIFO。
3. 已用实测反证的点：不存在"4× 固定软件开销"（rootcause §5）；planner 没有字节读放大
   （rootcause §12）；frontend 绑核不是主因（rootcause §6.2）；worker 直达 ring 在完整 workload 上
   没有超过 shared frontend（rootcause §18）——说明下一步优化应针对**每请求常数与争用**，
   而不是再次搬移拓扑。8j4 的 0.764 是历史会话结果，本次 Review 不能把它单独作为
   任一 P0 项的因果证据。

### 1.3 优先级总览

| 档位 | 条目 | 性质 | 主要受益口径 |
|---|---|---|---|
| P0 | 9 组局部优化 | 不改语义；池算法仍需并发正确性设计 | 8j4、RocksDB/MySQL 多线程、同步路径 |
| P1 | 7 项结构性修复 | 改实现，不改对外契约；含设计符合性修复 | 写增长下的读、MySQL 增长写、1M inode |
| P2 | 2 项契约/格式决策 + 1 项阶段级实现 + 1 项条件优化 | 仅涉及契约/格式者先改 `hfs2.0.md` | 设备受限读、checkpoint、长期扩展性 |

## 2. 架构现状与每请求复杂度账本

### 2.1 数据路径分段（与 tracepoint 对齐）

```text
FE_SUBMIT → [validate → snapshot → credit → Operation → sidecar → ring] → RING_RESERVE
  → ACTOR_BATCH → [plan → DMA lease → scheduler admit] → BDEV_SUBMIT
  → BDEV_COMPLETE → [ready queue → child 聚合] → PARENT_COMPLETE
  → CQ PUSH → CQ_REAP → FE_REAP
```

### 2.2 每请求复杂度账本

| 阶段 | 代码位置 | 现状 | 目标 | 档位 |
|---|---|---|---|---|
| 前端校验 | `src/core/engine.cpp:938` `processValid()` | 每次 API 1 次 `getpid()` syscall | O(1) 标志位 | P0-1 |
| Operation 分配 | `src/core/operation.cpp:31/58/99` | 全局 mutex + 4.6 KB 清零 | 无锁 freelist + 瘦身 | P0-2 |
| Sidecar 池 | `src/core/async_data_service.cpp:249` | O(pool) CAS 扫描 + 双次全量 reset | O(1) free stack + 按用量 reset | P0-3 |
| Backend slot | `src/core/spdk_async_backend.cpp:109` | O(maxInFlight) CAS 扫描（默认 4096 槽） | actor 私有 free stack | P0-4 |
| Extent 查询 | `src/metadata/inode_map.cpp:89` | O(E) 线性扫描（前端线程每读一次） | O(log E + k) 二分 | P0-5 |
| DMA 归属判定 | `src/core/dma_arena.cpp:207` | O(live) 全表扫描 + 全局 mutex | O(log N) interval index 或安全的摊销 O(1) 索引 | P0-6 |
| DMA 获取/释放 | `src/core/dma_arena.cpp:110/150` | 全局 mutex + hash 插入/删除 | per-class free stack | P0-6/P2-4 |
| RocksDB 异步读 | `rocksdb/hfs_filesystem.cpp:1271-1318` | 每读至少 1 次显式堆分配，回调可能再分配 + getenv + 2 次 entry mutex + 路径哈希 | 池化 + thread-local direct File 缓存 | P0-7 |
| FIO 槽位 | `fio/fio_hfs.cpp:692` | O(depth) 线性扫描 | O(1) free stack | P0-8 |
| 同步路径提交 | `src/runtime/spdk_runtime.cpp:2085/2095` | 每 I/O `new DataIoCall` + `spdk_thread_send_msg` + wake | reactor 本地有界提交队列 | P1-2 |
| 同步路径缓冲 | `src/core/data_path.cpp:211-215` | 每 I/O 4 次 vector 分配 | 栈上/复用 | P0-8 |
| Doorbell 唤醒 | `src/runtime/spdk_runtime.cpp:484` | runtime 全局 mutex + shared_ptr 拷贝 | 无锁 reactor 表 | P1-3 |
| File 快照 | `src/core/client.cpp:460-505` | 无条件 mutex + 全局 epoch + 失效时 O(E) 双拷贝 | 无锁发布 + per-inode epoch | P1-1 |
| Namespace 元数据 | `src/metadata/namespace.cpp:726/816/946` | reserve O(E) 快照、commit O(E) 整表拷贝 | O(log E + k) | P1-4 |
| Checkpoint | `src/metadata/namespace.cpp:1232` | 持 namespace 锁 O(N inode+entry+extent) | 增量、不阻塞 | P2-2 |
| Durable epoch | `src/metadata/namespace.cpp:1175/1195` | publish 每次 O(N inode)，prepare 复制全部 durable size | O(dirty inode) | P1-7 |
| Scheduler active index | `src/core/scheduler.cpp:169` | 未平衡 interval BST，最坏 O(A) | 有界/平衡 O(log A + K) | P1-6 |
| Scheduler complete | `src/core/scheduler.cpp:114` | 每次完成重扫 pending FIFO | 只处理受影响后继 | P1-6 |
| Scheduler ready scratch | `src/core/scheduler.hpp:90` | `std::vector::push_back` 可在 actor 热路径分配 | 预分配或 intrusive ready list | P1-5/P1-6 |

### 2.3 与设计文档的符合度

- §5.2 资源预约顺序、§6.3 ring/doorbell 的异步主路径、§7.4 parent completion 已在代码中落实；
  但同步 DataPath 的逐 I/O message、metadata mutation 的直接加锁以及 scheduler 复杂度与
  §6.3、§6.4、§7.2 尚不完全一致，不能统一归类为“只优化常数”。
- §16.5 门禁"trace off 时不执行 per-request unbounded allocation"：核心异步路径已满足
  （`AsyncDataService::submit` 在初始化后不分配），但存在两处例外：
  `File::Impl::asyncSnapshot` 失效重建（受 epoch 驱动，非每请求，但写增长负载下可接近每读一次）；
  RocksDB adapter 每读 `make_unique<AsyncReadState>`（`std::function` 是否额外分配取决于 SBO）；
  scheduler 的 `admittedScratch_` 首次或扩容时也会分配。OperationPool 首轮使用还会按请求
  `new (std::nothrow)`，虽然总数受 capacity 约束，仍会形成冷启动分配和尾延迟。

## 3. P0：优先级高的局部优化（按项验证）

> 共同特点：不改对外契约和磁盘格式。它们的实现风险并不相同：删除死字段、二分查找等是机械项，
> 池和并发所有权改造则必须单独设计、单独提交、单独 A/B，不能合并成一个改动单元后再归因。

### P0-1 移除每 API 一次的 `getpid()` syscall

- **现状**：`Client::submitRead/submitWrite/poll` 每次经 `validateAsyncClient → validateClient →
  Engine::processValid()` 调用 `::getpid()`（`src/core/engine.cpp:938-941`、`src/core/client.cpp:321/346/707`）。
  glibc ≥ 2.25 的 `getpid` 不再缓存，是真实 syscall。
- **成本**：每 I/O 提交一次 + 每轮 poll 一次；估算 0.3–0.7 µs/次 + 流水线扰动，8j4 形状下
  8 个前端线程 × 32k IOPS 量级。
- **目标**：安装 `pthread_atfork` child handler（只置原子 `forked` 标志），`processValid()` 读标志 +
  缓存 pid；fork child 在任何 core API 前即被判定。
- **风险**：handler 必须 async-signal-safe；不能默认任意 `std::atomic` 都满足 handler 约束，
  应使用明确 lock-free 的进程级 generation/flag 或 `sig_atomic_t`。如果未来允许 fork child
  创建全新 Engine，还要区分“继承的 Engine 失效”和“child 新建 Engine”；当前契约只要求
  继承对象 fail closed。
- **验证**：`perf stat -e syscalls:sys_enter_getpid`；FIO 8j4 与 MySQL 写 A/B；fork 单测保持。

### P0-2 OperationPool：全局锁 + 每请求 4.6 KB 清零

- **现状**：`OperationPool::create/destroy` 持全局 `std::mutex`（`src/core/operation.cpp:31/58`），
  跨前端线程与 reactor 线程争用；`resetForReuse()` 调 `asyncState_.reset()` 把
  `std::array<AsyncChildState, 64>`（≈4.6 KB，`src/core/operation.hpp:54-68`）整体清零。
- **证据**：生产异步路径只使用 `asyncState().childCount/submittedChildren/...`；
  `asyncState().children` 仅出现在 `tests/data_plane_test.cpp`（grep 结果）。该数组是 legacy 死重。
- **目标**：(a) 从 `AsyncIoState` 移除 64-child 数组并同步修改单测；
  (b) mount/初始化期预分配 Operation；池改为带 tag/generation 的 index freelist、已验证的
  bounded MPMC queue，或 per-frontend/per-shard cache + 有界中央池。
- **复杂度**：每请求 1 次跨线程 mutex + ~4.6 KB memset → O(1) 无锁 + ~0 清零。
- **风险**：移除死数组风险低；替换全局池不是“天然无 ABA”。节点即使不返还 OS，只要反复
  出栈/入栈复用就会发生 ABA；裸指针 Treiber stack 不可直接使用。spinlock 也不保证比 mutex
  更快，必须用争用 A/B 决定。需覆盖 pool 耗尽路径（返回 Busy 语义不变）。
- **验证**：`poolOverflow/liveCount` 计数不变；CTest；FIO 8j4；`perf lock` 观察争用下降。

### P0-3 Sidecar 池 O(N) CAS 扫描与双次 reset

- **现状**：`StatePool::acquire` 从 0 起逐槽 CAS 到第一个空闲（`src/core/async_data_service.cpp:249-263`）；
  `StatePool::release` 与 `release()` 各做一次全量 `RequestState::reset()`（`:191-225`，固定遍历
  64 child + 64 lease，典型请求只有 1 个 child）。
- **目标**：使用带 generation 的 bounded MPMC free-index queue，或按 frontend/shard 分区的
  owner-local free stack；`reset()` 只清上一轮 `childCount`/`leaseCount` 实际用到的条目，
  并只在 release 或 acquire 一侧执行一次完整资源交接。
- **并发约束**：`ClientState::reserveSlot` 是 frontend owner 私有，不能原样复制到 StatePool；
  StatePool 的 acquire 来自多个 frontend，release 来自一个或多个 reactor，是 MPMC 生命周期。
- **复杂度**：O(in-flight)/请求 → O(1)；reset 从 128 次遍历 → 1–2 次。
- **验证**：`async closure` 行（accepted/terminal/published）不变；CTest；FIO 8j4。

### P0-4 Backend slot O(maxInFlight) CAS 扫描

- **现状**：`SpdkAsyncDataBackend::acquireSlot` 从 0 起对 `slots_` 逐槽 CAS
  （`src/core/spdk_async_backend.cpp:109-122`）；`slots_`/`pending_` 大小 =
  `ceil(operation_pool / shardCount)`，单盘默认配置下为 **4096**（`src/core/engine.cpp:479`、
  `config/hfs_nvme.conf` `operation_pool = 4096`、`data_reactors = 1`）。
- **目标**：slots 只被所属 reactor actor 与其同线程 SPDK callback 触碰，
  per-slot `inUse` 原子可去掉，改 actor 私有 free stack（O(1)）；`inFlight_` 若仍供跨线程
  diagnostics/stop 查询则保留原子，不能与 slot owner 优化一起无条件删除。
- **验证**：`child_submit/child_complete` 闭合；CTest；FIO QD64。

### P0-5 `InodeExtentMap::lookup` 线性扫描（前端线程热点）

- **现状**：`lookup` 从 extent 0 线性扫到请求区间（`src/metadata/inode_map.cpp:89-120`）。
  调用点包括前端线程的 `ExtentSnapshotMap::capture`（`src/core/async_data_service.cpp:424`），
  即**每次 async read 提交**都会经 `File::Impl` 快照里的全量 extent map 做一次 O(E) 扫描；
  `DataPath`（同步路径）与 `ExtentSnapshotMap::lookup` 也走同一实现。
- **目标**：`std::lower_bound(logicalExtent)` 回退一格后线性前进/截断，O(log E + k)。
- **验证**：`io_planner_test`、`extent map` 单测；大文件（数千 extent）读延迟 A/B。

### P0-6 DmaArena 归属判定与释放路径

- **现状**：
  - `ownsRange` 在全局 mutex 下遍历 `live_` + `largeLive_`（`unordered_map`），
    `src/core/dma_arena.cpp:207-226`。读路径对非 arena buffer 是**否定也要扫完**，每 child 一次
    （`src/core/async_data_service.cpp:877`、`src/core/data_path.cpp:240/246`）。
  - `acquire` 在全局 mutex 下做 hash 插入（`:110-117`）；`release` 做 hash 查找 + **无条件**
    `cv_.notify_one()`（`:198`，无 waiter 也会进 futex）。
- **目标**：(a) 归属判定改为地址有序 interval map、页号/radix 索引或稳定 allocation descriptor，
  保持完整区间查询 O(log N) 或摊销 O(1)；(b) profile 证明 notify 有成本后，再增加在 mutex 下
  维护的 `waiters_` 并只在存在 waiter 时通知；(c) 评估把每个 RequestState 长期持有的
  16 KiB `reusableReadLease` 收敛成 per-shard cache，避免数千个长期 live allocation 放大否定查询。
- **风险**：`ownsRange` 语义必须保持"完整区间落在单个 live 分配内"。简单 header magic
  不能安全支持任意 interior pointer，也不能对 foreign pointer 向前读取 header；同时还要保持
  4 KiB 对齐和 `spdk_vtophys` 连续性，因此不作为默认方案。无 waiter 的 `notify_one()`
  不一定产生 futex syscall，收益必须测量。
- **验证**：`data_path_device_test`（direct/bounce 比例不变）；FIO 8j4；DMA busy 计数。

### P0-7 RocksDB worker-direct 读路径的重复查找与分配

- **现状**（`rocksdb/hfs_filesystem.cpp`）：
  - `ReadAsync` 每读 `std::make_unique<AsyncReadState>`（`:1271`）+ `std::function` 回调拷贝（`:1275`）；
  - 每读 `std::getenv("HFS_TEST_ASYNC_READ_DELAY_MS")`（`:1284`）；
  - 每读 `directWorkerEntry()`（thread_local map 查找，`:1301`）+ `directWorkerFile()`
    （`:1302` → `:554-568`：`entry->mutex` + `unordered_map<std::string, shared_ptr<File>>` 全路径哈希）；
  - 提交前再取一次 `entry->mutex`（`:1306`）。
- **目标**：(a) `AsyncReadState` 用有界池，容量按 live `io_handle` 而不是只按 client
  `maxOutstanding` 计算，因为完成后的 handle 可能尚未由 RocksDB deleter 释放；
  (b) 保持缓存 thread-local：以 context + file identity/path generation 为 key，或把
  `directWorkerFile` 查找、token 生成和 submit 合并到 DirectWorkerEntry 的一次锁内；
  不把单个 `DirectWorkerEntry*`/direct `File` 放进可跨 worker 共享的 `HfsRandomAccessFile` 普通成员；
  (c) 缓存 `getenv` 结果，测试如需动态切换则改用显式 test hook。
- **生命周期约束**：缓存失效继续与 `dropDirectPath`/retire 联动；`bug.md` #41 已证明按路径缓存
  open File 若不在 delete/rename 时失效会阻止 orphan extent 回收。
- **预期**：直接命中 `readrandom` 主形状（大工作集仍落后同盘 native direct 约 18%，
  rootcause §16/§18）。
- **验证**：RocksDB 大工作集 64 MiB cache 矩阵 A/B；`direct-calls` 统计；`hfsctl check`。

### P0-8 FIO 槽位与 DataPath 小成本

- **现状**：
  - FIO `findFreePending` 线性扫 `slotCount`（`fio/fio_hfs.cpp:692-699`）；
  - `DataPath::execute` 每 I/O 构造 4 个 `std::vector`（`src/core/data_path.cpp:211-215`）；
  - 每个 child 完成回调读 2–3 次 `getenv`（`:43-70`）；
  - 每 child 一对 `mutex/condvar` 交接（`:320/380/344/397`）。
- **目标**：FIO free stack；`executions/leaseSizes/leaseIndices` 用栈上有界数组
  或 bounded inline storage（plan.count ≤ 64 已知）；`getenv` 启动期缓存或 test hook；
  多 child 同步路径可进一步改成一个 parent wait/latch，避免每 child 一套 mutex/condvar。
- **已完成项**：RMW 第二阶段已经在 `DataPath::execute` 中通过 `prepareWait()` 重新 arm 并复用
  同一 `WaitState`，`bug.md` #39 已记录修复；本项不再列为待优化内容。直接放置
  `std::array<ChildExecution, 64>` 会在栈上构造 64 组 mutex/condvar，未测量前不应作为默认实现。
- **验证**：FIO 8j4、同步模式（`hfs_async=0`）回归。

### P0-9 其他每请求原子/引用成本（小项，也需独立归因）

- `AsyncDataService::submit` 的 `std::shared_ptr<void> sinkOwner` 与 `IoHandle::owner_` 会产生引用计数
  RMW（`src/core/async_data_service.cpp:441`、`src/core/client.cpp:669/701`）。不能直接改成裸指针 +
  现有 `handleCount`：frontend IoHandle 可以在 terminal completion 前释放，而 sink 仍必须存活。
  若 profile 证明有收益，应增加独立的 intrusive in-flight sink ref，并用关闭/提前释放用例验证。
- `File::read`（同步路径）每次调用重建 `extentMapFor`（`src/core/client.cpp:986` → O(E) 双拷贝），
  与 P1-4 一并处理。**已实施（2026-09-29）**：`File::read/write/append` 复用 per-inode
  快照（`cachedSnapshot`/`syncSnapshot`，churn 时回落 `buildSnapshot`）；见
  `hfs2_p09_remainder_20260929.md`。
- `AsyncDataService::prepare` 在 `stats=false` 时仍执行 planner 统计累加循环
  （`src/core/async_data_service.cpp:749-790`；`recordDataReadPlan` 内部才判开关）：
  加 `diagnostics::profilingEnabled()` 守卫；同步 `DataPath::execute` 的同类统计循环也要一起处理。
- `isBufferDmaCapable` 在 direct 路径被调用两次（`async_data_service.cpp:878` 与
  `spdk_async_backend.cpp:239`），可把 `spdk_vtophys` 结果缓存在 lease/snapshot 上。
  **已实施（2026-09-29）**：`Request::dmaVerified` + `ChildState::directVerified`，
  backend 对已验证的 direct 子请求跳过第二次 vtophys；见
  `hfs2_p09_remainder_20260929.md`。

## 4. P1：结构性修复（影响 8j4 与多线程口径）

### P1-1 File 快照缓存：per-inode data-map epoch + immutable 发布

- **现状**：`File::Impl::asyncSnapshot` 每次提交先锁 `asyncCacheMutex`，缓存 key 是
  **全局** `namespaceState->logicalEpochFast()`（`src/core/client.cpp:460-505`）；
  `Namespace::touch()` 在任何 size/extent 变化时递增全局 `logicalEpoch_`
  （`src/metadata/namespace.cpp:146-156`）→ **任何文件的增长都会让所有文件的下一次读重建快照**。
  重建路径 = `stat` + `extentMapFor`（`client.cpp:306-319`，`namespace->extents()` 全量拷贝
  `namespace.cpp:946-954` + `InodeExtentMap::restore` 再拷贝 + `make_shared`），全程持 namespace 全局锁。
- **目标**：(a) 新增只覆盖 logical size/extent/file type 的 per-inode `dataMapEpoch`，不要直接
  复用同时承载 namespace/durability 含义的 `logicalEpoch`；(b) metadata owner 发布
  `shared_ptr<const FileDataSnapshot>` 或等价 immutable view，frontend 快路径不再分别执行
  `stat + extents + restore`；(c) 使用安全的 atomic shared ownership、RCU/hazard 或明确 reader
  lifetime，不能仅用 seqlock 发布随后可能被释放的裸指针。
- **正确性论证要点**：快照只承载单 inode 的 data I/O 所需字段，跨文件变更不影响其内容；
  当前 rename/link/unlink 实际会对相关 inode 调用 `touch()` 并修改 `logicalEpoch`，所以直接使用
  现有 epoch 仍会产生不必要失效。FileHandle generation/inode slot 复用必须参与快照身份校验。
- **验证**：新增 trace 点 `SnapHit/SnapBuild`（§7）；MySQL/RocksDB 写增长负载下读回退；
  RocksDB L4 门禁（`doc/design/hfs2.0.md` §20.7）。

### P1-2 同步路径每 I/O 一个 SPDK message

- **现状**：`SpdkRuntime::submitDataRead/submitDataWrite` 每个 I/O 都
  `new Impl::DataIoCall` + `spdk_thread_send_msg` + `wakeDataReactor`
  （`src/runtime/spdk_runtime.cpp:2085-2101/2122-2138`），由 `DataPath` 使用
  （`src/core/data_path.cpp:316-339/376-393`：MySQL 增长写、同步读回退、`hfs_async=0`、`zeroRange/zeroExtents`）。
- **已有证据**：MySQL Shard-108 前 `read_only` 只有 native 的 15.1%，`perf` 显示连接线程 64.2% 采样在
  `spdk_ring_enqueue` 自旋（消息环满），根因正是这条同步逐 I/O 路径；惰性建立 thread client 后恢复到 96.1%
  （`mysql_baseline.md` §10）。当前它仍是所有回退/增长写/零填充的必经之路。
- **目标**：为每个 data reactor 增加有界 **MPSC** ingress（与 shard ring 同族）：
  多个调用线程只入队 + empty→non-empty doorbell，reactor poll 批处理并按序提交；请求节点可嵌入
  当前会一直等待 completion 的 `ChildExecution`，或来自有界池，避免逐请求 heap allocation；
  保留"每个请求恰好一次完成"语义。
  或：若调用线程已持有目标 shard（未来 actor 化后），直接复用既有 shard ring。
- **更正**：单个 reactor 会同时接收多个 MySQL/FIO/frontend 线程的请求，不能使用单一 SPSC queue；
  若选择 SPSC，必须是 per-frontend channel，并额外解决注册、扫描和关闭生命周期。
- **风险**：提交方当前依赖"回调在 reactor 线程 + 阻塞等待"；改造需保持回调线程模型与 timeout 语义；
  `zeroExtents` 等长流程要避免队满死锁（调用方等待期间必须持续推进 reactor）。
- **验证**：优先用 MySQL 增长写（redo/page 文件）与 `hfs_async=0` FIO；
  `perf` 核对 `spdk_ring_enqueue` 采样消失；closure 闭合。

### P1-3 Doorbell 无锁化

- **现状**：`AsyncDataBackend::wake()` → `SpdkRuntime::wakeDataReactor`
  先锁 `impl.mutex`，再拷贝 `std::shared_ptr<Reactor>`（2 次原子 RMW），最后判 `idleWait` 决定是否写 eventfd
  （`src/runtime/spdk_runtime.cpp:484-497`；调用点 `src/core/async_data_service.cpp:490-493`）。
  ring `empty→non-empty` 越频繁，这个跨线程锁就越热。
- **目标**：active generation 内 reactor 表不可变，`wakeFd/idleWait` 通过稳定发布的 handle 访问，
  `wake()` 快路径无 runtime mutex；仅当 `idleWait==true` 时写 eventfd。
- **生命周期约束**：runtime 支持 stop 后再次 start，stop 会销毁 reactor 并关闭 eventfd；不能仅发布
  裸 `Reactor*`。需要 ingress close/drain 栅栏 + generation 校验，或 atomic shared ownership/RCU，
  并覆盖 wake 与 stop 并发测试。
- **验证**：FIO 8j4（8 producer）；`doorbellWakes/doorbellTimeouts` 统计；
  `NotifyWrite/NotifyWait/NotifyRead` 轨迹不变。

### P1-4 Namespace 元数据 O(E) 操作

- **现状**（均在全局 `mutex_` 下）：
  - `reserveExtents` 每次写都 `inode.extents.snapshot()` 全量拷贝（`src/metadata/namespace.cpp:752`），
    即使请求区间已完全映射（fixed overwrite / redo 场景不产生分配）；
  - `commitExtents` 做 `InodeExtentMap candidate = inode.extents;` 整表拷贝（`:831`）后
    `addExtents` 再 O(E) 插入；
  - `extents()` 全量拷贝（`:946-954`）；
  - `File::read`（同步）每次调用重建 extent map（`src/core/client.cpp:986`）。
- **目标**：全部改二分定位 + 只操作相关区间；完全映射的 overwrite fast path 不构造任何 snapshot；
  `commitExtents` 先验证所有 descriptor 和 allocator state，再一次性构造/合并候选区间；
  `extents()`/快照共享 immutable 视图（与 P1-1 的版本化配合）。
- **风险**：reserve/commit 的原子性与回滚语义（rollback/allocator 状态守恒）必须逐条保持；
  不能简单“先原地插入再提交 allocator”，否则后续 allocator commit 失败时难以恢复旧 map。
  需要 extent map 单测覆盖"区间完全已映射"、批量相邻/离散插入、部分 commit 失败和 rollback。
- **验证**：extent 相关单测；大文件/长写负载下的写 TPS 与 p99；`hfsctl check`。

### P1-5 按实际用量约束 reset 与对象 footprint

- 与 P0-2/P0-3 同源：`RequestState` 约 12 KB × 4096、`AsyncIoState` 4.6 KB × Operation。
  目标：热/冷字段分离；`reset` 只触碰本轮使用过的 child/lease；
  scheduler 的 `admittedScratch_` 改预分配或 intrusive ready list；必要时把
  `Plan::kMaxChildren` 从编译期 64 拆成"热路径 4 + 有界溢出块"，溢出块必须来自 bounded pool，
  不能把热路径重新变成普通 heap allocation。
- 验收：trace-off 下 per-request allocation 审计（设计 §16.1）+ FIO 8j4。

### P1-6 Scheduler 未平衡 interval tree + `complete()` 重扫 pending FIFO

- **现状**：每次完成从 FIFO 头贪心重扫（`src/core/scheduler.cpp:114-133`），最坏 O(pending)/完成；
  active interval index 是没有旋转/颜色/高度的普通 BST（`:169-262`），单调 offset 可退化为链，
  admission/conflict/remove 最坏 O(A)，不满足设计 §7.2 的 `O(log A + K)`。
- **观察**：RocksDB/MySQL 单 writer 语义下 pending 通常为 0，当前不构成瓶颈；
  若未来出现多 writer 重叠写，这是 O(pending²) 级风险。
- **建议**：先补单调区间和高 pending microbenchmark；active index 改为 intrusive augmented
  balanced tree 或等价的有界结构。pending 不能仅按“与本次完成区间重叠”粗略筛选：一个节点可能
  同时依赖多个 active node。可维护 blocker count/reverse dependency，或用 pending interval index
  找出受本次完成影响的节点，再重新检查完整 active conflict set。barrier 的 FIFO 边界必须保持。
- **定位**：当前 workload pending 通常很小，性能优先级可低，但这是既定设计复杂度尚未落实，
  不属于需要先改设计的项目。

### P1-7 Durable epoch 只处理 dirty inode

- **现状**：`Namespace::publishDurableEpoch` 每次发布都遍历全部 `inodes_`；checkpoint 路径的
  `prepareDurableEpoch` 先复制全部 inode 的 durable size，再二次扫描更新，失败时 rollback 又扫描。
  在 1M inode 目标下，普通 metadata barrier 也会承担 O(N) CPU/锁持有时间。
- **目标**：每个 inode 保存 dirty generation/list membership；touch/size/extent mutation 只把 inode
  首次加入对应 epoch 的 dirty list。prepare/publish/rollback 只访问 `epoch <= target` 的 dirty inode，
  保存必要 before-image，并在成功后移出列表。全局 logical/durable epoch 语义不变。
- **复杂度**：O(all inode)/barrier → O(dirty inode covered by barrier)。
- **验证**：1M inode、少量 dirty inode 的 sync microbenchmark；并发 group commit；checkpoint 失败
  rollback；重启后 logical/durable size 守恒。

## 5. P2：格式/前端契约变更与阶段级优化

> P2-1 的 RocksDB/InnoDB frontend 契约和 P2-2 的增量磁盘格式实施前必须先更新
> `doc/design/hfs2.0.md`。P2-3 的 metadata actor 已是设计既定目标，属于阶段级实现收敛，
> 不应以“需要先修改设计”为前提。P2-4 只有 profile 证明 central arena 是瓶颈时才启动。

### P2-1 前端 zero-copy DMA（最大单点数据面收益）

- **现状**：RocksDB/MySQL 的用户 buffer 不是 arena 内存，`acquireLeases` 里
  `directCapable = arena_.ownsRange(...) && backend->isBufferDmaCapable(...)` 恒为 false →
  设备读经 arena bounce 后由 reactor 线程 copy-out 到用户 buffer
  （`src/core/async_data_service.cpp:872-886/1178-1200`）；
  rootcause §16.3 实测 `read_direct_user_children=0`、copy-out ≈4.1 µs/read。
- **方向 A**：InnoDB buffer pool（page cleaner/log buffer）从 HFS hugepage arena 分配
  （`Engine::acquireDmaBuffer` 已是公共 API），使 `ownsRange`/`spdk_vtophys` 直接成立。
  需要评估 `dma_arena_mb` 预算（默认 512 MiB 需容纳 buffer pool 切片）与 NUMA。
- **方向 B**：RocksDB `FSSupportedOps::kFSBuffer` + buffer allocator 契约（8.9.1/9.x 头文件实测）；
  设计 §11.2 当前明确"v1 不声明"，需要修订为可选的 zero-copy 模式。
- **边界**：core 设计 §8.1 已允许通过 `spdk_vtophys`/对齐/长度检查的 caller buffer direct，
  因而 core 内部消除不必要 bounce 不一定需要新 ABI；但 InnoDB allocator 接管和 RocksDB
  `kFSBuffer` 会改变 frontend 契约。RocksDB 8.9.1 的 `kFSBuffer` 只适用于其特定 non-direct
  MultiRead 语义，不能假设在当前 `use_direct_io()==true` 路径中声明后即可生效。
- **验证**：`read_direct_user_children > 0`；copy-out 计数/字节下降；
  正式 gate 必须 trace/stats off 且同会话 A/B。

### P2-2 增量 checkpoint

- **现状**：`Namespace::saveSnapshot` 持 namespace 全局 `mutex_` 完成全量序列化
  （`src/metadata/namespace.cpp:1232-1286`，含每个 inode + 目录 entry + extent + allocator 位图），
  由 barrier 线程调用（`src/core/engine.cpp:795-813`）。数据集大时是所有元数据操作的 stop-the-world。
- **证据**：设计 §17.1 目标 1M active inode；设计 §10.2 已预留"增量 dirty-object checkpoint 留待
  后续格式版本"。当前矩阵数据集（RocksDB 2M key 约 2 GB、MySQL 10 万行）看不出问题，
  但这是 1M inode 目标的硬风险。
- **两步目标**：(a) 不改磁盘格式的第一步：在 metadata owner 上冻结 immutable/COW full snapshot，
  锁外完成现有格式序列化和设备写，缩短 stop-the-world，但总字节仍为 O(N)；
  (b) 格式版本升级后实现 dirty inode/namespace/allocator 增量 checkpoint，recovery 组合 base + delta。
- **一致性要求**：仅把序列化移出 mutex 不足以保证一致快照；必须定义冻结 generation、后续 mutation
  进入 journal/delta 的边界，以及 checkpoint publish 前失败时的回退规则。
- **验证**：mount/replay 预算（设计 §17.1）+ crash 矩阵（§16.4）。

### P2-3 Metadata actor 收敛

- **现状**：当前 `Client` task 仍直接调用 `Namespace` mutation 并获取其全局锁，metadata reactor
  主要负责控制 I/O；数据路径构建读快照也会进入这把锁。`doc/Architecture/hfs2.0_architecture.md`
  §3.2 可作实现现状旁证，但架构判定仍以 `doc/design/hfs2.0.md` 为准。
- **目标**：namespace mutation、journal intent、durability request 统一收敛到 metadata actor
  ingress（设计 §4.2 目标模型），读侧提供无锁 immutable 视图（与 P1-1/P1-4 配套）。
- **风险**：Phase 级改造，涉及关闭顺序、取消与 replay 路径；不阻塞当前性能开发。
- **重新分类**：设计 §4.1、§6.4、§7.2 已明确 metadata actor ownership，因此这是设计符合性
  实现，不需要先修改架构边界。若实施需要改变 public Task 完成线程、错误/取消或关闭契约，
  再对这些具体变化单独修订设计。

### P2-4 Per-shard / per-class DMA arena

- **现状**：单个 mount-level arena 服务所有 data shard、同步路径与 FIO，锁与 `ownsRange`
  都是跨线程热点（`src/core/dma_arena.hpp:95-108`）。
- **目标**：按 reactor/shard 切分 arena 分区 + 全局上限保持有界；
  per-class free list 无锁化（单 owner 时天然无锁）。
- **启动条件**：当前正式配置只有一个 data reactor，async lease 主要由所属 actor 获取/释放；
  先完成 P0-6 的 membership/index 优化并用 lock profile 证明 central arena 仍有争用。
  设计 §8.2 已规定 central arena 是第一实现、profile 证明后才增加 bounded local magazine。
- **验证**：`dma_arena_mb` 预算（设计 §8.2/§8.3 hugepage preflight）；
  DMA busy / high watermark 统计；FIO 8j4。

## 6. 与已测结果的对应关系（为什么形状决定差距）

| 口径 | 结果 | 证据 | 与本文的对应 |
|---|---|---|---|
| FIO 4K/16K/64K，总 QD32/64（1–2 job） | HFS/raw 94.9–102.6%，p99 87–96% raw | `fio_baseline.md` §3/§4 | 深队列少线程时每请求常数被流水掩盖 |
| FIO 8j4 正式 gate | 历史中位比值 0.764 | `fio_baseline.md` §6 引用 | 8 前端线程放大 P0/P1 常数与争用 |
| FIO 64 线程 × QD1 | 60.3k vs 2 job × 32 的 106.8k（56%） | `fio_baseline.md` §8.2 | 6 vCPU 上的线程调度 + 每请求 CPU |
| RocksDB 32T fillseq/fillrandom | 97.3–106.8% native | `rocksdb_baseline.md` §3/§8.1 | 写路径基本持平；瓶颈在 memtable/compaction CPU |
| RocksDB readrandom | 129–401% native（native 冷读塌陷） | `rocksdb_baseline.md` §5.1 | HFS 读路径无软件优势，也没有劣化 |
| RocksDB 大工作集 | 同盘落后 native direct 约 18%，无字节放大，全 bounce | rootcause §16/§18 | P0-7 + P2-1 |
| MySQL read_only（大工作集） | 修复前 15.1% → 96.1% | `mysql_baseline.md` §10 | P1-2 的前车之鉴；同步逐 I/O 路径不可用于热数据 |
| MySQL write_only | 83.5–93.6% | `mysql_baseline.md` §9.1/§12.1 | 屏障设备往返（固有）+ 增长写走 P1-2 路径 |
| MySQL point_select/read_write | 91.6–99.5% | 同上 | 热数据在 buffer pool 内，瓶颈是 CPU |

结论：**每请求常数 × 前端线程数** 是与当前形状一致的解释变量，但除 MySQL 同步回退外，
现有材料没有逐项证明 OperationPool、StatePool、DMA membership 等分别贡献了多少差距。
物理设备与 planner 在少线程深队列口径下不是主瓶颈；局部优化应拆成可独立 A/B 的小批次，
不能把 P0-1…P0-9 合成一个性能提交后再归因。

## 7. Tracepoint 缺口与补充建议

### 7.1 已发射点位（数据主链基本完整，durability 链未闭合）

`FeSubmit/FeReap`、`PollSpin/PollReturn`、`ShardMap/ShardPump`、`RingReserve/Doorbell/Busy`、
`ActorBatch/ActorPickup/PlanReady`、`SchedWait/Admit/Done`、`DmaAcquire/Busy/Release`、
`ExtentPlan`、`ShardBackendSubmit/Complete`、`BdevSubmit/Complete`、`ParentComplete`、
`CqPush/CqReap`、`NotifyWrite/Wait/Read`、`RequestShardSubmit/Complete`、
`RequestBdevSubmit/Complete`、`RequestParentComplete`、`CopyIn/CopyOut`、`BackendErr`。

`MetaTxBegin/Commit`、`JournalFlush`、`EpochPublish` 当前只在
`metadata::GroupCommitCoordinator` 中发射，而生产 `Engine::syncMetadata()` 不使用该 coordinator；
因此不能把它们视为当前 durability 主链已经覆盖。`JournalAppend`、checkpoint、fsync、recovery、
retry/busy 点位仍需在真实生产路径补齐。

### 7.2 枚举已定义但未发射（先补发射成本最低）

| 事件 | 枚举 id | 建议发射位置 | 用途 |
|---|---|---|---|
| `ChkptBegin/End/Bytes` | 25/26/27 | `Engine::syncMetadata` checkpointPhase 前后 | P2-2 停顿归因 |
| `FsyncBegin/End` | 28/29 | `Engine::syncMetadata` 入口/出口 | MySQL 写分段 |
| `JournalAppend` | 23 | 生产 `Journal::append` 成功边界 | barrier 分解 |
| `Retry` | 34 | backend `WaitingForCredit`/retry 路径 | 背压分布 |
| `BusyReject` | 35 | `AsyncDataService::submit` 的 Busy 分支 | 容量拒绝 |
| `RecoveryBegin/End` | 31/32 | `Journal::replay` | mount 分解 |

### 7.3 建议新增点位（追加 id，保持只增不改）

| 事件 | 建议 id | 参数 | 采样 | 用途 |
|---|---|---|---|---|
| `FeSubmitBegin` | 53 | requestId, fileId, write | 高频，按 request id 成对采样 | 量出 validate+snapshot+submit 的前端固定成本 |
| `SnapHit` | 54 | fileId, epoch | 高频（按 request 采样） | P1-1 缓存命中率 |
| `SnapBuild` | 55 | fileId, epoch, extendCount, ns | 高频 | P1-1 重建频率与耗时 |
| `MetaLockWait` | 56 | opKind, ns | 仅 >阈值记录 | P1-4/P2-3 namespace 锁等待 |
| `OpPoolAcquire` | 57 | requestId, contended, ns | 高频采样 | P0-2 池争用；当前池不执行 wait |
| `DmaOwns` | 58 | bytes, found, ns | 高频采样 | P0-6 归属判定成本 |

配套工具改动：`tools/hfs_trace_dump.cpp` 的分段表增加
`FeSubmitBegin→FeSubmit→RequestShardSubmit→RequestBdevSubmit→RequestBdevComplete→
RequestParentComplete→CqPush→FeReap` 的推导与 p50/p99；`trace-manifest.json` 校验规则不变。

### 7.4 门禁

- trace off 时新增点位必须是 cached-bool + 采样序列的自增，不引入 per-event 原子/分配；
- 当前 `trace::event()` off 快路径实际读取 process-global atomic bool，并非 per-reactor cached bool；
  新增高频调用点前应先统一实现与设计 §16.5 的 cached-bool 约束，避免为每个建议点重复增加全局 load；
- 全量短跑要求 `trace_dropped == 0`（设计 §16.5）；
- 正式性能数字仍必须 trace/stats off。

## 8. 建议实施顺序与验收

### 阶段 A：归因与低风险机械项

- 内容：先补生产 durability trace 与 targeted microbenchmark；随后分别实施 dead
  `AsyncChildState[64]` 移除、extent lookup 二分、stats-off guard、test getenv 缓存、backend
  actor-local free stack、FIO frontend free index。每个逻辑项独立提交和 A/B，不把 P0 全部打包。
- 验收：CTest `26/26`；涉及热路径的项用 FIO 8j4 同会话 A/B（≥1 组 5×）；backend/extent
  项检查 closure；有效收益再扩展 RocksDB 大工作集与 MySQL。`hfsctl check` 必须
  `corruption=none`。
- 停止条件：任何 data mismatch、closure 不闭合、崩溃恢复失败 → 按设计 §17.3 立即停止。

### 阶段 B：有界池与 immutable data snapshot

- 内容：使用 ABA-safe 的 Operation/State pool；完成 P1-1/P1-4 的 per-inode data-map epoch、
  immutable extent snapshot 与 mapped-overwrite fast path；P1-7 dirty-inode durable publication。
- 验收：pool 长时间 churn/TSAN；MySQL/RocksDB 写增长场景读回退（RocksDB L4 门禁）；
  1M inode 少量 dirty sync；新增 trace 显示 `SnapBuild` 不再随其他文件写流量增长。

### 阶段 C：同步 ingress 与 runtime 生命周期

- 内容：P1-2 bounded MPSC ingress；完成后再评估 P1-3 doorbell 无锁化与 P1-5 hot/cold split。
- 验收：MySQL 增长写与 `hfs_async=0` 回归；多 producer/queue-full/timeout/stop 并发；
  `perf` 核对 `spdk_ring_enqueue` 消息环自旋消失；callback/closure 线程模型不变。

### 阶段 D：scheduler、metadata actor 与 P2 设计修订

- scheduler 先以 microbenchmark 证明触发条件，再实现 balanced active index/受影响后继准入；
  metadata actor 按既有设计收敛，不以修改设计为前置条件。
- zero-copy frontend 契约与增量 checkpoint 格式先修订 `hfs2.0.md` 并评审；arena 分区只有
  P0-6 后的 profile 仍证明 central arena 是瓶颈时才进入计划。

### 风险与缓解（与设计 §17.2 对齐）

| 风险 | 缓解 |
|---|---|
| 池改造引入 ABA/重复分配 | 使用 tagged index/已验证 bounded queue；长时间 churn、TSAN 与 pool closure 门禁 |
| P1-1 快照失效语义错误 | 独立 data-map epoch + FileHandle generation；RocksDB L4/MySQL 回归 |
| P1-2 改造破坏回调线程模型 | 使用 MPSC ingress；保留"回调在 reactor 线程 + 调用方有界等待"；先接 FIO `hfs_async=0` |
| P1-3 wake 与 stop 竞态 | generation/lifetime guard；并发 wake/stop/start 回归，禁止裸 reactor 指针逃逸 |
| P1-7 dirty list 遗漏 inode | epoch 去重 + before-image；1M inode、失败 rollback 与 crash recovery |
| zero-copy 引入 buffer 生命周期风险 | 先做 InnoDB buffer pool 子集试点；RocksDB kFSBuffer 单独评审 |
| 跨会话设备漂移 | 同会话交错 A/B；跨会话只比比值 |

## 9. 附带发现（不属于优化项）

1. 工作区 `doc/performance/baseline.md` 处于"已删除未提交"状态（`git status` 显示 ` D`）。
   若本次拆分（`fio_baseline.md` / `rocksdb_baseline.md` / `mysql_baseline.md`）是有意的，
   请把该删除一并提交，否则 `AGENTS.md` 引用的基线入口会缺失。
2. `trace.cpp` 的 writer 线程 1 ms 轮询 drain（`src/common/trace.cpp:278-281`）在 trace on 全量时
   可能成为背压来源之一；如需高频全量归因，建议改为 eventfd/条件变量唤醒（仅影响诊断路径）。
3. `SpdkRuntime::deviceCapacityBytes/deviceBlockSize` 每次取 `impl.mutex`
   （`src/runtime/spdk_runtime.cpp:2027-2041`），被 `Engine` 公开 API 与工具调用；不在数据热路径，
   可改为 mount 后只读缓存。
4. `Namespace::publishDurableEpoch` 与 scheduler 未平衡问题原 Review 未覆盖，已分别补为 P1-7
   与扩展后的 P1-6。
5. `scheduler::admittedScratch_` 可在 actor 热路径分配；这意味着“核心异步路径初始化后完全不分配”
   目前并不成立，需与 P1-5 一并收口。

## 10. Stage A 实施记录（2026-09-28）

> 本节记录按 §8 阶段 A 已实施并验证的内容，作为后续阶段的事实基线。
> 每一项都保留代码位置与验证证据；未做的单项 A/B 明确标注。

### 10.1 已实施项

| 项 | 内容 | 提交 |
|---|---|---|
| §7 trace | 生产 durability 链：`JournalAppend`、`ChkptBegin/End/Bytes`、`FsyncBegin/End`、`RecoveryBegin/End`、`BusyReject`（5 种原因）、`Retry`（DMA/backend credit）；trace 会话提前到 mount 开始时启动，失败由 `MountTraceGuard` 回收 | `feat(trace): emit production durability and backpressure trace points` |
| P0-2 部分 | 移除 `AsyncIoState` 中未被生产路径使用的 `AsyncChildState[64]`（每 operation 省约 4.6 KB 清零） | `perf(core): remove per-request constants and O(N) scans from the data path` |
| P0-4 | `SpdkAsyncDataBackend` 改 actor 私有 free list（O(maxInFlight) CAS 扫描 → O(1)），保留 exactly-once release 守卫与 `inFlight()`/新增 `freeSlots()` 诊断 | 同上 |
| P0-5 | `InodeExtentMap::lookup` 改二分（O(E) → O(log E + k)），新增 `tests/inode_map_test.cpp` 正确性 + 微基准 | 同上 |
| P0-9 部分 | `stats=false` 时跳过 read-plan 统计累加（异步路径与 `DataPath`） | 同上 |
| P0-7/P0-8 部分 | `DataPath` 完成回调、RocksDB `ReadAsync` 的测试延迟钩子改缓存原子值，生产路径不再 `getenv`；测试改用显式 setter | `perf(frontend): free-slot stacks and cached test hooks for FIO/RocksDB` |
| P0-8 部分 | FIO `findFreePending` 线性扫描改 free stack（`clearPending` 幂等入栈） | 同上 |

### 10.2 验证证据

- **extent lookup 微基准**（同机、同 test、4096 extents × 1,000,000 lookups）：
  - 旧线性扫描（`git HEAD` 源码单独编译）：661.6 ns/lookup
  - 新二分实现：69.4 ns/lookup（约 9.5×）
- **FIO 8j4 同会话交错 A/B**（4K `randrw` 50/50、256 MiB、8 jobs × iodepth 4、
  10 s、每侧 5 轮取中位数，baseline = `git HEAD` 单独构建，Stage A = `build-verify`）：

  | 侧 | 中位 IOPS | p99 读/写（µs） | closure |
  |---|---:|---|---|
  | baseline | 57679.6 | 1019.9 / 1019.9 | yes |
  | Stage A | 58241.3 | 995.3 / 995.3 | yes |

  `new/baseline = 1.0097`，p99 约 −2.4%；所有轮次 `queued=reaped`、`outstanding=0`。
  该 A/B 为**组合 A/B**（P0-2/4/5/7/8/9 同时生效），未做逐项 A/B；
  其中 extent 二分对大文件以外形状接近零影响，其余为每请求常数项。
- **生产 trace 点位**：真实 NVMe 上 `hfs_db_bench` trace-on 短跑，
  `manifest_dropped=0`、`manifest_closed=true`；dump 计数
  `event=23`（JournalAppend）、`25/26/27`（Chkpt）、`28/29`（Fsync）、`31/32`（Recovery）。
- **回归**：构建后完整 CTest `27/27` 通过（含真实 `Nvme0n1` 设备测试），
  新增 `inode_map_test`；测试后设备已恢复内核 `nvme` 驱动。

### 10.3 未包含在阶段 A 的内容（保持原优先级）

- P0-1 `getpid()`、P0-2 池无锁化、P0-3 StatePool、P0-6 DmaArena、P0-7 直接
  File 缓存、P0-8 DataPath 有界数组：留给阶段 B/C，按 §8 分项设计、提交与 A/B。
- P1/P2 各项不变。

## 11. Stage B 实施记录（2026-09-29）

> 按 §8 阶段 B 实施：有界池 + immutable data snapshot + 元数据 O(log)/dirty。
> 与 Stage A 相同口径：未做的逐项 A/B 明确标注，组合 A/B 与专项证据分开列出。

### 11.1 已实施项

| 项 | 内容 | 提交 |
|---|---|---|
| P0-1 | `processValid()` 移除每 API 的 `getpid()`：缓存 pid + `pthread_atfork` child 标志（只存 lock-free flag），fork child 仍 fail closed | `6d586c4` |
| P0-2/P0-3 | 新增 `BoundedIndexStack`（tagged head、预分配、ABA-safe）；`OperationPool` 去掉全局 mutex 并按 capacity 一次性预分配；`StatePool` 去掉 O(pool) CAS 扫描；`RequestState::reset` 只清本轮用到的 child/lease | `e714af3` |
| P0-6 | `DmaArena` live 分配改地址有序 map（`ownsRange` O(log N)），`release` 仅在存在 waiter 时 notify | `b2a9c54` |
| P1-1 | 新增 per-inode `dataVersionFast`（lock-free），`File::Impl::asyncSnapshot` 改以该版本为 key：其他文件增长不再使本文件快照失效；rename/link 等形状变更不失效；extent 提交/尺寸变化才失效。新增 `SnapHit`/`SnapBuild` trace 点与 snapshot 诊断计数 | `113f36f` |
| P1-4 | `reserveExtents` 二分定位后只走区间内 descriptor（不再全量 snapshot）；`commitExtents` 用 `validateInsert` 原位校验后直接插入（不再整表拷贝）；`addExtents` 增加单调增长 append 快路径 | `113f36f` |
| P1-7 | `publishDurableEpoch`/`prepareDurableEpoch`/`rollbackPreparedDurableEpoch` 只访问自上次发布以来 size 变化的 inode（dirty list + before-image） | `113f36f` |

### 11.2 验证证据

- **组合 A/B（Stage A → Stage B，同会话交错 5 轮）**：FIO 8j4 口径（4K `randrw`
  50/50、256 MiB、8 jobs × iodepth 4、10 s）：

  | 侧 | 中位 IOPS | p99 读/写（µs） | closure |
  |---|---:|---|---|
  | Stage A（`git HEAD` 单独构建） | 76851.1 | 667.6 / 667.6 | yes |
  | Stage B | 81892.4 | 626.7 / 626.7 | yes |

  `StageB/StageA = 1.0656`，p99 约 −6.1%，所有轮次 closure 完整。
- **P0-1 专项**：同一 160k-op RocksDB workload 的 `syscalls:sys_enter_getpid`：
  pre-A 5410、Stage A 5408、Stage B **258**（−95.2%）。
- **P1-1 专项**（`snapshot_cache_device_test`，真实 NVMe）：
  `hits=3 builds=2`，逐项断言：B 首次读 build、重复读 hit、**A 增长不使 B 失效**、
  B 新 extent 使 B 失效、B 原地覆盖仍 hit。
- **P1-7 专项**（`namespace_test`）：publish 平均耗时 1000 inode=58 ns、
  20000 inode=45 ns（与 inode 数无关）；prepare/rollback/重复 prepare Busy 断言通过。
- **P0-2/P0-3 专项**：`index_stack_test` 4 线程 × 200k 次 pop/push，
  800000 次交付、0 次空、无重复交付、256 个索引全部归位；`data_plane_test`
  并发池 churn 后 `liveCount==0`。
- **P0-6 专项**：`io_planner_test` 既有边界断言（interior、跨界、foreign、
  release 后失效）全部通过。
- **回归**：完整 CTest **29/29** 通过（新增 `index_stack_test`、
  `snapshot_cache_device_test`），含真实 `Nvme0n1` 设备测试与 `hfsctl check`
  语义路径；测试后设备恢复内核 `nvme` 驱动。

### 11.3 未包含在阶段 B 的内容

- P0-2 的 `RequestState` footprint 进一步热/冷拆分、P0-7 RocksDB direct File
  缓存/状态池、P0-8 `DataPath` 有界数组、P1-2 同步 ingress、P1-3 doorbell 无锁、
  P1-5 hot/cold split、P1-6 scheduler：留给阶段 C/D，按 §8 分项实施。
- P1-1 的快照发布仍持 per-File mutex（key 已改为 per-inode，失效范围问题已解决）；
  无锁发布/RCU 留待有 profile 证据后再做。

## 12. Stage C 实施记录（2026-09-29）

> 按 §8 阶段 C 实施同步 ingress 与 runtime 生命周期；P0-7/P0-8/P1-5/P1-6 仍留在阶段 D。

### 12.1 已实施项

| 项 | 内容 | 提交 |
|---|---|---|
| P1-2 | 新增 `BoundedMpscQueue`（Vyukov 有界 MPSC）+ per-data-reactor ingress：`submitDataRead/Write` 常见路径不再 `new DataIoCall`、不再 `spdk_thread_send_msg`；reactor poll 批量 drain 后直接提交；`DataIoHolder` 改为 per-reactor 池（reactor 线程私有 free list，溢出回退堆分配）；ingress 满时保留原消息路径作为罕见回退；stop() 在 join 后把未 drain 的条目以 `-ECANCELED` 交还回调 | `e843c6d` |
| P1-3 | doorbell 表改为无锁：`wakeEntries_`（`atomic<Reactor*>`）+ reader gate（enabled 位 + 读者计数，与 poller reader mask 同协议）；`wakeDataReactor` 不再取 runtime mutex、不再拷贝 `shared_ptr`；`disableWakeTable` 在停 reactor 前等待读者归零 | `e843c6d` |
| 生命周期 | reactor 空闲重检（`waitDataReactorIdle`）与循环退出条件加入 ingress pending；reactor 循环统计 `ingested` 工作；启动时在 data reactor 线程运行前初始化 ingress/holder 池/doorbell 表 | `e843c6d` |

### 12.2 验证证据

- **同步路径 A/B（Stage B → Stage C，同会话交错 3 轮）**：
  `hfs_async=0`、4K `randrw` 50/50、256 MiB、16 jobs × iodepth 8、10 s：

  | 侧 | 中位 IOPS | closure |
  |---|---:|---|
  | Stage B（消息路径） | 37553.8 | yes |
  | Stage C（有界 ingress） | 46126.8 | yes |

  `StageC/StageB = 1.2283`（+22.8%）；4-job 形状为 +0.65%（低并发下收益小）。
- **perf 归因**（同 workload、16 jobs）：Stage B 采样中
  `spdk_ring_enqueue` + `rte_wait_until_equal_32` 自旋占 **55.4% + 33.7%**；
  Stage C 中 `spdk_ring_enqueue`/`rte_wait_until_equal`/`spdk_thread_send_msg`
  **完全消失**（剩余热点为同步路径固有的 futex/condvar 等待与 NVMe 完成）。
- **正确性**：`hfs_async=0` + `--verify=crc32c --verify_fatal=1` 64 MiB randwrite
  校验通过（fio exit 0、`error=0`）；`data_path_device_test`（RMW/超时/sparse）
  与全量 CTest **29/29** 通过。
- **异步回归**：8j4 口径 Stage B → Stage C 中位 IOPS 81707 → 82233（+0.64%），
  p99 643.1 → 634.9 µs，closure 全部完整（doorbell 无锁化的直接收益有限，
  与 ring 转换频率一致）。
- 测试后设备恢复内核 `nvme` 驱动。

### 12.3 未包含在阶段 C 的内容

- **MySQL 增长写回归本轮未跑**：同步路径已由 `hfs_async=0` 的 verify 与
  16 线程 A/B 覆盖；MySQL 端到端矩阵（`doc/performance/mysql_baseline.md` 口径）
  留待阶段 D 与 P0-8 一起复测。
- P0-7 RocksDB direct File 缓存/状态池、P0-8 `DataPath` 有界数组与 parent latch、
  P1-5 hot/cold split、P1-6 scheduler：阶段 D。
- 同步路径剩余的每 I/O 一对 `mutex/condvar` 交接（新 profile 的主要热点）
  属于 P0-8 的 parent latch 方案。

## 13. Stage D 实施记录（2026-09-29）

> 按 §8 阶段 D 执行 P0-7、P0-8 与 P1-6 的触发条件验证；P1-5 与 P2 设计修订仍未开始。

### 13.1 已实施项

| 项 | 内容 | 提交 |
|---|---|---|
| P0-7 | `ReadAsync` 每读不再 `make_unique<AsyncReadState>` + `std::function` 拷贝：新增 per-worker 有界状态池（maxOutstanding+32，溢出回退堆），并把缓存 File 查找、token 分配与 submit 合并到 `DirectWorkerEntry` 一次加锁；删除不再使用的 `directWorkerFile`。池只拥有空闲状态，在用状态由 opaque handle 拥有，release 先等 terminal 再入池 | `1b1aa25` |
| P0-8 | `DataPath::execute` 的 4 个 `std::vector` 改为固定数组，每 child 的 mutex/condvar 改为每请求一个 `ParentLatch`：child 回调递减共享 pending，每个 phase 只做一次 condvar 交接；超时只报告一次但继续等迟到完成。 | `3fbb7e6` |
| P1-6 | 新增单调区间 + 4096 深 active window 的 microbenchmark（`scheduler_test`）证明触发条件：**5.9 µs/node**；生产形状每文件并发窗口为几十，未触发，暂不重写 balanced tree | `d1c13c7` |

### 13.2 验证证据

- **P0-7 RocksDB readrandom A/B（Stage C → Stage D，同会话交错 3 轮）**：
  32 线程、8 万 key × 1 KiB、16 MiB block cache、direct reads：

  | 侧 | 中位 readrandom（ops/s） | timeouts |
  |---|---:|---:|
  | Stage C | 63638.0 | 0 |
  | Stage D | 63912.0 | 0 |

  `StageD/StageC = 1.0043`；该形状受设备延迟支配（~16 µs/read），
  每读节省的分配/加锁不改变吞吐量级。`hfs_rocksdb_smoke`（含 timeout/abort/
  错误注入与 free-space reclaim）通过。
- **P0-8 同步路径 A/B（Stage C → Stage D）**：`hfs_async=0`、16 jobs × QD8、
  4K randrw 10 s，中位 **45503 → 45686 IOPS（+0.4%）**；同 workload
  task-clock 7578 → 7654 ms（噪声内）。该形状同样由设备往返支配，
  收益为去掉每 I/O 的堆分配与 N−1 次 condvar 交接；`data_path_device_test`
  （RMW/超时/sparse/direct-bounce）通过。
- **回归**：完整 CTest **29/29** 通过（含真实 `Nvme0n1` 设备测试）；
  `hfsctl check` 语义路径通过；测试后设备恢复内核 `nvme` 驱动。

### 13.3 观测到的偶发事件

- 在 P0-7 的首次大工作集 A/B 中，新构建某一轮 `fillrandom` 出现 27 次
  `data path child timed out`（>5 s 无完成），其余轮次与随后 3 次复现尝试
  均无超时；dmesg 无新增 NVMe 超时（设备由 SPDK 接管）。按 VM 已知的偶发
  设备停顿归类为环境事件并保留记录；若后续复现，按 bug.md 流程定位。

### 13.4 未包含在阶段 D 的内容

- P1-5 hot/cold split（`RequestState` footprint）与 P1-6 balanced tree：
  前者需要先有 footprint/分配证据；后者已由 §13.1 的 microbenchmark 判定未触发。
- P2-1 zero-copy frontend、P2-2 增量 checkpoint、P2-3 metadata actor 收敛、
  P2-4 arena 分区：均需先修订 `doc/design/hfs2.0.md` 或先有 profile 证据，
  尚未开始。

## 14. 实施状态总表（2026-09-29）

> 状态口径：✅ 完成（有实现 + 证据）；⚠️ 部分完成（列出剩余子项）；❌ 未开始。
> 详细证据见 §10–§13；提交号对应 `main` 分支。

### 14.1 P0（9 组局部优化）

| 项 | 状态 | 实现/证据 | 剩余 |
|---|---|---|---|
| P0-1 getpid | ✅ | `6d586c4`；同 workload getpid 5410→258 | — |
| P0-2 OperationPool | ✅ | `8b9c288`（移除 64-child 死数组）+ `e714af3`（tagged index freelist、预分配存储、无锁） | — |
| P0-3 Sidecar 池 | ✅ | `e714af3`（O(1) free stack + 按用量 reset） | — |
| P0-4 Backend slot | ✅ | `8b9c288`（actor 私有 free list，O(maxInFlight) CAS → O(1)） | — |
| P0-5 extent lookup | ✅ | `8b9c288` + `inode_map_test`；661.6 → 69.4 ns/lookup | — |
| P0-6 DmaArena | ✅ 主体 | `b2a9c54`（地址有序 index、O(log N) 归属、按 waiter 通知） | 子项：`reusableReadLease` 收敛为 per-shard cache 未评估 |
| P0-7 RocksDB 读路径 | ✅ | `29a3429`（getenv 缓存）+ `1b1aa25`（状态池 + 单锁 submit） | — |
| P0-8 FIO/DataPath | ✅ | `29a3429`（FIO free stack）+ `3fbb7e6`（固定数组 + parent latch） | — |
| P0-9 小项 | ✅ ②③ | `8b9c288`（stats-off guard）；本轮 `File::read/write/append` 复用 per-inode 快照（`syncSnapshot`/`cachedSnapshot`）+ direct 子请求 `dmaVerified` 缓存 vtophys；CTest 29/29、sync16 +0.09%、1j32 +0.18%（噪声内，无回退）；见 `hfs2_p09_remainder_20260929.md` | ① sink/handle intrusive ref 仍需 profile 证据 |

### 14.2 P1（7 项结构修复）

| 项 | 状态 | 实现/证据 | 剩余 |
|---|---|---|---|
| P1-1 per-inode 快照 | ✅ 主体 | `113f36f` + `snapshot_cache_device_test`（hits=3/builds=2 契约） | 子项：无锁发布/RCU 未做（per-File mutex 保留，§11.3） |
| P1-2 同步 ingress | ✅ | `e843c6d`；16 线程 +22.8%，`spdk_ring_enqueue` 自旋消失 | — |
| P1-3 doorbell 无锁 | ✅ | `e843c6d`（reader gate + atomic 表） | — |
| P1-4 namespace O(E) | ✅ | `113f36f`（二分区间 + 原位校验，不再整表拷贝） | — |
| P1-5 footprint | ⚠️ 部分 | Stage B：reset 只清实际用量（`e714af3`） | hot/cold split、`kMaxChildren` 4+有界溢出块 未做 |
| P1-6 scheduler | ⚠️ 触发验证 | `d1c13c7`：4096 深窗口 5.9 µs/node，生产窗口几十 → 未触发 | balanced active index 未实施（microbenchmark 作为触发门禁保留） |
| P1-7 dirty epoch | ✅ | `113f36f` + `namespace_test` 缩放验证（58/45 ns per publish） | — |

### 14.3 P2（4 项设计决策）

| 项 | 状态 | 前置 |
|---|---|---|
| P2-1 zero-copy DMA | ✅ MySQL + RocksDB | Shard-110 `321bc9c`：InnoDB buffer pool 从 HFS arena 分配（`read_direct_user_children` 917–1308、copy-out 显著下降、崩溃矩阵通过、pre/post 吞吐噪声内）；Shard-111：RocksDB kFSBuffer 读路径（`read_direct_user_children=170098`、bounce/copy-out=0、post/pre ops=1.0016）+ core dword 对齐修复；见 `hfs2_zero_copy_frontend_review_20260929.md`、`hfs2_mysql_zero_copy_20260929.md`、`hfs2_rocksdb_kfsbuffer_review_20260929.md`、`hfs2_rocksdb_kfsbuffer_20260929.md` |
| P2-2 增量 checkpoint | ⚠️ (a) 已实施 | (a) 冻结视图 + 锁外编码：`Namespace::freezeCheckpointViewLocked` + `encodeCheckpointView`，新增 `ChkptFreeze`/`syncCheckpointFreeze`（RocksDB 50k keys 实测冻结 7µs / 总 13µs / 设备写 115µs）；CTest 29/29 + 崩溃矩阵；见 `hfs2_incremental_checkpoint_review_20260929.md`；(b) dirty 增量待格式版本 +1 |
| P2-3 metadata actor 收敛 | ❌ | 阶段级实现（设计已定，无需先改边界） |
| P2-4 per-shard/per-class arena | ❌ | profile-gated：P0-6 后仍证明 central arena 是瓶颈才启动 |

### 14.4 Trace 点位（§7）

| 项 | 状态 | 说明 |
|---|---|---|
| §7.2 已定义未发射点 | ✅ | `d872c6a`：JournalAppend/Chkpt×3/Fsync×2/Recovery×2/Retry/BusyReject 已在生产路径发射并验证 |
| §7.3 新点位 | ⚠️ 部分 | 已实现 `SnapHit`(53)/`SnapBuild`(54)；未实现 `FeSubmitBegin`/`MetaLockWait`/`OpPoolAcquire`/`DmaOwns` |

### 14.5 门禁状态（2026-09-29）

- CTest 29/29、`hfsctl check` `corruption=none`、closure 全闭合；
- 同形状 FIO gate（§16.2 修订后）：1j32 HFS/raw 90.6%、p99 1.00；8j4 120.8%、p99 0.19；
- RocksDB/MySQL 缩减回归与既有基线一致（见 `hfs2_stage_abcd_regression_20260929.md`）；
- 未闭合：P2-2(b) 增量格式、P2-3、P2-4、P0-9①、P1-1 无锁发布子项、
  P1-5 剩余子项、P1-6 balanced index、P0-6 `reusableReadLease` 子项、
  §7.3 的 4 个点位（`FeSubmitBegin`/`MetaLockWait`/`OpPoolAcquire`/`DmaOwns`）；
- 待补量化：设计 §17.1 的 1M inode metadata microbenchmark 与 mount/replay 预算；
  MySQL 8j4 HFS/native 比值（历史 65.2%）与 RocksDB 大工作集 native 差距
  需在当前树（Stage A–D + P2-1/P0-9/P2-2a）上重测。

## 附录 A：优化点 → 代码位置索引

| 编号 | 主要位置 |
|---|---|
| P0-1 | `src/core/engine.cpp:938`、`src/core/client.cpp:321/346/707` |
| P0-2 | `src/core/operation.cpp:28-63/99-136`、`src/core/operation.hpp:54-68` |
| P0-3 | `src/core/async_data_service.cpp:191-225/249-263` |
| P0-4 | `src/core/spdk_async_backend.cpp:109-122`、`src/core/engine.cpp:479-481` |
| P0-5 | `src/metadata/inode_map.cpp:89-120`、`src/core/async_data_service.cpp:424` |
| P0-6 | `src/core/dma_arena.cpp:52-148/150-199/201-226` |
| P0-7 | `rocksdb/hfs_filesystem.cpp:554-568/1271-1318` |
| P0-8 | `fio/fio_hfs.cpp:692-699`、`src/core/data_path.cpp:43-70/211-215` |
| P0-9 | `src/core/client.cpp:669/701/986`、`src/core/async_data_service.cpp:749-790` |
| P1-1 | `src/core/client.cpp:306-319/460-505`、`src/metadata/namespace.cpp:146-156/946-954` |
| P1-2 | `src/runtime/spdk_runtime.cpp:2066-2138`、`src/core/data_path.cpp:316-405` |
| P1-3 | `src/runtime/spdk_runtime.cpp:469-497` |
| P1-4 | `src/metadata/namespace.cpp:726-954` |
| P1-5 | 同 P0-2/P0-3 |
| P1-6 | `src/core/scheduler.cpp:32-68/95-262`、`src/core/scheduler.hpp:90` |
| P1-7 | `src/metadata/namespace.cpp:1175-1229`、`src/core/engine.cpp:786-865` |
| P2-1 | `src/core/async_data_service.cpp:872-886/1178-1200`、`src/core/dma_arena.cpp` |
| P2-2 | `src/metadata/namespace.cpp:1232-1286`、`src/core/engine.cpp:795-813` |
| P2-3 | `doc/design/hfs2.0.md` §4.1/§6.4/§7.2、`src/metadata/namespace.hpp:202` |
| P2-4 | `src/core/dma_arena.hpp:95-108` |

## 附录 B：兼容性声明

- P0/P1 均按实现层改动规划：不改 public header 契约、不升 `HFS_CORE_ABI_MAJOR`、不改 `.conf`
  schema、不改 on-disk format；如果 metadata actor 收敛改变 public Task 完成/取消/关闭语义，
  则该具体变化必须先修订设计。
- P2-1 的 frontend buffer 契约：MySQL 已按 §20.8 实施（Shard-110，默认开启，arena
  预算计入 `dma_arena_mb`）；RocksDB 的读路径切换（kFSBuffer + 非 direct 读）仍需
  `hfs2.0.md` §11.2 修订与单独评审（Shard-111）。P2-2 的增量格式需要 §10.2 修订与
  单独评审；zero-copy 只作为可选模式，默认行为不得改变 RocksDB/FIO 既有验收口径。
  P2-3 是既有设计的实现收敛；P2-4 是 profile-gated 内部布局优化，不自动要求 public
  ABI 或磁盘格式变化。
