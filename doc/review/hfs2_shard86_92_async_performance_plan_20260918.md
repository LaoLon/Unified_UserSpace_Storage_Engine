# HFS 2.0 Shard-86~92 异步 FIO 性能与 trace 执行计划

日期：2026-09-18
状态：Shard-86 已实现并验证；Shard-87~92 尚未完成；Shard-81 正式性能 gate 未通过
事实源：

- `doc/design/hfs2.0.md`：架构、公共 API、性能门禁和 tracepoint 规范的唯一事实源；
- `doc/review/hfs2_shard81_fio_async_completion_design_20260918.md`：Shard-81 异步
  completion 实现边界；
- `doc/performance/hfs2_shard81_formal_gate_20260918.md`：真实 NVMe raw/HFS 五次 gate
  和当前失败证据。

本文只规划性能归因和优化，不降低 HFS 2.0 的错误、取消、恢复、设备独占或正式性能
门禁。任何实现与 `doc/design/hfs2.0.md` 冲突时，必须先修订事实源，再开始代码修改。

---

## 1. 当前基线与问题边界

### 1.1 已确认的基线

正式真实 NVMe 口径固定为：`config/hfs_nvme.conf`、`Nvme0n1`、PCI BDF
`0000:0b:00.0`、CPU 2-3、4 KiB `randrw` 50/50、256 MiB、10 秒、32 jobs、
`iodepth=1`、O_DIRECT、五次中位数。

| 指标 | raw SPDK FIO | HFS async | 当前结论 |
|---|---:|---:|---|
| 总 IOPS 中位数 | 108396.060 | 66538.784 | HFS/raw = 61.38%，低于 85% |
| read/write p99 中位数 | 659.456 us | 3227.648 us | HFS 约 4.9 倍，超过 raw×110% |
| HFS errors | N/A | 0 | 功能错误未出现 |
| HFS closure | N/A | queued=reaped，outstanding=0 | 生命周期闭合 |
| HFS peak | N/A | 32 | 有效总 outstanding 已形成 |

最新正式证据保存在 `/mnt/hfs-data/hfs2-shard81-async-gate-final20260918`，报告见
`doc/performance/hfs2_shard81_formal_gate_20260918.md`。

### 1.2 已有诊断证据

当前版本的短时 trace on/off 探针结果：

| 模式 | IOPS | CPU | IO 数 | errors |
|---|---:|---:|---:|---:|
| trace off | 74233.813 | 3.422% | 371466 | 0 |
| trace on | 72617.629 | 3.342% | 363306 | 0 |

trace on 相对下降约 2.18%，但没有生成任何 `trace-*` 文件。

当前 `strace -f -c` 短跑完成 65536 次 IO，主要结果为：

| syscall | 次数 | 观察 |
|---|---:|---|
| `poll` | 66028 | 与每 IO 一次 frontend 等待高度吻合，占累计 syscall 时间 81.97% |
| `write` | 65668 | 与 eventfd producer wake 高度吻合 |
| `read` | 66224 | 与 eventfd consumer drain 高度吻合 |
| `eventfd2` | 32 | 32 个 FIO worker 各自创建 notifier |
| `gettid` | 65582 | 基本每个 IO 一次 actor-local 线程校验 |

closure 中 `scheduler_wait` 仅约 97~126 次/约 60 万次 IO，`plan_fail`、`dma_fail`、
`preparing` 为 0，`child_submit == child_complete`。因此当前第一嫌疑是 frontend
completion wait 和跨 reactor handoff，不是 scheduler、DMA 或 NVMe 错误。

### 1.3 Shard-86 前的 trace 缺口与当前落地状态

Shard-86 前的 `src/common/trace.cpp` 只做计数：`event()` 丢弃 `a0-a3`，使用全局
原子计数，不记录 TSC、source、request id 或 status，也不创建 trace 文件；当前 async
路径的 `FE_SUBMIT/FE_REAP`、`AsyncCompletionQueue` 和 actor-local bdev 也没有完整
producer。该历史缺口是本计划启动的原因。

