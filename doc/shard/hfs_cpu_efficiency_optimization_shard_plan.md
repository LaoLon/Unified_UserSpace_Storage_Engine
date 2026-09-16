# Shard-67：HFS 统一 I/O 执行架构重构计划

> **状态**：67A operation lifetime、67B data actor ring 与 67C scheduler core 已实现；67D～67G 待实施
>
> **日期**：2026-09-16
>
> **上位设计**：[`doc/design/hfs_spdk_cpu_efficiency_optimization_design.md`](../design/hfs_spdk_cpu_efficiency_optimization_design.md)
>
> **重构目标**：优先降低请求路径的时间复杂度、空间复杂度和状态组合数；reactor 数量只作为 shard actor 的部署参数，不作为本 shard 的优化主线。

## 1. 为什么重新立项

当前 HFS 的一次同步 POSIX I/O 经过以下层级：

```text
POSIX worker
  → metadata/io locks
  → ticket 或 range stripes
  → DMA pool lease
  → request/result/completion/batch item
  → linked-list runtime queue
  → messageSubmitLock + SPDK message
  → owner/data reactor
  → callback credit + futex
  → copy-out + 多层 unlock
```

旧 Shard-66 路线主要比较 reactor 数量、idle backoff 和等待参数，能够解释 2/4 reactor 的差异，但没有移除上述层级。因此本 shard 不再安排“reactor 数量矩阵”作为开发任务，改为先重构架构，再用一个固定部署配置做回归。

## 2. 当前复杂度基线

设 `U` 为请求覆盖的逻辑 unit 数，`A` 为 inode 上 active/waiting ticket 数，`B` 为一个 runtime batch 的 item 数，`I` 为 runtime registry 的 inode 数，`R` 为 shard 数，`K` 为真实冲突数，`E` 为必要 extent 数。

### 2.1 时间复杂度问题

| 代码路径 | 当前结构 | 重构要求 |
|---|---|---|
| `hfsIoRangeLock` | 先按 unit 扫描，再依次操作最多 64 个 lock，`O(U+64)` | 用一个 range dependency index，按实际冲突处理 |
| `hfsPosixTicketAdmit` | 持锁扫描 `ticketHead`，`O(A)` | interval index 查询，`O(log A+K)` |
| `hfsPosixTicketComplete` | 删除自身后再次扫描全表并广播，`O(A)` | 只访问受影响后继，`O(log A+K)` |
| `hfsRuntimeDispatchDataBatch` | 先遍历 batch 计数，再逐 item 取得 `batchLock` | ring 连续消费，batch 状态一次更新 |
| data credit | callback 每次加锁并广播 cond | actor 本地计数，按状态边沿通知 |
| data ingress | 不同路径共享全局 `messageSubmitLock` | 每个 shard 只发一次 empty→non-empty doorbell |
| pipeline wait | 每个 child 单独 completion/futex，调用者逐项 wait | parent 一个 completion，child 只记录结果 |
| metadata checkpoint | 扫描所有 runtime bucket | intrusive dirty set，按脏集合处理 |

### 2.2 空间复杂度问题

- `HfsInodeRuntime` 为每个 inode 固定初始化 64 个 `HfsRangeLock`，每个 lock 还含 mutex/cond；同时存在 `rangeGate`、`syncLock`、`appendLock`、`writeGapLock`、`resizeLock` 和 ticket primitives。
- `HfsIoPool` 按 `dataReactorCount` 复制完整的 4K/16K/64K/1M 四类缓冲。每个 shard 的默认静态容量约 10.375 MiB；8 vCPU 自动 3 shard 约 31.125 MiB，最大 8 shard 约 83 MiB。
- 一个 data child 同时携带 `HfsIoRequest`、`HfsRequestResult`、大尺寸 `HfsCompletion`、`HfsRuntimeBatchItem`、profile/trace 字段和 `HfsIoLease`。
- 每个 ticket 都要初始化和销毁独立 mutex/cond，即使它最终无冲突。

