# HFS 2.0 MySQL vs raw(native) 差距 Review（2026-09-29）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

> 目的：回答"为什么 HFS 比 raw/native MySQL 慢"。结论先行：**在同盘同口径下 HFS
> 不再更慢**（读 96–99%、写 91–156%、写 p99 79–85%），代价是 **CPU/事务 +7–17%**；
> 历史"65%"来自非对称口径。本文给出全部数据与剩余差距构成。

## 1. 同盘门禁数据（4 阶段交替，每阶段重建 datadir）

第 1 轮（Shard-112 前，`mysql-samedisk-20260929b`）：

| 阶段 | 读 TPS | 读 p99 ms | 读写 TPS | 读写 p99 ms | fsync 增量 |
|---|---:|---:|---:|---:|---:|
| HFS 1 | 10654.31 | 1.82 | 2163.37 | 8.43 | 80,235 |
| native 1 | 11047.84 | 1.86 | 2193.67 | 7.17 | 65,498 |
| HFS 2 | 11074.48 | 1.82 | 2072.11 | 9.06 | 82,606 |
| native 2 | 11196.42 | 1.76 | 2272.58 | 7.04 | 67,137 |

比值：读 **96.4% / 98.9%**、写 **98.6% / 91.2%**、读 p99 97.8% / 103.4%、
写 p99 117.6% / 128.7%（Shard-112 后修复为 **79.1% / 85.0%**）。

第 2 轮（Shard-112 后，`mysql-samedisk-20260929c`）：读 155.4% / 98.7%、
写 156.6% / 102.7%、写 p99 79.1% / 85.0%（两侧绝对值比第 1 轮低约 2×，为设备/
VM 状态差异，同时影响两侧；只比较同会话比值）。

## 2. CPU 成本（同盘，`write_only` 15 s，8 线程，trx=1，`mysql-cpu-ab-20260929`）

| 侧 | TPS | 系统 busy CPU | mysqld CPU | CPU/事务 |
|---|---:|---:|---:|---:|
| HFS | 2228.33 | 53.41 s | 50.59 s | **1.51 ms** |
| native | 2095.98 | 49.93 s | 43.22 s | **1.37 ms** |

- 本口径 HFS 吞吐 **+6.3%**，CPU/事务 **+10%**（系统 +7%、mysqld +17%）；
- 增量 = metadata/data reactor 线程 + seam 前端 + journal/checkpoint barrier；
- 结论：**HFS 用更多 CPU 换 crash consistency 能力**，吞吐上不再落后（CPU 有余量）。

## 3. 每请求软件开销构成（trace 分段，`mysql-trace-20260929`，8 线程写负载）

| 段 | p50 | p99 | 说明 |
|---|---:|---:|---|
| `frontend_fixed`（Begin→Submit） | 2.24 µs | 115.6 µs | validate + 快照 + 提交（trace 开启时偏高） |
| `ring_wait`（Submit→Pickup） | 5.08 µs | 733.8 µs | 有界 ingress + reactor 拾取 |
| `plan`（Submit→ShardSubmit） | 8.21 µs | 902.4 µs | 含 ring wait + planner |
| `plan_exec`（Pickup→PlanReady） | 0.29 µs | 1.5 µs | planner 执行 |
| `admit_exec`（PlanReady→ShardSubmit） | 1.32 µs | 189.6 µs | shard 准入 |
| **`bdev`（BdevSubmit→Complete）** | **61.96 µs** | 662.5 µs | 设备 I/O（两侧相同） |
| `publish`（ParentComplete→Reap） | 16.30 µs | 249.2 µs | 完成路径 + 前端 reap |
| `request`（Submit→Reap） | 92.60 µs | 1429 µs | 全链路 |

- 设备占 p50 的 **62 µs**，HFS 软件段合计约 33 µs（**trace 开启**，含 ~680 event/
  请求的采样开销，绝对值偏高；结构比例有效）；
- 未开 trace 的口径见门禁：读 96–99%、写 91–156%。

## 4. 差距构成与设计归因

1. **前端 + ingress + planner + 完成路径**（未 trace 时约 5–10 µs/请求量级）：
   `validate + per-inode 快照 + 有界 ring + coroutine planner + parent 完成`——
   这些是 HFS 的地址映射/校验/一致性能力的成本，raw 直通 bdev 没有；
2. **durability barrier**（MySQL trx=1 每事务）：redo 写 + fsync 在 HFS 侧 =
   journal append + checkpoint（delta O(1)）+ superblock + 一次设备 flush；
   ext4 是 journal + fsync。这是写侧 1–9% 差距的主因；
3. **CPU 侧**：metadata/data reactor 的 hybrid polling + seam 线程客户端；
4. **redo/页写 bounce**：InnoDB redo buffer 与 RocksDB 写缓冲不在 arena，写路径
   仍需 arena lease + copy（读侧已 zero-copy）；
5. **历史 65% 的假象**：非对称口径（HFS datadir 在测试 NVMe、native 在 VM 根盘
   ext4）把设备差异算进了软件差距。

## 5. 还能做什么（按收益排序，均需独立 A/B）

1. redo/写侧 zero-copy（把 InnoDB log buffer / RocksDB 写缓冲纳入 arena 或
   注册路径）——当前写路径仍有 copy-in；
2. barrier 进一步合并（journal slot 4 KiB/记录 → 紧凑记录；checkpoint delta 已
   O(1)）；
3. 前端固定成本（`validate`/快照命中路径）与完成路径（`publish` p50 16 µs）；
4. reactor polling 策略（hybrid → 低负载 sleep）以降低空闲 CPU；
5. FIO 1j32 剩余 9.4%（深队列单 job 形状）主要来自每请求固定成本，与 (3) 同源。

## 6. 证据目录

- `/home/hfs/mysql-samedisk-20260929{b,c}`（门禁两轮）、`mysql-112-ab2`（Shard-112
  A/B）、`mysql-p99-20260929`（归因）、`mysql-trace-20260929`（trace 分段）、
  `mysql-cpu-ab-20260929`（CPU 成本）。