Shard-86 已补齐最小可用链路：固定 48B record、每 producer 一个 SPSC ring、独立后台
writer、`trace-manifest.json`、sample/drop 统计，并在 Client、AsyncCompletionQueue、
CompletionNotifier 和 `SpdkRuntime` 的现行 async 路径补埋点。`tools/hfs_trace_dump`
可离线统计各 `trace-*.bin` 的文件数、record 数和 event 计数；不在线格式化，也不参与
I/O 控制流。仍未覆盖的 `Retry/BusyReject/Checkpoint/Recovery` producer 留给后续故障
与生命周期 shard，不把不存在的事件伪装成已完成。

---

## 2. 优先级与不变式

### 2.1 优先级

```text
P0 真实 trace 与分段延迟
P0 completion notifier/eventfd 和跨 reactor handoff
P0 FIO 多 worker 的 Client/completion 拓扑
P1 shared-file 单 shard 与多 reactor 利用率
P1 data reactor poller 热循环和线程校验
P2 Client/FIO 小额管理开销
P0 最终 raw/HFS gate 与回归归档
```

### 2.2 所有 shard 必须保持的约束

1. `spdk_submit == spdk_complete`，任何 callback 都不能提前释放 `DataIoRequest`、
   `IoHandle`、FIO buffer 或 coroutine frame。
2. 不改变 `Client::submitRead/submitWrite` 的接受语义；publish 后的错误必须成为唯一
   terminal completion，不能伪装成 `FIO_Q_BUSY`。
3. 不恢复 POSIX preload、fd wrapper、FUSE、Blobstore 或旧 async ABI。
4. 不把 Malloc/Aio 作为正式 NVMe 结论；正式测试始终使用 `.conf` 和真实 BDF。
5. 性能优化不能删除 inode scheduler、extent snapshot、DMA ownership 或 close/drain
   边界；如果需要改变 file-to-shard 规则，必须先更新 `doc/design/hfs2.0.md`。
6. trace 默认关闭；正式 gate 必须 trace/stats off。
7. 每个 shard 单独提交、单独 VM 回归；失败最多两轮修复，之后保留证据并报告阻塞。

---

## 3. Shard 总览

| shard | 目标 | 主要目录/文件边界 | 依赖 | 交付判定 |
|---|---|---|---|---|
| 86 | 可用的请求级 trace | `src/common/trace.*`、现行 async callsites、`tools/hfs_trace_dump`、trace tests | 无 | 可输出完整链路记录，trace off 无热路径分配 |
| 87 | completion notifier batching | `src/core/async_completion_queue.*`、queue tests | 86 的事件定义可先并行设计，集成依赖 86 | 降低 wake/poll 频率且 lifecycle 矩阵全过 |
| 88 | FIO 多 worker / aggregate completion | `fio/`、FIO 专用 tests | 86、87 | 32 jobs 有效 outstanding 保持 32，完成通知可批量化 |
| 89 | file shard / reactor 利用率 | `src/core/async_data_service.*`、`src/core/scheduler.*` | 86、88 的 trace 结果 | 不破坏 inode ordering，确认或修复单 shard 热点 |
| 90 | reactor 热循环 | `src/runtime/spdk_runtime.*`、runtime tests | 86、89 | 移除每 IO syscall 和每轮无必要锁，stop/clear 无 UAF |
| 91 | Client/FIO 低风险微优化 | `src/core/client.*`、`include/hfs/core.hpp`、client tests | 87、88、90 | O(1) slot/reap 或等价优化，无 API 语义回退 |
| 92 | 正式 gate 与归档 | `doc/performance`、`doc/review`、VM scripts | 86~91 | raw/HFS 全门禁通过，或明确保留阻塞 |

Shard-86~91 不得单独宣布 Shard-81 完成；只有 Shard-92 的正式 gate 全部满足，
才可以关闭性能阻塞。

---

## 4. 各 shard 设计

### Shard-86：真实 tracepoint 与离线归因基础

#### 目标

把当前“只计数、不落盘”的占位 trace 改成可关联的、低侵入的请求级 trace。只增加
观测能力，不改变 completion、调度和 reactor 行为。

#### 允许修改的范围

- `src/common/trace.hpp`
- `src/common/trace.cpp`
- 现行 async 观测插点：`src/core/client.cpp`、`src/core/async_completion_queue.cpp`、
  `src/core/engine.cpp`、`src/runtime/spdk_runtime.cpp`；