当前近似为：

```text
O(I×64 + R×pool_bytes + P×request_state + A×ticket_state)
```

其中 `P` 是并行 child 数。目标是把它收敛为：

```text
O(I + Q + W + B_global + E)
```

`Q` 为 ring/pending 窗口，`W` 为实际等待的 operation 数，`B_global` 为按需 DMA arena 容量。

## 3. 目标架构

### 3.1 一个 operation descriptor

新增内部 `HfsOp`，统一承载 data operation 的生命周期、依赖、buffer、执行和结果：

```text
HfsOp {
    state: NEW | WAIT_DEP | READY | SUBMITTED | DONE | ABORTED
    lifetime: caller/actor/parent references
    kind: read | write | rmw | append | resize | barrier
    inode + [offset, end) + ordering flags
    HfsBufferRef
    shard/channel
    parent + child index (optional)
    result + completed units
    intrusive ring/scheduler links
    diagnostics sidecar (trace/profile only)
}
```

要求：

1. `HfsIoRequest`、`HfsRequestResult`、`HfsRuntimeBatchItem` 不再重复保存同一 operation 的 callback、context、链表和完成信息。
2. trace-off 路径从固定 slab/thread cache 获取 `HfsOp`，不允许每请求 `calloc` 或 pthread primitive init/destroy。
3. callback 不释放 caller 仍可能观察的 op；timeout 只改变返回值，引用计数负责生命周期。
4. trace/profile 使用可选 sidecar，不能让诊断字段扩张每个正常请求的 cache footprint。

### 3.2 一个 per-inode dependency scheduler

将以下机制合并：

```text
syncLock / rangeStripes / rangeGate
appendLock / writeGapLock / resizeLock
ticketLock / ticketHead / ticket waiter
```

每个 inode runtime 只保留一个 scheduler 状态和一个短保护边界。操作带 range、读写属性、append/size mutation/barrier 标志和提交 epoch。

推荐 intrusive interval tree（节点维护 `maxEnd`）；若后续采用位图，也必须由同一 scheduler 管理，不得恢复两套 ordering 实现。

调度语义：

- 非重叠读/写直接进入 ready set；重叠写、RMW 和读写冲突只对实际冲突节点建立依赖。
- completion 删除节点并只检查受影响的后继，不扫描整条 active list。
- append/EOF gap、truncate/resize 和 fsync/fdatasync 作为 barrier operation，使用同一 epoch 状态机，不再由多个锁嵌套表达。
- `HFS_ORDERING=none` 仍可作为兼容/故障定位开关；`ticket` 和 `range` 映射到同一 scheduler，而不是两套代码。

### 3.3 一个 per-shard bounded ring + actor

data path 不再先经过 owner-side linked-list batch。每个 shard 由一个 SPDK consumer actor 拥有：

- 用 `hash(inodeId) % shard_count` 选择稳定 owner；同一 inode 的 data、RMW、append、resize 和 barrier 必须进入同一个 actor，避免跨 shard 维护 range/ticket 状态。
- producer 短暂预约固定 ring slot，写入 `HfsOp *`；业务 dependency 不在 ring lock 内等待。
- actor 连续消费 ring，queue depth 由 head/tail 序列推导，backend credit 在 actor 内维护。
- ring 由 empty 变为 non-empty 时只发送一个 doorbell；consumer 清空后再次检查 tail 再清 armed 位，防止丢唤醒。
- `spdk_thread_send_msg` 的全局保护仅用于 doorbell/control 边界，不为每个 data request 发送 message。
- ring 满时使用有界 pending/backpressure，不能持有 inode dependency 再等待 queue credit，也不能恢复 bug #76 的无序多生产者 SPDK ring reservation。

资源预约顺序固定为：`op slot → DMA buffer（如需 bounce）→ inode dependency → backend submit`。资源不足时停留在当前状态，不允许拿着上一层锁等待下一层资源；这条规则是消除 queue/range lock 反转的必要条件。

