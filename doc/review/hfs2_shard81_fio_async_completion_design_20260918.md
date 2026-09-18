# Shard-81 FIO core-native 异步完成详细设计

日期：2026-09-18
状态：核心实现与结构矩阵已完成；真实 NVMe 功能闭合，正式性能 gate 未通过，保留为后续优化阻塞
事实源：

- `doc/design/hfs2.0.md` §5.2、§5.3（coroutine operation 与 completion sink）
- `doc/design/hfs2.0.md` §6.3、§6.4（ShardRing、reactor 与所有权）
- `doc/design/hfs2.0.md` §7（Operation、唯一 inode scheduler、data path）
- `doc/design/hfs2.0.md` §12.2（FIO lifecycle）
- `doc/design/hfs2.0.md` §14（错误、取消和关闭）
- `doc/design/hfs2.0.md` §16.2（性能 gate）
- `doc/performance/hfs2_shard81_formal_gate_20260918.md`（当前失败证据）

本文是 Shard-81 异步实现的落地设计；与 `doc/design/hfs2.0.md` 冲突时，以后者为准，
并应先修订事实源再改实现。

当前实现边界：M4-0 至 M4-5 的 bounded operation、actor-local SPDK backend、FIO
`queue/getevents/event`、close/drain、DMA/bounce、preallocation single-flight 和真实
NVMe 结构矩阵已经落地。FIO 默认 `hfs_async=1`。core async Client 当前公开的是
`submitRead/submitWrite(File, IoVec, offset, token)`，不是把 lazy `File::read/write`
task 直接交给 Client；这是本阶段为固定 contiguous buffer 和明确 snapshot 生命周期采用的
实际 API。core operation 级 cancel/timeout 还没有公开入口，当前 FIO timeout 只表示等待
超时，cleanup 继续 drain；扩展写、append、truncate、durability barrier 仍不属于本 shard。
正式 raw/HFS gate 的所有错误和生命周期条件已通过，但 HFS 中位吞吐只有 raw 的约 61%，
因此不能宣称性能完成。详见 `doc/performance/hfs2_shard81_formal_gate_20260918.md`。

---

## 1. 结论与修订摘要

FIO 必须获得真正的异步 outstanding，但不能为此绕过 HFS 2.0 已定义的数据调度链。
最终方案固定为：

```text
FIO queue
  -> Client credit + Operation pool + ShardRing reservation
  -> file 所属 shard actor
  -> InodeDependencyScheduler
  -> actor-owned DMA/backend state
  -> actor 直接使用本 reactor 的 SPDK channel
  -> per-shard SPSC completion ring
  -> Client::poll
  -> FIO getevents/event
```

相对初稿，本次修订包含以下硬性改变：

1. 不新增一套与 `Operation` 平行的 `DataIo` 请求生命周期；异步 backend state 是
   `Operation` 的 bounded sidecar。
2. 不允许 `File::submit* -> DataPlane::submit -> send_msg` 旁路；所有 file I/O 必须经过
   `ShardRing` 和唯一的 `InodeDependencyScheduler`。
3. 引入 move-only `IoHandle`，FIO pending slot 保存 handle；completion token 使用
   `slot + generation`，不把裸 `FioHfsPending*` 跨线程传递。
4. completion queue 改为每个 data shard 一个 SPSC ring；reactor callback 不获取 mutex、
   不执行堆分配，也不调用 FIO/frontend 代码。
5. “全有或全无”只定义到 queue acceptance 边界。请求一旦发布到 shard ring，后续
   planner、DMA、SPDK submit 或 metadata 失败都通过唯一异步 terminal event 返回，
   不能再伪装成可安全重试的 `FIO_Q_BUSY`。
6. SPDK `-ENOMEM` 不结束请求，必须在同一 reactor/channel 上使用
   `spdk_bdev_queue_io_wait` 重试。
7. 第一阶段只异步化 read 和“已预分配、固定 logical size 的覆盖写”；扩展写必须按
   reserve/zero/commit/data/setSize 状态机单独落地。
8. cleanup 超时不能返回后释放 FIO 内存。v1 即使超过告警期限也必须继续 drain，直到
   所有 callback、completion token 和 `IoHandle` 闭合。

## 2. 目标

1. `queue()` 接受请求后返回 `FIO_Q_QUEUED`，不在 FIO frontend 线程等待设备完成。
2. `getevents()` 通过 `Client::poll()` 消费完成事件，`event()` 返回已完成的 `io_u`。
3. 每个 FIO job 最多保持 `hfs_client_depth` 个 outstanding；正式 gate 的
   `32 jobs × iodepth=1` 应达到 32 个总 outstanding。
4. 保持 HFS 的 file-to-shard 固定映射、inode 依赖顺序、SPDK channel 所有权和
   bounded resource 约束。
5. 错误、backend errno、超时、取消、late completion、close 和 Engine stop 都有
   单一且可测试的闭合语义。
6. 为后续 `32 jobs × iodepth=32`（最多 1024 outstanding）压力矩阵提供明确的资源预算，
   但不把该压力矩阵混同于正式 gate。