- 新增 `tools/hfs_trace_dump.*` 或等价离线解析工具；
- 新增 trace 专用测试。

`CMakeLists.txt` 由主线程串行集成；本 shard 的构建改动只增加 trace 工具/测试目标，
不改变既有 target 的源文件边界。

#### 记录格式

沿用 `doc/design/hfs2.0.md` §16.5 的固定标量 record，至少包含：

```text
event_id, format_version, reactor_or_client_id, tsc,
request_id, arg0, arg1, arg2, arg3
```

必须支持 wall-clock 校准点，离线工具能合并多 reactor/client 文件，并输出：

- 每个 event 的计数和 drop 数；
- `FE_SUBMIT → RING_RESERVE → ACTOR_BATCH → SCHED_ADMIT`；
- `DMA_ACQUIRE → ASYNC_BDEV_SUBMIT → ASYNC_BDEV_COMPLETE`；
- `PARENT_COMPLETE → CQ_PUSH → CQ_REAP/FE_REAP`；
- eventfd notify、wait、read 和 active-spin 次数。

高频点使用 per-ring sample counter；不能用一个全局 `fetch_add` 作为所有线程的采样
序列。trace off 时不得做 per-event allocation、格式化、锁或全局 atomic RMW。

#### 验收

- trace on 短跑生成 active producer 的 `trace-*.bin`，manifest 在 stop 后标记 closed；
- trace record 的 request id 能闭合 submit/complete/reap；
- 人为制造 backend error、busy、timeout 时能看到对应事件；
- trace ring 满时只增加 `traceDropped`，不阻塞 I/O；
- trace off 跑结构测试时不创建 session、不落盘，代码路径不分配；
- trace off 短测相对当前基线不得出现超过 2% 的稳定回退；
- 真实 NVMe 4K randread 短跑 errors=0、queued=reaped、outstanding=0。

#### 失败边界

如果无法在不增加热路径全局原子的情况下落盘，保留旧 trace stub 作为回退，
不得为了 trace 牺牲正式 gate；性能优化 shard 不得基于没有请求级时间线的猜测继续扩展。

---

### Shard-87：completion notifier batching 和等待策略

#### 目标

减少当前每个浅队列 IO 都经过 `eventfd write → poll → read` 的成本，同时保持
`Client::poll(min,max,timeout)` 的精确语义和 close/drain 安全。

#### 允许修改的范围

- `src/core/async_completion_queue.hpp`
- `src/core/async_completion_queue.cpp`
- `tests/async_completion_queue_test.cpp`

不修改 FIO job ownership、`Client` public API 或 SPDK backend；这些属于 Shard-88/90。

#### 方案候选

按以下顺序验证，不允许一次合并多个未验证机制：

1. **level-triggered notifier**：只在 ring 从 empty 变成 non-empty 时通知；已经有
   wake arm 的实现必须通过 trace 验证实际 transition 次数。
2. **adaptive active poll**：根据最近 completion 延迟动态选择短 spin 或直接等待；
   spin 有上限，不得无限忙等。
3. **completion batch drain**：一次 `poll()` 优先 drain 所有可用 shard，避免每个
   completion 重新进入 notifier。
4. 如仍不足，再评估 shared notifier 或 dispatcher；不得在本 shard 直接引入 FIO
   多线程共享 Client。

#### 验收

固定比较：单 Client `iodepth=32`、32 Client `iodepth=1`、`min=1` 和 `min=8`。
必须报告：

- `notify_write/op`、`wait/poll/op`、`notify_read/op`；
- active-spin 次数和等待时间；
- ring high watermark、wake transitions、overflow；
- p50/p99 completion-to-reap；
- CPU、IOPS、errors 和 closure。

功能门禁：所有 async completion queue 单测、真实 NVMe file API、FIO cleanup/late
completion 全通过。若 q1 workload 的 syscall 仍接近一 IO 一次，必须把结果交给
Shard-88，不能声称 notifier 已解决瓶颈。

回滚开关保留现有 eventfd wait 路径；任何 timeout/min/max 语义变化都判定失败。

