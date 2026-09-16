# Shard-67：HFS CPU 效率优化开发计划

> **状态**：开发计划，尚未开始实现
>
> **日期**：2026-09-16
>
> **设计依据**：[`doc/design/hfs_spdk_cpu_efficiency_optimization_design.md`](../design/hfs_spdk_cpu_efficiency_optimization_design.md)
>
> **适用架构**：POSIX-only `LD_PRELOAD` HFS，VM 真实 NVMe `Nvme0n1`
>
> **一句话目标**：在不改变 POSIX、ordering、durability 和 crash-recovery 语义的前提下，把 HFS 的 IOPS/CPU 提升到可部署水平。

## 1. 结论与推荐实现版本

本 shard 采用“先减少空转，再减少批量 bookkeeping，最后按证据处理 completion”的顺序。推荐初始版本如下：

1. 保持按 CPU 自动选择的 `owner + data reactor` 拓扑；8 vCPU 使用 `total=3, data=2`。
2. 应用线程继续默认使用 futex；`HFS_WAIT_MODE=adaptive` 只能作为 gated A/B 开关。
3. 优先实现可唤醒的 reactor adaptive idle：有队列或 backend inflight 时继续 poll，完全空闲时进入有界等待，新 batch、stop 或 timer 到期时唤醒。
4. 第二步把 per-request queue/inflight 更新改为 per-batch credit accounting，减少 `batchLock`、cacheline 抖动和重复唤醒。
5. completion wake、单 data reactor、staging/pool、ticket/fd/metadata 只在本计划规定的证据门槛满足后立项。

本计划不是把更多 reactor 加入默认路径，也不是恢复已删除的 async/DMA ABI。

## 2. 证据基线

### 2.1 已有任务结论

| 来源 | 已确认事实 | 对本 shard 的影响 |
|---|---|---|
| `task90150022` | T6-T7（SPDK submit→callback）和 T8-T9（wake→应用恢复）是主要长段；copy 不是第一热点 | 先处理 reactor/completion 边界，不先重写 DMA |
| `task09150832` | 2 data reactor 相比 4 data reactor，读约 +24%、写约 +33%，CPU 同时下降；jobs=16 延迟更稳 | 维持自动 `owner+2 data`，避免 oversubscription |
| `task09151930` | raw Blobstore 读的最佳点为 2 channel，4 channel 无线性收益；请求分布约 53.5%/46.5% | 不盲目增加 reactor/channel；先做 wake/queue 成本 |
| `task09160035` | 旧 5-total-reactor 首个 4K/32-job 样本仅 58,277.97 IOPS、595% CPU，触发 70K 停止线 | 五 reactor 不得作为默认基线 |
| `task09160854` | 8 vCPU 自动拓扑为 `total=3, data=2`，正确性和短 FIO 通过 | 自动匹配保留，正式矩阵仍需 clean rerun |

### 2.2 当前进程级 CPU 对照

以下是 2026-09-16 在 VM 真实 NVMe 上的单次 5 秒诊断样本，不替换正式性能基线：

| 块大小 | HFS IOPS | raw SPDK IOPS | HFS/raw | HFS 进程 CPU | raw SPDK CPU | HFS IOPS/核 |
|---:|---:|---:|---:|---:|---:|---:|
| 4K | 94,527 | 118,589 | 79.7% | 394% | 98% | 23,992 |
| 8K | 85,998 | 107,673 | 79.9% | 432% | 98% | 19,907 |
| 16K | 76,422 | 91,907 | 83.2% | 433% | 98% | 17,649 |
| 32K | 63,113 | 73,072 | 86.4% | 387% | 98% | 16,308 |
| 64K | 46,863 | 51,462 | 91.1% | 376% | 98% | 12,464 |

口径：8 vCPU；HFS 为完整 POSIX preload、32 个 `psync` job、自动 `owner+2 data`；raw SPDK 为单 FIO job/QD32；CPU 为 `/usr/bin/time` 的进程级 user+sys，包含 HFS 后台线程。FIO JSON 中的 `usr_cpu/sys_cpu` 只代表 FIO worker，不能用于本 shard 的总 CPU 门禁。

证据目录为 VM `/mnt/hfs-data/hfs-cpu-process-20260916-015757`。64K raw SPDK 的退出清理阶段曾出现残留并被终止，虽然 JSON/time 为 `error=0`，该行只用于诊断；正式矩阵必须干净重跑并释放 NVMe。

## 3. 范围、约束与不做事项

### 3.1 必须保持

