# HFS 2.0 P2-3 metadata actor 收敛评估与迁移计划（2026-09-29）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

> 设计依据：`hfs2.0.md` §4.2 模块布局、§6.4/§7.2 的 actor ownership 表
> （"namespace/journal/checkpoint → metadata actor"、"extent map 由 metadata actor
> 维护，data shard 持有 immutable/versioned snapshot"、"路径 API 只进 metadata
> actor"）。结论：**这是阶段级设计符合性项，当前性能门禁未触发**，按本文 §3 的
> 步骤在后续阶段实施。

## 1. 现状与差距

- 现状：`Namespace` 的 mutation（create/remove/rename/setSize/reserve/commit）由
  **调用者线程**直接执行，用 namespace 全局 `mutex_` 串行化；journal/checkpoint
  的 barrier 由发起 sync 的线程驱动（metadata reactor 只做控制 I/O）；
- 目标模型：mutation、journal intent、durability request 收敛到 **metadata actor**
  单线程执行；extent map/namespace 由 actor 独占；data shard 与前端只持有
  immutable/versioned 快照；
- 差距 = 所有权模型（单线程 actor vs 全局锁 + 多线程进入），不是锁性能。

## 2. 性能门禁测量（2026-09-29，trace 开启，`MetaLockWait` ≥1 µs）

MySQL `oltp_write_only`（8 线程，10 s，trx=1，trace_sample=1）：

| 事件 | 计数 |
|---|---:|
| `FeSubmitBegin`(59) | 66,079 |
| `SnapHit`(53) / `SnapBuild`(54) | 53,218 / 12,861（命中率 80.5%） |
| **`MetaLockWait`(56) ≥1 µs** | **99**（占提交 **0.15%**） |

- 早前 RocksDB 20k keys trace：`MetaLockWait` 在 1 µs 阈值下为 **0**；
- 结论：当前负载下 namespace 锁**无实质争用**（P1-4 的 O(log E) + 快速 mutation
  路径已消除原 O(E) 拷贝），**P2-3 的性能收益未被证据支持**；
- 再评估条件：`MetaLockWait` 占比 >1% 的 metadata 操作、metadata op 率增长 >5×、
  或出现 metadata 锁导致的 p99 恶化。

## 3. 迁移计划（阶段级，供排期）

1. **Ingress**：metadata reactor 上增加有界 actor ingress（MPSC 队列 + 完成通知，
   复用现有 `Task`/`Operation` 机制）；仅 control phase 切换开关；
2. **Mutation 路由**：`Namespace` 的 mutation API 改为提交 actor 任务并 await；
   actor 内单线程执行 → 移除全局 `mutex_`（读侧改快照）；
3. **读侧 immutable 视图**：actor 发布 per-inode versioned 快照（复用 P1-1 的
   dataVersion + 现有 snapshot cache）；前端/data shard 命中不进入 actor，miss 时
   向 actor 请求一次视图；
4. **Durability**：journal append/checkpoint/epoch publish 统一在 actor 上串行
   （与 §10.4 的 delta checkpoint、§9.3 的 staged records 合并）；
5. **边界**：关闭顺序（client → actor drain → journal → runtime）、取消/超时
   （actor 任务的取消与迟到完成）、mount replay（actor 启动后先 replay 再开
   ingress）；以上若改变 public Task 完成线程/取消语义，需先修订设计相应小节；
6. **验收**：CTest 29/29、MySQL SIGKILL 崩溃矩阵、同盘门禁与 FIO 同形状不回退、
   `MetaLockWait` 与 `frontend_fixed_latency` 不恶化；迁移按 mutation 类别分批
   （先 setSize/commit，再 create/rename），每批独立 A/B。

## 4. 证据

- 本文 §2 的 trace 计数（`/home/hfs/mysql-trace-20260929`）；
- `hfs2_p09_remainder_20260929.md`（P1-4/快照复用已降低锁路径成本）；
- 设计 §4.2/§6.4/§7.2、§9.3、§10.4。