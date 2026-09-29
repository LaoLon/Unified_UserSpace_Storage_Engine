# HFS 2.0 增量 checkpoint 设计评审（P2-2，2026-09-29）

> 台账：`doc/review/hfs2_architecture_optimization_review_20260928.md` §5 P2-2。
> 结论：分两步实施——**(a) 冻结视图 + 锁外编码**（不改磁盘格式，本轮实施）；
> **(b) dirty-object 增量 checkpoint**（格式版本 +1，后续 shard）。同时记录
> “异步 checkpoint”作为 (b) 之后的候选，不在本轮范围。

## 1. 现状与证据

- `Namespace::saveSnapshot`（`src/metadata/namespace.cpp`）持 namespace 全局
  `mutex_`，对每个 inode 逐字段编码、复制目录项与 extent、序列化 allocator
  位图；`Journal::checkpointLocked`（`src/storage/journal.cpp:1141`）在
  checkpoint 路径调用它，设备写（`writeCheckpointAndSuperblock`）已在锁外；
- 停顿来源是**编码本身**（O(N) 字段追加 + 每 inode `extents.snapshot()` 复制 +
  目录名字符串复制），数据集大时是所有元数据操作的 stop-the-world；
- 当前矩阵数据集（RocksDB 2M key、MySQL 10 万行）checkpoint 仅几十 KB，
  看不出问题；风险在设计 §17.1 的 1,000,000 active inode 目标；
- 已有归因点位：`ChkptBegin/End/Bytes`（§7.2）与 `syncCheckpointSnapshot` 计数。

## 2. 方案评估

| 方案 | 结论 | 理由 |
|---|---|---|
| A. 冻结视图 + 锁外编码 | **采纳（本轮）** | 不改格式；锁内只做扁平拷贝（标量 + 目录项 + extent 描述符 + 位图），编码与设备写全在锁外；锁内时间约降为原来的 1/2–1/3 |
| B. dirty-object 增量 checkpoint | **采纳（下一格式版本）** | 锁内 O(dirty) 而非 O(N)；需要 delta 链、journal 保留窗口、superblock/格式版本升级 |
| C. 异步 checkpoint（barrier 不等 checkpoint） | 候选（B 之后） | 需要 journal 保留到 checkpoint durable，`journal_mb` 预算与恢复协议更复杂；B 之后 checkpoint 变便宜，收益下降 |
| D. 目录索引 COW / RCU | 候选 | 目录名复制是 (a) 锁内主要成本之一；改动面大，留待 B 的 dirty 目录项机制一并考虑 |

## 3. 契约

### 3.1 (a) 冻结视图 + 锁外编码（不改磁盘格式）

1. `Namespace::freezeCheckpointViewLocked(txid)` 在锁内捕获
   `CheckpointView`：header（txid/logicalEpoch/durableEpoch/namespaceBytes）、
   inode 标量（扁平 POD 数组 + entries/extents 的 begin/count 索引）、目录项
   （name+child 扁平数组）、extent 描述符（扁平数组）、`freeSlots`、
   allocator 位图字节；
2. `snapshotTxid_` 在冻结时发布；
3. `encodeCheckpointView(view, out)` 在锁外按现有字段顺序编码，磁盘格式与
   `kNamespaceSnapshotVersion` 不变；
4. 一致性：view 是一次性深拷贝；冻结后的 mutation 不影响 view；编码失败与
   设备写失败的既有回退（`rollbackPreparedDurableEpoch` + `requireCheckpoint`）
   不变；
5. 内存：峰值 ≈ view + 编码缓冲（同阶）；1M inode 目标下需在 §17.1 预算内
   复核（当前 inode 元数据预算 1 KiB/inode）；
6. 度量：新增 `syncCheckpointFreeze` 计数与 `ChkptFreeze` trace，报告冻结/编码
   占比；`syncCheckpointSnapshot` 保持为总时间。

### 3.2 (b) dirty-object 增量 checkpoint（格式版本 +1）

1. dirty 集合：inode 标量版本、目录项增删（name+child）、extent 变更
   （logical→physical 描述符）、allocator 位图范围；
2. checkpoint 链：最近一次 base（全量）+ 自 base 以来的 delta 序列；每条
   delta 带 base txid 与 sequence；
3. 恢复：装载最新**有效** base，按 sequence 应用 delta，再从 base txid 之后的
   journal 记录 replay；superblock 指向链头；
4. journal 保留：delta 未 durable 前不得把 `tail` 推进到链覆盖的 sequence 之前；
   `journal_mb` 预算需覆盖该窗口；
5. 发布顺序：delta 记录 → superblock → 一次设备 flush；失败时链不变；
6. 容量：`checkpoint_mb` 需容纳 base + deltas + 余量；达到阈值触发新 base；
7. 兼容：格式版本 +1 只在 mount 时按版本分派；旧卷可被新二进制读取，
   新卷对旧二进制 fail-closed（version gate）。

## 4. 实施计划与验收

1. **本轮（a）**：实现冻结/编码拆分与计数；验收 = CTest 29/29、RocksDB L4
   readrandom 与 MySQL 短矩阵无回退、崩溃矩阵通过；用 `ChkptFreeze`/
   `syncCheckpointSnapshot` 报告占比；
   **已完成（2026-09-29）**：`freezeCheckpointViewLocked` + `encodeCheckpointView` +
   observer 注入；RocksDB 50k keys 实测 `ckpt_freeze=7/17 µs`、
   `ckpt_snapshot=13/30 µs`、`meta_write=115/212 µs`（设备写本就在锁外）；
   CTest 29/29；MySQL SIGKILL 崩溃矩阵通过（`corruption=none`）。证据见
   `doc/performance/hfs2_checkpoint_freeze_20260929.md`。
2. **下一 shard（b）**：先写格式版本设计（§10.4 已给契约），实现 dirty 跟踪 +
   delta 链 + 恢复组合；验收 = 1M inode 规模下 checkpoint 锁内 O(dirty)、
   crash 矩阵、`hfsctl check`、容量预算；
3. 1M inode 的 metadata microbenchmark（设计 §17.1 要求 ≥20,000 ops/s）作为
   (a)/(b) 的量化前置，纳入同一 shard。

## 5. 风险

| 风险 | 缓解 |
|---|---|
| 冻结拷贝的内存峰值 | view 编码后立即释放；1M inode 预算复核；必要时按 inode 分块编码 |
| 冻结期间 mutation 改变一致性 | view 深拷贝、一次性捕获；`snapshotTxid_` 冻结时发布 |
| 回退路径不一致 | 沿用既有 prepared-durable-size 回退与 `requireCheckpoint`；崩溃矩阵覆盖 |
| (b) 的 delta 链损坏 | 链头校验 + 回退到上一个有效 base；`hfsctl check` 报告链状态 |
| journal 保留窗口放大写放大 | 容量预算 + 阈值触发 base；测量 `journal_mb` 余量 |

## 6. 参考

- `doc/design/hfs2.0.md` §10.2/§10.4、§17.1
- `src/metadata/namespace.cpp`（saveSnapshot/loadSnapshot）、
  `src/storage/journal.cpp`（checkpointLocked）
- `doc/review/hfs2_architecture_optimization_review_20260928.md` §5 P2-2、§7 trace