- 完整 POSIX preload 是唯一对外入口；不新增或恢复公开 async、registered-DMA、DMA allocation ABI。
- `ticket/range` ordering、`fsync/fdatasync`、dirty epoch、metadata transaction 和 crash recovery 语义不变。
- 正式性能验证固定使用 VM 内真实 NVMe：`config/hfs_nvme.json`、`HFS_BDEV_NAME=Nvme0n1`、PCI BDF `0000:0b:00.0`，并确认 `uio_pci_generic` 与 `vm.nr_hugepages=1024`。
- 正式吞吐 trace-off/stats-off；trace-on 只用于归因。结果必须满足 `errors=0`、`spdk_submit=spdk_complete`、credit/lease/parent-child 闭合。
- 任何测试完成后将 NVMe 释放回内核 `nvme` 驱动，并确认无残留 FIO/SPDK 进程。

### 3.2 本 shard 不做

- 不增加默认 reactor 数，不把 pure spin 或 adaptive spin 设为默认。
- 不先重构 ticket scheduler、fd/open-file、metadata 或 durability。
- 不因“direct=0”改 allocator；uio+hugepage 下普通应用 buffer 不具备 `spdk_vtophys` 直通条件。
- 不通过扩大 queue、放宽语义或切换 Malloc/Aio 后端制造性能收益。

## 4. 子 shard 与实施顺序

每个子阶段只引入一个主要变量，拥有独立开关、A/B 结果和回滚点。阶段未通过不得进入下一阶段。

### 67A：测量合同与 clean baseline

**目标**：让 CPU 结论可复现，避免把 FIO worker CPU 当作 HFS 总 CPU。

**工作项**：

- 增加或收口统一 harness，记录 `/usr/bin/time` 的 elapsed、user、sys、CPU%，以及 `/proc/<pid>/task/*/stat` 或 `pidstat -t` 的 owner/data/FIO worker CPU。
- 每 200 ms 记录在线 CPU、run queue、context switch/s 和 CPU 频率；输出 `IOPS/(CPU%/100)`。
- 正式样本 trace-off/stats-off，归因样本单独 trace-on；不覆盖既有正式 baseline。
- 修复正式 harness 的退出清理顺序，确保 raw SPDK 64K 等大块样本无残留后再收集 release 状态。

**交付物**：`scripts/db/hfs_cpu_efficiency_benchmark.sh`、`scripts/db/hfs_cpu_efficiency_summary.py`（若现有脚本已覆盖功能则补齐，不重复造轮子），以及 `/mnt/hfs-data/<run>/process-time.tsv`、`thread-cpu.tsv`、`summary.tsv`。

**门禁**：同一提交、VM 工作树干净、真实 NVMe、3 次 5 秒快速 A/B 均可复现；FIO `error=0`，释放状态正确。

### 67B：owner 可唤醒 idle

**目标**：回收 owner 在无数据任务时持续调用 `spdk_thread_poll()` 消耗的 CPU。

**实现要点**：

- owner 状态为 `ACTIVE → QUIET → PARKED`；只有队列、inflight 和待处理控制消息都为空时才允许 park。
- 使用独立的 `wakeSequence`（独占 cacheline）配合 futex/condition timed wait；入睡前读取 sequence、重新检查条件，再执行 wait，避免 lost wake。
- 新 batch、stop、channel 初始化/释放和 direct-message fallback 都递增 sequence 并唤醒 owner；不为每个请求发 wake syscall。
- owner 使用有界 timeout 定期服务 SPDK timer/poller；默认保留 busy 行为，通过 `HFS_REACTOR_WAIT_MODE=adaptive` 显式启用。
- 复用 `HFS_REACTOR_IDLE_BACKOFF_US` 作为最大等待上限，初始测试 5/10/20/50 us。

**门禁**：4K randrw/32-job 进程 CPU 下降至少 15%，IOPS 回退不超过 2%，p99 回退不超过 10%；不满足则默认保持 busy poll，仅保留实验开关。

### 67C：data reactor 可唤醒 park

**目标**：只在 data reactor 没有 backend inflight 时减少空转，不牺牲 completion latency。

**实现要点**：

- `queueDepth != 0`、`inflight != 0` 或本轮 poll 有事件时保持 `ACTIVE`；`inflight != 0` 时禁止 `PARKED`。
- producer 在 batch 从未调度变为已调度时递增 sequence 并唤醒对应 data reactor；consumer 以 batch 为单位检查队列。
- stop 和 shutdown drain 必须唤醒所有 reactor；wait 返回后先处理 stop，再恢复 poll。
- `HFS_REACTOR_WAIT_MODE=busy` 是无行为变化的回退；adaptive 的 timeout 不超过 50 us，避免长尾被睡眠放大。

**门禁**：在 67B 基础上进程 CPU 累计下降至少 20%，IOPS 回退不超过 3%，p99 回退不超过 10%；持续 QD32 若无收益也必须记录，不得据空闲 workload 外推。

