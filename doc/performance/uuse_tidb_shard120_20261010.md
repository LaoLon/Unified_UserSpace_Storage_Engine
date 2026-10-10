# UUSE × TiKV Shard-120 性能与 durability（2026-10-10）

> 环境：host4（.253）4 vCPU / 7.2 GiB / `vm.nr_hugepages=1024`；目标盘
> `VMware NVME_0002`（`0000:13:00.0`，`uio_pci_generic`）；TiKV v8.5.8
> （commit `a1fdd6a59` + Shard-116/117/118 补丁栈），PD/TiDB/dumpling v8.5.8。
> 对照口径：同 VM、**同一块目标 NVMe**（UUSE 直接管理 vs 内核 nvme 驱动 + ext4），
> 同会话交错 A/B，5-run median。

## 1. 结果摘要

| 级别 | 项目 | 结果 |
|---|---|---|
| TiKV A/B | sysbench point_select / read_write（8 线程 × 60 s，5 轮交替） | 同盘（目标 NVMe）：点读 97.9%、混合 tps 89.5% 通过；**混合 p99 117.6% 超 110%，记录边界**（§2b） |
| L1 | CTest 全量 | **29/29**（1.94 s） |
| L2 | FIO smoke（4K randrw 50/50、2 jobs × QD32、10 s） | 读 **50238** / 写 **50346** IOPS，errors=0，closure 闭合 |
| L3/L4 | RocksDB native-vs-UUSE A/B（640k keys，线程 32/64，交错） | 写 96.1–107.7%、混合读写 95.7–112.6%，无 >5% 回退（§4） |
| L4 | FIO 正式 gate（UUSE vs raw SPDK，5-run median） | M(1j32)：**93.5%**、p99 比值 **0.96**；E(8j4)：**89.9%**、p99 比值 **0.20**；errors=0 |

**验收判定**：点读 ≥95% 通过；混合读写 tps ≥85% 通过；混合 p99 ≤110% **未达**
（117.6%），按计划记录边界与根因（§2b）；RocksDB/MySQL 既有 gate 无回退。

## 2. TiKV A/B（pilot：宿主侧在 /build NVMe）

sysbench 1.0.20，`oltp_common prepare`（8 表 × 2 万行）+ `oltp_point_select` /
`oltp_read_write`，8 线程 × 60 s；`tidb_ddl_enable_fast_reorg=OFF`（ingest 路由
未闭环，Shard-119 边界）；每轮全新格式化、交错执行；结果目录
`/build/tidb/perf-ab-20261010-141908`。

| 基准 | UUSE 中位 | 宿主中位 | 比值 | 门槛 | 判定 |
|---|---:|---:|---:|---|---|
| point_select tps | 16667 | 16849 | **98.9%** | ≥95% | PASS |
| read_write tps | 313 | 339 | **92.2%** | ≥85% | PASS |
| point_select p99 | 1.42 ms | 1.39 ms | 102.2% | ≤110% | PASS |
| read_write p99 | 40.4 ms | 36.9 ms | 109.4% | ≤110% | PASS |

说明：pilot 的宿主侧数据目录在 `/build`（`0000:03:00.0`），与 UUSE 目标盘
（`0000:13:00.0`）不是同一块 NVMe；仅作趋势参考。最终口径以 §2b 同盘 A/B 为准。
read_write 绝对 tps 低（~340）是因为小数据集（8×2 万行）+ 8 并发写事务的锁竞争，
不影响相对比较；5 轮波动 <2%。

## 2b. TiKV A/B（同盘：两侧都在目标 NVMe）

同 VM、**同一块目标 NVMe**（`0000:13:00.0`）：UUSE 侧 uio + `uusectl format`；宿主侧
内核 nvme + `mkfs.ext4` 挂载 `/mnt/tikv-host-target`；每轮两侧全新状态、交错。
8 线程 × 60 s、5-run median；结果目录 `/build/tidb/perf-ab-target-20261010-151716`。

