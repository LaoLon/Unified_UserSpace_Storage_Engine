# HFS 2.0 RocksDB WAL 写回缓冲优化设计（2026-09-19）

状态：提案 v3（已补齐四层语义、direct I/O、可见性、格式评审、状态机、指标拆分、五断点 crash 验证；
2026-09-19 决策：§6 采用 **B（全部 buffered）**，§8 lazy unwritten **同意暂缓**；
语义已并入 `doc/design/hfs2.0.md` §10.2/§11.2/§11.3/§14.3）
依据：`doc/performance/hfs2_rocksdb_db_bench_20260919.md` §5/§5.1/§6

## 1. 目标与指标拆分

总目标：让 "RocksDB on HFS" 在目标场景不慢于、并力争快于 "RocksDB on ext4"。

统一口径：同一 NVMe（BDF `0000:0b:00.0`）、RocksDB 8.9.1、`--num=20000
--threads=8 --key_size=16 --value_size=100`、5-run 中位；ext4 侧加
`drop_caches`/O_DIRECT 约束，HFS 侧 `config/hfs_nvme.conf`。
按 §6 决策 B，`--use_direct_io_for_flush_and_compaction` 对 HFS **无效**
（writable 侧 `use_direct_io()==false`），基准两侧可保留该 flag，但报告必须
声明 HFS 侧忽略它；ext4 侧该 flag 生效属已知口径差异。

### 1.1 P0（adapter 写回缓冲 + Flush 语义）可影响的指标

| 指标 | 现状 | P0 目标 | 结果（2026-09-19 5-run 中位） |
|---|---:|---:|---|
| `fillseq` HFS/ext4 | 2.5% | ≥100% | **169.6%**（达标） |
| `overwrite` HFS/ext4 | 5.1% | ≥100% | **184.8%**（达标） |
| `readrandom` HFS/ext4 | 23.6% | 不回归 | 52.7%（达标，P1 目标顺带达成） |
| `readwhilewriting` writer ops/s | 315 puts/s（被拖垮） | ≥2x 现状且 ≥50% ext4 | 115k puts/s（2x 大幅达标；50% 未达，归 P1） |
| WAL 设备写放大 | ~2x（RMW + 预写零） | ≤1.1x | 仍约 2x（extent 预写零），P1 补集置零 |
| WAL `Sync` 延迟 | 基线 | 不回归（±10%） | 未单列；sync_calls=9/run，无回归迹象 |

### 1.2 P0 明确不影响（不得作为 P0 验收项）

- `readrandom`：读路径未改，P0 只要求"相对现状不回归（±5%）"。
- `readwhilewriting` reader ops/s：reader 路径未改。
- SST flush/compaction 吞吐：仅当 direct 写路径落地（§6）后才有变化，属 P1。
- 读 p99、CPU 占用：P1 处理。

### 1.3 P0.5（append 免 RMW）指标

- 部分块 flush 的设备读次数下降（探针：512B vs 4K 同步写延迟比从 1.9x 收敛到 ≤1.2x）。
- `fillseq` 相对 P0 不回归。

### 1.4 P1 指标（2026-09-19 结果）

- `readrandom` HFS/ext4 ≥50%：**58.0% 达标**；stretch ≥80% 未达——计数器显示
  readrandom 几乎不触发 HFS 读 I/O（160k Get 仅 5 次 read），差距来自 CPU 路径
  与 busy-poll reactor 争核（见性能报告 §9.2）。
- `readwhilewriting` writer ≥50% ext4：**25.4% 未达**；idle 让出实验使 FIO gate
  回退 12-22%，已回退（§9.3），留待可配置策略。
- SST flush/compaction 密集负载 ≥80% ext4：未单独测量。
- extent 零写放大消除（补集置零，§8.2）：**已实现并通过正确性测试**。

非目标：不改变 WAL 记录格式与 replay；不引入 POSIX preload/FUSE；不做
page-cache 等价的全局读缓存（P1 只做口径与 block cache）。

## 2. 现状与问题（证据）

- 差距 100% 来自 WAL 写路径：WAL off 时 HFS ≈ ext4（1 线程 1.14M vs 1.52M，
  8 线程 974k vs 548k）。
