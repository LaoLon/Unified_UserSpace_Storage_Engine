# Shard-66：POSIX 同步边界效率与拓扑自适应优化

> **状态**：实现完成；正式 NVMe 66A 全矩阵需在目标 VM 上按 harness 执行
> **基线**：`main` 当前 HEAD（Shard-65 已完成；task90150022 / task09150832 / task09151930 三阶段测量已收口）
> **目标**：在不破坏 POSIX 语义、不弱化 durability、不引入 async API 的前提下，削减同步 POSIX 边界每请求软件成本与 CPU 浪费，把 4K~64K steady-state 性能推向实测 raw Blobstore 上限。
> **证据来源**：`doc/task_result/task90150022_result.md`、`doc/task_result/task09150832_result.md`、`doc/task_result/task09151930_result.md`、`doc/performance/hfs_final_performance_baseline_20260913.md`
> **不包含**：ticket scheduler 重构、fd/metadata fast path、durability 语义改动、allocator/registered-buffer 改造、pure spin 默认值、盲目扩大 reactor 数。

## 1. 三阶段测量后的架构结论

### 1.1 当前位置（正式口径，trace-off）

| 口径 | 4K 读 IOPS | 4K 写 IOPS | 说明 |
|---|---:|---:|---|
| raw NVMe（spdk_nvme_perf QD32） | 383,039 | 未测 | 设备裸能力参考值 |
| raw bdev（自研 bench QD32） | 147,518 | 104,818 | A1 |
| raw Blobstore（预分配 blob，QD32，1ch/2ch） | 79,083 / 99,202 | 74,693 / 70,381 | task09151930 修正口径 |
| HFS POSIX（32 同步 worker，共享文件） | 92,200 | 70,900 | task90150022 A4 |
| HFS POSIX randrw50（32 job，正式基线） | — | — | total 77,249 vs raw bdev FIO 121,911 = 63.4% |

关键修正：task90150022 的 A2 raw blob 读 139K 是在**新 format 的薄分配 Blobstore** 上测得，未分配 cluster 的读直接返回零、不下发设备，属虚高；task09151930 用预分配 blob 测得真实值 79K@1ch / 99K@2ch。**HFS 4K 纯读已达真实 raw Blobstore 上限的约 93%，纯写约 95%**。剩余差距不是"慢"，而是"贵"和"尾巴长"。

### 1.2 成本分解（Little's law 决定一切）

psync 模型下 32 个同步线程，每线程同时只有 1 个 outstanding I/O：

```text
IOPS = 线程数 / 平均单请求时延
```

任何阶段削减 1 µs 都直接转化为吞吐。32-job 各阶段中位数（task09150832，单位 µs）：

| 阶段 | 含义 | 1 job | 32 jobs | 性质 |
|---|---|---:|---:|---|
| T0-T1 | fd/open-file/inode | ~2-3 | ~3 | 已很小，不动 |
| T1-T2 | ticket admission | 6 | 25-27 | 共享文件争用，A/B 证明 ≤3% |
| T2-T3 | buffer/lease 准备 | ~12-16 | ~12-16 | staging 固定成本 |
| T3-T4 | enqueue/message | ~12-16 | ~12-16 | messageSubmitLock p99 仅 1-4 µs，非瓶颈 |
| T4-T5 | reactor 拾取 | 2.8 | 29-39 | reactor 被 CPU 争用放大 |
| T5-T6 | dequeue→submit | ~1 | ~1 | 已很小 |
| T6-T7 | submit→callback | 43-48 | 340-480 | 主体是设备 QD~32 排队，与 raw 同口径一致 |
| T7-T8 | callback→wake 发布 | ~8-9 | ~8-9 | 小 |
| T8-T9 | wake→应用恢复 | 18 | 100-110 | CPU oversubscription + futex 往返 |
| T9-T10 | copy-out+ticket 完成+清理 | 3-5 | 24-31 | 锁争用放大 |
| T10-T11 | POSIX 返回 | ~13 | ~13 | 小 |

