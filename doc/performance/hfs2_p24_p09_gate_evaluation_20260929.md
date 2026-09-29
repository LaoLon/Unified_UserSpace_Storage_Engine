# HFS 2.0 P2-4 / P0-9① 门禁评估（2026-09-29）

## 1. P2-4 per-shard / per-class arena：门禁未满足

- 门禁定义（台账 §5 P2-4）："profile 证明 central arena 仍是瓶颈才启动"；
- 零拷贝（P2-1）后的 arena 流量：
  - direct 读/写不再取 lease，但每次 direct 子请求仍调用 `ownsRange`
    （同一个 central mutex + 地址有序索引，~50–100 ns）；
  - 最重口径的 direct 子请求率：RocksDB 32 线程 readrandom 167k ops/s、
    MySQL async 提交 0.1–0.2 M/s；
  - bounce lease（redo/未对齐/RMW）同量级；
- 折算：central arena 的总 mutex 占用 ≈ **2–4% 单核**；`DmaBusy`/`dma_fail`
  在所有实测口径为 0（无预算耗尽）；
- 结论：**不启动 P2-4**；再评估条件：arena 相关 CPU 占比 >10%（perf）、
  `DmaBusy` 非零、或 direct 子请求率增长 >5×。

## 2. P0-9① intrusive in-flight sink ref：门禁未满足

- 门禁定义：需要 profile 证据证明 sink/handle 的 shared_ptr 引用计数是成本；
- 每请求的引用计数操作：`sinkOwner` 复制/释放 + `IoHandle` owner
  ≈ 2–4 次原子 RMW ≈ **20–40 ns**；
- 请求总成本：FIO 1j32 ≈ 11 µs/IO、RocksDB readrandom ≈ 6 µs/op、
  MySQL commit ≈ 1 ms（fsync 口径）→ shared_ptr 占比 **<0.5%**；
- 前端固定成本（trace）：`frontend_fixed_latency` p50 0.80 µs（含
  validate+snapshot+submit），intrusive ref 的收益上限 ≈ 40 ns/请求
  （<5% 固定成本、<0.5% 请求成本）；
- 结论：**不实施**；再评估条件：perf 显示引用计数操作 >5% CPU，或出现
  无锁 ingress 之外的 sink 生命周期问题。

## 3. 证据

- `hfs2_p16_p06_evaluation_20260929.md`（arena 流量与 mutex 折算）；
- `hfs2_gate_remeasure_20260929.md` §2（167k ops/s）、
  `hfs2_mysql_async_append_20260929.md`（async_writes）；
- trace 运行 `frontend_fixed_latency p50=0.80 µs`（§7.3 点位）。
