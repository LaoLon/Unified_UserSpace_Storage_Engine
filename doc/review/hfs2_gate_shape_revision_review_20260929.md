# HFS 2.0 性能 gate 形状修订评审（2026-09-29）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

> 评审对象：`doc/design/hfs2.0.md` §16.2 的 FIO 主 gate 定义。
> 触发证据：`doc/performance/hfs2_8j4_shape_attribution_20260929.md`、
> `doc/performance/hfs2_stage_abcd_regression_20260929.md`。
> 结论：**接受修订**——正式 gate 改为同形状对照；原跨形状口径降为历史记录。

## 1. 问题

§16.2 原主 gate 定义为：

```text
4K shared-file randrw：HFS 8 jobs × iodepth 4  vs  raw 1 job × iodepth 32
判据：HFS/raw ≥85%，HFS p99 ≤ raw 的 110%，errors=0，submit/complete 闭合
```

2026-09-29 Stage A–D 合并回归复测得到 `HFS/raw = 72.7%`、p99 比值 1.146，
未达标；但同会话证据显示该比值主要来自**形状差异**，而非 HFS 单请求成本：

1. **trace 分段**（8j4，trace-on 采样，`dropped=0`）：
   p50 398 µs 中 bdev 段 334 µs（84%），ring_wait 21.7 µs、publish 17.2 µs，
   plan/admit 合计 0.4 µs。HFS 软件段合计约 40–60 µs（含 trace 开销上限）。
2. **shape sweep**（固定总 QD=32，3 轮中位）：
   raw 在 8j4 形状自身塌陷 43%（113.9k → 65.0k），HFS 只降 21%（103.3k → 82.1k）；
   同一 8j4 形状下 HFS 比 raw 快 26%。
3. **gate-revision 复测**（5 轮同会话交错，双侧同形状）：
   - 1 job × QD32：HFS/raw = **90.6%**，p99 比值 **1.00**（553/561 µs）
   - 8 jobs × QD4：HFS/raw = **120.8%**，p99 比值 **0.19**
     （HFS 643 µs vs raw 3457 µs）

## 2. 决策

1. **正式吞吐/延迟 gate 改为同形状对照**：双侧使用完全相同的
   `numjobs × iodepth`，判据不变（HFS/raw ≥85%，p99 ≤110%，errors=0，
   closure 完整，CPU/RSS 记录）。
2. **主形状**：1 job × QD32（raw 在该 VM/控制器上可持续的形状；64 线程会使
   控制器 I/O 队列耗尽，见 `doc/performance/fio_baseline.md` §8.2）。
3. **扩展性形状**：8 jobs × QD4，双侧同形状；该形状用于检查多 worker 下的
   前端/线程可扩展性，不与 raw 的其它形状混比。
4. **历史记录保留**：2026-09-18 的 8j4/1j32 跨形状比值（0.764、0.702、0.605 等）
   继续保留为历史对比，不再作为验收判据；本文生效后不得再用跨形状比值宣称
   通过或失败。
5. 阈值、CPU 上限、closure 要求、多设备与完整矩阵要求均不变。

## 3. 影响与现状

- 修订后当前状态（2026-09-29，6 vCPU VM、`Nvme0n1`、`HEAD 74c500c`）：
  主形状通过（90.6% / 1.00），扩展性形状通过（120.8% / 0.19）。
- 该结论限于单盘、4K randrw、本 VM；§16.2 的 bs/iodepth/numjobs 完整矩阵与
  多 NVMe device set 验收仍需按原要求执行。
- raw 8j4 的塌陷是 FIO SPDK plugin 在多 worker 下轮询行为的已知边界；
  若后续更换 raw 对照工具（bdevperf）需要重新采集双侧同形状基线。

## 4. 风险

| 风险 | 缓解 |
|---|---|
| 同形状 1j32 对 HFS 有利（单前端线程） | 同时报告 8j4 扩展性形状；两形状都判定 |
| raw 在 8j4 的塌陷被误读为 HFS 优势 | 在 §16.2 明确记录 raw 8j4 的 p99 3.46 ms 与 IOPS 塌陷边界 |
| 更换 VM/工具后形状结论漂移 | 每次正式 gate 同会话采集双侧同形状基线；跨会话只比比值 |
| 历史跨形状数字被继续引用 | §16.2 标注为历史记录，验收只认同形状口径 |

## 5. 追加：1j32 读写分离复测（2026-09-29）

为核对主形状的读/写分别表现，在同一 VM、同一设备上按 1 job × QD32、4K、
256 MiB、10 s、5 轮（randrw）/3 轮（纯读纯写）交错复测：

| 工作负载 | 侧 | 读 IOPS | 写 IOPS | 读 p50/95/99（µs） | 写 p50/95/99（µs） |
|---|---|---:|---:|---|---|
| randrw 50/50 | raw | 57407.5 | 57329.5 | 261.1 / 378.9 / 544.8 | 264.2 / 383.0 / 553.0 |
| randrw 50/50 | HFS | 52363.8 | 52297.5 | 292.9 / 383.0 / 561.2 | 292.9 / 387.1 / 561.2 |
| randread | raw | 131515.3 | — | 228.4 / 329.7 / 477.2 | — |
| randread | HFS | 117864.2 | — | 259.1 / 342.0 / 497.7 | — |
| randwrite | raw | — | 102297.8 | — | 292.9 / 432.1 / 602.1 |
| randwrite | HFS | — | 95793.4 | — | 317.4 / 415.7 / 602.1 |

| 指标 | HFS/raw |
|---|---:|
| randrw 读 IOPS | 91.2% |
| randrw 写 IOPS | 91.2% |
| randrw 读 p99 | 1.030× |
| randrw 写 p99 | 1.015× |
| randread IOPS / p99 | 89.6% / 1.043× |
| randwrite IOPS / p99 | 93.6% / 1.000× |

结论：主形状下 HFS 的读、写分别达到 raw 的 91.2%/91.2%（混合）、89.6%（纯读）、
93.6%（纯写），p99 比值 1.00–1.04×，全部满足修订后 gate 的 ≥85% 与 ≤110% 判据；
与 §3 的 randrw 聚合结果（90.6%）一致。证据：
`/home/hfs/retest-1j32-20260929`（22 个计时 JSON + metrics）。

## 6. 参考

- `doc/design/hfs2.0.md` §16.2（本次修订）
- `doc/performance/hfs2_8j4_shape_attribution_20260929.md`
- `doc/performance/hfs2_stage_abcd_regression_20260929.md`
- `doc/performance/fio_baseline.md` §8.2（raw 64 线程控制器队列上限）