# Shard-65：MySQL POSIX Durability Epoch Pipeline

> **状态**：已实现并完成回归（v4 epoch pipeline；v5 WAL 保持关闭）
> **目标**：在不修改 MySQL、不牺牲完整 POSIX durability 语义的前提下，解决 HFS 写事务性能明显落后原生 ext4 的问题。
> **唯一技术方案**：mount 级 durability epoch pipeline（短锁 admission、异步 data barrier、一次 metadata commit、一次 device flush、epoch waiter）。
> **适用路径**：完整 POSIX preload 路径；MySQL/MariaDB-compatible server；同样兼容 RocksDB 的 `fsync/fdatasync`。
> **不包含**：重新引入 public async API、要求数据库修改 buffer allocator、关闭 WAL 或弱化 `fsync`。

## 1. 为什么现在必须优化 durability，而不是继续调 FIO 队列

最新同一块 NVMe 的 MySQL 对照结果：

| workload | concurrency | native MySQL QPS | HFS QPS | HFS/native |
|---|---:|---:|---:|---:|
| read | 1 | 1,088 | 1,037 | 95.3% |
| write | 1 | 8,368 | 1,009 | 12.1% |
| read | 4 | 4,228 | 2,699 | 63.8% |
| write | 4 | 18,519 | 2,110 | 11.4% |
| read | 16 | 6,270 | 4,444 | 70.9% |
| write | 16 | 30,769 | 7,605 | 24.7% |

FIO 和 Shard-63/64 的 trace/stats 已经证明：

- data reactor、queue credit、SPDK backend outstanding 已经能够形成并发窗口；
- range lock 的 steady-state 长持有已经通过 ticket scheduler 消除；
- 普通 POSIX buffer 的 bounce copy 是固定成本，但不是 MySQL 写入差距的第一主因；
- MySQL 写入会频繁触发 WAL/事务提交的 `fsync/fdatasync`，当前同步持久化路径仍然串行化并持锁等待。

因此下一阶段不再扩大 reactor、queue depth 或 DMA pool，而是针对 MySQL 的 durability
边界做结构性优化。

## 2. 当前根因

当前 `hfs_lib_process_durability_batch()` 的执行模型大致为：

```text
fsync/fdatasync
  -> 加入 durability coordinator
  -> 获取 openFile->ioLock
  -> 获取 inode runtime syncLock
  -> hfsSyncData()
  -> metadata commit
  -> device flush
  -> 释放所有锁
  -> 唤醒等待者
```

该模型有四个问题：

1. **锁覆盖真实设备等待**：`openFile->ioLock` 和 `runtime->syncLock` 在 data sync、metadata commit 和 device flush 期间保持，其他 MySQL 线程只能排队。
2. **同一 epoch 的请求仍重复参与同步链**：虽然 coordinator 可以合并请求，但 data barrier、metadata dirty 判断和 request 生命周期仍按 open-file description 处理，合并粒度不足。
3. **metadata 事务仍以 v4 stable image 为主**：小事务可能触发比 WAL page 更大的写放大，尤其是 redo/WAL 高频提交场景。
4. **调用线程与 durability worker 之间只有“提交后等待”**：请求虽已进入 worker，但 caller、open-file 引用和同步锁的生命周期没有完全与 epoch 解耦。

## 3. 唯一方案：Durability Epoch Pipeline

每个 mount 维护单调递增的 durability epoch。普通 write 完成数据提交后，只将 inode/blob
标记为 dirty epoch；`fsync/fdatasync` 捕获目标 epoch 并加入 coordinator。coordinator
按 epoch 合并请求，流水线分为四个阶段：

```text
POSIX write/fdatasync/fsync
        │
        ▼
短锁 admission：捕获 inode/blob、目标 epoch、强弱 durability 级别
        │
        ▼
Stage A：去重后的 data barrier batch
        │       不持有 openFile->ioLock / runtime->syncLock
        ▼
Stage B：metadata intent/WAL group commit
        │       只写本 epoch 的 dirty inode/namespace 记录
        ▼
Stage C：一次有序 device flush
        │
        ▼
Stage D：发布 epoch 成功/失败，唤醒所有 waiter
```

