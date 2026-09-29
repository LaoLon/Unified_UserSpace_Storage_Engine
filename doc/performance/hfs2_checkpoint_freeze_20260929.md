# HFS 2.0 checkpoint 冻结/编码拆分（P2-2a）实施与验收（2026-09-29）

> 设计：`doc/design/hfs2.0.md` §10.4；评审：
> `doc/review/hfs2_incremental_checkpoint_review_20260929.md`。
> 实现：`src/metadata/namespace.{hpp,cpp}`（`CheckpointView` + 冻结/编码拆分）、
> `src/core/engine.cpp`（observer 注入）、`src/common/{stats,trace}.hpp`
> （`syncCheckpointFreeze` / `ChkptFreeze`）。

## 1. 机制

1. `freezeCheckpointViewLocked(txid, view)` 在 namespace 锁内把所有 checkpoint
   需要的状态拷贝为扁平 `CheckpointView`：header（txid/logicalEpoch/durableEpoch/
   namespaceBytes）、inode 标量数组（含 entries/extents 的 begin/count 索引）、
   目录项（name+child）、extent 描述符、`freeSlots`、allocator 位图字节；
2. `snapshotTxid_` 在冻结时发布；
3. `encodeCheckpointView(view, out)` 在锁外按既有字段顺序编码，磁盘格式与
   `kNamespaceSnapshotVersion` 完全不变（`namespace_test` 的 round-trip 覆盖）；
4. 归因经 `Namespace::setCheckpointFreezeObserver` 注入（metadata 层不依赖
   stats/trace）：engine 记录 `syncCheckpointFreeze` 与 `ChkptFreeze`；
5. 失败/回退路径不变：`rollbackPreparedDurableEpoch` + `requireCheckpoint`。

## 2. 验证

- **CTest 29/29**（`namespace_test` 快照 round-trip、`journal_test`、
  `journal_device_test`、`format_mount_test`、`file_data_test` 等）；
- **RocksDB observer**（50k keys，8 线程，fill+compact+readrandom，
  `HFS_DIAG_SYNC_STATS=1`）：

```
hfs: sync phases us avg/max: barrier=168/340 device_flush=33/82 ckpt_freeze=7/17
     ckpt_snapshot=13/30 meta_write=115/212 ckpt_flush=0/0
```

  - 锁内冻结 ≈7 µs（max 17 µs），快照总时间 ≈13 µs → 编码 ≈6 µs 已移出锁；
  - `meta_write`（checkpoint 记录 + superblock 设备写）≈115 µs，本就在锁外；
  - 与改动前的定性对比：锁内时间由“全量编码”变为“冻结拷贝”，总量略增（多一次
    标量/索引拷贝），这是 (a) 的既定权衡（§10.4：总字节仍为 O(N)）；本轮未在
    同会话重建 pre 库做绝对对比，占比口径以 `ckpt_freeze`/`ckpt_snapshot` 为准；
- **崩溃矩阵**（MySQL SIGKILL：在线 DDL 中断 + torn page 中断）：
  `rows=131072` 一致、两次 `hfsctl check` 均
  `corruption=none`（checkpointA/B valid、journal head=tail）。

## 3. 边界

- (a) 仍为 O(N)：总字节与磁盘格式不变，只是把编码移出锁；1M active inode 下
  锁内时间 ≈ 冻结拷贝（扁平 memcpy + 目录名字符串复制），峰值内存 ≈
  view + 编码缓冲（同阶，需在 §17.1 预算复核）；
- 目录名复制是冻结锁内的主要成本之一（与 (b) 的 dirty 目录项机制一并考虑）；
- (b) dirty-object 增量 checkpoint（格式版本 +1、delta 链、journal 保留窗口）
  按 §10.4 契约与评审文档实施；本轮不含 (b)；
- 1M inode 的 metadata microbenchmark 仍待建立（设计 §17.1），
  作为 (a)/(b) 的量化前置。