## 3. 非目标

- 不引入 external worker pool、普通线程池或每个 FIO job 一个 SPDK thread。
- 不让 FIO/frontend 线程直接持有 `spdk_io_channel` 或调用 bdev submit。
- 不恢复 POSIX/FUSE/preload/registered-DMA ABI。
- 不在第一阶段支持 append、truncate、扩展写或在线 extent 重排的异步化。
- 不仅凭“能产生 outstanding”宣称通过 §16.2；性能、CPU、p99 和错误闭合仍需真实 NVMe
  五次同口径 gate。

## 4. 现状与前置缺陷

### 4.1 当前同步边界

- `fioHfsQueue()` 调用 `File::read/write(...).wait()`，然后返回 `FIO_Q_COMPLETED`；每个
  job 实际 in-flight 恒为 1。
- `DataPath` 虽使用异步 SPDK callback，但当前在调用线程用 `WaitState` 等待 child，
  因此 frontend 仍是同步的。
- `SpdkRuntime::submitDataRead/Write` 为每个 child `new DataIoCall` 并逐请求
  `spdk_thread_send_msg`；这不符合 §6.3 的“ring 只发 doorbell，不为每请求发消息”。
- 当前 `Operation`、`ShardRing`、`ShardActor` 和 `InodeDependencyScheduler` 已存在，
  但 `File::read/write` 尚未接入它们。

### 4.2 必须先修的两个一致性问题

1. **共享文件预分配 single-flight**：32 个 job 同时 open 同一路径时，只允许一个 owner
   执行 `allocate(0, size)`；其余 waiter 复用结果。正式异步 workload 启动前必须满足
   `owner=1、waiter=31、failure=0`。
2. **DMA 子区间识别**：FIO 的 `orig_buffer` 是一整块 arena allocation，`io_u->xfer_buf`
   通常是其中的 interior pointer。当前 `DmaArena::owns()` 只匹配 allocation base，导致
   大多数 FIO slice 被误判为非 DMA buffer。必须新增
   `ownsRange(const void *ptr, size_t bytes)`，验证 `[ptr, ptr+bytes)` 完整落在某个 live
   allocation 内，并在 direct path 同时做 `spdk_vtophys` 连续长度检查。

以上两项不是性能微调，而是进入异步 gate 的前置正确性条件。

## 5. 不变量

- **I1 唯一入口**：每个 file operation 必须按 `hash(fileId) % R` 进入一个 ShardRing；
  禁止 frontend 直接调用 async DataPlane/SPDK。
- **I2 唯一顺序器**：同一 inode 的 read/write/RMW/size mutation 只由所属 shard 的
  `InodeDependencyScheduler` 排序。
- **I3 callback 最小化**：SPDK callback 只释放 bdev I/O、记录 child status、把 intrusive
  ready node 放入 actor-local 队列并触发 actor；不得推进 scheduler、递归提交下一阶段、
  锁 frontend mutex、分配内存、休眠或调用 FIO。
- **I4 bounded**：Operation、async sidecar、child、DMA lease、backend wait entry、ring
  和 completion token 都有 mount/client 配置确定的上限。
- **I5 单一 terminal**：一个 accepted operation 恰好产生一个 terminal completion；
  child first-error 被保存，但必须等待所有已提交 child drain。
- **I6 acceptance 边界**：只有在 operation pool、client credit 和 ShardRing reservation
  失败且尚未 publish 时，`queue()` 才返回 `FIO_Q_BUSY`。publish 后的失败只能异步完成。
- **I7 引用闭合**：caller、actor、scheduler/parent/backend 和 frontend handle 各自持有
  明确引用；最后一个引用释放后 operation/sidecar 才回池。
- **I8 无裸 frontend 指针**：跨线程 completion 只携带 value token；不携带 `io_u*`、
  `thread_data*`、`FioHfsPending*` 或可发生 ABA 的裸地址。
- **I9 close 状态机**：Client 为 `Open -> Closing -> Closed`；Closing 后拒绝新 submit，
  Closed 前 outstanding、completion ring、handles 和 open files 必须闭合。
- **I10 可见性与持久性分离**：write terminal 表示逻辑写已完成并可见，不表示 durable；
  durable 仍由 `datasync/sync` 的 epoch/flush 契约决定。

## 6. 总体架构

```text
FIO job/frontend                         file-owned data shard/reactor
+-------------------------------+       +----------------------------------+
| queue(io_u)                   |       | ShardActor drains ShardRing      |
|  reserve pending slot         |       |  -> acquire DMA/backend state    |
|  Client::submitRead/Write(...)|------>|  -> scheduler.admit              |
|  save IoHandle               |       |  -> actor-local bdev submit      |
|  return FIO_Q_QUEUED         |       |  -> child callbacks / io_wait    |
|                               |       |  -> scheduler.complete           |
| getevents(min,max,timeout)    |       |  -> SPSC completion.push(token) |
|  Client::poll  <--------------|-------|  -> nonblocking notifier         |
|  validate slot+generation    |       +----------------------------------+
|                               |
| event(i)                     |
|  return io_u; free slot      |
+-------------------------------+
                    metadata actor
        reserve/commit/setSize/epoch only when operation requires it
```