- 每次 `Put` 的 WAL 记录（~116B）都会调用 `HfsWritableFile::Append`：
  - `file_->append()` 同步直写设备（无写回缓冲）；
  - <4K 非对齐写触发 RMW（读 4K + 写 4K，两次阻塞设备往返）；
  - 每次 append 还有 `stat`/`reserveExtents` 快照/`extentMapFor` 快照/`setSize`
    的 namespace 开销（全局互斥 + logical epoch 推进）。
- `FSWritableFile::use_direct_io()` 默认 false 且 adapter 未 override →
  WAL/SST/MANIFEST 全部走同步 `Append`；`PositionedAppend` 对 RocksDB 8.9
  默认不可达；`use_direct_io_for_flush_and_compaction` 实际无效。
- `FSWritableFile::Flush` 当前映射为 `File::sync()`（完整 checkpoint），语义过强。
- 1→8 线程仅 3.4x 扩展：namespace 互斥 + 单 data reactor + 每 child 阻塞等待。

## 3. 方案总览

| 层 | 改动 | 阶段 |
|---|---|---|
| Adapter | **全部可写文件**写回缓冲（决策 B）+ `Flush/Sync/Close` 语义修正 + 缓冲大小可见性 | P0 |
| Core | append 免 RMW（已知 EOF 时跳过读回） | P0.5 |
| Core | 补集置零（新 extent 只零写未被本次写覆盖的部分） | P1 |
| Core | namespace 快路径、`reserveForAppend`（可选） | P1 |
| Adapter | `MultiRead` 并行化、读缓存口径 | P1 |

## 4. Adapter 写回缓冲（P0 主体）

### 4.1 结构与接口

```cpp
class HfsWritableFile final : public rocksdb::FSWritableFile {
    std::shared_ptr<hfs::File> file_;
    std::shared_ptr<HfsContext> context_;   /* pending registry + size visibility */
    std::vector<char> buffer_;              /* all writable files (decision B) */
    std::uint64_t logicalOffset_ = 0;       /* HFS logical EOF of flushed bytes */
    static constexpr std::size_t kFlushThreshold = 1u << 20; /* 1 MiB, 4K 对齐 */
};
```

### 4.2 语义表（决策 B：全部可写文件 buffered）

| RocksDB 回调 | 语义 |
|---|---|
| `Append(data)` | memcpy 进 `buffer_`；达到阈值才 `flushBuffer()`；`data.size() >= kFlushThreshold` 时先 flush 再直写 |
| `Flush()` | **no-op**（实测 RocksDB 8.9 每个写组都调用 Flush：`flush_calls==append_calls`；Flush 不是 durability 屏障，若落盘则缓冲失效。字节留在缓冲，由阈值/`Sync`/`Close`/读打开强制写出） |
| `Sync()`/`Fsync()` | `flushBuffer()` + `file_->sync()` |
| `RangeSync()` | `flushBuffer()` + `file_->sync()` |
| `Close()` | `flushBuffer()` + `file_->sync()` + 注销 registry + 释放 handle |
| `PositionedAppend()` | 直写（`file_->write(offset)`；RocksDB 8.9 默认不可达，保留 API 完整性） |
| `Allocate()` | `file_->allocate()`（保持"分配即零"契约） |
| `PrepareWrite()` | no-op |

`flushBuffer()`：`file_->write(buffer_, logicalOffset_)`（显式偏移，避免每次
`stat()`）；成功后 `logicalOffset_ += size`、清空缓冲、更新 pending registry；
失败保留缓冲并返回错误（见 §9）。

### 4.3 大小可见性

- `HfsContext` 维护 pending registry（§7）：`GetFileSize`、
  `GetChildrenFileAttributes` 返回 `core size + pendingBytes`。
- flush 成功后同步更新 registry，避免 WAL 轮转/备份看到滞后大小。

### 4.4 内存与并发

- 每可写文件缓冲上限 = 阈值；`Append` 大于阈值时先 flush 再直写，缓冲不放大。
- RocksDB 保证单写者；adapter 侧并发打开保护见 §7。

## 5. 四层语义（buffered / logical / durable / recovered）