- 软件段合计 ≈ 150-200 µs（占 366-480 µs 全程的 40-45%），设备段占 55-60%。
- T6-T7 与 raw Blobstore 同 QD 的平均时延（322-405 µs）一致，**不是 HFS 软件开销**；TB0-TB1/TB1-TB2 只有约 1-2 µs，submit 入口已排除。
- 软件段的共同放大器是 **CPU oversubscription**：5 个 100% busy-poll reactor + 32 个应用线程抢 8 vCPU，runq≈11，ctx/s≈14 万。

### 1.3 最强已验证杠杆：reactor 拓扑

固定 owner CPU0、应用 CPU5-7（task09150832，共享文件，32 job）：

| data reactors | 读 IOPS | 写 IOPS | 读 CPU | 写 CPU |
|---:|---:|---:|---:|---:|
| 2 | 108,734 | 90,617 | 445% | 428% |
| 4 | 87,413 | 67,885 | 657% | 629% |

2 reactor 读 +24%、写 +33%，同时 CPU 大幅下降。当前默认（`HFS_DATA_REACTORS` 未设）在 8 vCPU 上为 4 data + 1 owner = 5 个轮询线程，是**错误的默认值**。raw Blobstore 通道对照同样显示 2ch 为读最佳点、4ch 不再提升，佐证该 VM/NVMe 的有效并行度有限。

### 1.4 已排除/降级项（不得再作为本阶段主线）

- **ticket scheduler**：ticket/range/none A/B 差异 ≤3%，结构保留；`HFS_POSIX_TICKET=1` 维持默认。
- **messageSubmitLock**：p99 仅 1-4 µs，远小于 T4-T5；不重构 ingress。
- **fd/open-file/metadata**：T0-T1 p99 约 2-3.3 µs，不动。
- **app buffer DMA direct**：当前环境为 uio_pci_generic + hugepage，`spdk_vtophys` 只覆盖 SPDK 注册的 DMA 内存，普通应用 buffer 不可能直通（实测 direct=0%）；不重写 allocator，不引入 registered-buffer ABI。
- **durability**：shard-64/65 已收口，本阶段不改语义、不动 epoch pipeline。

### 1.5 测量口径遗留问题（必须先收口）

1. task09151930 的 144 个样本为 trace-on，数值系统性低于 task09150832 的 trace-sample 轮次（同拓扑 d=2/32j：64.8K vs 108.7K）；跨任务不可直接比较。
2. 观测到 run-to-run 大幅波动（fixed d=2/16j 读中位数曾低至 16.4K）。
3. 三个任务的正式矩阵都没完成 16K/64K 与 jobs=1/8 的 30 秒 × 3 次样本。
4. 官方基线脚本是共享文件 + randrw50；ticket 争用结论来自共享文件，ordering A/B 用的是独立文件——两种口径都必须显式声明，不得混用。

## 2. 总体路线

```text
66A 测量口径收口与基线 v2（纯测量，不改架构）
        ↓
66B reactor 拓扑默认值与亲和策略（小改动、已验证收益）
        ↓
66C 同步完成边界精简（wake 路径瘦身 + gated adaptive spin）
        ↓
66D TN0/TN1 backend 分解（纯测量，决定是否需要 66E 之后的 backend 工作）
        ↓
66E staging/pool 与 16K-64K 收口
        ↓
66F 共享文件 ticket 争用微优化（仅当 66A 重测显示 T1-T2 占比 >5%）
```

每个子 shard 独立环境变量开关、独立 A/B、独立可回滚；一次只改一个主要变量。

## 3. Shard-66A：测量口径收口与基线 v2（先行）

### 3.1 锁定 benchmark harness v2

- 每轮正式运行前重置 Blobstore 状态：`HFS_FORMAT=1` 重建后用 FIO 预写一次 workload 文件（消除薄分配虚读），再进入正式轮。
- 同时声明并分别报告两种文件口径：`shared-file`（单 `/hfs/fio/bench.dat`，对应官方基线与 MySQL ibdata 类热点文件）与 `per-job-file`（`filename_format=...$jobnum.dat`，对应 RocksDB SST 类多文件）。
- 正式数字一律 trace-off、stats-off；归因轮单独跑 trace-on，两组数字不得混排。
- 每点 10 s ramp + 30 s runtime，**5 次取 median**（原 3 次不足以压制已观测到的漂移）；记录当日 VM CPU 频率、runq、ctx/s。
- 补齐缺口矩阵：4K/16K/64K × randread/randwrite × jobs=1/8/16/32。
- raw 对照固定为：预分配 blob、channels=1/2、总 QD=32、10 s；raw bdev FIO engine（官方基线口径）保留作对外比值。