同一 file 的 operation 始终进入同一 shard，因此 completion ring 的 producer 对该 shard
是单线程 actor，Client/frontend 是唯一 consumer。多个 shard 不共享 producer index。

## 7. 公共 API 与标识

### 7.1 CompletionToken 与 CompletionEvent

```cpp
/* Value token validated by the frontend slot table; never a raw pointer. */
struct CompletionToken {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
};

struct CompletionEvent {
    std::uint64_t requestId = 0;
    CompletionToken token{};
    Status status = Status::Ok;
    int backendErrno = 0;
    std::uint64_t completedBytes = 0;
};
```

`Status` 和 `backendErrno` 必须分字段保存；不得用一个 `int` 同时编码 HFS status 与
负 errno。FIO 映射优先使用 `backendErrno`，否则按 `Status` 映射。

### 7.2 IoHandle

```cpp
/* Move-only frontend reference to one accepted Operation. */
class IoHandle {
public:
    IoHandle() noexcept = default;
    IoHandle(IoHandle &&) noexcept;
    IoHandle &operator=(IoHandle &&) noexcept;
    IoHandle(const IoHandle &) = delete;
    IoHandle &operator=(const IoHandle &) = delete;
    ~IoHandle();

    bool valid() const noexcept;
    std::uint64_t requestId() const noexcept;
    CompletionToken token() const noexcept;
    void reset() noexcept;

private:
    Operation *operation_ = nullptr; /* intrusive frontend reference */
    std::uint64_t requestId_ = 0;
    CompletionToken token_{};
    /* Client sink ownership is released with the frontend handle. */
    std::shared_ptr<void> owner_;
};
```

- `Client::submitRead/submitWrite` 成功时返回 `IoHandle`；失败时不得发布 operation。
- `IoHandle` 析构只释放 frontend 引用，不隐式取消已接受请求。
- FIO 把 handle 保存在 pending slot，收到并校验 completion 后再 `reset()`。
- requestId 单调递增；slot 每次复用时 generation 加一。completion 必须同时匹配
  requestId、slot 和 generation，任一不符都计为 corruption/late-token bug，不能交给 FIO。

### 7.3 Client API

API 与 `hfs2.0.md` §5.3 的 event-queue 模型保持一致：

```cpp
struct ClientOptions {
    std::uint32_t maxOpenFiles = 0;
    std::uint32_t maxOutstanding = 32;
};

struct PollResult {
    Status status = Status::Ok;
    std::size_t count = 0;
};

class Client {
public:
    Result<IoHandle> submitRead(const std::shared_ptr<File> &file, IoVec buffer,
                                std::uint64_t offset, CompletionToken token);
    Result<IoHandle> submitWrite(const std::shared_ptr<File> &file, IoVec buffer,
                                 std::uint64_t offset, CompletionToken token);

    PollResult poll(std::span<CompletionEvent> events, std::size_t min,
                    std::size_t max, std::chrono::microseconds timeout);
    std::size_t outstanding() const noexcept;
    std::size_t maxOutstanding() const noexcept;
};
```

异步固定覆盖读写通过 `submitRead/submitWrite` 进入同一 scheduler/data path；这不是绕过
ShardRing 的 File 旁路。调用前校验 logical size，并把 FileHandle、contiguous IoVec、
token、sink 和按请求裁剪的 extent snapshot materialize 到 bounded operation state；不得把
调用栈上的临时 descriptor 留到 frontend 返回以后。同步 `File::read/write` 仍保留给同步
frontend。

当前 `fio_hfs.cpp` 把 `hfs_client_depth` 映射到每个 Client 的 `maxOutstanding`；不能依赖
`ClientOptions` 聚合字段顺序。

### 7.4 iovec 边界

第一阶段 async backend 只接受一个 contiguous `IoVec`，与当前 FIO 调用一致。公共
`File::read/write(span<IoVec>)` 在 operation 创建时验证并把 descriptor 复制到 bounded
sidecar；不得保存指向调用栈上 `IoVec` 数组的 span。多 iovec 支持必须在 sidecar 中有
固定上限并增加跨 child copy 测试，不能让 backend 保存短命 descriptor。

## 8. Operation 与 actor 状态

### 8.1 复用现有 Operation

不得再建立独立 intrusive `DataIo` 池。扩展现有 `Operation` 或为其挂接 bounded
`AsyncIoState`：

```cpp
/* Pool-backed backend state owned by one Operation. */
struct AsyncIoState {
    SchedulerNode scheduler;
    IoPlanner::Plan plan;
    std::array<ChildState, IoPlanner::Plan::kMaxChildren> children;
    ParentCompletion parent;
    CompletionToken token;
    Status status = Status::Ok;
    int backendErrno = 0;
    std::uint64_t completedBytes = 0;
    std::uint32_t submittedChildren = 0;
    std::uint32_t drainedChildren = 0;
    bool completionPublished = false;
};
```

