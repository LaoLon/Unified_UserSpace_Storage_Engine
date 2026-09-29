# HFS 2.0 8j4 gate 形状归因（2026-09-29）

日期：2026-09-29
范围：解释 Stage A–D 合并回归中 FIO 8j4 gate 的 `HFS/raw = 72.7%`，
用同一会话的 trace 分段与 shape sweep 分离"HFS 软件成本"和"形状/线程"两个因素。
证据目录：`/home/hfs/trace-gate-20260929`、`/home/hfs/shape-sweep-20260929`。

## 1. 方法

1. **trace 分段**：trace-on（`trace_sample=16`，`dropped=0`）跑一次 10 s 8j4，
   与 trace-off 参考同会话对比，用 `hfs_trace_dump` 输出分段 p50/p99。
2. **shape sweep**：固定总 QD=32，分别用 1/2/4/8 个 job（QD 32/16/8/4），
   HFS 与 raw 各 3 轮交错取中位，分离"每 worker 深度"与引擎软件成本。

## 2. Trace 分段（8j4，trace-on 采样；绝对值为 trace 开销上限）

| 分段 | p50 | p99 | 说明 |
|---|---:|---:|---|
| request（FeSubmit→FeReap） | 398.2 µs | 823.2 µs | 前端可见端到端 |
| ring_wait（FeSubmit→ActorPickup） | 21.7 µs | 78.2 µs | ring 排队 + actor 调度 |
| plan_exec（ActorPickup→PlanReady） | 0.21 µs | 0.41 µs | planner |
| admit_exec（PlanReady→ShardSubmit） | 0.19 µs | 0.48 µs | DMA + scheduler + submit |
| shard / bdev（Submit→Complete） | 334.0 / 333.8 µs | 716.0 / 715.8 µs | **设备段（占 p50 84%）** |
| publish（ParentComplete→FeReap） | 17.2 µs | 64.9 µs | 完成环 + 前端 reap |
| parent（FeSubmit→ParentComplete） | 380.2 µs | 777.6 µs | |

结论：8j4 的 p50/p99 由**设备段**支配（QD=32 下 ~334/716 µs）；
HFS 软件各段合计约 40–60 µs（含 trace 开销），不是主要项。
trace-on 相对 trace-off：IOPS 83.3k→74.9k、p99 618→823 µs，
因此上表绝对值只用于分层比例，不用于性能结论。

## 3. Shape sweep（总 QD=32，3 轮中位）

| 形状 | raw IOPS | HFS IOPS | HFS/raw（同形状） |
|---|---:|---:|---:|
| 1 job × QD32 | 113916.4 | 103317.0 | **90.7%** |
| 2 jobs × QD16 | — | 99311.9 | — |
| 4 jobs × QD8 | — | 90772.9 | — |
| 8 jobs × QD4（正式 gate 的 HFS 侧） | 64962.7 | 82080.9 | **126.4%** |

关键观察：

1. **raw 在 8j4 形状自身塌陷 43%**（113.9k → 65.0k）；HFS 只降 21%
   （103.3k → 82.1k）。同一 8j4 形状下 HFS 比 raw 快 **26%**。
2. 因此正式 gate 的 `HFS 8j4 / raw 1j32 = 72.7%` 是**不同形状**的比较：
   HFS 用 8 个前端线程、每 worker 深度 4，raw 用 1 个线程、深度 32。
3. **同形状（1j32）下 HFS 已达 raw 的 90.7%**，即 Stage A–D 之后 HFS
   相对 raw 的真实软件差距约 9.3%，而不是 27%。
4. 该 VM 为 6 vCPU：8 个 worker + reactor/metadata/frontend 线程存在 CPU
   竞争；raw 的 FIO plugin 在 worker 内轮询 SPDK，8 worker 时塌陷更明显；
   HFS 的 worker 不轮询设备（reactor 承担），因此在多 worker 形状更稳。

## 4. 结论与建议

1. **8j4 gate 未达标的直接原因不是 HFS 单请求软件成本**（设备段占 84%，
   软件段 ~10%），而是 gate 形状把 HFS 的最差形状与 raw 的最优形状相比。
2. HFS 同形状差距 9.3%，仍高于 §16.2 的 ≤110% p99 目标之外的 85% 吞吐目标；
   继续压缩前端每请求成本（ring 排队、完成唤醒/reap）可以收窄这 9.3%，
   但**无法**把 8j4/1j32 的比值从 72.7% 拉到 85%，因为 raw 在该形状自身只有
   65k（HFS 反而更快）。
3. 建议对 `doc/design/hfs2.0.md` §16.2 的 gate 定义做一次评审：
   - 保留 8j4 作为**多 worker 扩展性**检查，但对照物改为 raw 同形状（或记录
     raw 的 8j4 塌陷作为已知边界）；
   - 正式吞吐 gate 用同形状比较（1j32 或 8j4 双侧一致），并沿用 ≥85% / p99 ≤110%。
   该变更属于验收口径调整，需按项目规则先更新设计文档。

## 5. 证据

- `/home/hfs/trace-gate-20260929/`：`plain`/`traced` JSON+stderr、`dump.txt`
  （11.8M records、`dropped=0`、分段 p50/p99、事件计数）、trace 会话目录
- `/home/hfs/shape-sweep-20260929/`：6 个 cell × 3 轮 JSON/metrics、`spdk-nvme.json`