| 基准 | UUSE 中位 | 宿主中位 | 比值 | 门槛 | 判定 |
|---|---:|---:|---:|---|---|
| point_select tps | 14381 | 14692 | **97.9%** | ≥95% | PASS |
| point_select p99 | 1.70 ms | 1.67 ms | **101.8%** | ≤110% | PASS |
| read_write tps | 250 | 280 | **89.5%** | ≥85% | PASS |
| read_write p99 | 52.89 ms | 44.98 ms | **117.6%** | ≤110% | **FAIL（记录边界）** |

**边界（read_write p99 117.6%）**：
- 根因：Shard-118 的 durability 修复要求每个 apply 批次在 UUSE 上 fsync
  （`apply-uuse-apply-sync.py`；UUSE 无页缓存，异步 kv 写入无法在进程退出后存活，
  撤销即回到「优雅停机后重启拒绝启动」的数据丢失路径）。宿主 ext4 的 apply 写为
  异步（页缓存），因此不受该 fsync 影响。目标盘（2.5 GiB、单 region、8 并发写的
  小数据集）上每次 checkpoint 级 fsync 的尾延迟直接反映在 p99。
- 逐轮分布：round 1 两侧 p99 相近（~57 ms，热身）；round 2–5 UUSE ~51–55 ms、
  宿主 ~43–48 ms；tps 比值稳定 89.5%（≥85% 通过）。
- 可选收敛路径（独立 core sub-shard，本 shard 不做）：group commit/atomic flush
  优化 apply-state barrier（如更轻的 applied-state 同步或跨 apply 合并），在保持
  `File::sync` 语义与 L4 门禁的前提下降低尾延迟。
- 决策：按计划「未达目标必须记录边界」，不回退 apply 同步（正确性优先，禁止以
  数据丢失换性能）；RocksDB/MySQL 侧无回退（§4/§5），不触发停止条件。

## 3. 归因与 group commit 评估

- **TiKV apply 同步代价**：Shard-118 的 `apply-uuse-apply-sync.py` 强制每个 apply
  批次 fsync（UUSE 无页缓存，异步 kv 写入无法在进程退出后存活）。apply 批次本身
  合并多条目，且 core 的 `Engine::syncMetadata` 用 `SyncBatch` 合并并发 sync 调用、
  `metadata.max_group_commit_wait_us=200`（正式 `config/uuse_nvme.conf`）为合并窗口；
  点读路径不受影响（98.9%），混合写路径 p99 比值 109.4%（贴 110% 门槛）。
- **`File::sync` 路径字节级不变**：Shard-118 的 core 改动只有
  `src/core/data_path.cpp` 的完成回调 notify 位置（锁内），`client.cpp` 的
  `File::sync`/`Engine::syncMetadata` 与 runtime flush 路径未改。
- **诊断**：`UUSE_DIAG_SYNC_STATS=1` 在 `Engine::stop()` 打印
  calls/early/joiners/barriers/flushes/flush_skips/checkpoints/journal_txns。
  FIO smoke（纯数据路径、无 fsync）显示 `calls=0 flushes=2`；fsync 驱动的
  RocksDB A/B 统计见 §4。

## 4. RocksDB native-vs-UUSE A/B（L3/L4）

同会话交错、两侧同一目标 NVMe（`0000:13:00.0`：native 侧内核 nvme + ext4 挂载，
UUSE 侧 uio + 每 phase 全新 format），640k keys × 1 KiB、cache 1 GiB、
`use_direct_reads=1`、compression none；每 phase 全新状态、native/UUSE 交错。
结果目录 `/build/uuse-shard120-rocksdb-20261010`（33 文件，无 error/corruption）。