---

### Shard-88：FIO 多 worker 的 Client/completion 拓扑

#### 目标

解决当前 32 jobs 实际是 32 个独立 Client、32 个 eventfd 的拓扑问题，同时保持 FIO
线程安全、slot ABA 防护和每个 job 的 `io_u` 所有权。

#### 允许修改的范围

- `fio/fio_hfs.cpp`
- 新增 `fio/` 下的 FIO completion hub/dispatcher 文件；
- FIO adapter 专用测试和测试脚本。

Core `Client` API 如需扩展，先由主线程建立接口变更单，再由 Shard-91 实现，避免
本 shard 和 Shard-91 同时修改 `src/core/client.*`。

#### 设计约束

- FIO 的 process-wide `Engine` 继续共享；
- 不能让多个 frontend thread 无锁调用当前只允许创建线程使用的 `Client`；
- completion hub 只能传 value token、request id、status 和 completed bytes；
- 不能把 `io_u*`、`thread_data*` 或 `FioHfsPending*` 传入 data reactor；
- aggregate effective outstanding 必须仍由 closure 证明为 32；
- 同一 job 的 `queue/getevents/event` 顺序必须保持，不能让别的 job 消费其 `io_u`。

#### 实验顺序

1. 32 jobs × q1 与 1 job × q32，保持总有效 outstanding=32；
2. 每 job notifier 与 process-level dispatcher 的 syscall/latency 对照；
3. completion batch size 为 1、2、4、8、32；
4. shared-file randrw 与只读 randread 分别测试，排除写冲突影响。

#### 验收

- `queued == reaped`、`outstanding == 0`、`token_mismatch == 0`；
- FIO `event()` 之后才释放 pending slot；
- cleanup 超时仍保留全部 buffer、handle 和 callback context；
- 32 jobs 的 aggregate peak 恰为 32，不把每个 Client 的 depth 相加成虚假 QD；
- 相对 Shard-81 当前 66538.784 IOPS 基线取得可重复提升；中间目标为达到 raw
  中位数的 75%（约 81297 IOPS），否则将结果标为未解决并进入 Shard-89/90 归因；
- 不改变同步 bring-up 模式 `hfs_async=0`。

如果共享 completion hub 导致 frontend 争用或 wake-up storm，立即回退到每 job queue，
保留 trace 结果，不以降低 CPU 但增加 p99 的方式判定成功。

---

### Shard-89：file shard affinity 与多 reactor 利用率

#### 目标

确认 shared-file workload 是否把全部请求固定在一个 data shard；如确认，设计在不
破坏 inode dependency scheduler 的前提下提升 data reactor 利用率。

#### 允许修改的范围

- `src/core/async_data_service.hpp/.cpp`
- `src/core/scheduler.hpp/.cpp`
- 对应 scheduler/data service 测试。

不得直接修改 runtime、FIO completion 或公共 API。

#### 先测量，后实现

trace 必须输出：

- file id → logical shard；
- 每 shard ring reserve/doorbell/actor batch；
- 每 shard scheduler active/pending/wait；
- 每 shard backend submit/complete；
- data reactor busy/idle 时间。

至少比较：

```text
data_reactors=1 vs 2
data_cpus=1,2 vs auto
shared-file vs 多 file id
read-only vs randrw
```

#### 候选实现边界

当前“同一 file 固定到一个 shard”是架构不变式，不能直接改成按请求随机分发。若
证据证明单 shard 是瓶颈，只允许在事实源批准后选择以下一种：

1. 保持逻辑 scheduler 单 owner，但将已 admission、互不重叠的 read child 分发到
   多个 actor-local backend；
2. 引入 file execution sub-shard，写入、重叠范围和 barrier 仍由 owner shard 串行化；
3. 保持 file pinning，只优化同一 shard 的 actor batch、backend batch 和 completion
   drain，不改变一致性边界。

#### 验收

- 同文件 overlapping read/write、barrier、取消和 late completion 语义不变；
- 同一 file 的 scheduler ordering 测试全部通过；
- 每个 active data reactor 的 submit/complete 和 busy ratio 可见；
- 不因“看起来有两个 reactor”而把未使用的 reactor 计入性能收益；
- 如果单 shard 不是主要瓶颈，保留现有 file affinity，不做无收益的架构改动。