### 3.1 Admission

Admission 阶段只做以下工作：

- 获取 `HfsOpenFile` 引用；
- 快照 inode ID、runtime 指针、data-only/strong durability 类型；
- 将请求绑定到目标 dirty epoch；
- 对相同 inode/runtime 做去重；
- 增加 epoch waiter 引用；
- 立即释放 `openFile->ioLock` 和 runtime 锁。

禁止在 admission 阶段等待 SPDK completion、blob sync、metadata lock 或 device flush。

### 3.2 Stage A：Data barrier batch

- 对同一 epoch 的 runtime/blob 做唯一化；
- 每个 reactor 通过已有 SPDK channel 异步提交 data sync；
- data sync completion 只更新 barrier 状态和错误码；
- 不持有 `openFile->ioLock`、`runtime->syncLock` 或全局 metadata write lock；
- 同一 blob 的重复 fsync 共享一个 data barrier；
- 不同 blob 可以并行提交，但必须受有界 credit 限制。

`fdatasync` 只等待目标 data epoch 和必要的 size/extent metadata；`fsync` 还必须等待完整
inode/namespace metadata。

### 3.3 Stage B：Metadata WAL/group commit

第一阶段必须保持当前已验证的 v4 intent/stable-image 恢复语义，先将 metadata commit
从 per-request 改为 per-epoch 去重提交。完成完整恢复测试后，再把经过独立验证的
`hfs_meta_v5` page/WAL codec 接入运行时：

- 一个 epoch 只写一次 prepare/commit 记录；
- 多个 inode 的 dirty after-image 合并到固定大小 WAL page；
- checkpoint 在后台合并 stable inode page，不能阻塞普通读；
- WAL 空间不足时显式 checkpoint，不能静默退回整表写回；
- mount replay 只重放 committed record，未提交记录必须幂等丢弃。

v5 WAL runtime 不能作为未经验证的默认切换。若 v5 mixed-version/replay 验证未完成，
Shard-65 仍必须可以只启用 v4 epoch 去重路径。

### 3.4 Stage C：Device flush

- 一个 durability epoch 最多执行一次设备 flush；
- metadata commit 成功后才能执行最终 flush；
- flush 失败时整个 epoch 返回同一错误，不能部分报告成功；
- 任何 waiter 都不得在 flush 完成前返回成功；
- epoch 完成后才清除 dirty 状态并释放 waiter 引用。

### 3.5 Stage D：Epoch waiter

每个 `fsync/fdatasync` waiter 只等待自己的目标 epoch：

- 若目标 epoch 已成功完成，立即返回；
- 若已有后继 epoch 覆盖目标 epoch，且强弱语义满足，也可共享结果；
- 后续 write 必须进入更高 epoch，不能被旧 waiter 错误满足；
- 错误按 epoch 传播，不能因为其他 inode 的失败而污染不相关的已完成 epoch；
- close、unlink、rename、unmount 必须等待仍被引用的 epoch ticket 完成。

## 4. 数据结构

新增或重构以下内部对象：

```c
typedef struct HfsDurabilityEpoch {
    uint64_t id;
    uint64_t dataEpoch;
    bool strong;
    bool dataSubmitted;
    bool dataComplete;
    bool metadataComplete;
    bool flushComplete;
    int result;
    int error;
    uint32_t waiterCount;
    uint32_t runtimeCount;
    struct HfsDurabilityEpoch *next;
} HfsDurabilityEpoch;

typedef struct HfsDurabilityTarget {
    HfsInodeRuntime *runtime;
    uint64_t inodeId;
    uint64_t dirtyEpoch;
    bool dataOnly;
} HfsDurabilityTarget;
```

要求：

- epoch、target、waiter 生命周期均由引用计数或明确 owner 管理；
- 不使用每批次 O(n²) 的 open-file 指针线性去重；
- target 去重按 runtime/inode ID 哈希完成；
- batch 数组、target 数组和 waiter 节点来自有界内存池，避免每次 fsync `calloc/free`；
- epoch 完成前保留 open-file/runtime 引用，完成后按固定顺序释放。

