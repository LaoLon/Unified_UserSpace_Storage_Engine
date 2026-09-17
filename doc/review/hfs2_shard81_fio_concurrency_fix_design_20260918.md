# Shard-81 FIO 32 并发退化分析与修复设计

日期：2026-09-18  
范围：HFS 2.0 `fio-hfs.so`、真实 `Nvme0n1`、Shard-81 §16.2 性能 gate

## 1. 目标与结论

目标不是把一个退化的 FIO 命令调到能出数字，而是让 `numjobs=32` 的 shared-file
测试真正进入 workload 阶段，并在不破坏 SPDK reactor 所有权、请求生命周期和
submit/complete 闭合的前提下，重新获得可审计的性能结果。

当前结论：问题由两个层次叠加形成。

1. `fioHfsOpen()` 对每一个 FIO job 都执行同一个 shared file 的
   `allocate(0, 256 MiB)`，32 个 job 会重复进入 HFS Namespace 和 extent 初始化路径。
2. 当前为规避此前 external worker pool 引发的 SPDK 崩溃，`queue()` 在 FIO 提交线程
   同步执行 `File::read/write().wait()`。这避免了跨线程 reactor 生命周期错误，但每个
   job 内没有异步 pipeline，无法满足真正的高 QD/高并发 gate。

第一个问题应先修复并单独验证；第二个问题是正式恢复 FIO `queue/getevents/event`
三态前必须完成的结构性工作。不能通过增加 reactor、扩大 arena 或降低 jobs 来掩盖。

## 2. 现状调用链

正式命令的核心参数为：

```text
fio --ioengine=external:/.../fio-hfs.so --thread=1 \
    --hfs_config=config/hfs_nvme.conf \
    --numjobs=32 --iodepth=1 --bs=4k --size=256m \
    --rw=randrw --rwmixread=50 --direct=1 --time_based=1 --runtime=10 \
    --filename=/shard81-fixed.dat --group_reporting=1
```

这里的含义是一个进程内 32 个 FIO thread，每个 thread 的目标 iodepth 为 1；目标
总有效 outstanding 为 32。`--thread=1` 是 FIO 的线程模式开关，不是把并发度设为
1。`group_reporting=1` 会把多个 job 汇总到 JSON 的聚合记录中，因此
`jobs[0]` 不能用来证明“只创建了一个 job”；真正暴露问题的是总 `io_bytes` 极低。

当前 engine 的实际链路如下：

```text
FIO 32 threads
  ├─ fioHfsInit() × 32
  │    ├─ 共享 processEngine
  │    └─ 创建 32 个 Client state
  ├─ fioHfsOpen() × 32
  │    ├─ Client::open(shared path)
  │    ├─ stat()
  │    └─ File::allocate(0, 256 MiB)  ← 当前重复入口
  └─ fioHfsQueue()
       └─ executeFioIo()
            └─ File::read/write().wait()
                 ├─ stat()
                 ├─ extentMapFor()
                 ├─ DataPath::execute()
                 │    ├─ 选择两个 data reactor 之一
                 │    ├─ spdk_thread_send_msg()
                 │    ├─ condition_variable 等待 child completion
                 │    └─ child 完成后继续
                 └─ write 还会 setSize()
```

当前 `queue()` 返回 `FIO_Q_COMPLETED`，所以 FIO 不应再调用 engine 的
`getevents()`/`event()` 来回收这次请求。此前 external worker pool 将 HFS core 调用
放到普通 `std::thread`，已出现 SPDK `msg_queue_run_batch` 崩溃和 glibc heap
corruption；该路径不能恢复。

## 3. 证据与事实边界

修复同步提交路径后，真实 NVMe 4 jobs、4 KiB、randrw、3 秒 smoke 能完成约 20 MiB，
错误为 0，4 个 client 能正常关闭。随后按 32 jobs、256 MiB、10 秒、五次口径重跑：

| layer | 五次 IOPS | 中位数 |
|---|---|---:|
| raw bdevperf | 5241.33 / 5004.26 / 5334.68 / 5108.72 / 4175.39 | 5108.72 |
| HFS FIO | 0.899910 / 1.098791 / 0.199601 / 0.899820 / 10.496851 | 0.899910 |

HFS/raw 为 `0.00017615`，约为 raw 的 `0.0176%`。五次 HFS 的 `error` 均为 0，
但每次总完成量只有约 20 KiB 至 430 KiB。错误数为 0 只说明已完成的少量请求没有
报告 I/O error，不能说明 workload 已正常运行。

