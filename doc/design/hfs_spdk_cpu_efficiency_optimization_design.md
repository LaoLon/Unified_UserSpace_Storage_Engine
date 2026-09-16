# HFS 与 SPDK CPU 效率优化设计（架构优先版）

> **日期**：2026-09-16
>
> **适用架构**：POSIX-only `LD_PRELOAD` HFS，VM 真实 NVMe `Nvme0n1`
>
> **设计状态**：替换旧的“reactor 数量/等待参数优先”方案
>
> **核心判断**：当前主要问题不是“用了几个 reactor”，而是一次 POSIX I/O 同时穿过多个队列、多个同步层、多个请求对象和多个固定资源池，导致时间复杂度、空间复杂度和状态组合数都偏高。reactor 数量只应是部署参数，不能继续作为优化主线。

## 1. 重新定义问题

### 1.1 旧方案的问题

旧方案把 `owner + N data reactor`、idle backoff、adaptive wait 和不同 reactor 数量的 FIO 结果放在路线中心。这些结果可以解释现象，却没有改变请求的基本结构：一次同步 POSIX I/O 仍然需要构造请求、获取多类锁、获取 DMA lease、进入 linked-list queue、发送 SPDK message、由 reactor 再次更新 credit、callback 唤醒 futex，最后由应用线程释放资源。

因此，继续测更多 reactor 数量只能在高复杂度架构上寻找局部最优点，不能解决以下根因：

- 同一请求被 `HfsIoRequest`、`HfsRequestResult`、`HfsCompletion`、`HfsRuntimeBatchItem`、`HfsIoLease`（必要时再加 `HfsPosixIoTicket`）分段表示。
- 同一请求在 POSIX 线程、inode 同步层、ticket/range 层、DMA pool、runtime queue、SPDK reactor、completion waiter 之间反复切换。
- ordering 同时存在 per-inode ticket list 和 range stripe lock 两套实现，`syncLock`、`appendLock`、`writeGapLock`、`resizeLock` 又形成额外锁层。
- 每个 inode runtime 初始化 64 个带 mutex/cond 的 range lock；每个 reactor 又复制完整的 4 类 DMA pool。
- ticket admission 和 completion 都扫描 active list；range lock 先按请求覆盖的 unit 构造 stripe 集合；metadata checkpoint 还要遍历 runtime registry。

### 1.2 当前路径

```text
POSIX worker
  ├─ g_metaLock / HfsOpenFile.ioLock
  ├─ inode syncLock
  ├─ ticket list 或 64-stripe range locks
  ├─ DMA pool class + lease condvar
  ├─ HfsIoRequest + HfsRequestResult + HfsCompletion
  ├─ per-reactor linked-list batch + batchLock
  ├─ global messageSubmitLock + SPDK message ring
  └─ futex wait
          ↓
owner/data SPDK thread poll
  ├─ 再次取得 batchLock，逐项更新 queue/inflight
  ├─ spdk_blob_io_* submit
  └─ callback：stats/profile → credit lock/broadcast → result store → futex wake
          ↓
POSIX worker copy-out / lease release / ticket complete / range unlock / sync unlock
```

## 2. 复杂度审计

以下是对当前源码的结构性审计。`U` 为请求覆盖的逻辑 unit 数，`A` 为 inode 上 active/waiting ticket 数，`B` 为一次 runtime batch 的 item 数，`I` 为 runtime registry 中的 inode 数，`R` 为 reactor/shard 数，`K` 为实际冲突操作数，`E` 为一个大请求被拆出的 extent 数。

### 2.1 时间复杂度

| 路径 | 当前成本 | 问题 |
|---|---|---|
| range admission | `O(U + 64)`，逐 unit 构造 stripe 集合，再依次取得多个 lock | 大请求按长度重复扫描；最多触碰 64 个带 condvar 的锁 |
| ticket admission | `O(A)`，持 `ticketLock` 扫描整条链 | 无冲突请求也必须检查所有旧 ticket |
| ticket complete | `O(A)`，先找自身再扫描全部 ticket 并逐个唤醒 | completion 处在最敏感的 callback 边界，却承担全表扫描 |
| data batch dispatch | `O(B)` 数 item，再对每 item 取得 `batchLock` 更新 queue/inflight | 批量队列没有真正批量化；存在指针追踪和重复锁操作 |
| data credit release | 每个 callback 取得 `batchLock` 并 `cond_broadcast` | credit 归还、唤醒和业务 completion 耦合，可能唤醒无关生产者 |
| message ingress | 所有 SPDK message 经过全局 `messageSubmitLock` | 不同 shard 之间被一个锁串行化；为修复 ring reservation stall 而付出全局成本 |
| completion wait | 每个 child 一次 futex 状态和一次 waiter 路径；pipeline 按 child 逐项 wait | 一个 POSIX 调用的同步边界数量随 child 数增长 |
| metadata checkpoint | `hfsInodeRuntimeDrainPendingLocked()` 遍历 registry 中所有 bucket/runtime | 脏 inode 很少时仍扫描全量 runtime |