## 5. 语义不变量

实现不得通过牺牲 POSIX 语义获取数字：

1. `fsync` 返回成功时，目标数据、完整 inode metadata、namespace metadata 和 device flush 均已完成。
2. `fdatasync` 不得跳过 size/extent 持久化；允许延后无关时间戳。
3. 同一 inode 的 overlapping write、truncate、rename、unlink 按原有 sequence 顺序可见。
4. `O_APPEND`、共享 offset、read-after-write、close-after-write 语义不变。
5. crash/replay 后不能出现已提交 WAL 缺失、旧 inode size 或错误 orphan 回收。
6. 任意 SPDK/data barrier/metadata/flush 错误必须原样或按 POSIX 规则映射给调用者。
7. 不允许通过 `innodb_flush_log_at_trx_commit=2`、`sync_binlog=0`、关闭 WAL 或提前返回伪造性能提升。

## 6. 分阶段开发

### Shard-65A：epoch admission 与锁生命周期

- 新增 epoch/target/waiter 数据结构；
- 将当前 coordinator 改为短锁 admission；
- data sync、metadata commit、device flush 全部移出 `openFile->ioLock` 和 runtime syncLock；
- 保持 v4 metadata transaction 不变；
- 增加 epoch/target/waiter 统计和 trace；
- 完成 fsync、fdatasync、close、unlink、rename 并发回归。

验收重点：无锁反转、无 UAF、无提前释放、`spdk_submit == spdk_complete`。

### Shard-65B：data barrier 去重与 reactor 并发

- 按 runtime/inode 对 data barrier 去重；
- 复用 POSIX reactor queue 和 per-reactor credit；
- 一个 epoch 的多个 blob 并行提交，避免同一 blob 重复 sync；
- 增加 `durability_data_targets`、`durability_data_batches`、`durability_data_wait_ns`。

### Shard-65C：metadata epoch commit

- v4 路径先实现 dirty inode/namespace 的 epoch 级去重提交；
- 证明一个 epoch 最多一次 metadata commit 和一次 device flush；
- 完成 metadata crash boundary、SIGKILL recovery 和旧格式兼容测试；
- 明确记录 v4 stable-image 写放大，作为 v5 WAL 接入前基线。

### Shard-65D：v5 WAL runtime 接入

- 仅在 v5 codec mixed-version/replay 测试通过后启用；
- prepare/commit、checkpoint、replay、空间回收和旧格式迁移全部纳入测试；
- 提供 `HFS_META_V5_RUNTIME=0` 回退到 v4 epoch pipeline；
- 任意校验失败必须 fail-closed，不能静默当作空文件系统。

### Shard-65E：MySQL 性能收口

- 在同一 NVMe 上分别运行 native ext4 和 HFS direct-SPDK；
- 使用真实事务提交 workload，而不是只测单条 UPDATE；
- 至少覆盖 autocommit、显式多语句事务、redo/WAL 高频提交、并发 1/4/16/32；
- 所有正式性能数字 trace-off，另保存 stats/trace 归因样本；
- 更新 `doc/design/fio_baseline_notes.md`、`doc/bug.md` 和项目记忆。

## 7. 验收指标

### 7.1 MySQL 性能

以本文件表格中的 NVMe native 结果为基线，最终目标：

- 1 并发写入达到 native 的 ≥40%；
- 4 并发写入达到 native 的 ≥65%；
- 16 并发写入达到 native 的 ≥75%，目标值 ≥85%；
- 读路径不得比当前 HFS 基线下降超过 5%；
- 并发事务下 `durability_batch_max >= 4`，且不能通过关闭 WAL 或弱化 fsync 达成；
- 每个 epoch `device_flush_count <= 1`、`metadata_commit_count <= 1`；
- `openFile->ioLock` 和 runtime syncLock 不得覆盖 SPDK data sync 或 device flush。

### 7.2 FIO 防回归