`Operation` 继续保存 requestId、fileId、range、kind、shard、user buffer 引用和 completion
continuation。高层 `shared_ptr<Client>`/`shared_ptr<File>` 不进入 reactor callback；FIO
pending 在 frontend 保存 `shared_ptr<File>`，operation 内只保存稳定的 `FileHandle`、
engine generation 和必要 snapshot/sidecar。operation 另持有 pool-backed intrusive
`CompletionSinkRef`；它只保证 Client completion sink/state 活到 event publish，不触发高层
对象析构。Client facade 始终保留最后一个 frontend ref 到 Closed，因此 sink/state 的最终
析构不会发生在 reactor callback。

### 8.2 状态转换

```text
Created
  -> Enqueued
  -> Preparing            (plan + DMA lease; no scheduler node held while waiting)
  -> WaitingDependency
  -> Ready
  -> Submitted
  -> Draining             (first error/cancel/timeout, wait submitted children)
  -> Completed | Failed | Cancelled
  -> CompletionPublished
  -> Reaped
```

资源顺序遵守事实源：

```text
Operation/frame -> ShardRing slot -> DMA lease -> scheduler node
                -> backend credit/io_wait -> SPDK submit
```

`Client::submitRead/submitWrite` 的 acceptance 顺序固定为：校验 Client/thread -> 获取 client credit ->
创建 Operation/sidecar -> 建立 frontend handle 引用 -> reserve ring -> 填好 sink/token/state ->
publish。publish 前失败按相反顺序回滚；frontend 引用必须在 publish 前建立，避免 actor
快速完成后提前回池。

actor 从 ring 取走 operation 后才做 plan/DMA preparation。所有 child 所需 bounce/RMW
buffer 使用 `DmaArena::tryAcquireBatch` 原子地全取或全不取，禁止持有部分 lease 等剩余
资源。暂时无空间时 operation 停在 bounded `Preparing` FIFO，由 arena release doorbell
唤醒 actor 重试；此时不持 scheduler node。永久 plan/参数错误或 preparation timeout 通过
异步 terminal completion 返回，不能回到 FIO 的 `FIO_Q_BUSY` 分支。mount preflight 必须
保证单个最大 operation 的 batch 不超过 arena cap。

### 8.3 scheduler 范围

- read：`[offset,end)`，read/read 可并行。
- fixed-size overwrite：`[offset,end)` write；与重叠 read/write/RMW 冲突。
- RMW：scheduler 覆盖完整受影响 metadata block，而不是只覆盖用户子区间。
- 扩展写、gap zero、append、truncate、setSize：使用 inode barrier，直到 metadata 可见性
  提交完成才 `scheduler.complete`。
- sync/datasync：使用 durability barrier；不得与普通 write 的 data phase 乱序。

## 9. actor-owned async backend

### 9.1 不再逐请求 send_msg

`ShardActor` 与 file 所属 data reactor 同线程运行，actor 直接使用该 reactor 创建的
`spdk_io_channel`。新的窄接口只允许 actor 调用：

```cpp
class DataBackend {
public:
    /* Called only by the channel-owning actor thread. */
    virtual SubmitResult submitRead(ChildState &child) noexcept = 0;
    virtual SubmitResult submitWrite(ChildState &child) noexcept = 0;
    virtual bool isDmaCapable(const void *buffer, std::size_t bytes) const noexcept = 0;
};
```

当前 `SpdkRuntime::submitDataRead/Write` 可暂时保留给同步兼容路径和测试，但 async hot path
不能使用其 `new DataIoCall + spdk_thread_send_msg` 模型。ring 只在 empty-to-nonempty 时发
doorbell。

### 9.2 child 生命周期与 actor continuation

每个 `ChildState` 至少包含：

- owner operation/sidecar 引用；
- direct caller lease 或 arena bounce lease；
- RMW phase（ReadOld/WriteMerged/Done）；
- device、offset、blocks、buffer offset/length；
- `spdk_bdev_io_wait_entry`；
- submitted、waitingForCredit、completed 标志；
- backend errno 和完成字节。

SPDK callback 只执行：

1. 读取 completion status 并 `spdk_bdev_free_io`；
2. 把 status 写入 `ChildState`；
3. 将 child 的预分配 intrusive ready node 放入 reactor-local actor ready queue；
4. arm actor continuation，然后返回。

actor 主循环随后执行：

1. drain ready queue；
2. 推进 bounded copy-out 或 RMW phase；RMW read 成功后由 actor 提交 write phase；
3. child 真正 terminal 后释放 backend credit/lease，并调用 `parent.childDone`；
4. 最后一个 child 由 actor 调用 `scheduler.complete`；
5. actor 构造 `CompletionEvent`、push Client 的 shard SPSC ring 并通知 poller；
6. actor 释放 backend/scheduler 引用。

这样 scheduler 的 admit callback 和下一笔 bdev submit 不会递归发生在 SPDK callback 栈中。
actor ready queue 容量由本 shard 最大 active child 数确定，节点内嵌在 `ChildState`，不得
动态分配或静默丢失。

callback 不得 `sleep_for`。`HFS_TEST_DATA_IO_DELAY_MS` 必须改成 fake backend 的 reactor
timer/deferred completion，不得阻塞真实 reactor。