这里的 `O(...)` 是软件控制路径的上界，不代表设备 I/O 本身可以被消除。设计目标是让设备排队之外的软件成本随“实际冲突数/extent 数”增长，而不是随“请求长度、全部 active ticket、全部 reactor queue item”增长。

### 2.2 空间复杂度

| 对象 | 当前布局 | 结构性代价 |
|---|---|---|
| inode runtime | 每个 inode 固定 64 个 `HfsRangeLock`，每个 lock 含 mutex+cond，再加 rangeGate、sync/append/writeGap/resize/ticket primitives | `O(I × 64)` 个重型同步对象，且大多数 inode 并不需要 64 路并发锁 |
| DMA pool | 每个 `dataReactorCount` 复制 4K/16K/64K/1M 四类存储；默认每 shard 约 10.375 MiB，8 vCPU 自动 3 shard 约 31.125 MiB，最大 8 shard 约 83 MiB | 1M class 在没有大请求时也被预留；内存随 reactor 数复制 |
| request state | data child 同时携带 request、result、completion、batch item、profile/trace 字段和 lease | 正常 trace-off 请求也携带诊断/路由字段；pipeline 还在线性数组中复制多个 child 状态 |
| ticket waiter | 每个 ticket 初始化并销毁独立 mutex+cond | 每次 I/O 都付出同步对象生命周期成本，且 waiter 只服务一个 stack request |
| queue node | linked-list node 嵌入 request，但 queue 仍需要 head/tail/scheduled/queueDepth/inflight 等多份状态 | 结构分散、cache locality 差，状态组合容易出现 drain/stop 边界问题 |

当前 HFS 的空间复杂度可近似写为：

```text
O(I × 64 + R × pool_bytes + P × request_state + A × ticket_state)
```

其中 `P` 是并行 child 数。这个式子说明仅调整 `R` 不会降低根因，甚至可能放大内存与同步对象数量。

## 3. 设计目标与不变量

### 3.1 目标

1. 用一个内部 operation descriptor 贯穿 POSIX、依赖调度、队列、SPDK submit 和 completion。
2. 用一个 per-inode dependency scheduler 统一 ticket、range、sync、append、resize 和 barrier 语义。
3. 用固定容量 ring 和单消费者 actor 取代 linked-list batch、逐请求 credit bookkeeping 和 data-path 全局 message lock。
4. 用共享、按需、受上限约束的 DMA arena 取代按 reactor 复制的完整 pool。
5. 对 pipeline 使用一个 parent wait；child 只保留结果和 buffer slice，不为每个 child 建立独立等待边界。
6. 让 metadata/durability 以 dirty set 和 epoch 处理，不再为每次 checkpoint 扫描所有 runtime。

### 3.2 必须保持

- POSIX preload 是唯一支持入口，不恢复公开 async/DMA ABI。
- `ticket/range` 的 ordering 语义、非重叠 I/O 并发、append/EOF gap、truncate、`fsync/fdatasync` 和 crash recovery 语义不变。
- 正式验证固定使用真实 NVMe：`config/hfs_nvme.json`、`HFS_BDEV_NAME=Nvme0n1`、PCI BDF `0000:0b:00.0`，并确认 `uio_pci_generic` 与 `vm.nr_hugepages=1024`。
- trace/stats 关闭时不得把诊断字段、动态分配和高成本计时带入热路径。
- 所有 operation、credit、DMA lease、metadata epoch 在成功、失败、timeout、fork 和 shutdown 路径上都必须闭合。

### 3.3 不以此设计解决的问题

- 不通过增加 reactor 数量、扩大 queue 或 pure spin 掩盖架构成本。
- 不把一次 FIO 样本的 CPU 数字当作设计正确性的证明；性能数据只用于收口和回归。
- 不在没有证据时重写 SPDK NVMe backend，也不从 Windows 同步/修改 VM 内 SPDK 源码。