### 3.4 一个共享、按需的 DMA arena

把 per-reactor 完整 pool 换成共享 arena：

- size class 可保留 4K/16K/64K/大块，但 slab 按首次需求分配。
- 每 shard 只保留小型 local magazine，空闲 slice 回 central free stack。
- 1M class 不预分配给每个 shard；arena 有全局上限，超过上限产生 backpressure。
- `HfsBufferRef` 统一表示 caller DMA span 或 arena slice；当前 uio+hugepage 下 direct=0 仍是环境事实，不通过新 ABI 规避。

### 3.5 一个 parent completion

大 I/O 先构造 `HfsOpBatch`：

- 对齐连续区域合并为 extent；非对齐只生成必要的 head/middle/tail RMW extent。
- actor 可按 credit 分批提交 child，但 caller 只等待 parent 一次。
- child callback 记录结果和 slice；最后一个 child 完成后唤醒 parent，caller 再统一 copy-out/release。
- pipeline 深度是 bounded memory contract，不再为每个 child 建立独立 mutex/cond/futex。

## 4. 实施分片

### 67A：operation lifetime 与适配层

**范围**：只新增 `HfsOp`、parent/child 生命周期和旧 runtime adapter，不改变执行时序。

**交付**：

- `src/hfs_op.[ch]`（或等价内部模块）；
- operation state/lifetime/timeout/shutdown contract；
- trace-off 固定 slab/thread cache；trace/profile sidecar；
- old path compatibility adapter。

**结构门禁**：无每请求 pthread primitive init/destroy；timeout、fork、shutdown 无 UAF；现有 POSIX、ticket、metadata、数据库 smoke 全通过。

### 67B：data actor ring

**范围**：只替换 data ingress/dispatch；暂时保留旧 ticket/range 和 DMA pool。

**交付**：

- per-shard fixed ring；
- empty→non-empty doorbell 和 lost-wake 防护；
- actor-owned queue/credit；
- `HFS_IO_EXECUTOR=legacy|actor` 回退开关。

**必须删除的热路径结构**：data linked-list batch、逐 item `batchLock`、逐 item queue/inflight 更新、data request 的 per-item SPDK message。

**结构门禁**：submit 每 operation 最多一次 ring reservation；doorbell 按边沿发送；ring head/tail、credit、SPDK submit/complete 和 shutdown drain 守恒。

#### 67B 实施记录（2026-09-16）

已将 data ingress 从默认 linked-list batch 切换为 per-reactor fixed ring + SPDK actor：

- `HFS_IO_EXECUTOR` 默认 `actor`，显式 `legacy` 保留旧 linked-list queue 回退；
- producer 只在短 `ringLock` 临界区预约固定槽位，ring 满时先释放锁，再走已有 direct fallback；
- actor 一次快照并连续消费多个槽位，backend credit 在 actor ring 内维护，completion 只做原子归还；
- empty→non-empty 只发送一个 doorbell，满窗口时由 completion 边沿重新 arm，覆盖 shutdown drain；
- 新增 `ring_doorbell`、`ring_reservation`、`ring_items`、`legacy_data_submit` 结构统计，默认 stats-off 不产生原子计数开销。

验证脚本为 `E:\Code\test_script\run_shard67b_smoke.py`（脚本位于工作区外，不上传到 HFS 仓库），
固定使用 VM 真实 NVMe 和自动 reactor 拓扑，未运行 reactor 数量矩阵：

- VM `make -B app lib`：通过；
- VM CMake Release（`build-cmake-shard67a`）：通过；
- actor `POSIX CONTRACT`、`REQUEST-LOCAL FAST IO`：通过，`ring_reservation=46`、
  `ring_items=46`、`ring_doorbell=46`、`spdk_submit=210`、`spdk_complete=210`；
- `HFS_IO_EXECUTOR=legacy` 回退 `REQUEST-LOCAL FAST IO`：通过，
  `legacy_data_submit=46`、`spdk_submit=210`、`spdk_complete=210`；