### 9.3 SPDK -ENOMEM

`spdk_bdev_readv_blocks/writev_blocks` 返回 `-ENOMEM` 表示 io_channel 暂时没有资源，处理为：

1. 在 `ChildState` 中填好 `spdk_bdev_io_wait_entry`；
2. 调用 `spdk_bdev_queue_io_wait(bdev, channel, &entry)`；
3. wait callback 在同一 reactor/channel 把 child 放入 actor ready queue，由 actor 重试；
4. operation 保持 scheduler admission、buffer lease 和引用；
5. queue wait 注册失败才记录 terminal backend error，并继续 drain 其他已提交 child。

不得把 `-ENOMEM` 映射成 FIO submit `BUSY`，因为 operation 此时已经 accepted，且可能已有
其它 child 到达设备。

### 9.4 acceptance 与部分提交

“全有或全无”仅指 `queue()` 返回 `FIO_Q_BUSY` 前没有 publish、没有设备 I/O、没有 metadata
副作用。publish 后 child 是逐个提交的，后一个 child 失败时前一个 child 无法回滚。因此：

- accepted operation 只对 frontend 暴露一次整体 terminal result；
- first error 被保留，所有已提交 child 必须 drain；
- 未提交 child 必须补偿 parent pending；
- write 失败允许底层出现部分物理写，但 metadata/scheduler 状态不得把未完成扩展错误地
  发布为成功；
- verify/failure-injection 测试负责证明重试后数据语义，而不是宣称物理 I/O 可回滚。

## 10. 完成队列与通知

### 10.1 per-shard SPSC

每个 Client 为每个 data shard 建一个 SPSC completion ring：

```text
shard 0 actor -> client.ring[0] --+
shard 1 actor -> client.ring[1] --+--> single frontend Client::poll
...                               |
shard R actor -> client.ring[R] --+
```

- 每个 ring 的 producer 是唯一 shard actor，consumer 是创建该 Client 的 frontend。
- 每个 ring 容量至少为 `maxOutstanding`，保证最坏情况下全部请求集中到同一 shard 时也
  不溢出；总内存为 `R × maxOutstanding × sizeof(CompletionEvent)`，纳入 preflight。
- client credit 在 submit 前获取，在 completion 被 poll/reap 后释放，因此任一 ring 的
  未消费 token 不会超过 maxOutstanding。
- `push` 不得失败；release/acquire 顺序发布 event。溢出视为 core corruption，增加 fatal
  计数并阻止 Engine 正常 stop，不能静默 dropped。

### 10.2 无锁通知

生产者 callback 不使用 `mutex + condition_variable`。Linux/SPDK 正式实现使用 nonblocking
`eventfd`（只在 empty-to-nonempty 时通知）；`Client::poll` 先扫描所有 ring，不足 min 时用
`poll/ppoll` 等待 eventfd，再重新扫描以关闭 lost-wake 窗口。eventfd 计数饱和只意味着已有
未消费通知，不能丢 completion。

Windows host 单测可使用原子 epoch + consumer 侧 bounded wait/poll fallback；producer 路径
仍不得拿锁或分配。通知只是唤醒优化，ring 中的 event 才是事实源。

### 10.3 poll 语义

- `max` 被限制在 output span 容量内；先 drain 已有事件，再决定是否等待。
- `min == 0` 或 timeout 为 0 时非阻塞。
- 超时返回当前已取得数量和 `Status::Ok`，不改变仍在途 operation。
- 每次从 ring 取出 event 时释放一个 client outstanding credit；FIO slot 直到 `event()`
  返回该 `io_u` 后才可复用。
- Client 只允许创建它的 frontend 调用 `poll`；跨线程 poll 返回 `InvalidState`。

## 11. write/metadata 状态机

### 11.1 第一阶段：已预分配覆盖写

Shard-81 gate 使用 `FIO_NOEXTEND + hfs_preallocate=1`。open single-flight 完成后，workload
范围必须满足 `end <= logicalSize` 且 extent snapshot 已覆盖全部范围。此路径：

```text
snapshot validate -> scheduler range admission -> data/RMW -> terminal
```

它不执行 `reserveExtents`、`commitExtents` 或 `setSize`，是第一阶段异步 write 的唯一范围。
若 FIO 请求越过预分配 size，返回异步 `InvalidArgument`/`ENOSPC`，不能悄悄进入同步扩展写。

### 11.2 后续阶段：扩展写

扩展写必须作为同一 Operation 的 inode barrier 状态机实现：

```text
AdmittedBarrier
  -> RecheckEOF
  -> ReserveMissingExtents
  -> ZeroReservedExtentsAndGap
  -> CommitExtentsAndPublishSnapshot
  -> WriteUserData
  -> SetLogicalSizeAndPublishEpoch
  -> Terminal
```

失败规则：

- reserve 后、commit 前失败：release 全部 reservation；logical size 不变。
- zero 失败：等待已提交 zero child drain，再 release reservation。
- commit 后、user data 完成前失败：已 commit extent 保持 zero-initialized 且 logical size
  不增长；它们可由后续写复用，不作为用户可见数据。