## 4. 推荐目标架构

目标名称为 **Unified HFS I/O Operation Graph**。reactor 在该架构中只是 shard actor 的执行载体，数量不再决定核心算法。

```text
POSIX worker
  │  validate + metadata snapshot + acquire HfsOp from slab
  ▼
HfsOp / HfsOpBatch
  │  one intrusive node, one lifetime/refcount, one completion state
  ▼
per-shard bounded ring (MPSC producer, one SPDK consumer)
  │  one empty→non-empty doorbell, no per-item SPDK message
  ▼
shard actor
  ├─ per-inode dependency scheduler: admit / hold / promote
  ├─ DMA arena lease and extent planner
  ├─ SPDK channel submit and callback
  └─ parent completion aggregation
  ▼
one parent futex / caller resumes
  │
  ├─ copy-out from arena slices
  ├─ release op/buffer reference
  └─ publish metadata timestamp or enqueue durability epoch

control/meta actor (owner SPDK thread)
  └─ mount, metadata mutation, dirty-set checkpoint, bdev flush
```

### 4.1 统一 operation descriptor

将 data path 当前的多个对象收敛为一个内部对象。字段只保留正常路径需要的内容，trace/profile 放到可选 sidecar：

```text
HfsOp {
    atomic state          // NEW, WAIT_DEP, READY, SUBMITTED, DONE, ABORTED
    refcount/lifetime     // caller + actor + optional parent references
    op kind / flags       // read, write, rmw, append, resize, barrier
    inode id + range      // [offset, end), extent mask/summary
    HfsBufferRef buffer   // caller DMA span or arena slice
    shard id / channel
    parent pointer + child index (optional)
    result / completed units
    intrusive queue/scheduler links
    optional diagnostics sidecar
}
```

规则：

- `HfsRuntimeBatchItem`、data child 的 `HfsRequestResult` 和 `HfsIoRequest` 不再为同一 data operation 重复表达函数、上下文、完成状态和链表链接。
- `HfsOp` 从 filesystem 固定 slab 或 thread cache 获取；trace-off 不允许每请求 `calloc`。超出上限时返回 backpressure，而不是无限分配。
- actor 和 caller 通过引用计数保持生命周期；timeout 只改变观察结果，不提前释放仍可能被 SPDK callback 使用的 op。
- `HfsOpBatch` 只有一个 parent completion。child callback 写入结果位图/状态数组，最后一个 child 完成才唤醒 caller；read copy-out 在 parent 完成后按 extent 处理。

### 4.2 统一 per-inode dependency scheduler

用一个 scheduler 替换以下并行存在的机制：

```text
syncLock + 64 rangeStripes + rangeGate
appendLock + writeGapLock + resizeLock
ticketLock + ticketHead + ticket waiter condvars
```

每个 `HfsInodeRuntime` 只保留一个短 scheduler lock（或由 shard actor 独占访问）、一个 interval index 和少量计数。operation 带有：

- `range=[start,end)` 和读/写/不可分割 RMW 属性；
- `APPEND`、`SIZE_MUTATION`、`DURABILITY_BARRIER` 等 barrier 标志；
- 提交序号/epoch，用于保留原有的先提交先观察语义。

推荐用带 `maxEnd` 增强字段的 intrusive interval tree；如果实现阶段证明固定 stripe 位图足够，也只能把它作为同一 scheduler 的内部索引，不能恢复两套独立锁语义。

操作流程：

1. actor 以 `O(log A)` 找到可能重叠的 active 节点。
2. 只对实际冲突的 `K` 个节点建立依赖；非冲突请求直接进入 ready set。
3. completion 从 index 中 `O(log A)` 删除自身，只重新检查受影响区间并唤醒后继，不扫描整条 ticket list。
4. barrier 操作以一个 epoch 节点阻止后续操作，等前序 data op 和 metadata publication 达到规定状态后再放行。
5. `HFS_ORDERING=none` 仅作为明确的兼容/故障定位模式；`ticket` 与 `range` 映射到同一 scheduler policy，不再维护两套数据结构。

这样既保留重叠写互斥和读共享，也去掉每个请求初始化 pthread condvar、持有多把 range lock 和在 callback 扫描全表的成本。

### 4.3 per-shard bounded ring 与 actor

每个 shard 使用固定容量 ring：