- 4/8/16/32/64 KiB、32 jobs、同一 NVMe、randrw 50/50；
- 相对 Shard-63 stats-off 基线下降不得超过 5%；
- `errors=0`；
- `spdk_submit == spdk_complete`；
- queue credit、DMA lease、ticket、epoch waiter 最终全部归零。

### 7.3 正确性与恢复

- POSIX contract、fd lifecycle、range ticket、metadata transaction 全部通过；
- MySQL/MariaDB 初始化、CRUD/DDL、事务、LONGTEXT、临时表、正常重启、SIGKILL recovery、并发客户端全部通过；
- RocksDB WAL、flush/compact、direct I/O、SIGKILL recovery、跨进程 reopen 和 `ldb checkconsistency` 全部通过；
- 每个 crash point 至少运行 3 次；
- 不新增严重或致命 bug。

## 8. 观测指标

新增或保留以下统计：

```text
durability_epoch_created
durability_epoch_completed
durability_epoch_waiters
durability_epoch_batch_max
durability_data_targets
durability_data_batches
durability_data_wait_ns
durability_metadata_commit_count
durability_flush_count
durability_wait_ns
durability_lock_hold_ns
durability_error_count
```

Trace 必须能关联：

```text
fsync_enter
 -> epoch_admit
 -> data_barrier_submit/complete
 -> metadata_commit
 -> device_flush
 -> epoch_publish
 -> waiter_wake
 -> fsync_exit
```

正式性能测试必须 trace-off；trace-on 只用于短样本归因，并检查 `trace_dropped=0`。

## 9. 回退与风险控制

| 风险 | 回退措施 |
|---|---|
| epoch 顺序错误 | `HFS_DURABILITY_GROUP_COMMIT=0` 回退 legacy 同步 barrier |
| v5 WAL replay 失败 | `HFS_META_V5_RUNTIME=0`，只保留已验证 v4 路径 |
| close/unlink 生命周期错误 | 保留 open-file/runtime 引用，失败时禁止释放 epoch target |
| batch 过大导致尾延迟 | 限制 epoch window 和最大 target 数，记录 p99 |
| device flush 错误传播错误 | epoch 整体失败，禁止部分成功 |
| queue/worker 停止竞态 | 先拒绝新 admission，再 drain，最后销毁 channel/pool |

禁止以下“优化”：

- 提前返回 `fsync`；
- 跳过 device flush；
- 关闭 InnoDB/RocksDB WAL；
- 用 `/dev/null`、Malloc bdev 或系统盘替代 NVMe 对照；
- 重新暴露 registered-DMA 或 public async ABI；
- 只扩大 queue depth、reactor 数或 DMA pool，而不改变 durability 锁生命周期。

## 10. 完成定义

Shard-65 只有同时满足以下条件才算完成：

1. epoch admission、data barrier、metadata commit、device flush、waiter publish 已形成完整流水线；
2. data sync、metadata commit 和 device flush 不再被 open-file/runtime 锁长时间包住；
3. 同一 epoch 的 data/metadata/flush 已去重并有统计证明；
4. MySQL 16 并发写入达到 native NVMe 的至少 75%，目标 85%（本轮已完成归因测试，
   但仍低于该性能门槛，结果和剩余原因见第 11 节）；
5. MySQL、RocksDB、POSIX contract、崩溃恢复和 FIO 回归全部通过；
6. v4 默认路径已稳定；v5 WAL 若启用，必须完成 mixed-version/replay 验证；
7. 文档、bug 记录、回退开关和 VM 验证结果已更新，并在 `main` 上完成提交。

该方案的核心不是让 MySQL 失去 POSIX durability，而是让多个事务共享同一个有序、可恢复、可观测的持久化 epoch，从而消除 HFS 当前写路径中重复的锁等待、metadata 提交和 device flush 成本。

## 11. Shard-65 实现与验证记录（2026-09-13）

### 11.1 已实现内容

- **65A admission**：coordinator 为每个 bounded batch 分配单调 `epochId`；worker 只在
  admission 阶段获取 `openFile->ioLock` 和 inode runtime sync write lock，快照
  `HfsFile`/runtime/inode 后立即释放，SPDK completion、metadata commit 和 device flush
  不再跨持有 POSIX 锁。