- user data 全部成功后才执行 `setSize`；setSize 失败则新 EOF 不可见并返回错误。
- scheduler inode barrier 覆盖整个状态机，直到 snapshot/logical epoch 发布或 rollback
  完成；不得让并发 read 看见半提交 extent/size。
- write terminal 只代表 logical epoch 已发布；`datasync/sync` 才推进 durable epoch。

append、truncate 和 allocate 使用同一 barrier/metadata actor 协议，不在 Shard-81 第一阶段
顺带实现。

## 12. FIO adapter

### 12.1 options

| option | 语义 | 默认 |
|---|---|---|
| `hfs_config` | HFS `.conf` | 必填 |
| `hfs_direct` | direct/bounce 选择 | 1 |
| `hfs_preallocate` | shared-file single-flight 预分配 | 1 |
| `hfs_client_depth` | 每 job Client 的 maxOutstanding | 32 |
| `hfs_async` | 1=异步；0=同步 bring-up/回归 | 1 |

每个 FIO job 当前创建一个 Client，因此资源总上界为所有 job 的
`sum(maxOutstanding)`。正式 gate 是 `32 × 1 = 32`；压力矩阵 `32 × 32 = 1024`，两者必须
分别记录，不能用 client depth=32 推断正式 gate 实际有 1024 outstanding。

### 12.2 pending slot

```cpp
enum class PendingState : std::uint8_t {
    Free, Submitted, Reaped
};

struct FioHfsPending {
    PendingState state = PendingState::Free;
    std::uint32_t generation = 0;
    std::uint64_t requestId = 0;
    io_u *io = nullptr;
    std::shared_ptr<hfs::File> file;
    hfs::IoHandle handle;
    hfs::CompletionEvent completion;
};
```

实现中的 slot 转换为：

```text
Free -> Submitted -> Reaped -> Free
```

`Reaped` 覆盖 completion 已由 `getevents()` 取出但尚未由 `event()` 交还 `io_u` 的阶段，
因此仍禁止复用 slot；generation 在每次从 Free 重新提交时递增并跳过 0。文档中的
Reserved/Completed/Reported 是协议边界，不额外复制为三个 adapter 状态。

### 12.3 queue/getevents/event

```text
queue(io_u):
  validate state/file/direction/range
  reserve Free slot; increment generation; save io_u + shared_ptr<File>
  result = client->submitRead/submitWrite(file, single IoVec, offset, {slot, generation})
  Busy/TryAgain before publish -> clear slot; return FIO_Q_BUSY
  permanent error before publish -> fill io->error/resid; clear slot;
                                    return FIO_Q_COMPLETED
  Ok -> save IoHandle/requestId; state=Submitted; return FIO_Q_QUEUED

getevents(min,max,timeout):
  result = client->poll(events, min, max, timeout)
  for each event:
    validate slot, generation, requestId and state==Submitted
    copy event into pending; map status/backendErrno to io->error/resid
    pending.handle.reset(); pending.state=Reaped
    append slot index to ready[]
  return ready.size()

event(i):
  locate ready slot; require state==Reaped
  result = pending.io
  clear io/file/completion; state=Free
  return result
```

`getevents()` 不直接保存 completion 中的裸指针；`ready` 保存 slot index。若 token 校验失败，
adapter 记录 fatal contract error，不能把可能属于旧 generation 的 `io_u` 交给 FIO。

### 12.4 buffer ownership

- `iomem_alloc` 仍一次提供 FIO 请求的整块 arena allocation。
- async operation 只借用 `io_u->xfer_buf`；FIO 保证从 `FIO_Q_QUEUED` 到 `event()` 之间
  buffer 有效。
- direct 判定必须使用 `ownsRange(xfer_buf, xfer_buflen)`，不是 base-only `owns()`。
- `hfs_direct=0` 的 bounce buffer 必须来自 bounded arena lease，不能在 `queue()` 中使用
  `std::vector` 临时分配。

## 13. 超时、取消与关闭

### 13.1 超时

- FIO `getevents` timeout 只是等待超时，不取消 I/O。
- 本阶段 core async Client 尚未公开 operation timer；因此 core 不宣称 operation-level
  timeout。FIO `getevents` timeout 只结束本次 frontend 等待，accepted operation 继续由
  reactor 完成，cleanup 必须 drain 到闭合。未来增加 timer 时，必须沿用 Draining 语义：
  buffer/operation/scheduler 引用保持到 callback 全部返回，terminal event 才能报告
  `TimedOut`。
- 不允许 callback 在超时后访问栈上 `WaitState`、已释放 `io_u` 或已析构 Client。

### 13.2 取消

- 本阶段 FIO/core async Client 尚未公开 operation cancel API；不能把 FIO cleanup 或
  `IoHandle::reset()` 解释为取消。`IoHandle::reset()` 只释放 frontend 引用，accepted
  operation 仍必须完成。未来增加 cancel 时，才按 admission 前移除、已提交 child drain
  的状态机实现。