- 测试结束 NVMe 已恢复内核 `nvme` 驱动，hugepages 保持 1024，无残留测试进程。

本阶段只证明 actor/legacy 执行器的结构和正确性守恒，不输出吞吐或 CPU 提升结论；下一阶段进入
67C inode dependency scheduler。

### 67C：inode dependency scheduler

**范围**：统一 ordering、range、sync、append、resize 和 barrier；不调整 reactor 数量。

**交付**：

- `HfsInodeScheduler` 和 interval index；
- `HFS_ORDERING` compatibility adapter；
- barrier/epoch 状态机；
- 删除 per-ticket wait mutex/cond、64 range lock 初始化及重复锁层。

**语义门禁**：同范围写串行，读共享，非重叠请求并发；append gap、truncate、fsync/fdatasync、SIGKILL recovery 与旧实现一致；admit/complete 无 active 全表扫描。

#### 67C 实施记录（2026-09-16）

本阶段先交付 inode scheduler 的核心数据结构和 POSIX 读写适配，不提前声称已经完成所有旧锁删除：

- `HfsInodeScheduler` 为每个 inode 提供一个共享 `pthread_mutex_t`、一个条件变量和一个 intrusive interval treap；ticket 节点只保留左右子树、`maxEnd` 和确定性 priority，不再初始化/销毁 per-ticket mutex/cond。
- admission/complete 只沿 `maxEnd` 可达的重叠区间遍历，依赖计数和后继 promotion 的候选集合为 `O(log A + K)` 期望复杂度；容量等待与依赖等待共用 scheduler 条件变量，避免重复 waiter 状态。
- `hfsIoReadAt`/`hfsIoWriteAt` 将 `HFS_ORDERING=ticket` 与兼容的 `HFS_ORDERING=range` 收敛到同一 scheduler，`HFS_ORDERING=none` 才绕过 ordering；append/resize 的同步边界保持不变。
- `hfsIoSubmitVector`、pipeline window reservation 以及 zero/punch 路径仍暂时保留 range-lock compatibility；因此 64 个 range stripe 的初始化和最终删除留给 67G，不能把本阶段描述为旧锁全部移除。

VM 真实 NVMe 短回归使用自动 reactor 拓扑（未设置 reactor 数量或 CPU 映射），并在测试结束恢复内核 `nvme` 驱动：

- Make 与 CMake Release 均通过；POSIX-only API、POSIX contract、request-local fast-I/O smoke 均通过。
- 32 线程同范围写通过：`ticket_admit=33`、`ticket_ready=33`、`ticket_complete=33`、`ticket_dependency_waits=15`、`ticket_pending_peak=8`，`spdk_submit=96`、`spdk_complete=96`、`errors=0`。
- `HFS_ORDERING=range HFS_POSIX_TICKET=0` 兼容适配通过：`ticket_admit=26`、`ticket_complete=26`，`spdk_submit=210`、`spdk_complete=210`、`errors=0`。

这些结果只证明 scheduler 的结构守恒和正确性，不输出 CPU/吞吐提升结论，也不做 reactor 数量矩阵；下一步为 67D 共享 DMA arena。

### 67D：共享 DMA arena

**范围**：替换 per-reactor pool 和 lease 表示，不改变 buffer direct 判定规则。

**交付**：

- central arena + bounded local magazine；
- `HfsBufferRef`；
- 按需 slab、全局 byte cap 和 backpressure；
- RMW/copy-in/out/错误回滚适配。

**空间门禁**：无请求时不预留大块 class；内存高水位由实际 outstanding operation 决定；active/lease/API users 在所有错误和 shutdown 路径闭合。

### 67E：parent batch 与单 completion

**范围**：重构大 I/O pipeline 和同步等待边界。

**交付**：

- extent planner；
- parent pending counter/result bitmap；
- read copy-out 与 lease release 的统一顺序；
- per-child completion 的兼容回退。