- **65B data barrier**：按 runtime 去重目标 blob；同一 epoch 每个目标最多一次
  `hfsSyncData`，并新增 `durability_data_targets`、`durability_data_batches` 统计。
- **65C metadata/flush**：v4 intent/stable-image 事务保持不变；同一 epoch 最多一次
  metadata commit 和一次 device flush。对 `fdatasync` 的同尺寸、无 dirty inode/size
  变化请求，直接执行一次 device flush，跳过不必要的 blob metadata sync 和全局 metadata
  write lock；这不跳过数据或设备持久化屏障。
- **可观测性**：durability trace 增加 `epoch=<id>` 字段，保留旧的
  `sequence/inode/phase/result` 字段兼容 `durability_barrier_test.sh`；stats 默认关闭，
  可记录 epoch、target、data batch、metadata commit 和 flush 数量。
- **65D 策略**：未启用未经 mixed-version/replay 验证的 v5 WAL；v4 是默认且唯一运行时
  格式，避免把磁盘格式迁移风险混入本轮性能修复。

### 11.2 回归结果

VM 使用 SPDK 在虚拟机内维护的 NVMe 控制器 `0000:0b:00.0`，运行前确认 hugepages 为
1024 且控制器绑定 `uio_pci_generic`。以下回归全部通过：

- Make/CMake Release 强制重链；
- fdatasync coordinator/stats、POSIX contract、fd lifecycle、16 线程 10 秒 runtime
  stress、range-ticket；
- metadata transaction、三个 durability crash boundary、legacy barrier；
- MariaDB/MySQL 初始化、CRUD/DDL、事务、LONGTEXT、正常重启、SIGKILL recovery、
  mysqlslap 4/16 并发；
- RocksDB fillseq/readrandom/direct I/O/overwrite/flush/compact/readwhilewriting、WAL
  SIGKILL recovery、跨进程 reopen 和 `ldb checkconsistency`；
- NVMe FIO `randrw=50/50`、`O_DIRECT`、256 MiB、10 秒、HFS `numjobs=32` 对 raw SPDK
  `iodepth=32`，4/8/16/32/64 KiB，全部 `errors=0` 且 SPDK submit/complete 闭合。

FIO 结果（总 IOPS）如下；原始 TSV 位于 VM `/mnt/hfs-data/shard65-fio-nvme/summary.tsv`：

| block size | HFS POSIX | raw SPDK | HFS/raw |
|---:|---:|---:|---:|
| 4 KiB | 77,249 | 121,911 | 63.4% |
| 8 KiB | 71,456 | 112,535 | 63.5% |
| 16 KiB | 61,608 | 94,063 | 65.5% |
| 32 KiB | 50,260 | 73,673 | 68.2% |
| 64 KiB | 37,623 | 53,026 | 70.9% |

### 11.3 性能结论与未达门槛原因

MySQL NVMe stats 样本显示 `durability_batch_max=1`、`durability_flushes≈durability_group_requests`，
说明 mysqlslap 的事务/`fdatasync` 到达方式本身没有形成足够的重叠 epoch；扩大
`HFS_DURABILITY_GROUP_WINDOW_US` 会把等待直接加入每个提交的尾延迟，不能作为默认优化。
同尺寸 `fdatasync` 已从“blob metadata sync + metadata commit + flush”收敛到“一次设备
flush”，但 POSIX 语义仍要求每个独立 durability 边界等待真实 flush。因此本轮 MySQL
16 并发写入已改善但未稳定达到计划中的 native 75% 门槛；不能用跳过 flush、关闭 WAL、
提前返回或伪造完成来填补该差距。

后续若要继续突破该门槛，必须针对数据库实际事务到达模式设计可证明的跨请求 epoch
等待/提交策略，并以 trace 中 `epoch`、`device_flush` 和 waiter 关系验证不会扩大
durability 语义；本轮代码已完成安全的 v4 pipeline 和可观测基础，未引入未经验证的 v5
磁盘格式。