### 67D：per-reactor batch credit accounting

**目标**：消除逐请求 `batchLock`、credit 更新和条件变量广播。

**实现要点**：

1. consumer 一次获取 `batchLock` 并 detach 整个 batch。
2. 在同一临界区执行 `queueDepth -= batchSize`、`inflight += batchSize`，保持 bounded queue 语义。
3. completion 使用原子 credit 归还；仅在 credit 从“满”变为“可用”时 signal producer。
4. 保留 direct-submit fallback，禁止恢复多生产者无序向 SPDK ring 提交（bug #76 的 reservation stall 风险仍有效）。
5. shutdown 时 drain queue、inflight、lease 和 waiter，所有计数在退出前闭合。

**门禁**：相对 67C CPU 再下降 8%～10%，T4-T5 p99 不增加，IOPS 不下降；stats 中 queue/inflight/submit/complete 全部闭合。

### 67E：completion wake（证据门控）

**启动条件**：仅当 perf/syscall 证实 futex wake 或 context switch 占总 CPU ≥5%，且能覆盖 completion-before-wait、wait-before-completion、timeout、shutdown 四类竞态。

**候选实现**：completion state 增加 `DONE/WAITER_ARMED/TIMED_OUT` 状态，只有 waiter 已 armed 时才 wake；保持一次 release publish 和现有 futex 回退。

**门禁**：16-job 与 32-job 各 5 次 median，CPU 或 p99 至少改善 5%，且无 correctness 回归。否则关闭该方向，`HFS_WAIT_MODE=futex` 保持默认。

### 67F：可选拓扑、staging 与 metadata 微优化

这些不是本 shard 的默认动作，只能由 67A～67E 的新证据触发：

- **单 data reactor A/B**：只比较 `owner+1` 与 `owner+2`；只有吞吐保留 ≥90% 且 CPU/IOPS 改善 ≥15% 才面向小 CPU/低 QD 主机提供推荐，不动态迁移 stripe。
- **staging/pool**：仅当 16K/64K 的 lease wait >3% 或 copy CPU >5% 时，按 queue depth 调整 class 计数/批量大小；不引入注册 buffer ABI。
- **ticket/fd/metadata**：仅当新 trace 显示对应段超过总延迟 10%（ticket 另用 5% 门槛）时建立独立子任务；否则在结果中明确关闭方向。

## 5. 运行时设计不变量

实现必须满足以下不变量，并在代码 review 和 trace 中逐项验证：

| 不变量 | 验证方式 |
|---|---|
| `inflight>0` 的 data reactor 不得 park | thread trace、runtime stats、压力测试 |
| queue detach 与 credit 增减成批且守恒 | queue/inflight/submit/complete 计数闭合 |
| producer 唤醒不会丢失 | sequence 单调性 + wait 前后二次条件检查 |
| stop 能唤醒并排空所有 reactor | shutdown stress、超时与残留线程检查 |
| completion 发布先于 waiter 观察 | acquire/release 审查 + repeated race smoke |
| ordering/durability 不受优化影响 | ticket、fsync、crash recovery、数据库 smoke |
| busy 模式可完全回退 | `HFS_REACTOR_WAIT_MODE=busy` A/B 与默认行为对照 |

## 6. 文件与代码边界

预期修改范围如下，实际以阶段证据为准：

- `src/hfs_runtime.c`、`src/hfs_runtime.h`：owner/data 状态机、wake sequence、batch detach/credit、等待与 stop 路径。
- `src/hfs_stats.c`、`src/hfs_stats.h`：park/wake、queue/inflight、线程 CPU 及计数闭合字段。
- completion 所在的现有头/源文件：仅在 67E 门禁满足时调整状态机；不改变外部 POSIX ABI。
- `scripts/db/`：快速 A/B、线程 CPU 采样、summary 和 NVMe release 检查。
- `doc/design/`、`doc/performance/`、`doc/task_result/`：只记录同口径证据，不把短诊断样本升级为正式基线。
- `doc/bug.md`：发现或修复真实 bug 时立即记录现象、排查、根因和修复；纯性能假设不登记为 bug。

## 7. 验证矩阵

### 7.1 快速门禁

每个子阶段先运行真实 NVMe 的短矩阵：

| workload | block size | 并发 | runtime | 重复 |
|---|---:|---:|---:|---:|
| randrw 50/50 | 4K | HFS 32 / raw SPDK QD32 | 5 s | 3 |
| randread | 4K | HFS 16、32 | 5 s | 3 |
| randwrite | 4K | HFS 16、32 | 5 s | 3 |