- 以 `hash(inodeId) % shard_count` 作为稳定 owner；同一 inode 的 data、RMW、append、resize 和 barrier operation 必须进入同一个 actor。这样 dependency index 由单一 owner 管理，不需要跨 shard range lock。
- producer 只在短临界区预约一个 slot 并写入 `HfsOp *`；不把业务依赖锁带入 ring lock。
- consumer 是唯一的 SPDK 线程，按连续区间批量取出 op；queue depth 由 head/tail 序列推导，不再逐 item 维护 `queueDepth/inflight`。
- ring 从 empty 变为 non-empty 时只发送一次 doorbell；consumer 清空后要再次检查 tail，再清除 armed 位，防止 lost wake。
- `spdk_thread_send_msg` 的全局保护只存在于 doorbell/control 的极少数边界；data path 不为每个 request 争用 `messageSubmitLock`。
- backend credit 是 shard actor 的本地计数；submit、callback 和 shutdown 都在同一 actor 状态机内更新，caller 不再等待 queue credit 时持有 inode dependency。
- ring 满时 operation 留在 bounded pending window 或返回可重试 backpressure，禁止在持有依赖锁时阻塞；不得恢复 bug #76 所暴露的无序多生产者 SPDK ring reservation 路径。

资源预约顺序固定为 `op slot → DMA buffer（如需 bounce）→ inode dependency → backend submit`。任一阶段资源不足时 operation 仍未进入下一阶段，不允许持有前一层锁等待后一层资源；这样可以从架构上消除 queue credit 与 range/ticket 反转。

### 4.4 共享 DMA arena 与 buffer reference

将 `R × 4 class` 的完整复制改为一个按需增长、有总上限的 DMA arena：

- 4K/16K/64K/大块仍可使用 size class，但 storage slab 只在第一次需要时分配。
- 每个 shard 只保留很小的 local magazine；空闲 slice 回到 central free stack，不能为每个 shard 预留 1M class 全量容量。
- `HfsBufferRef` 只存 arena slice/length/class 或 caller DMA span；lease 释放由 op lifetime 驱动。
- arena 上限以全局最大 outstanding operation 和最大 extent 计算，而不是乘以 reactor 数；超过上限时产生 backpressure。
- 普通应用 buffer 在当前 uio+hugepage VM 仍可能 `directEligible=false`；该环境限制不通过引入 registered-buffer ABI 解决。

目标空间复杂度为 `O(B_global + Q + P)`，其中 `B_global` 是实际配置/使用的 arena 容量，不再是 `O(R × 10.375 MiB)`。

### 4.5 parent batch 与 extent planner

POSIX 大 I/O 先生成一个逻辑 `HfsOpBatch`：

- 对齐连续区域尽可能合并成一个 extent；非对齐请求最多产生 head/middle/tail 必要的 RMW extent。
- actor 按 backend credit 分批提交 child，但 caller 只等待 parent 一次。
- read child 的完成结果和 arena slice 写入 parent 位图；parent 完成后 caller 顺序 copy-out 并释放 slice。
- pipeline 上限仍然存在，但它是一个 bounded memory contract，不是逐 child 建立锁/cond/futex 的理由。

## 5. 复杂度目标

| 操作 | 当前 | 目标 |
|---|---|---|
| data submit | linked-list append + batch gate + 可能的全局 message lock | ring slot `O(1)` amortized + 一次 doorbell transition |
| dependency admission | range unit scan / ticket full scan | interval index `O(log A + K)` |
| dependency completion | ticket full scan + broadcasts | index delete `O(log A + K)` |
| backend dispatch | `O(B)` 计数 + 每 child lock/credit 更新 | actor 连续消费，单次 batch bookkeeping |
| synchronous wait | 每 child 一个 completion wait | 每 parent 一个 futex wait `O(1)` |
| DMA acquire | shard/class bitmap + slow cond path | arena free-stack `O(1)` amortized |
| metadata checkpoint | 扫描全部 runtime | intrusive dirty set `O(M_dirty)` |
| per-inode state | 64 range locks + 多个独立 primitives | 一个 scheduler + active/wait nodes |
| DMA state | `O(R × pool_bytes)` | `O(B_global)`，按需增长 |

完整 data operation 的软件成本目标为：

```text
O(log A + K + E)
```

其中 `K` 是真实冲突数，`E` 是必要 extent 数；当无冲突、对齐且 caller buffer DMA-capable 时，热路径应接近 `O(1)`。设备排队时间不在此复杂度内。