- first backend error 与 cancel 同时发生时，按 `doc/design/hfs2.0.md` §14 的优先级规则；
  若事实源尚未定义优先级，必须先补充事实源，不能在 adapter 自行决定。

### 13.3 Client close

```text
Open --beginClose--> Closing --all resources closed--> Closed
```

- Closing 后 submit 返回 `InvalidState`。
- `Client::close()` 在 outstanding/queued completion/IoHandle/open File 任一非零时返回 Busy，
  但保持 Closing；调用方 drain 后重试。
- ClientState 和 completion rings 由 Client 自身拥有，Closed 前不得销毁。
- operation 的 `CompletionSinkRef` 在 event publish 后由 actor 释放；Client facade 的最后
  一个 frontend ref 只在 Closed 后释放。显式 close 是 public contract，析构不得在还有
  outstanding 时直接释放 sink/ring。

### 13.4 FIO cleanup

`fioHfsCleanup` 是 void，不能“超时报错后返回”并让 FIO 释放 `td/io_u/orig_buffer`。v1 流程：

1. Client 进入 Closing，禁止新 submit；
2. 循环 `poll` 并完成所有 Submitted slot；
3. 对超过告警期限的 drain 输出一次聚合诊断，但继续 drain，不释放 FIO/core 内存；
4. 所有 ready event 走完 adapter 内部回收，确认 pending 全 Free、handles=0；
5. close File，再 `Client::close()` 到成功；
6. processUsers--；最后一个用户执行 `Engine::drain/stop`。

若未来要实现可返回的强制超时，必须先设计完全 process-owned 的 quarantine，且其中不得
引用任何 FIO memory；不属于 v1。

## 14. 预分配 single-flight

共享 FIO 文件的 preallocation 按 `(engine generation, FileHandle, offset, length)` 建立
process-level bounded 状态：

```text
Absent -> Allocating(owner) -> Ready(result)
                         \-> Failed(result)
```

- 第一位 job 成为 owner 并执行 allocate；其余 job 等待同一结果，不重复 reserve/commit。
- waiter 不持有 FIO/core hot-path lock；open 阶段可以使用 mutex/CV。
- owner 失败唤醒全部 waiter，所有 job 返回同一明确 errno。
- Ready 必须校验 logical size 和 extent coverage，不能只缓存“调用成功”。
- 最后一个 process user cleanup 时释放表；Engine generation 变化后旧项不可复用。

## 15. 遥测与闭合条件

禁止 per-I/O 日志；按 Client/shard 聚合：

```text
fio:
  queue_queued / queue_busy / queue_completed_error
  slots_free / slots_submitted / slots_completed / token_mismatch

client:
  outstanding_current / outstanding_peak
  poll_calls / poll_timeouts / events_reaped

shard:
  ring_busy / scheduler_wait / scheduler_active_peak
  operation_created / terminal / completion_published / operation_freed

backend:
  child_submit / child_complete / io_wait_queued / io_wait_retry
  direct_children / bounce_children / copy_bytes / backend_errors

closure:
  operation_created == operation_freed
  accepted == completion_published == events_reaped
  child_submit == child_complete
  client_credit_acquire == client_credit_release
  completion_overflow == 0
  token_mismatch == 0
```

`accepted` 指 ring publish 成功，不含 `FIO_Q_BUSY`。cleanup 和 Engine stop 时输出一次闭合
摘要；任一不等式不满足都不能把测试标记为通过。

## 16. 测试与验收

### 16.1 host/fake backend

1. `IoHandle` move/reset、caller/actor/parent 引用归零、pool slot 复用。
2. pending slot generation ABA：旧 token 到达时必须被拒绝且不得访问旧 `io_u`。
3. per-shard SPSC：多 shard 并发 producer、单 consumer、lost wake、ring 最坏集中度。
4. scheduler：重叠 read/write/RMW、非重叠并行、barrier、cancel before admission。
5. child：多 child 首错、部分 submit、`-ENOMEM -> io_wait -> retry`、wait 注册失败。
6. timeout/cancel 后 late completion，确保 callback 不访问 frontend/FIO memory。
7. cleanup 超过告警期限仍 drain，最后所有计数闭合。
8. `ownsRange`：base、interior、尾边界、越界、释放后地址。
9. `tryAcquireBatch`：全取/全不取、Preparing FIFO 唤醒、timeout/cancel、无部分 lease 泄漏。
10. 扩展写状态机的每个故障点：reservation release、zero drain、commit 后 EOF 不增长、
   setSize 失败不可见。

fake delay/failure 必须由 fake backend 的 deferred callback 驱动，不能在 reactor callback
`sleep`。

### 16.2 真实 NVMe 梯度

| 场景 | 参数 | 验收 |
|---|---|---|
| preallocation | 32 jobs，同一文件 | owner=1、waiter=31、failure=0 |
| 单 job depth | 1 job × iodepth 1/8/32 | peak 分别达到 1/8/32，计数闭合 |
| 多 job gate 形态 | 1/2/4/8/16/32 jobs × depth 1 | peak 达到有效 job 数 |
| 压力 | 32 jobs × depth 32 | 最多 1024，总资源有界，无 token overflow |
| verify | 4K/16K read/write + crc32c | err=0、resid 正确、数据校验通过 |
| bounce | direct=0 | 无热路径堆分配，copy/lease 计数正确 |
| backend credit | 注入 `-ENOMEM` | io_wait 重试，最终完成且不双回调 |
| lifecycle | 延迟完成 + cleanup/close | 不 UAF、不提前返回、闭合等式成立 |