### 3.2 输出

- 更新 `doc/performance/` 基线 v2：A0/A1/A2(修正)/A3/A4 分层表 + HFS/raw 比值；`doc/design/fio_baseline_notes.md` 同步指向。
- 修正 task90150022 遗留的 A2 虚高说明（139K → 99K 口径），在 `doc/bug.md` 记录口径问题。
- 66A 完成后重新核算各阶段占比，作为 66C-66F 的门槛依据。

## 4. Shard-66B：reactor 拓扑默认值与亲和策略

### 4.1 默认值修正

- `HFS_DATA_REACTORS` 未设置时：在线 CPU ≤ 8 核 → data reactor = 2（含 owner 共 3 线程轮询）；> 8 核 → min(4, nproc/4) data reactor。保留显式环境变量覆盖，范围 1-8 不变。
- `HFS_POSIX_QUEUE_DEPTH` 默认维持 32；66A 若显示 queue depth 峰值长期 <8，可另行评估降到 16 的 A/B（不在本子 shard 默认动作内）。

### 4.2 亲和策略

- 新增 `HFS_REACTOR_PIN`（默认 `0`，保持现有行为）：`1` 时 owner 绑 CPU0、data reactor i 绑 CPU(i)，复用现有 `HFS_OWNER_REACTOR_CPU`/`HFS_DATA_REACTOR_CPUS` 解析路径；显式 CPU 列表优先于自动编号。
- 库**不得**绑定应用线程；在性能文档中给出推荐布局（8 vCPU：owner CPU0、data CPU1-2、应用 CPU3-7）。
- 验收：8 vCPU、共享文件、32 job、trace-off：`HFS_REACTOR_PIN=1` + 推荐应用掩码相对当前默认 ≥ +15% total IOPS（4K randrw 与 randread 各测），CPU% 同步下降；不满足则只保留开关、不改默认值。

### 4.3 reactor 空闲轮询（gated 实验，非默认）

- busy-poll reactor 在数据库空闲期仍烧 CPU。新增实验开关 `HFS_REACTOR_IDLE_BACKOFF_US`（默认 0 = 纯 busy-poll）：仅当本 reactor 连续 N 轮 poll 无事件且无 inflight 时进入有界 backoff（上限为该值），有新 batch/message 时立即恢复。
- 必须证明：数据库空闲 CPU 下降明显，且 FIO steady-state IOPS 回退 <2%、p99 不回退，否则只保留开关不设默认。

## 5. Shard-66C：同步完成边界精简

目标段：T7-T8（callback→wake 发布）、T8-T9（wake→resume）、T9-T10（copy-out+ticket 完成+清理）。

### 5.1 wake 路径瘦身（默认动作）

- `HfsCompletion` 布局：把 futex 字 `state` 与 callback 写的 `runtimeCreditHeld/runtimeReactorId` 隔离到独立 64B cacheline，trace 字段集中到另一 line，消除 callback 与等待线程间的伪共享（改前用 `perf c2c`/统计对照确认，禁止仅凭布局猜测）。
- `hfsRuntimeCompletionComplete` 在 stats-off/trace-off 路径上只做：credit 归还（原子）、state 发布（一次 store-release）、futex wake（一次 syscall）；trace/profile/stats 分支保持现状但不得进入热路径。
- 应用侧 T9-T10：核对 fd release、sync unlock、ticket complete 顺序，确保 copy-out 不持任何锁；发现持锁 copy 即修复。

### 5.2 `HFS_WAIT_MODE=futex|adaptive`（gated A/B）