本轮证据目录为：

```text
/mnt/hfs-data/hfs2-shard81-formal-fixed-20260918
```

证据中 raw 日志和 HFS JSON 完整存在，设备最终恢复为内核 `nvme`。raw bdevperf
本轮只有 aggregate randrw latency histogram，没有读写分离 p99；因此 p99 gate 仍
不能伪装成完整通过证据。

## 4. 根因分层

### 4.1 已由源码确认：每个 job 重复预分配 shared file

`fioHfsOpen()` 使用每个 job 的 `td->o.size`：

```cpp
const std::uint64_t requestedSize = static_cast<std::uint64_t>(td->o.size);
if ((options == nullptr || options->preallocate != 0) && requestedSize != 0) {
    auto allocated = file->file->allocate(0, requestedSize).wait();
    ...
}
```

因此正式命令的 32 个 job 都会对 `/shard81-fixed.dat` 执行一次 256 MiB
preallocate。`File::allocate()` 并不是单纯的 FIO 元数据设置，它可能执行：

- Namespace extent reservation；
- 为新 extent 调用 `DataPath::zeroExtents()`；
- extent commit；
- inode size 更新。

这些操作共享同一个 Engine 的 Namespace、allocator 和 data plane。当前实现没有
“同一路径同一目标 size 的 single-flight”协调，多个 job 会同时或串行重复进入该
路径。该问题可以直接解释 4-job/16 MiB smoke 与 32-job/256 MiB gate 的数量级差异，
但仍应通过 open/preallocate 计时确认其占比，不能只凭结果猜测。

### 4.2 已由源码确认：当前同步 queue 没有 job 内 pipeline

`fioHfsQueue()` 直接调用 `executeFioIo()` 并返回 `FIO_Q_COMPLETED`。而
`Task::wait()` 会在调用线程上 resume 协程直到完成；`DataPath::execute()` 对每个
child 提交 SPDK 消息后用条件变量等待完成。

这使每个 FIO job 的行为变为：

```text
queue one request → block until HFS completes → return completed → queue next request
```

32 个 job 可以提供横向并发，但不能提供每个 job 的深度并发；任何 shared Namespace
锁、data reactor 处理延迟、DMA credit 或 metadata barrier 都会直接反映为 FIO
thread 阻塞。

### 4.3 高概率放大器：共享 Namespace 的同步元数据路径

`File::read()` 每次会重新 `stat()` 并重建 extent map。`File::write()` 还会做
reserve/commit（在未预分配时）和 `setSize()`。32 个 Client 虽然独立，但它们共享
同一个 Engine 的 Namespace 和 DataPlane；因此 Client 数量并不等于 32 个独立的
元数据执行上下文。

对 4 KiB aligned direct I/O，数据 child 通常很小，但同步实现仍然是一次 parent
只等待一个 child 的阻塞式路径。该设计适合 smoke，不适合证明 §16.2 的异步吞吐。

### 4.4 已确认不可采用：普通外部 worker pool

此前 worker pool 把 `File::read/write().wait()` 放到普通外部线程，实际触发过
SPDK 消息队列崩溃或堆损坏。问题不是“worker 数量不够”，而是 core 的 reactor
归属、请求 handle 保活和停止顺序没有形成契约。修复不能退回该方案。

## 5. 修复设计

### 5.1 第一阶段：shared-file preallocation single-flight

先修复不涉及异步 ABI 的确定性问题，目标是让 32 个 job 的 open 阶段只做一次必要
的 allocation。

新增进程级布局协调器，生命周期与 `processEngine` 一致：

```text
PreallocationRegistry
  mutex
  entries[path]
    completedSize
    requestedSize
    inFlight
    lastStatus
    condition_variable
```

`fioHfsOpen()` 的流程改为：

1. 保持现有 `Client::open()`，取得当前 File handle；
2. 读取当前 stat；
3. 若 `logicalSize >= requestedSize`，直接跳过 allocation；
4. 若同一路径已有相同或更大 size 的 allocation 在执行，等待其完成后重新 stat；
5. 若没有 owner，将当前 job 设为 owner，只由它调用一次 `allocate()`；
6. owner 完成后发布 `completedSize` 和状态，唤醒 waiters；
7. allocation 失败时所有等待者收到同一错误，entry 允许下一次明确重试；
8. `processUsers` 归零、Engine stop 前清空 registry，避免跨 mount 复用旧状态。

