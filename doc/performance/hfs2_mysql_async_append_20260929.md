# HFS 2.0 MySQL 追加写异步化（Shard-112）实施与验收（2026-09-29）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

> 前置：`doc/performance/hfs2_mysql_write_p99_attribution_20260929.md`（归因：
> 约 20% 的 InnoDB 追加写被异步固定覆盖契约拒绝 → 同步消息环 → 写 p99）。
> 实现：`mysql/patches/hfs_seam_shard112_async_append.patch`（seam `write`）。

## 1. 机制

seam 的 `hfs_vfs::write` 在尝试异步快路径前，对**增长写**
（`offset + n > logicalSize`）：

1. `file->allocate(offset, n)`：预留 extent、零填充、提交并 `setSize(offset+n)`——
   与同步 `File::write` 的增长语义一致，**不改变文件 size 语义**；
2. 然后走既有异步快路径（此时 `offset + n <= logicalSize`，满足固定覆盖契约）。

已提交 extent 内的追加（常见情况）只做一次 `setSize` 元数据更新，数据 I/O 回到
异步路径；核心契约、`.conf` schema、磁盘格式均不变。

## 2. 验收（同会话 pre/post，每轮全新 datadir，write_only 15 s，8 线程，trx=1）

| 轮次 | 写 TPS | p99 ms | async_fallbacks |
|---|---:|---:|---:|
| pre 1 | 2115.83 | 11.04 | 16,368 |
| post 1 | 2562.40 | 9.91 | **0** |
| post 2 | 2846.11 | 9.06 | **0** |
| pre 2 | 2501.72 | 10.27 | 19,785 |

- **写 TPS：均值 2704.3 vs 2308.8 → +17.1%**；
- **p99：均值 9.49 ms vs 10.66 ms → −11.0%**；
- **`async_fallbacks` 16k–20k → 0**（机制直接证据）；
- `async_writes` 从 ~58k 升到 ~94–109k（追加写回到异步路径）；
- 每轮 `hfsctl format` + `initialize` + `prepare` + `write_only`，交替顺序
  （pre-1, post-1, post-2, pre-2）；
- 证据：`/home/hfs/mysql-112-ab2`、`/home/hfs/mysqld-{pre,post}112`。

## 3. 边界

- 收益集中在**增长阶段**（初始化、数据装载、redo/undo 扩展）：文件稳定后
  `async_fallbacks` 自然为 0，稳态写路径不变；
- 未改动核心异步契约（固定覆盖），未新增 public ABI；`build_mysql_vm.sh` 增加
  shard-112 marker；
- 同盘门禁的写 p99（117–129% vs ≤110%）需在含增长阶段的完整门禁里复测
  （本文的 write_only A/B 已显示 −11% p99；完整门禁复测作为后续验收项）。