| 层 | 定义（何时成立） | 可见性 | 进程崩溃 | 机器崩溃 |
|---|---|---|---|---|
| **buffered** | `Append` 返回后，字节在 `buffer_` 中 | 仅经 registry overlay/`GetFileSize`；HFS core 与其它 handle 不可见（§7） | 丢失 | 丢失 |
| **logical** | `File::write` 成功返回：数据已提交设备（未 flush）、namespace 内存态（extent map、logical size、logicalEpoch）已更新 | 同进程所有 HFS handle 可读 | 丢失（namespace 未持久；设备字节无元数据引用） | 丢失 |
| **durable** | `File::sync()/datasync()` 成功返回：`Engine::syncMetadata` 完成（设备 flush → staged sizes → journal COMMIT + checkpoint 写槽并 flush → `publishDurableEpoch`） | 崩溃后 mount 可恢复 | 保留 | 保留（依赖设备 flush 语义） |
| **recovered** | mount replay 完成：最新有效 checkpoint + 到最后一个 durable COMMIT 的 journal 重放 | 全部 handle 可读 | — | — |

转换事件：`Append` → buffered；`Flush`/阈值 → logical；`Sync` → durable；
crash + mount → recovered。恢复不变量：`recovered ⊆ durable`（前缀），
无半事务（BEGIN 无 durable COMMIT 丢弃），generation/CRC 校验通过。

RocksDB 回调映射：`Append`=buffered；`Flush`=logical（**不** durable）；
`Sync`/`Fsync`=durable；`WriteOptions.sync=1` → `Sync`；DB close → `Sync`。

与 ext4 的关键差异（必须记入 `hfs2.0.md` §11/§14 边界）：ext4 的 logical
（page cache）可跨进程崩溃存活；**HFS 的 logical 本身就不跨进程崩溃**
（namespace 内存态 + 未持久 checkpoint），所以 adapter 缓冲没有引入新的
持久性等级，只是把"未同步字节"从"已提交设备但无元数据引用"提前到
"adapter 内存"。崩溃恢复边界始终是最后一次 durable epoch。
需要"进程崩溃不丢"的场景必须 `sync=1`。

## 6. Direct I/O 与 buffered 冲突（决策 B：全部 buffered）

事实：`FSWritableFile::use_direct_io()` 默认 false（`file_system.h:1117`），
adapter 未 override；`NewWritableFile` 忽略 `FileOptions`；因此
`use_direct_io_for_flush_and_compaction=1` 无效、`PositionedAppend` 对
RocksDB 8.9 默认不可达。

**决策（2026-09-19）：B —— 写侧全部 buffered。**

- `HfsWritableFile::use_direct_io()` 保持 false（不 override）；所有可写文件
  （WAL/MANIFEST/SST）统一走 §4.2 缓冲语义。
- `NewWritableFile` 继续忽略 `FileOptions`；`use_direct_io_for_flush_and_compaction`
  对 HFS 明确无效，需在基准口径与文档中声明（§1、§11）。
- `PositionedAppend` 保留直写实现，但 RocksDB 8.9 不会调用；作为 API
  完整性保留，未来若切换 direct 写再启用。
- 读侧保持现状：`HfsRandomAccessFile::use_direct_io()==true` +
  `GetRequiredBufferAlignment()=4096`（RocksDB 传对齐 buffer，HFS core 按
  planner 处理对齐/RMW，不冲突）。

理由与取舍：

- 全部 buffered 实现最简，P0 只动 adapter 一个类；避免 direct 写路径的
  对齐/bounce 复杂度与额外测试面。
- SST 写入（大块、连续）经缓冲后由 `Sync`/`Close` 一次批量落盘，行为接近
  ext4 的 buffered write + fsync；对 compaction 吞吐影响待 P0 复测确认。
- 代价：与 ext4 的 O_DIRECT 口径不完全对称；基准报告需显式声明，且
  ext4 侧该 flag 生效属已知差异。

冲突点清单与结论：

1. direct 文件与缓冲不得混用 → B 下不存在 direct 写文件，无冲突。
2. `Append`/`PositionedAppend` 混用：RocksDB 只用 `Append`（`use_direct_io()`
   false），`PositionedAppend` 不可达。
3. `Flush` = logical（不 durable），`Sync` = durable；direct 语义不参与。
4. 未来若要 direct 写（SST），按被否的 A 方案重新评审（override
   `use_direct_io`/对齐 + 对 direct 文件禁用缓冲），需要单独设计与测试。