---

### Shard-90：data reactor 热循环与线程校验

#### 目标

降低 `spdk_thread_poll()` 后每轮执行的 poller mutex，以及每个 child submit 的
`SYS_gettid` 校验开销，同时保留 poller clear 的 quiescence 和 UAF 防护。

#### 允许修改的范围

- `src/runtime/spdk_runtime.hpp`
- `src/runtime/spdk_runtime.cpp`
- `tests/spdk_runtime_test.cpp`

#### 方案

1. 将正常运行态的 poller callback/context 改成 atomic snapshot 或 reactor-owned
   状态；stop/clear 时进入显式 quiescent state，只在卸载阶段等待 in-flight callback。
2. 把 `SYS_gettid` 校验移到 reactor 建立/调试校验路径；热路径使用已确认的
   actor-local fast path。若保留安全检查，必须提供可测量的 debug/strict 模式。
3. 不在 reactor loop 中引入 `std::function`、动态分配或跨线程锁。

#### 验收

- `setDataReactorPoller/clearDataReactorPoller` 并发清理测试无 UAF、死锁或 callback
  访问已释放 context；
- mount/drain/stop/restart/engine destructor 全部通过；
- 正常 async workload 中 `gettid` 次数显著下降或为 0；
- reactor busy/idle 和 poller callback 次数由 Shard-86 trace 可见；
- 真实 NVMe 32 jobs q1 不出现 p99 回退或 close 泄漏。

失败时保留旧 mutex/strict tid-check 路径作为默认回退，不能为了吞吐删除生命周期同步。

---

### Shard-91：Client/FIO 低风险微优化

#### 目标

在 P0/P1 完成后处理仍然由 trace 证明存在的前端管理开销，不凭代码直觉提前优化。

#### 允许修改的范围

- `include/hfs/core.hpp`
- `src/core/client.cpp`
- `tests/file_api_test.cpp` 或对应 client tests

FIO adapter 的拓扑改动已经在 Shard-88 完成；本 shard 不再改 `fio/fio_hfs.cpp`。

#### 候选项

- `reserveSlot()` / `reapEvent()` 从线性扫描改成 bounded free-list 或 requestId 直接
  索引；generation 校验必须保持；
- 只在 trace 证明仍有成本时，优化 `AsyncFileSnapshot` 的共享引用和 epoch cache；
- 审计 `shared_ptr`、mutex 和 per-request extent snapshot 的引用次数；
- 不删除 metadata epoch race retry，也不重新引入每 IO `stat + extentMapFor`。

#### 验收

- slot ABA、重复 completion、错误 token、close Busy 和 late completion 测试全通过；
- API 行为与 `doc/design/hfs2.0.md` §13/§14 完全一致；
- microbenchmark 报告每 IO slot 操作、snapshot 和 completion reaping 成本；
- 只有在 p99/IOPS 有可重复改善时才保留改动，否则回退，避免增加复杂度。

---

### Shard-92：正式 raw/HFS gate、回归与归档

#### 前置条件

Shard-86~91 的实现、测试和 trace 归因报告全部完成；任何一个 shard 有未解释的
completion mismatch、overflow、UAF、错误映射或设备释放失败，都不能进入正式 gate。

#### 固定命令口径

- Windows 工作区先同步 HFS 自身源码、脚本和文档；不上传或覆盖 VM 内 SPDK；
- VM 确认 BDF `0000:0b:00.0` 为 `uio_pci_generic`，`vm.nr_hugepages=1024`；
- raw 与 HFS 使用同一设备、同一 CPU placement、同一 block size/有效 outstanding；
- 正式 gate trace/stats off，五次取 median；
- 大体积结果写 `/mnt/hfs-data`，不写满 `/var/tmp`。

#### 必须归档

1. raw/HFS 五次 JSON、stderr、closure、driver/hugepages 状态；
2. trace on 短跑的 manifest、每 shard 文件和 drop 统计；
3. per-stage p50/p99、eventfd/syscall、ring/actor/scheduler/DMA/backend 统计；
4. 32 jobs q1、1 job q32、不同 reactor 数和 shared/multi-file 对照；
5. `ctest`、FIO 功能矩阵以及 Shard-79/83/84 错误/超时/取消回归。

