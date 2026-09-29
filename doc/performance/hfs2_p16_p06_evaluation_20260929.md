# HFS 2.0 P1-6 / P0-6 子项评估（2026-09-29）

## 1. P1-6 balanced active index：触发门禁未满足

- 复核（当前树）：`scheduler_test` 输出
  `scheduler monotonic: total=5000 window=4096 ns_per_node=6323`
  （4096 深窗口 6.3 µs/node，与 2026-09-28 的 5.9 µs/node 同档）；
- 生产窗口是几十个 node（scheduler microbenchmark 作为触发门禁保留），线性扫描
  成本远低于门禁 → **balanced active index 不实施**；
- 再评估条件：生产 profile 出现 scheduler 窗口 >1k node，或 `SchedWait` 占比
  显著上升。

## 2. P0-6 `reusableReadLease` per-shard cache：不值得

- 现状：mount 级 central arena（一个 mutex + 地址有序索引），lease 获取/释放
  ~50–100 ns/对；`reusableReadLease`（16 KiB 单请求复用）已覆盖最常见的
  coalesced 读；
- 实测流量：零拷贝（P2-1）后，**direct 读不再取 lease**（InnoDB buffer pool /
  RocksDB kFSBuffer 直通），剩余 lease 主要来自 bounce 写（redo/未对齐）与 RMW，
  量级 ≈ async 提交率 **0.1–0.2 M/s**（MySQL `async_writes=94–109k`、RocksDB
  32 线程 readrandom 167k ops/s）；
- 折算：central arena 的 mutex 占用 ≈ 0.1–0.2 M × ~100 ns ≈ **1–2% 单核** →
  不是瓶颈；per-shard cache 只省这部分，且引入分区/预算复杂度；
- 再评估条件：lease 率增长 >10×（例如 direct 路径回退或新的 bounce 前端），或
  `DmaBusy` 计数出现非零。

## 3. 证据

- `scheduler_test`（本文件 §1 输出）；
- `doc/performance/hfs2_gate_remeasure_20260929.md` §2（RocksDB 167k ops/s）、
  `hfs2_mysql_async_append_20260929.md`（`async_writes` 94–109k）；
- 零拷贝消除 direct lease：`hfs2_mysql_zero_copy_20260929.md`、
  `hfs2_rocksdb_kfsbuffer_20260929.md`。