**门禁**：一个 parent 只有一个 waiter；child 顺序、短读、错误传播、RMW 首尾与旧实现一致；无 timeout UAF 或未回收 slice。

### 67F：metadata dirty-set 与 durability actor

**范围**：减少 metadata checkpoint 的全 registry 扫描和锁嵌套，不改变持久化顺序。

**交付**：

- intrusive dirty inode set；
- data actor → metadata owner 的 epoch event；
- image/flush/publish 顺序保持现状；
- 每个 inode 只保留一个 dirty-list node，checkpoint 复杂度按 `M_dirty` 计算；
- 正常重启、SIGKILL recovery、MariaDB/RocksDB recovery。

### 67G：删除旧实现

只有 67A～67F 的结构和正确性门禁全部通过后，才删除旧 data batch、ticket list、range stripe、per-reactor full pool 和兼容 adapter。删除前保留一次 legacy/actor 对照，避免无法回滚。

## 5. 锁与所有权规则

目标架构中锁的数量不是靠调参减少，而是靠所有权减少：

| 状态 | 唯一 owner | POSIX worker 行为 |
|---|---|---|
| ring head/tail、doorbell、backend credit | shard actor（slot reservation 由短 producer gate 完成） | 只写入 op，不等待 actor 内部状态 |
| inode active/wait index、依赖 promotion | inode scheduler/所属 actor | 不持依赖状态跨 queue/arena wait |
| DMA free list | central arena + local magazine | 只持有 `HfsBufferRef` 引用 |
| SPDK channel/submit/callback | shard actor | 不从 caller 线程直接操作 |
| metadata image、dirty epoch、bdev flush | metadata owner actor | 通过 epoch event 请求，不嵌套 data lock |
| op lifetime/result | caller + actor + parent refcount | timeout 不提前释放 callback 仍可见对象 |

任何新锁都必须回答“谁是 owner、是否能改为 actor 状态、是否会跨层等待”。不能用增加 mutex 解决竞态。

## 6. 验证策略

### 6.1 先做结构验证

每个阶段先检查：

- trace-off 无 per-request heap、pthread primitive 生命周期和全表扫描；
- ring reservation/doorbell、scheduler node、arena lease、op refcount 守恒；
- operation 在 success/error/timeout/fork/shutdown 都有唯一回收路径；
- lock-order 静态审查不允许“持 inode dependency 等 queue/arena”，也不允许 callback 反向取得 caller-owned lock；
- `spdk_submit=spdk_complete`、parent/child 闭合、NVMe 可释放。

建议增加结构统计：`op_alloc_fallback`、`ring_doorbell`、`ring_batch_items`、`scheduler_index_probe`、`scheduler_conflict_count`、`arena_bytes_high_watermark`、`parent_wait_count`、`legacy_path_count`。这些统计用于证明重构是否生效，不用于选择 reactor 数量。

### 6.2 正确性门禁

VM 必须使用真实 NVMe `0000:0b:00.0`、`Nvme0n1`、`config/hfs_nvme.json`、`uio_pci_generic` 和 `vm.nr_hugepages=1024`。运行 Make/CMake、POSIX contract、I/O fast path、queue/runtime、ticket compatibility、metadata transaction、fd lifecycle、durability、MariaDB/RocksDB smoke，以及 `io_trace_smoke.sh`。

正式结论前必须确认 trace `trace_dropped=0`、parent/child 闭合、`spdk_submit=spdk_complete`，并将 NVMe 释放回内核 `nvme` 驱动，确认无残留 FIO/SPDK 进程。

### 6.3 最后才做性能回归

性能只用于判断“架构重构是否回退”，不用于决定 reactor 数量：

- 快速：4K randrw 50/50，HFS 32 job/raw SPDK QD32，5 秒 × 3；
- 正式：4K～64K，10 秒 ramp + 30 秒 × 5 median，同时报告 jobs=16/32、process CPU、IOPS/核和 p99；
- trace-off/stats-off 正式，单独 trace-on 归因；不覆盖既有正式 baseline；
- 首个 actor/scheduler/arena 版本 IOPS 回退不得超过同口径 3%；只要结构门禁不通过，即使吞吐提升也不能设为默认。