实现要求：

- 不在持有 registry mutex 时调用 `File::allocate().wait()`；
- owner 失败时必须清除 `inFlight` 并通知所有 waiter；
- 更大的后续 requestedSize 不能被较小的已完成 size 错误满足；
- 不以 path 字符串的偶然别名作为跨 mount 的永久身份，至少以 canonical FIO path
  加目标 size 重新校验 stat；
- `hfs_preallocate=0` 仍保留为显式实验选项，但正式 shared-file gate 必须记录其
  实际值，不得静默改变测试语义。

第一阶段的最小验收不是 IOPS，而是 32 job 的计数：

```text
open_calls = 32
preallocate_owner_calls = 1
preallocate_waiters = 31
allocation_failures = 0
all_open_jobs_reach_workload = 32
```

### 5.2 第二阶段：增加可审计的 FIO 生命周期遥测

在不改变默认行为的前提下，增加 bounded counters，至少覆盖：

- `init_started/init_completed`；
- `open_started/open_completed`；
- `preallocate_owner/waiter/skipped/failed`；
- `queue_entered/queue_completed/queue_failed`；
- 当前 active FIO requests 的峰值；
- HFS submit/complete/error；
- 每个阶段 p50/p99 elapsed；
- cleanup 时 pending、completed、orphan 数量。

测试模式下将这些计数写入有限大小的汇总输出；不做 per-request unbounded log，也
不把 trace 开关默认打开。FIO JSON 的 `jobs[0]` 仍只作为吞吐聚合，job 数和生命周期
计数必须由 engine 自己输出或由 FIO debug 证据确认。

### 5.3 第三阶段：恢复 core-native asynchronous queue

第一阶段解决 open 阶段后，再恢复正式 gate 所需的真正异步路径。设计原则是：

- 不创建普通外部 worker 来调用 HFS core；
- SPDK I/O 只在合法 data reactor 上提交和完成；
- FIO 的 `io_u`、buffer、File handle 和 core operation 由一个明确的
  `FioHfsPending`/`IoHandle` 共同保活；
- SPDK callback 只释放 bdev I/O、保存状态并发布 completion token，不直接持 FIO/core
  锁，也不直接调用外部 C++ callback；
- FIO owner thread 在 `getevents()`/`event()` 阶段消费 token 并填写 `io_u` 结果；
- cleanup、cancel、timeout 必须等待或接管所有 late completion，不能让 callback
  写入已释放的 `io_u` 或 buffer。

建议的接口形态：

```text
File::submitRead/submitWrite(...)
    -> IoHandle
       - request id
       - parent/child completion state
       - BufferLease ownership
       - File lifetime reference
       - cancel/abandon state
       - completion status
```

FIO engine 的目标链路：

```text
fioHfsQueue(io_u)
  ├─ 创建 FioHfsPending，持有 File、buffer 和 io_u 引用
  ├─ File::submitRead/Write()
  ├─ 发送 data reactor message
  └─ 返回 FIO_Q_QUEUED

SPDK data reactor callback
  ├─ 完成 child/parent 状态
  ├─ 释放 spdk_bdev_io
  └─ 发布有界 completion token

fioHfsGetevents(min,max,timeout)
  ├─ 等待 completion token
  ├─ 批量转移最多 max 个 token
  └─ 返回数量

fioHfsEvent(index)
  └─ 返回仍由 pending 保活的 io_u
```

`DataPath::execute()` 当前是同步 child loop，不能直接被 worker 包装成异步。需要
将 parent/child state、BufferLease 和 late completion 处理下沉到 core，确保每个
child completion 都能安全推进 parent，而不是由外部线程调用 `.wait()`。

### 5.4 线程与 reactor 归属

正式 gate 使用 CPU 2-3 时，当前 runtime 启动两个 data reactor。修复后应保留
DataPath 的 round-robin，但明确以下边界：

- FIO thread 只负责提交和消费 completion，不直接操作 SPDK bdev channel；
- 每个 data reactor 只使用自己创建的 channel；
- callback 不访问已结束的 FIO thread state；
- Engine stop 前先停止接收新请求，再等待所有 IoHandle 进入 terminal state，最后
  销毁 channel/reactor；
- `spdk_submit == spdk_complete` 必须按每个 data child、parent 和 FIO request
  分层统计。

## 6. 实施顺序

