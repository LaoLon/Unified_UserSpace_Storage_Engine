# HFS 2.0 per-File 快照缓存争用评估（P1-1 子项，2026-09-29）

> 背景：P1-1 主体已实施（per-inode dataVersion + 快照缓存）；子项"无锁发布/RCU
> 未做（per-File mutex 保留）"需要一个是否值得实施的证据判断。

## 1. 结论

**保留 per-File mutex；不实施无锁发布/RCU。** 依据：

- 临界区 = `dataVersionFast` 读取 + `asyncCache`（shared_ptr）复制 + 版本比较
  ≈ 20–40 ns；串行化容量约 25–50 M lookup/s；
- 实测最重口径的快照查询率：
  - RocksDB 32 线程 `readrandom`：**167,591 ops/s**（每 op 一次 async submit →
    一次快照查询）→ 占串行容量 **≈0.3–0.7%**（约 150–300× 余量）；
  - MySQL 8 线程写负载：async 提交约 100–170k/s（`async_writes` 实测）→ 同量级；
- 已测 A/B 佐证：FIO 同步 16 线程复用快照缓存 +0.09%（噪声内），trace 的
  `frontend_fixed_latency` p50 = 0.80 µs（含 validate + 快照 + submit，8 线程）；
- 替代方案不划算：libstdc++ 的 `std::atomic<std::shared_ptr<T>>` 是 **spinlock**
  实现（非 lock-free），争用时比 mutex 更差；真正的 RCU/hazard pointer 复杂度高、
  收益上限被上面的余量否决。

## 2. 复现口径

- 查询率：`doc/performance/hfs2_gate_remeasure_20260929.md` §2 的 HFS 32 线程
  `readrandom` 167,591 ops/s；`hfs2_mysql_async_append_20260929.md` 的
  `async_writes=94–109k`（15 s 口径）；
- 临界区构成：`src/core/client.cpp` 的 `cachedSnapshot()`（version load + mutex +
  shared_ptr copy）；
- 固定成本分布：`doc/performance/hfs2_*` trace 运行（`frontend_fixed_latency`
  p50 0.80 µs / p99 10.5 µs）。

## 3. 边界与再评估条件

- 若未来出现单 File 上 >10 M lookup/s 的口径（当前最重 0.17 M/s），或
  `cachedSnapshot` 的临界区扩大到包含分配/构建，再评估无锁发布；
- 该结论不改变 P1-1 主体的实现（版本键控缓存 + churn 回落 `buildSnapshot`）。