## 7. Pending buffer 与其它 handle 的可见性

- **Registry**：`HfsContext` 维护 `path -> PendingFileState{owner, bytes,
  logicalOffset}`，`NewWritableFile` 登记、`Close`/析构注销。
- **单写者**：同一 path 已有 pending owner 时，第二次 `NewWritableFile`
  返回 `Busy`（RocksDB 不会这样做；防御性）。`NewWritableFile` 带 Truncate
  语义：若同 path 有 pending，丢弃 pending（截断语义）并记录 trace。
- **读可见性（P0 选 flush-on-open）**：`NewSequentialFile`/
  `NewRandomAccessFile` 打开某 path 时，若存在 pending，先 `flushBuffer()`
  再打开 → "Append 返回后的字节在打开读句柄时可见"。同一 WritableFile 无读
  接口，RocksDB 不读自己正在写的 WAL，因此无需逐读 overlay。
- **大小可见性**：`GetFileSize`/`GetChildrenFileAttributes` 返回
  `core size + pending`（不触发 flush）。
- **rename/link**：目标/源 path 有 pending 时先 flush 再执行。
- **delete**：先执行 core delete，成功后丢弃 pending（文件已删除）。
- **崩溃**：pending 直接丢失；registry 是内存态，mount 后为空。
- **不变量**：任一时刻，同一 path 至多一个 pending owner；`GetFileSize`
  单调不减（直到 truncate）；flush 成功后 `logicalOffset_` 单调递增。

## 8. unwritten / reserve 与 on-disk format 评审

现状事实：

- Allocator 已有四状态 `Free → Reserved → Initialized → Committed`
  （`allocator.hpp:13-19`），快照 `kSnapshotVersion = 1`（magic `H2AL`），
  **随 namespace checkpoint 一起持久化**（`namespace.cpp:1065/1196`）。
- `reclaimReserved()` 已实现但**仅测试调用**（`allocator_test.cpp`），
  mount/replay 路径未接线。
- `File::allocate` 语义 = 零写 + commit + `setSize(offset+length)`
  （`client.cpp:1022-1053`），不能用于"预分配但不改 logical size"。

### 8.1 结论表

| 方案 | 是否改 on-disk format | 版本/迁移 | 评审结论 |
|---|---|---|---|
| P0/P0.5：adapter 缓冲、append 免 RMW | 否 | 无 | 通过 |
| P1-a：**补集置零**（新 extent 只零写未被本次 `[offset, offset+len)` 覆盖的部分，按块外扩对齐） | 否 | 无 | **2026-09-19 已实现**（`DataPlane::zeroRange` + `File::write`），测试通过；保持"committed extent 的字节要么被写过、要么为零"不变式 |
| P1-b：lazy unwritten（读未写区域返回零） | **是**：新增 ExtentState（如 `CommittedUnwritten`）+ `kSnapshotVersion` 升级 + superblock format 版本 | clean-break：旧镜像拒绝挂载，需显式迁移策略 | **2026-09-19 同意暂缓**；触发条件：补集置零仍被证明是瓶颈（WAL/大文件顺序写场景） |
| P1-c：`reserveForAppend`（只预留、不改 size、不置零） | 否（预留只在内存，不入快照/日志；崩溃即随 allocator 内存态消失） | 无 | 有条件通过：若允许预留跨 checkpoint 持久化，必须同时把 `reclaimReserved()` 接入 mount/replay 并加测试；否则限定"预留生命周期 = 单次写调用" |

### 8.2 补集置零的精确语义（P1-a）

- 对本次写新分配的 extent，置零范围 = extent 范围 − 写范围，且按 4K 块
  **向外对齐**（避免覆盖写范围边缘已写入的字节）。
- 写范围内未对齐块仍走 RMW，其块内写范围外的字节必须已被置零或写过
  （由补集置零保证）。
- 不变量：已 commit 的 extent 内，每个字节 ∈ {被写过, 为零}；因此
  `truncate` 增长/空洞读仍返回零，无需新状态。
- WAL 顺序写场景：flush 通常整 extent 覆盖 → 补集为空 → 零写为 0。

## 9. 写路径状态机（partial write / retry / Close failure）

状态：`Empty → Buffering → Flushing → Buffering`、`Syncing`、`Closed`；
错误不设终态，缓冲保留可重试。