## 6. 分阶段实施

### 6.1 阶段 A：语义冻结与内部适配层

先写 operation state/lifetime contract 和 lock-order 文档，覆盖 read/write/RMW/append/resize/fsync/fdatasync、timeout、fork、shutdown、crash recovery。

新增 `HfsOp` 及 adapter，但暂时映射到现有 runtime，目的只是让后续阶段不再同时修改 POSIX 语义和执行器。结构门禁：trace-off 无 per-request heap、每个 op 只有一个 owner/lifetime、旧回归全通过。

### 6.2 阶段 B：直接 data actor ring

先保留旧 scheduler，仅把 data path 从 owner-side batch + linked list 改成 per-shard ring + doorbell + actor。控制/metadata 仍走 owner。

完成后删除 data path 的逐 item `batchLock`、`queueDepth/inflight` 双重 bookkeeping 和 per-request data message；保留 `HFS_IO_EXECUTOR=legacy|actor` 回退开关，直到 stress 和 shutdown drain 通过。

### 6.3 阶段 C：统一 inode scheduler

引入 interval index 和 operation barrier，把 ticket/range/sync/append/resize 语义集中到一个 scheduler。先以 compatibility adapter 支持现有 `HFS_ORDERING` 环境变量，再删除 per-ticket mutex/cond、64 range lock 初始化和重复锁层。

此阶段的核心验收不是 FIO 数字，而是：同范围写串行、读共享、非重叠并发、append gap、truncate、fsync barrier 和 crash recovery 与旧实现一致，且 admission/complete 不再做全表扫描。

### 6.4 阶段 D：共享 DMA arena 与 parent batch

把 pool shard 合并为共享、按需 arena，统一 `HfsBufferRef`；把 pipeline child 收敛到一个 parent wait，并保留 direct caller buffer 识别。

重点验证 lease lifetime、copy-in/out、RMW 首尾、错误回滚、arena 上限和 shutdown drain。只有在这些不变量稳定后才调整 class 大小或 extent chunk。

### 6.5 阶段 E：metadata dirty-set 与 durability actor

用 inode dirty intrusive list/epoch 替换 checkpoint 对全 runtime registry 的扫描；每个 inode 只允许一个 dirty-list node，data actor 只发布 epoch 事件，metadata owner actor 串行执行 image/flush/publish。

该阶段不改变 durability 顺序，只减少重复查找和锁嵌套；必须覆盖正常重启、SIGKILL recovery、`fsync`/`fdatasync` 和 metadata transaction。

### 6.6 阶段 F：删除旧路径

当 actor、scheduler、arena 在完整回归中稳定后，删除旧 data batch、ticket list、range stripe 和 per-reactor full pool。兼容环境变量保留为 no-op 或明确映射，文档不再把 reactor 数量作为优化旋钮。

## 7. Reactor 的新定位

`HFS_DATA_REACTORS` 仅决定 shard actor 的实例数量，用于容量隔离、CPU 亲和和部署兼容；自动选择公式可以保持现状。它不参与 scheduler 算法，不决定 buffer arena 大小，也不决定请求状态的数量级。

代码和文档禁止以“扫描更多 reactor 数量”作为主要开发阶段。只需做两类验证：

- 任意受支持的 shard 数都能保持 operation/credit/lease/stop 闭合。
- 在目标 VM 上选择一个部署配置作为回归环境；若性能退化，先检查 actor/scheduler/arena 的复杂度不变量，而不是继续扩大 reactor 矩阵。

## 8. 验证与门禁

### 8.1 结构门禁（先于性能）

- trace-off 热路径无 per-request `calloc`、pthread mutex/cond init/destroy 或全表扫描。
- data submit 最多一次 ring reservation 和一次 empty→non-empty doorbell；无每 child `spdk_thread_send_msg`。
- 一个 parent 只观察一个 completion；child 状态可被 actor/parent 安全回收。
- scheduler 的 active/wait 节点数量、ring head/tail、arena lease、SPDK submit/complete 全部守恒。
- 任何锁都不得在等待 queue/arena/dependency 时跨层持有；shutdown 可唤醒并排空全部 actor。

建议为 stats 增加计数：`op_alloc_fallback`、`scheduler_index_probe`、`scheduler_conflict_count`、`ring_doorbell`、`ring_batch_items`、`arena_bytes_high_watermark`、`parent_wait_count`、`legacy_path_count`。这些计数用于证明架构是否按预期工作，不用于选择 reactor 数。