- adaptive：submit 后先 bounded spin（cpu_relax + state 检查），窗口可选 0.5/1/2/5 µs（默认 0 = futex，环境变量选择），超时未完成后落入现有 futex 路径。
- 依据 task09150832 结论：T8-T9 低并发 18 µs、32 job 100 µs，主要放大器是调度器争用而非 futex API；预期 adaptive 只在 jobs ≤ 8 且设备 QD 低时有效，32 job 高 QD 下收益应接近 0——**必须如实验证，不得预设结论**。
- 门禁：16j 与 32j 各 5 次 median；IOPS 提升 ≥5% 且应用 CPU 增加 <10% 才允许把 adaptive 设为某档位的推荐值（仍非默认）；否则保留开关、默认 futex 不变。
- 禁止 pure spin、禁止 >5 µs 的默认窗口、禁止为 spin 引入 per-request 堆分配。

## 6. Shard-66D：TN0/TN1 backend 分解（纯测量）

- 目标：把 T6-T7 拆成 `TN1-TN0`（NVMe 提交→驱动观测到完成）与 `TN2-TN1`（驱动→HFS callback 传播）。
- 优先使用 SPDK 已有 trace framework（bdev/nvme tracepoint + tpoint 分析）；当前构建无可用逐 I/O 事件时，允许在 VM 内 SPDK 树加**最小时时间戳补丁**（仅 nvme 驱动 completion 入口打点 + 导出），以本地 patch 形式保留，不同步回 Windows、不污染 HFS 仓库的 SPDK 锁定 commit。
- 判定：最佳拓扑（66B）下若 propagation（TN2-TN1）占 T6-T7 >10%，才允许立项评估 reactor poll 策略调整；否则在文档中确认"T6-T7 ≈ 设备"，**永久关闭** backend poll 方向，不再反复重开。

## 7. Shard-66E：staging/pool 与 16K-64K 收口

- DMA pool 核查：当前每 reactor shard 为 4K×32、16K×16、64K×32、1M×8；queue depth 32。66A 的 16K/64K 矩阵若出现 lease wait（trace `lease_*`）占比 >3%，按"每 class 计数 ≥ queue depth"原则调整 16K class 计数，A/B 验证。
- 64K copy 成本：copy tick 在 4K 约 0.4-0.9 µs/方向，64K 按字节放大；若 66A 显示 copy ticks 占请求周期 >5%，评估只针对 64K 的优化（例如增大单次 pipeline chunk、减少 copy 次数），不做全路径重写。
- 明确写入文档：uio+hugepage 环境下应用 buffer DMA direct 不可用，`directEligible` 恒为 false 是环境限制而非 bug；禁止为此改 allocator 或引入注册 buffer。

## 8. Shard-66F：共享文件 ticket 争用微优化（gated）

- 仅当 66A 用共享文件口径重测后 `T1-T2 + T9-T10(ticket 部分)` 占总时延 >5% 才启动。
- 允许的最小动作：admission 快路径（active list 为空或无重叠时 O(1) 通过）、completion 扫描早停；不得改 ticket 语义、不得删依赖追踪。
- 若 ≤5%：在文档中关闭此方向。

## 9. 性能门禁与验收标准

所有正式数字：trace-off、5 次 median、66A 的状态重置口径、NVMe `Nvme0n1`、`config/hfs_nvme.json`。

| 指标 | 当前基线 | Shard-66 目标 |
|---|---:|---:|
| 4K randrw50 32j total IOPS | 77,249 | ≥ 89,000（+15%）且 CPU ≤ 500% |
| 4K randread 32j IOPS | 92,200 | ≥ 100,000 |
| 4K randwrite 32j IOPS | 70,900 | ≥ 80,000 |
| 4K jobs=16 p99（读/写） | 436 / 545 µs | 下降 ≥20% |
| 16K/64K HFS/raw bdev FIO 比值 | 65.5% / 70.9% | ≥ 75% |
| jobs=1 单线程平均时延 | 读 49.2 / 写 52.3 µs | 不回退（±3%） |
| stats-off 回归（既有 shard-62/63 口径脚本） | — | 下降 <5% |

兼容性门禁（全部通过才算完成）：