优先比较同一 VM、同一提交、同一文件口径的 median；任何 correctness、release 或门禁失败立即停止该阶段。

### 7.2 正式矩阵

阶段门禁通过后才运行 5 次 median：4K/8K/16K/32K/64K，randrw 50/50，HFS 32 jobs，raw SPDK QD32，10 s ramp + 30 s runtime；另报 jobs=16 的吞吐/尾延迟。正式结果必须包含 IOPS、avg/p99、进程 CPU、线程 CPU、runq、ctx/s、IOPS/核和 HFS/raw 比值，并将 shared-file 与 per-job-file 分开。

### 7.3 正确性与资源释放

每个候选版本至少执行：

- `make -B app lib` 与 CMake Release；
- `scripts/db/posix_contract_test.sh`、`io_fast_path_smoke.sh`、queue/ticket、metadata、fd、durability 和 runtime stress；
- `scripts/db/io_trace_smoke.sh`，检查 `trace_dropped=0`、parent/child 闭合和 `spdk_submit=spdk_complete`；
- MariaDB/RocksDB 缩短 smoke、正常重启和 SIGKILL recovery；
- 测试结束执行 NVMe release 检查：内核驱动为 `nvme`、`/dev/nvme0n1` 存在、无 FIO/SPDK 残留。

正式结论不得使用 Malloc/Aio 替代真实 NVMe，也不得在设备未绑定或 hugepages 未满足时输出。

## 8. 性能目标与停止条件

目标分两层，避免用一次短样本过度承诺：

| 指标 | 67B～67D 阶段目标 | 收口目标 |
|---|---:|---:|
| 4K randrw/32-job HFS IOPS | ≥当前同口径 97%（约 91.7K） | median ≥90K |
| 4K randrw/32-job 进程 CPU | ≤320% | ≤300%，stretch ≤250% |
| 4K IOPS/核 | 相对当前提升 ≥20% | ≥30K |
| HFS/raw 吞吐比 | 不低于当前同日对照 | 4K～64K 不低于当前对照的 95% |
| p99 | 回退 ≤10% | jobs=16/32 均无显著回退 |

任一条件触发即回滚对应子阶段并保留实验开关：

1. IOPS 提升不足 2% 且 CPU 或代码复杂度上升。
2. CPU 增加超过 10% 而 IOPS 提升不足 5%。
3. adaptive park 造成 p99 增加超过 10%、shutdown 超时或 lost wake。
4. 任一 credit、DMA lease、parent/child、submit/complete 不闭合。
5. correctness、durability、数据库 recovery 或 NVMe release 失败。

## 9. 提交、回滚与交付边界

建议提交序列：

1. `docs/test: define CPU measurement contract`（67A）
2. `feat(runtime): add owner adaptive idle`（67B）
3. `feat(runtime): wake and park data reactors`（67C）
4. `perf(runtime): batch reactor credit accounting`（67D）
5. `perf(runtime): gate completion wake optimization`（仅 67E 通过时）
6. `docs(perf): publish shard-67 NVMe results`（正式矩阵收口）

每个提交都必须在 VM 强制构建和对应回归后合入 `main`，保留独立环境变量回退路径。任何阶段失败时只回滚该提交，不覆盖既有正式基线和历史结果。

## 10. 当前待办

- [ ] 在当前 `main` 提交上同步 VM HFS 源码，确认 VM 工作树干净。
- [ ] 完成 67A clean baseline，并补齐进程/线程 CPU 采样。
- [ ] 实现 owner adaptive idle，完成 67B 快速门禁。
- [ ] 实现 data reactor wake/park，完成 67C 快速门禁。
- [ ] 实现 per-batch credit accounting，完成 67D 快速门禁。
- [ ] 依据 syscall/perf 证据决定是否启动 67E。
- [ ] 通过正式矩阵后再更新 performance baseline；未通过则保留本计划和完整回滚记录。

## 11. 参考资料

- [`hfs_spdk_cpu_efficiency_optimization_design.md`](../design/hfs_spdk_cpu_efficiency_optimization_design.md)
- [`hfs_posix_sync_boundary_topology_shard_plan.md`](hfs_posix_sync_boundary_topology_shard_plan.md)
- [`shard_progress_memory.md`](shard_progress_memory.md)
- [`task90150022_result.md`](../task_result/task90150022_result.md)
- [`task09150832_result.md`](../task_result/task09150832_result.md)
- [`task09151930_result.md`](../task_result/task09151930_result.md)
- [`task09160035_result.md`](../task_result/task09160035_result.md)
- [`task09160854_result.md`](../task_result/task09160854_result.md)
- [`hfs_final_performance_baseline_20260913.md`](../performance/hfs_final_performance_baseline_20260913.md)