### 8.2 正确性门禁

在 VM 真实 NVMe、`config/hfs_nvme.json`、`Nvme0n1`、hugepages=1024 且设备绑定 `uio_pci_generic` 后运行：

- `make -B app lib`、CMake Release；
- POSIX contract、I/O fast path、queue/runtime、ticket compatibility、metadata transaction、fd lifecycle、durability/persistent smoke；
- MariaDB/RocksDB 缩短端到端、正常重启与 SIGKILL recovery；
- `io_trace_smoke.sh`，检查 `trace_dropped=0`、parent/child 闭合、`spdk_submit=spdk_complete`；
- 测试结束释放 NVMe 回内核 `nvme` 驱动，确认 `/dev/nvme0n1` 存在且没有 FIO/SPDK 残留。

### 8.3 性能回归门禁

性能只做同一提交、同一 harness、同一 VM 的回归，不再用 reactor 数量矩阵寻找架构方向：

- 快速：4K randrw 50/50，HFS 32 job/raw SPDK QD32，5 秒 × 3；
- 正式：4K～64K，10 秒 ramp + 30 秒 × 5 median，并同时报告 jobs=16/32；
- trace-off/stats-off 正式，单独 trace-on 归因；报告 process CPU、IOPS/核、p99、runq、ctx/s；
- 以当前 4K 诊断样本 HFS 94,527 IOPS、约 394% 进程 CPU 作为软件回归参考，不覆盖既有正式 baseline；首个架构版本 IOPS 回退不得超过 3%，并应在稳定后降低 process CPU/提高 IOPS/核。

任何性能提升都必须伴随结构计数和 correctness 通过，不能用“增加 reactor/queue”单独达标。

## 9. 风险、回滚与停止条件

- actor ring 出现 lost wake、ring full 丢请求、stop drain 不闭合：立即切回 `HFS_IO_EXECUTOR=legacy`。
- scheduler 出现 ordering、append、truncate 或 barrier 回归：保留 adapter，切回旧 ticket/range 实现，不删除旧数据结构。
- arena 出现 lease 泄漏、越界、DMA 可见性或内存上限失控：切回 sharded pool，保留 `HfsBufferRef` 适配层。
- parent completion 出现 child 未闭合或 timeout UAF：恢复 per-child completion，先修复生命周期模型。
- 任何结构门禁不满足，即使 FIO 数字提升也不得合入默认路径。
- trace/stats 误进入热路径、`spdk_submit != spdk_complete`、NVMe 未释放或数据库 recovery 失败时，不输出性能结论。

## 10. 交付物与提交边界

建议每个阶段一个可回退提交：

1. `refactor: introduce unified hfs operation lifetime`
2. `refactor: route data IO through shard actor ring`
3. `refactor: replace ticket and range locks with inode scheduler`
4. `perf: share dma arena and parent completion`
5. `refactor: publish metadata dirty epochs`
6. `docs: record architecture-first validation`

每个提交都要在 `main` 上完成对应构建、正确性和资源释放验证。正式性能 baseline 只有在阶段 F 收口后更新；短诊断结果、旧架构结果和架构重构结果必须分开记录。

## 11. 参考证据

- [`task90150022_result.md`](../task_result/task90150022_result.md)：分层 profiling，T6-T7/T8-T9 及 runtime 成本。
- [`task09150832_result.md`](../task_result/task09150832_result.md)：并发、锁和线程调度历史结果。
- [`task09151930_result.md`](../task_result/task09151930_result.md)：reactor/backend 分解；仅作为背景，不再作为调参路线。
- [`task09160035_result.md`](../task_result/task09160035_result.md)：旧五 reactor 短门禁失败，说明扩大 reactor 不是架构解法。
- [`task09160854_result.md`](../task_result/task09160854_result.md)：CPU 自动拓扑正确性结果；自动拓扑保留为部署兼容。
- [`hfs_final_performance_baseline_20260913.md`](../performance/hfs_final_performance_baseline_20260913.md)：正式基线，不能被短诊断样本覆盖。
- [`hfs_posix_sync_boundary_topology_shard_plan.md`](../shard/hfs_posix_sync_boundary_topology_shard_plan.md)：历史 Shard-66 计划，本文档的架构优先路线取代其中的 reactor 调参主线。