### M1：确认 open 阶段瓶颈

先不改变异步 ABI，只加计时和计数，跑 `numjobs=1/4/8/16/32`，确认每个级别的：

- open 总耗时；
- allocation owner/waiter 数；
- 首个 workload request 时间；
- 3 秒 smoke 的 read/write bytes；
- cleanup 是否闭合。

### M2：实现 single-flight preallocation

只修改 `fio/fio_hfs.cpp` 的进程级布局协调和清理逻辑，补充同路径并发 open 测试，
再跑真实 NVMe 梯度。M2 的验收是 32 个 job 都能快速完成 open，并且 allocation
owner 恰好为 1；不在此阶段宣称性能 gate 通过。

### M3：验证同步路径边界

在 single-flight 后用当前同步 `FIO_Q_COMPLETED` 路径重新测 1/4/8/16/32 jobs。
如果 32 jobs 仍无法形成稳定请求量，应把结论归因到同步 DataPath/Namespace
边界，而不是继续扩大资源。

### M4：实现 core-native async handle

先做单 job `iodepth=1/8/32`，再做 `numjobs=1/2/4/8`，最后才做 32 jobs。每一步都
必须通过 cleanup、cancel、timeout、late completion 和 submit/complete 计数。

### M5：重跑正式 Shard-81 gate

固定 8 vCPU、CPU 2-3、真实 `Nvme0n1`、O_DIRECT、256 MiB、shared-file、4K
randrw 50/50、32 jobs、10 秒、五次中位数。raw/HFS 必须使用同一设备、同一有效
outstanding、同一拓扑，并补齐读写分离 p50/p99、CPU、RSS、errors、submit/complete
和 journal bytes/op；缺少字段时明确标记 incomplete，不以 aggregate 数据替代。

## 7. 验收矩阵

### 7.1 FIO 生命周期矩阵

| jobs | iodepth | workload | 目的 |
|---:|---:|---|---|
| 1/4/8/16/32 | 1 | read/write/randrw | open、同步路径、single-flight |
| 1/2/4/8 | 8/32 | read/write/randrw | async handle 与 queue/getevents |
| 32 | 1 | shared-file randrw | Shard-81 目标并发 |

每项必须记录：实际 job 数、queue 数、complete 数、errors、pending 峰值、cleanup
时 pending 数和设备 driver 状态。

### 7.2 故障与生命周期矩阵

- preallocation owner 成功、waiter 成功；
- owner 失败，所有 waiter 同一错误且无永久阻塞；
- job 在 open/preallocate 等待时退出；
- queue 后 cancel；
- completion 到达前 timeout；
- completion 与 cleanup 并发；
- FIO stop 后 late SPDK completion；
- Engine stop 与最后一个 Client close 的顺序；
- `spdk_submit == spdk_complete`、无 orphan handle、无 double free/double resume。

### 7.3 性能 gate

只有同时满足以下条件才可将 Shard-81 标记完成：

- HFS/raw ≥85%；
- CPU ≤500%（8-vCPU 口径）；
- errors=0；
- submit/complete 闭合；
- HFS 读写 p99 ≤ raw 对应 p99 的 110%；
- 32 jobs 真正进入 workload，且 preallocation 不重复；
- raw/HFS 证据字段完整，设备最终回绑并可释放。

任一结构条件失败，即使 IOPS 数字看起来较高，也保持 Shard-81 未完成。

## 8. 明确不采用的修复

- 恢复普通 `std::thread` worker pool 调用 HFS core；
- 仅增加 data reactor 数量或 DMA arena 大小；
- 把 `numjobs=32` 改成低并发后宣称正式 gate；
- 使用 Malloc/Aio 替代真实 NVMe；
- 用 `group_reporting` 后的单个 JSON 聚合 job 记录推断实际 job 数；
- 用 raw aggregate p99 冒充读 p99 和写 p99；
- 在没有 submit/complete、late completion 和 cleanup 证据时宣称性能通过。

## 9. 预期交付物

1. `fio/fio_hfs.cpp` 的 single-flight preallocation 修复及生命周期计数；
2. core-native `IoHandle`/parent-child async completion 实现；
3. 1/4/8/16/32 jobs 梯度结果和故障矩阵结果；
4. 更新后的 Shard-81 五次 raw/HFS 正式报告；
5. 只有在全部结构与性能条件满足后，才更新设计事实源将 Shard-81 标记完成。