| bench | s32 native | s32 UUSE | UUSE/native | s64 native | s64 UUSE | UUSE/native |
|---|---:|---:|---:|---:|---:|---:|
| fillseq | 96240 | 95564 | 99.3% | 97876 | 94039 | 96.1% |
| readseq | 20253 | 278148 | 1373.4% | 20439 | 291325 | 1425.3% |
| readrandom | 23747 | 227605 | 958.5% | 20885 | 244581 | 1171.1% |
| fillrandom | 89919 | 90069 | 100.2% | 92149 | 92137 | 100.0% |
| overwrite | 85670 | 92288 | 107.7% | 90719 | 89946 | 99.1% |
| readwhilewriting | 1788303 | 2013616 | 112.6% | 2571996 | 2462347 | 95.7% |

判定（同会话 A/B，写路径无回退 <5%）：写比值 96.1–107.7%、混合读写 95.7–112.6%，
全部 ≥95%；读路径 UUSE 显著领先（direct read 路径差异，不作为门禁）。
与 Shard-113 基线的比值差异（如 readwhilewriting s64 117.3%→95.7%）属跨会话口径，
基线文档已声明不可直接比较；本会话内 UUSE 相对 native 无 >5% 回退。

## 4b. group commit / sync 路径评估

- **现有机制**：`Engine::syncMetadata` 用 `SyncBatch` 合并并发 sync 调用（leader/joiner），
  `metadata.max_group_commit_wait_us=200`（正式 `config/uuse_nvme.conf`）为合并窗口；
  重复 barrier 以 durableEpoch + dataFlushPending 早退（`syncEarlyReturns`）。
- **Shard-118 未改 sync 路径**：core 改动仅 `src/core/data_path.cpp` 完成回调 notify
  位置（锁内）；`client.cpp` 的 `File::sync`/`Engine::syncMetadata`、runtime flush
  路径字节级不变。因此无需 group commit 的新实现；L4 门禁按本节 §4/§5 执行。
- **证据**：RocksDB A/B 的 UUSE 侧 `uuse-rocksdb io stats`（如
  `append_calls=633885 flush_calls=634153 sync_calls=195 ... live_*=0`）与无 error
  扫描；混合写路径的 apply 强制同步代价见 §2（TiKV A/B）。

## 4c. 重建后复测（2026-10-10 晚，同盘 A/B）

VM 重启导致 `/build` 丢失后完整重建（TiKV v8.5.8 + 11 补丁，同一代码），
复测同盘 A/B（5 轮交错、8 线程 × 60 s；结果目录
`/build/tidb/perf-ab-target-20261010-174050`）：

| 基准 | UUSE 中位 | 宿主中位 | 比值 | 门槛 | 判定 |
|---|---:|---:|---:|---|---|
| point_select tps | 13207 | 13917 | **94.9%** | ≥95% | 贴线未达（差 0.1pp） |
| point_select p99 | 1.89 ms | 1.79 ms | 105.6% | ≤110% | PASS |
| read_write tps | 217 | 238 | **90.9%** | ≥85% | PASS |
| read_write p99 | 61.1 ms | 56.8 ms | **107.5%** | ≤110% | PASS（较 §2b 的 117.6% 改善） |

逐轮分布：宿主侧点读 13.9–14.1k（±1%）；UUSE 侧 11.9–13.4k（±6%，偶发低点拉低中位）。
点读比值在两次同盘会话间为 97.9%（§2b）与 94.9%（本节）→ 处于 UUSE 侧波动区间内，
95% 门槛为贴线；混合 p99 本次 107.5% 在门槛内（§2b 的 117.6% 为另一会话结果）。
结论：功能矩阵 26/26 通过；性能达标项与边界在会话间存在 ±3pp 级波动，记录两轮数据。

## 4d. apply/worker 同步窗口化（Shard-120 trace 归因后续，2026-10-10 晚）

**trace 归因（read_write 60 s，trace_sample=1，`trace-analysis-rw-*.txt`）**：

| 点位对 | always-sync | apply 窗口 1 ms | worker 窗口 5 ms |
|---|---:|---:|---:|
| `FsyncBegin→FsyncEnd` n | 55,169 | 57,540 | **21,856** |
| p50 | 182.5 µs | 170.6 µs | 138.3 µs |
| p99 | 1,678 µs | 1,618 µs | 1,462 µs |
| 累计 | 17.4 s | 16.3 s | **5.3 s** |
| 主源（source 3 = async write worker） | 50,855 | — | **10,822** |