- VM `make -B app lib` 与 CMake Release 强制构建；
- `posix_contract_test.sh`、`io_fast_path_smoke.sh`、`posix_queue_engine_test.sh`、`posix_range_ticket_test.sh`、`metadata_transaction_smoke.sh`、`metadata_path_concurrency_smoke.sh`、`fd_lifecycle_smoke.sh`、`io_trace_smoke.sh`、持久化 bdev smoke、16 线程 10 s runtime stress；
- MariaDB `mysql_smoke.sh` 与 RocksDB `rocksdb_smoke.sh` 全量通过；
- 正式样本 `errors=0`、`spdk_submit=spdk_complete`、`profile_invalid=0`；
- durability 语义不变：不允许虚假 fsync 成功，`durability_batch` 统计不得因本阶段改动异常。

## 10. 停止条件（任一触发即停止对应子项并回滚）

1. 子项 A/B 显示 IOPS 提升 <2% 且 CPU/复杂度上升 → 回滚，仅保留开关。
2. CPU 增加 >10% 换 <5% IOPS → 停止。
3. 发现 benchmark/profiling/统计本身有错误 → 立即停止性能工作，先修测量（沿用三阶段任务的停止规则）。
4. 任何 correctness 回归（read-after-write、overlap ordering、append、truncate、dup、fsync/fdatasync、crash consistency）→ 立即回滚。
5. 66D 确认 propagation ≤10% 后，不得再以"reactor poll"为名新开性能子项。

## 11. 风险与已知边界

- **跨轮漂移**：已观测到同配置中位数 16.4K vs 65.2K 的波动；66A 的状态重置 + 5 次 median 是缓解，不是消除。所有对比必须同日内、同 harness 完成。
- **共享文件 vs 独立文件**：MySQL ibdata/redo 是共享文件模型，RocksDB 是多文件模型；两种口径必须同时报，任何单一口径的结论不得外推。
- **虚拟化 NVMe 上限**：本 VM 的 VMware 虚拟 NVMe（15ad:07f0）QD1 时延 ~43-48 µs，QD32 排队后 ~320-480 µs；HFS 无法突破设备排队曲线，目标只是把软件段压回去，不是追赶 raw NVMe 工具数字。
- **单请求时延与吞吐的权衡**：jobs=16 是吞吐/时延 knee（task09151930 Q4），优化不得以 jobs=32 单点数字宣称整体更优；正式报告必须同时含 jobs=16 与 jobs=32。
- **不引入新 ABI**：本阶段全部通过 env 开关 + 内部路径完成，POSIX preload 外部接口不变。

## 12. 执行顺序与交付

```text
66A（测量收口，产出基线 v2）
  → 提交基线文档与 harness 脚本
66B（拓扑默认 + 亲和，A/B 验证）
  → 提交 runtime 改动 + 文档推荐布局
66C（wake 瘦身 → adaptive spin gated A/B）
66D（TN0/TN1 测量，一次性结论）
66E（pool/大块收口）
66F（gated，可能整体跳过）
  → 最终合并回归 + 更新 AGENTS.md/基线文档/bug.md
```

每个子 shard 完成后：VM 强制构建（Make + CMake Release）→ correctness 套件 → 性能 A/B → 决定保留/回滚 → 提交并合入 main → 更新本文档实现记录。

## 13. Shard-66 implementation record

### 13.1 66A harness

- Added `scripts/db/shard66_baseline_v2.sh`. Each formal sample formats a fresh HFS
  mount, prewrites the target file(s) to remove thin-allocation read artifacts, and then
  runs trace-off/stats-off FIO. It reports `shared-file` and `per-job-file` separately,
  covers 4K/16K/64K, randread/randwrite, jobs 1/8/16/32, and defaults to five samples.
- Added `scripts/db/shard66_summary.py`, which emits formal sample and median TSV/JSON
  files while excluding the optional trace-on attribution sample from formal medians.
  Results belong under `/mnt/hfs-data`, not `doc/task_result`.

### 13.2 66B topology and affinity

- Preserved the existing total-reactor environment contract: `HFS_DATA_REACTORS=N`
  means owner plus `N-1` data reactors. When unset, one-vCPU hosts use one data
  reactor, 2–8-vCPU hosts use two data reactors, and larger hosts use
  `min(4, max(1, nproc/4))` data reactors.