| 事件 | 动作 | 失败行为 |
|---|---|---|
| `Append(n)`，`buffer_.size()+n < 阈值` | memcpy | —（不涉及设备） |
| `Append(n)`，达到阈值 | `Flushing`：`file_->write(buffer_, logicalOffset_)` | 缓冲保留、返回 IOError、状态回 `Buffering` |
| `Append(n)`，`n >= 阈值` | 先 flush 已有缓冲，再直写 n | 同上；直写失败同样保留（重试幂等） |
| `Flush` | no-op（不落盘） | — |
| `Sync`/`Fsync` | `Flushing` + `file_->sync()` | flush 失败返回；sync 失败返回；缓冲保留 |
| `Close` | `Flushing` + `file_->sync()` + 注销 registry + 释放 handle | **不释放**：保留 handle 与缓冲，返回 IOError，状态 `CloseFailed`；析构函数做一次 best-effort flush+sync 并记录错误 |
| SIGKILL（任意状态） | — | 进程死亡；未 flush 字节丢失；已 flush 字节按 logical/durable 层语义处理 |

语义要点：

- `File::write` 对失败不返回已写字节数（`client.cpp:895-985` 成功才返回
  `WriteResult{total}`），因此重试策略是**整缓冲重写**；该操作幂等（相同
  偏移、相同数据、extent 已分配；失败路径不改 `logical size`），不会重复
  计费或产生错位。
- 不在 adapter 内部无限重试；由 RocksDB 决定重试或报错。每次
  `Append`/`Flush`/`Sync` 至多触发一次 flush。
- `Flush` 失败后再次 `Flush`/`Sync`/`Close` 会重试同一缓冲；
  `logicalOffset_` 只在 flush 成功后推进。
- `Close` 失败后 RocksDB 若不再调用 Close，析构的 best-effort 是最后机会；
  失败时通过 trace/log 显式记录（不静默丢数据）。
- `Close` 成功后任何回调都返回 `InvalidArgument`（句柄已释放）。

## 10. 测试与验收

### 10.1 功能测试

- 新增 adapter 单测：阈值触发、`Flush` 不落 durability、`Sync` 后 durable、
  `Close` 落盘、`GetFileSize` 含 pending、flush 失败后重试成功、Close 失败
  后析构兜底（注入失败）。
- 既有 Shard-84 全量 smoke：fillseq/readrandom/overwrite/readwhilewriting、
  reopen、`hfs_ldb checkconsistency`、双盘、LockFile、8 线程并发 sync 回归
  （`file_data_test` 已含）。
- 回归：FIO 正式 gate（≥85%、p99 ≤1.10x）与设备测试全绿。

### 10.2 五断点 crash 验证（按 §16.4）—— 2026-09-19 已实施

实施结果见 `doc/performance/hfs2_rocksdb_db_bench_20260919.md` §8.2/§8.3：
断点 1/2/4/5 全栈 × {sync=1, sync=0} 共 8 例恢复一致且 durable baseline 保留；
断点 3 由 `journal_test`/`journal_device_test` 的 torn-COMMIT 用例覆盖（当前
全栈路径不经过 `Journal::append`，group commit 未接线）；缓冲窗口随机 SIGKILL
×10 无损坏，sync=1 恢复条目随 kill 时刻递增，sync=0 恢复为空前缀（符合 §5 契约）。

新增进程级 crash 钩子 `HFS_TEST_CRASH_AT=<point>`（命中即 `_exit(137)`，
模拟硬崩溃，不做清理/flush），与现有错误注入钩子并存：

| §16.4 断点 | 注入位置 | 现有钩子 | 新增 crash 值 |
|---|---|---|---|
| 1. data child 完成 → device flush 前 | `spdk_runtime.cpp` flush | `HFS_TEST_FAIL_FLUSH_DEVICE` | `data_flush` |
| 2. data flush 完成 → metadata commit 前 | `engine.cpp:665` | `HFS_TEST_FAIL_METADATA_COMMIT` | `metadata_commit` |
| 3. journal COMMIT 写入 → journal flush 前 | `journal.cpp` COMMIT 槽写入后、`device->flush()` 前（当前无钩子） | 新增 | `journal_commit` |
| 4. checkpoint inactive slot flush → superblock publish 前 | `journal.cpp:352` | `HFS_TEST_FAIL_CHECKPOINT_FLUSH` | `checkpoint_flush` |
| 5. clean flag 写入前 | `volume.cpp:415` | `HFS_TEST_FAIL_CLEAN_FLAG` | `clean_flag` |