#### 最终 gate

只有同时满足以下条件，才能将性能阻塞关闭：

```text
HFS/raw >= 85%
read/write p99 <= raw p99 * 110%
errors == 0
queued == reaped
accepted == terminal == published
child_submit == child_complete
outstanding == 0
token_mismatch == 0
trace/stats off 的正式结果可复现
```

否则报告实际比例、剩余 trace 证据和下一阻塞点，保持 Shard-81 未完成。

---

## 5. 测试矩阵与停止条件

### 5.1 每个代码 shard 的最小验证

```text
host/unit:      ctest --test-dir build-async --output-on-failure
real-NVMe:      config/hfs_nvme.conf / Nvme0n1 / 0000:0b:00.0
functional:     read, write, randread, randrw, direct, bounce, verify
lifecycle:      timeout wait, cleanup drain, late completion, close Busy
observability:  trace off / trace sample=1 / trace sample=N / trace drop
```

任何测试失败都要记录：命令、配置、BDF、driver、hugepages、closure、日志路径和是否
改变了设备内容。不得用一次成功 smoke 覆盖失败的矩阵项。

### 5.2 停止条件

立即停止当前 shard 并回退的条件：

- completion 数量不闭合或出现 token mismatch/overflow；
- `spdk_submit != spdk_complete`；
- trace on 改变请求顺序、生命周期或引入 UAF/double resume；
- CPU 降低但 p99 超过 gate，或 IOPS 提升来自降低有效 outstanding；
- 共享 file 的写入顺序、barrier、取消语义发生变化；
- 设备未释放、SPDK 绑定状态改变或 hugepage 预检失败。

### 5.3 Git/协作边界

- 主线程负责方案、共享 `CMakeLists.txt`、`doc/design/hfs2.0.md`、最终 review、
  完整测试和集成提交；
- 每个实现 shard 只修改表中指定目录，不创建下级 Agent，不触碰另一个 shard 的文件；
- `CMakeLists.txt` 和事实源文档由主线程串行集成，避免两个 Agent 同时修改；
- 每个 shard 完成后单独提交并推送，提交信息使用 `feat(perf): complete shard-XX ...`；
- 用户已有删除或未跟踪文件不得被 `git add -A` 覆盖；
- 任一 shard 最多进行两轮修复，之后保留失败证据并报告阻塞原因。

---

## 6. 预期结果

### 6.1 Shard-86 完成记录

- VM `cmake --build build-async` 成功，`ctest` 22/22 通过；新增 `trace_test` 和
  `hfs_trace_dump` target 均通过编译。
- 真实 NVMe `Nvme0n1`、32 jobs、iodepth=1、trace sample=1 短跑：
  `queued=169943`、`reaped=169943`、`outstanding=0`、`token_mismatch=0`、
  `completed_error=0`、`peak=32`。
- trace manifest：`recorded=2876086`、`dropped=0`、`closed=true`；离线工具统计的
  binary record 数与 manifest recorded 一致，覆盖 FE/CQ/ring/actor/scheduler/DMA/
  bdev/parent/notify/poll 事件。
- trace off 的既有 32-worker real-NVMe smoke 仍保持 `queued=reaped`、
  `outstanding=0`、`errors=0`；正式性能 gate 尚未重跑，不能据此关闭 Shard-81 性能阻塞。

本计划的第一目标不是立即把 IOPS 拉到 85%，而是把当前“约 61% 且不知道时间花在哪”
变成可重复的分段证据。预期顺序为：

```text
真实 trace
  → 确认 eventfd/cross-reactor/shard 利用率占比
  → completion batching 与 FIO topology 修复
  → reactor 热循环和低风险微优化
  → trace off 五次正式 raw/HFS gate
```

如果在 Shard-88 之后仍低于中间目标，不能继续堆叠微优化，应回到 Shard-89 重新判断
“单文件单 shard”和“FIO 多 Client”是否与当前架构目标冲突。任何未通过正式 gate 的
结果都只能作为性能诊断证据，不能标记 HFS 2.0 性能完成。