- Added `HFS_REACTOR_PIN=1`. Without explicit CPU entries it pins owner to CPU0 and
  data reactor `i` to CPU `i`; `HFS_OWNER_REACTOR_CPU` and
  `HFS_DATA_REACTOR_CPUS` remain higher-priority overrides. Application threads are
  never affinity-bound by HFS.
- Added gated `HFS_REACTOR_IDLE_BACKOFF_US` with an eight-empty-poll threshold and a
  bounded nanosleep. The default remains zero, preserving busy polling for steady-state
  performance.

### 13.3 66C completion boundary

- Reorganized `HfsCompletion` into 64-byte state, callback-credit, and diagnostic
  cacheline regions with compile-time offset checks. The stats-off/trace-off completion
  path now avoids profile/trace/stat work and performs only credit retirement, one
  release store, and one futex wake.
- Added gated `HFS_WAIT_MODE=adaptive` plus `HFS_WAIT_SPIN_US` (or the compatibility
  alias `HFS_ADAPTIVE_SPIN_US`), bounded to 0–5 microseconds. The default is futex
  waiting and no per-request allocation is used.

### 13.4 66D measurement and 66E/66F gates

- Added per-reactor `poll_gap_complete_ns` statistics, sampled at the HFS callback from
  the previous poll exit. This is a scheduler/poll-delay upper-bound signal; it is not
  mislabeled as a direct NVMe completion timestamp. SPDK remains unmodified.
- The existing `HFS_IO_POOL_CLASS_COUNTS` switch is sufficient for the 66E gated
  16K-class A/B; no default pool or allocator change is made before 66A evidence.
- Ticket scheduler code is unchanged. 66F stays closed until shared-file 66A evidence
  shows ticket admission plus ticket completion exceed five percent of request time.

### 13.5 VM validation on 2026-09-16

- Make and CMake Release builds passed after the final source sync.
- With `HFS_REACTOR_PIN=1`, `HFS_WAIT_MODE=adaptive`, and a 0.5 us bounded window,
  POSIX contract, queue engine, range ticket, IO trace, POSIX data contract, fd lifecycle,
  and an 8-thread/5-second runtime stress all passed. The trace smoke produced 33 phases;
  queue and ticket submit/complete accounting remained closed.
- The same settings passed data-reactor stripe, DMA backpressure, redundant-request,
  metadata path, metadata transaction crash-boundary, durability barrier, and persistent
  bdev smoke tests. MariaDB persistent recovery passed with mysqlslap fallback, and the
  shortened 2,000-key/5-second RocksDB WAL/recovery run passed `ldb checkconsistency`.
- The default-topology probe on the 8-vCPU VM created exactly two data reactors (IDs 1 and
  2). Poll-gap telemetry was emitted for the timing profile, with the metric retained as a
  scheduler-delay upper bound.
- The bounded NVMe 66A harness smoke passed 16 samples (shared/per-job, 4K/16K,
  randread/randwrite, jobs 1/8). The full 5-run 4K/16K/64K × two file models ×
  randread/randwrite × jobs 1/8/16/32 matrix remains a formal long-run measurement; no
  performance gate or default adaptive/pinning recommendation is claimed from the smoke.

### 13.6 Revised short task09160035 rerun

- The revised short-test contract was executed on the real NVMe backend only, with
  `Nvme0n1`, `uio_pci_generic`, `vm.nr_hugepages=1024`, fresh format/prefill per sample,
  trace/stats off, and no concurrent HFS/FIO processes. The quick
  `posix_contract_test.sh` and `io_fast_path_smoke.sh` correctness checks both passed.
- The first A-layout formal sample (five total reactors, data CPUs 1-4, shared file, 4K
  randread, 32 jobs) measured `58,277.97 IOPS`, zero FIO errors, and `595%` process CPU.
  Its fresh 256 MiB-per-job shared-file prefill took `272.261 s`; the next prefill exceeded
  the short-run wall-clock budget. Because the first A randread is below the task's
  `70K IOPS` stop gate, B topology and adaptive-wait samples were not run.
- No topology, pinning, or wait-mode default was changed. The full five-run matrix and
  its formal performance decision remain open. The real-NVMe sparse-extension issue found
  during the rerun is recorded as bug 90; its concurrent-safe EOF-gap fix is included in
  the current working changes and passed the quick correctness suite.