每项记录实际 job 数、iodepth、accepted/completed、outstanding peak、CPU 拓扑、设备 BDF/
bdev、最终 driver 状态和 closure summary。

### 16.3 正式性能 gate

- 使用 `config/hfs_nvme.conf`、`Nvme0n1`、`0000:0b:00.0` 和同一 CPU placement。
- raw/HFS 五次同口径，比较中位数；HFS/raw ≥85%、CPU ≤500%、errors=0、读写分离
  p99 ≤ raw×110%。
- 32 个 job 必须全部进入 workload；不能只看 fio process 数。
- raw 与 HFS 必须报告相同的有效 outstanding；字段缺失标记 incomplete。
- 只有结构矩阵、数据 verify、生命周期闭合和性能条件全部满足，Shard-81 才能完成。

## 17. 实施顺序

1. **M4-0 前置一致性**：共享文件 preallocation single-flight；`DmaArena::ownsRange` 与
   `tryAcquireBatch`；修复 `ClientOptions` 字段映射。
2. **M4-1 operation 主链**：把 `File::read` 接入 Operation -> ShardRing -> actor ->
   scheduler；建立 bounded sidecar 与 fake backend。
3. **M4-2 completion**：`IoHandle`、CompletionToken、per-shard SPSC rings、eventfd notifier、
   Client credit/poll/close 状态机。
4. **M4-3 backend**：actor-local SPDK submit、`spdk_bdev_queue_io_wait`、async read；通过
   depth、错误、超时、取消和 late completion host/真实 NVMe 测试。
5. **M4-4 fixed overwrite**：已预分配范围的 async write/RMW 和 FIO adapter；同步模式
   保留为 bring-up 回退。
6. **M4-5 FIO 矩阵**：1×1/8/32、1..32 jobs×1、32×32、verify、bounce、cleanup；结构与
   生命周期条件已通过，`hfs_async` 已默认启用；正式性能 gate 仍未通过。
7. **M4-6 metadata write**：按 §11.2 实现扩展写，再处理 allocate/append/truncate；不得
   为赶 gate 混入 M4-4。
8. **M4-7 profile 优化**：extent snapshot/logical size cache；按 generation + logicalEpoch
   失效，避免每 I/O `stat + extentMapFor`。
9. **M5 正式 gate**：五次 raw/HFS，当前已采集但性能条件失败；需要先优化完成通知/跨
   reactor 前端等待路径，再重跑并更新报告。

每个里程碑独立提交并在 VM 回归；M4-0 至 M4-5 任一步未闭合，不进入正式性能结论。

## 18. 风险与回滚

| 风险 | 约束/缓解 |
|---|---|
| async 路径绕过架构 | 只复用 Operation/ShardRing/scheduler；review 禁止 File 直达 DataPlane |
| callback 锁、分配或递归提交 | intrusive actor-ready queue + per-shard SPSC；callback checklist |
| slot ABA/UAF | CompletionToken generation + requestId + IoHandle；不传 pending 裸指针 |
| completion ring 溢出 | per-ring capacity=maxOutstanding + client credit；overflow=fatal |
| SPDK 暂时无 credit | 同 channel `spdk_bdev_queue_io_wait`，不误报 FIO_Q_BUSY |
| 部分 child 已提交 | first-error + drain；只承诺 acceptance 前无副作用 |
| 扩展写半提交 | inode barrier + reserve/zero/commit/data/setSize 状态机 |
| cleanup 超时 UAF | v1 不提前返回，告警后继续 drain |
| DMA slice 被误 bounce | `ownsRange` + vtophys span 验证 |
| 同步回归 | `hfs_async=0` 保留为 bring-up/对照；默认使用异步路径 |

回滚只允许把 FIO 选择切回同步 adapter；不能保留一个绕过 scheduler 的“临时 async”分支。

## 19. 交付物

1. 本文定稿，以及实现前对 `doc/design/hfs2.0.md` 公共 API/错误优先级的必要同步。
2. preallocation single-flight、`ownsRange`、`tryAcquireBatch` 和 ClientOptions 映射修复。
3. Operation async sidecar、ShardRing/actor/scheduler 主链、actor-local DataBackend。
4. IoHandle、CompletionToken、per-shard SPSC queue/notifier、Client poll/close。
5. SPDK `-ENOMEM` io-wait、异步 read、预分配 fixed overwrite/RMW。
6. FIO pending slot 状态机、QUEUED/getevents/event、cleanup drain 和遥测。
7. host/fake backend 错误矩阵、真实 NVMe 梯度、verify/lifecycle 证据。
8. 五次 raw/HFS 正式报告；只有所有结构与性能条件满足才关闭 Shard-81。