当前 4K HFS 约 94.5K IOPS、约 394% 进程 CPU 的短样本只作诊断参考，不是新正式基线。

## 7. 回滚和停止条件

- ring lost wake、队列溢出、shutdown drain 失败：切回 `HFS_IO_EXECUTOR=legacy`。
- scheduler ordering、barrier、append 或 recovery 回归：切回旧 ticket/range adapter。
- arena lease 泄漏、越界、DMA 可见性或内存上限失控：切回 sharded pool。
- parent completion 生命周期不闭合：恢复 per-child completion，先修复 refcount/state machine。
- 任何 correctness、durability、数据库 recovery、NVMe release 或 `submit/complete` 不闭合：停止性能测试，不输出结论。

## 8. 提交边界

建议按架构边界提交并合入 `main`：

1. `refactor: introduce unified hfs operation lifetime`
2. `refactor: route data IO through shard actor ring`
3. `refactor: replace ticket and range locks with inode scheduler`
4. `perf: share dma arena and parent completion`
5. `refactor: publish metadata dirty epochs`
6. `docs: record shard-67 architecture validation`

每个提交只解决一个架构边界，保留回退开关和对应的结构/正确性证据。reactor 数量不作为提交拆分依据。

## 9. 67A 实施记录（2026-09-16）

### 已交付

- 新增 `src/hfs_op.[ch]`，提供 `NEW → READY → SUBMITTED → DONE/ABORTED` 生命周期，
  `WAIT_DEP` 状态预留给后续 inode scheduler，并以 caller/actor 双引用闭合请求生命周期。
- `HfsRequestResult` 持有唯一 `HfsOp`；旧的 owner/data batch adapter 只传递 `HfsOp *`，
  暂不改变 linked-list batch、ticket/range、DMA pool 或执行时序。
- submit reject、completion callback、同步 wait 和 timeout 观察路径都走统一引用回收；
  runtime batch dispatch 在执行边界记录 `SUBMITTED`。
- `Makefile` 与 `CMakeLists.txt` 均纳入新模块，保持旧构建入口和 POSIX-only ABI 不变。

### 验证

- VM `make -B app lib`：通过。
- VM CMake Release（`build-cmake-shard67a`）：通过。
- 真实 NVMe `0000:0b:00.0`、`Nvme0n1`、hugepages=1024、无显式 reactor 设置下：
  `POSIX-ONLY API OK`、`POSIX CONTRACT OK`、`REQUEST-LOCAL FAST IO OK` 均通过。
- 测试结束已将 NVMe 从 `uio_pci_generic` 恢复到内核 `nvme`，无残留测试进程。

### 边界

本阶段只冻结 operation lifetime 和旧路径适配，不宣称已经降低 data ingress、依赖调度、
DMA pool 或 parent wait 的复杂度；下一提交必须单独实现 67B per-shard actor ring。

## 10. 参考资料

- [`hfs_spdk_cpu_efficiency_optimization_design.md`](../design/hfs_spdk_cpu_efficiency_optimization_design.md)
- [`hfs_posix_sync_boundary_topology_shard_plan.md`](hfs_posix_sync_boundary_topology_shard_plan.md)（历史 Shard-66 计划，reactor 调参主线已被本文取代）
- [`shard_progress_memory.md`](shard_progress_memory.md)
- [`task90150022_result.md`](../task_result/task90150022_result.md)
- [`task09150832_result.md`](../task_result/task09150832_result.md)
- [`task09151930_result.md`](../task_result/task09151930_result.md)
- [`task09160035_result.md`](../task_result/task09160035_result.md)
- [`task09160854_result.md`](../task_result/task09160854_result.md)
- [`hfs_final_performance_baseline_20260913.md`](../performance/hfs_final_performance_baseline_20260913.md)
