# HFS 2.0 journal-backed 增量 checkpoint（P2-2b）实施与验收（2026-09-29）

> 设计：`doc/design/hfs2.0.md` §10.4 (b)；评审：
> `doc/review/hfs2_incremental_checkpoint_review_20260929.md`。
> 实现：`src/storage/journal.cpp`、`include/hfs/journal.hpp`、`src/core/engine.cpp`、
> `src/metadata/namespace.cpp`。

## 1. 机制

- checkpoint 两种记录（`CheckpointHeader.kind`，`formatVersion=2`；v1 槽位按 full 读）：
  - **full base**：全量快照（P2-2a 的冻结/编码），写在**非 base** 槽位；
  - **delta**：`DeltaPayload{baseSlot, baseTxid, baseSequence}`，1 个 metadata block；
- journal 就是 delta：delta 只把当前 `head` 记入 checkpoint，`tail` 保持 base
  sequence；恢复 = 装载 base 快照 + 从 base txid 起 replay（无需 per-object dirty）；
- **shape 变更约束**：journal 记录格式（objectId/arg0/arg1）不能携带名字，
  Create/Link/Unlink/Rename 不写 journal；shape 变更置 `checkpointRequired` 并
  经 `Journal::requireFullCheckpoint()` 强制下一次写 **full base**，delta 只覆盖
  无 shape 变更的窗口（SetSize/Alloc/Free/Truncate/SetTimes）；
- full 时机：无 base、`forceFullPending`（shape 变更）、窗口 ≥ `force_percent`(80%)
  ring、ring 将满（append 强制）；
- staged 记录：delta 前由 engine 先写入 journal（`nextCheckpointWillBeFull()` 判定
  full 时沿用旧优化，快照覆盖）；
- 触发节奏：`needsCheckpoint` = 自上次 checkpoint 的 append ≥ `trigger_percent/2` ring。

## 2. 验收

- **CTest 29/29**（`journal_test` 更新为 delta 语义：mock 变更必须经 journal）；
- **MySQL SIGKILL 崩溃矩阵**（format + initialize + 在线 DDL 中断 + torn page 中断）：
  `rows=131072` 一致、两次 `hfsctl check` `corruption=none`；矩阵日志中
  **361 delta / 442 full**，delta 恢复路径被 SIGKILL 恢复覆盖；
- **1M inode 基准**：create 1,000,000 级（10 次 shape 触发 full + commit 阶段
  1 次 delta）；full 成本（1M 快照约 110 MB）：`ckpt_freeze` 42 ms avg / 76 ms max、
  `ckpt_snapshot` 104 ms avg / 190 ms max、`meta_write` 24 ms avg；delta 记录
  4 KB + superblock（写 ~0.1 ms 级）；
- **fresh-process 恢复**（1M 状态、最大窗口）：`mount_seconds=2.05 s`
  （checkpoint 装载 + replay；设计 §17.1 目标 mount ≤10 s / replay ≤2 s，处于边界）。

## 3. 期间发现并修复的缺陷

1. **delta 丢 staged 记录**：`checkpointPhase` 原以 `clearPendingRecords()` 丢弃
   staged 记录（假定快照覆盖），delta 下这些记录既不在快照也不在 journal →
   恢复报 `replayed Alloc inode is missing`（MySQL init 后 mount 失败）。修复：
   delta 前把 staged 记录写入 journal；full 保持原优化。
2. **shape 变更不可 replay**：Create/Link/Unlink/Rename 从未写 journal（记录格式
   无名字），delta 无法覆盖 → 修复：shape 变更强制 full base（§1 约束）。
3. **bug #46**（同进程 stop→mount 在重 sync 负载后崩溃）：**pre-(b) 已存在**
   （回退 HEAD 版本复现），未修复；生产路径每进程 mount 一次不受影响。

## 4. 边界

- delta 收益取决于窗口内是否有 shape 变更：稳态写负载（MySQL DML/RocksDB
  overwrite）以 delta 为主；create/DDL 密集窗口仍写 full；
- replay 窗口 ≤ `force_percent` ring（256 MiB journal → 1M 状态恢复 2.05 s）；
- 每个 delta 只写 1 块 + superblock（O(1)），full 频率 ≈ 每 `force_percent` ring
  或每次 shape 变更；
- 同进程 remount 的工具需规避 bug #46（用 fresh process 验证恢复）。