矩阵与判定（每格一个证据目录）：

- 5 断点 × {`sync=1`, `sync=0`} × adapter 缓冲开启 = 10 例；
  每例：format → `hfs_db_bench`（fillseq+overwrite，固定 N）命中 crash →
  remount → `hfs_ldb checkconsistency` → 校验：
  1. namespace 可加载、generation 单调、checkpoint CRC 有效；
  2. allocator bitmap/state 无非法组合（无 Reserved 泄漏、无双重 commit）；
  3. `logical/durable size` 满足 recovered ≤ durable ≤ logical 的历史前缀；
  4. orphan reclaim：恢复后 free extent 数 = 期望值（无未引用泄漏）；
  5. RocksDB reopen 成功，`sync=1` 下已确认写全部在，`sync=0` 下恢复前缀
     = 最后 durable epoch，无半记录；
  6. 与 ext4 同逻辑 workload 的已提交前缀一致（ext4 侧 `sync=1` 全在；
     `sync=0` 允许 ext4 更多，HFS 只要求自身一致）。
- 缓冲窗口专项：`fillseq` 运行中随机时刻 SIGKILL ×10（sync=0/sync=1 各 5），
  验证丢失窗口 ≤ 1MiB/文件、`checkconsistency` 通过。
- 错误注入（非 crash）路径沿用现有 `file_api_test` 用例，新增 flush 失败重试
  与 Close 失败用例。

### 10.3 性能矩阵

- 主口径：§1 的 db_bench 矩阵（P0/P0.5/P1 分别跑 5-run 中位）。
- 反例场景：working set > block cache 时对比单层缓存优势（ext4 `drop_caches`）。
- 证据目录见 §13。

## 11. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 进程崩溃丢缓冲窗口 | §5 契约文档化；`sync=1` 保证；§10.2 SIGKILL 量化窗口 |
| WAL 大小可见性滞后 | pending registry 修正 GetFileSize/属性 |
| `Flush` 语义变更影响既有验收 | 跑全量 Shard-84 smoke；与 RocksDB 契约对齐 |
| `use_direct_io_for_flush_and_compaction` 对 HFS 无效 | §6 决策 B 明确声明；基准报告标注口径差异；不引入 direct 写路径 |
| 补集置零破坏空洞读语义 | 不变量测试（truncate 增长后读零、RMW 边缘字节） |
| `reserveForAppend` 跨 checkpoint 泄漏 | 限定内存生命周期；若持久化则接 `reclaimReserved` 到 mount 并加测试 |
| crash 钩子进 release | 仅 env 命中时生效，未设置零开销；CMake 单测覆盖 |
| Close 失败后数据丢失 | 析构 best-effort + 显式错误记录；RocksDB 收到 IOError |

## 12. 实施顺序

1. P0：adapter 全部可写文件缓冲 + `Flush` 语义 + pending registry（§4/§6/§7）；
   db_bench 复测 §1.1 指标。
2. P0.5：append 免 RMW（§5 之外的 core 改动）；复测。
3. §10.2 五断点 crash 矩阵 + 缓冲窗口 SIGKILL；修问题并回归。
4. P1：补集置零（§8.2）、namespace 快路径、`reserveForAppend`（按 §8.1 限定）、
   `MultiRead` 并行、读缓存口径；复测 §1.4。
5. 归档：更新 `doc/design/hfs2.0.md` §11.2/§11.3/§14.3、`doc/bug.md`、
   `doc/performance/` 报告。

## 13. 证据目录（计划）

- `/mnt/hfs-data/hfs2-rocksdb-wal-p0-20260919`
- `/mnt/hfs-data/hfs2-rocksdb-wal-p0_5-20260919`
- `/mnt/hfs-data/hfs2-rocksdb-crash-<point>-<syncmode>-20260919`
- `/mnt/hfs-data/hfs2-rocksdb-wal-p1-20260919`
- FIO 回归沿用 `/mnt/hfs-data/hfs2-postfix-gate3-*` 口径重跑