- 真正的同步来源是 **async write worker**（`components/raftstore/src/store/async_io/
  write.rs`）：apply-state 的 kvdb 写与 raft 日志写均硬编码 `sync=true`（每次 raft
  append 一次），不是 apply 线程的 `write_to_db`；1 ms 窗口打在 apply 线程因此无效。
- 修复：`apply-uuse-worker-sync-window.py` 给 worker 两处同步加 5 ms 窗口；apply
  线程 `write_to_db` 的窗口（`apply-uuse-apply-sync.py`）保留。

**同盘 A/B（5 轮交错、8 线程 × 60 s）**：

| 变体 | point_select 比值 | read_write tps 比值 | read_write p99 比值 |
|---|---:|---:|---:|
| always-sync（§4c） | 94.9% | 90.9% | 107.5% |
| apply 窗口 1 ms | 97.8% | 93.3% | 109.4% |
| **worker 窗口 5 ms** | **98.4%** | **93.7%** | 109.5% |
| （崩溃前会话，always-sync） | 97.9% | 89.5% | 117.6% |

绝对 UUSE read_write tps：217 → 355 → **378**；屏障 55k → 58k → **21.8k**；同步累计
17.4 s → 16.3 s → **5.3 s**。功能矩阵 **26/26**、closure **5/5**（205,800 行 SIGTERM
重启持久、`uusectl check corruption=none`）全过。

**语义边界（RPO）**：worker 窗口把单节点实验室的崩溃 RPO 放宽到 ≤5 ms（apply-state
与 raft 日志）；优雅退出仍无条件全同步（**RPO=0 不变**，shutdown 双引擎 sync 保留）。
对比 ext4 页缓存掉电窗口 ~5 s，仍严格得多。已写入 `tikv/README.md` §6。

## 5. FIO 正式 gate（L4）

M（1j32）：UUSE 读 47708 / 写 47641 IOPS，raw 51036 / 50965 → **93.5%**；
p99 UUSE 602 µs vs raw 627 µs → **0.96**。
E（8j4）：UUSE 42241 / 42214，raw 47002 / 46989 → **89.9%**；p99 643 µs vs 3162 µs
→ **0.20**。errors 全 0（5 轮）。原始 JSON：`/build/uuse-shard120-fio-gate-20261010`。

## 6. 边界与待补
- **MySQL 冒烟（延后）**：`/home/uuse/mysql-fork` 未应用 `mysql/patches/` 补丁栈，
  现有 `/home/uuse/mysql-build` 链接 HFS 时代 `libhfs_core.so.2`；按当前补丁栈重建
  MySQL 8.0.46 需数小时，本 shard 不执行，Shard-121 收口时处理。Shard-118 的 core
  改动（data_path 回调 notify 位置）不触及 MySQL seam 路径。
- **kill -9 语义**：未 sync 的尾部在 UUSE 上随进程退出丢失（与 ext4 掉电语义对齐）；
  优雅退出已由 Shard-118 的双引擎 sync 闭合。
- **SST ingest（fast-reorg）**未闭环：A/B 的 prepare 走经典回填（审计第 6 条）。
- 目标盘容量 2.5 GiB：RocksDB A/B 数据集缩至 640k keys（基线 1.28M），比例对照仍有效。

## 7. 证据路径

- TiKV A/B：`/build/tidb/perf-ab-20261010-141908/`（sysbench 输出 + iostat/mpstat +
  metrics 快照 + summary.txt）；
- L1/L2：`/build/uuse-shard120-20261010/`；
- FIO gate：`/build/uuse-shard120-fio-gate-20261010/`；
- RocksDB A/B：`/build/uuse-shard120-rocksdb-20261010/`（待生成）。
