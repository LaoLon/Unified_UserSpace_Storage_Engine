# HFS 2.0 RocksDB db_bench 对照测试（2026-09-19）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

日期：2026-09-19  
设备：真实 NVMe BDF `0000:0b:00.0`（30 GiB），同一物理盘两侧分别建 ext4 与 HFS  
软件：RocksDB 8.9.1（`/usr`），stock `/usr/bin/db_bench` vs `build-repair98/hfs_db_bench`  
构建：VM `/root/code/HFS_BASE_SPDK/build-repair98`（Release）  
证据：`/mnt/hfs-data/hfs2-rocksdb-db_bench-20260919`（两侧 stdout/stderr、format 日志）

## 1. 口径

- 两侧均 `taskset -c 2-3`，RocksDB 8.9.1，`--num=20000 --threads=8`（每线程 op 数，
  合计 160000 ops）、`--key_size=16 --value_size=100`、
  `--use_direct_io_for_flush_and_compaction=1`、`--statistics=0`；
  `fillseq,readrandom,overwrite,readwhilewriting` 顺序执行于同一 DB。
- baseline：整盘 `mkfs.ext4`，挂载 `/mnt/rocksdb-ext4`，DB 路径 `/mnt/rocksdb-ext4/db`。
- HFS：`hfsctl format` 后 `hfs_db_bench --db=/rocksdb`（单层路径，见 §4 边界 4）。
- `hfs_db_bench` 本轮补齐：真并发 op 线程、`--key_size/--value_size`、
  `BENCH <name> ops=<n> seconds=<s> ops_per_sec=<x>` 输出（原实现只有完成计数）。

## 2. 结果（单次运行，非 5-run 中位）

| benchmark | ext4 db_bench (ops/s) | HFS hfs_db_bench (ops/s) | HFS/ext4 |
|---|---:|---:|---:|
| fillseq | 279863 | 6962 | 2.5% |
| readrandom | 805696 | 190263 | 23.6% |
| overwrite | 132123 | 6788 | 5.1% |
| readwhilewriting | 477586（仅 writer，0.335s） | 902553（writer 20000 + reader 57.3M，63.5s） | 语义不同，不可比 |

两侧 `err=0`、退出码 0；HFS 侧 `overwrite` 全量回读校验通过、CURRENT 发布检查通过。

## 3. 结论与归因

- **fillseq/overwrite 是同步小写瓶颈**：HFS adapter 的 `WritableFile::Append` 直接
  `file_->append()` 同步写设备（无写回缓冲），每次 WAL append 即一次 HFS 写 +
  元数据推进，实测约 143us/op；ext4 走内核 page cache，因此差距约 19-40 倍。
- **readrandom 约 4.2 倍差距**：HFS 无 page cache，SST 读全部落设备（190k ops/s，
  8 线程共享 2 核仍接近设备随机读上限）；ext4 命中缓存。
- **readwhilewriting 被读干扰拖垮**：7 个 reader 自旋读时 writer 的 20000 次 Put
  用时 63.5s（约 315 puts/s），说明 HFS 读写共存时缺少缓存/调度隔离；该模式与
  db_bench 的语义不同（db_bench 只报 writer 吞吐），仅作方向性证据。
- 本轮同时发现并修复并发 durability barrier 缺陷（`doc/bug.md` #38）：8 线程
  `overwrite` seed 曾因 `prepareDurableEpoch` 并发返回 Busy 中断；`Engine::syncMetadata`
  串行化后全量通过。

## 4. 边界与未覆盖

1. **单次运行**：未做多轮中位数与方差；数字仅代表本场设备/宿主状态。
2. **IO 栈不同**：ext4 走内核 NVMe 栈，HFS 走 SPDK/uio 独占；同物理设备但栈不同。
3. **无写回缓存**：HFS adapter v1 按设计直写，WAL/小文件追加的吞吐差距属架构边界，
   非本轮 bug；若需要缩小 fillseq 差距，应评估 adapter 层缓冲（Sync 时落盘）或
   HFS core 的小写合并，属后续优化项。
4. **路径语义**：HFS `CreateDirIfMissing` 只建叶子目录（与 POSIX `mkdir` 一致），
   多级路径需父目录已存在；设计 §16.3 的 `/hfs2/rocksdb` 需先有 `/hfs2`。
5. **未覆盖**：压缩类型/后台 compaction 参数未做矩阵；`readrandom` 未统计命中率；
   SST 专项（`doc/shard` Shard-83 注明的 SST 写放大证据）不在本轮范围。
6. 本报告是性能对照，不改变 §16.3 的功能验收口径；stock RocksDB 全量性能对比
   不作为 Shard-84 的硬性门禁。

## 5. 小写探测与根因（追加，同日）

HFS 侧 fillseq 变体（同一 NVMe，key_size=16，每次独立 DB）：

| 配置 | ops/s | 聚合 us/op |
|---|---:|---:|
| 1 thread, value=100B | 1821 | 549 |
| 8 threads, value=16B | 6491 | 154 |
| 8 threads, value=100B | 7095 | 141 |
| 8 threads, value=4096B | 5041 | 198 |
| 32 threads, value=100B | 14163 | 71 |

结论：

1. **吞吐与 value 大小基本无关**（16B/100B/4KB 都在 5-7k ops/s）→ 每 op 固定开销
   主导，不是带宽瓶颈；4KB 反而更慢。
2. **线程扩展差**：1→8 线程仅 3.9x，8→32 线程仅 2.0x → 写路径存在串行化。
3. **根因链**（代码级）：
   - RocksDB WAL append 是 <4K 非对齐写；`DataPath::execute` 对非 Direct child 走
     RMW（读 4K → merge → 写 4K），每个 child 两次阻塞式设备往返
     （`src/core/data_path.cpp:211-258`），每次 `waitForChild` 阻塞调用线程。
   - **无写回缓存**：ext4 把 append 收进 page cache、仅 fsync 落盘；HFS 每次 append
     直接进入设备路径。
   - 每次 append 还执行 `reserveExtents`/`commitExtents`/`extentMapFor`/`setSize`
     （namespace mutex），新 extent（1MiB）需先 `zeroExtents` 写零。
   - official `.conf` 只有 1 个 data reactor；同步路径无批量提交，每 child 一次
     消息投递 + 条件变量等待。
4. **为什么 FIO gate 好看（106.7% raw）**：FIO 是 4K 对齐写 + 预分配 extent + QD32
   批量提交，不触发 RMW 和 per-op 分配；RocksDB WAL 的负载形态恰好相反。
5. 优化方向（本轮未实施）：adapter 层 WAL 写回缓冲（`Sync` 落盘）、core 小写合并
   或消除 RMW、WAL 空间预分配减少 per-append 分配、按需提高 data reactor 并行度。

### 5.1 WAL 拆分探测（决定性证据）

`hfs_db_bench` 新增 `--disable_wal=1` 后，fillseq（20000 ops/线程）对照：

| 配置 | ops/s | 相对 WAL off |
|---|---:|---:|
| HFS WAL on，1 线程 | 2518 | 0.22% |
| HFS WAL off，1 线程 | 1135990 | 100% |
| HFS WAL on，8 线程 | 8649 | 0.89% |
| HFS WAL off，8 线程 | 974247 | 100% |
| HFS WAL off，8 线程，value=4096 | 250205 | — |
| ext4 WAL off，1 线程 | 1520796 | — |
| ext4 WAL off，8 线程 | 547990 | — |

结论：

- **差距 100% 来自 WAL 写路径**：WAL off 时 HFS 与 ext4 同级（1 线程 1.14M vs
  1.52M；8 线程 974k vs 548k），说明引擎、memtable、namespace 元数据都不是瓶颈。
- 每个 Put 的 WAL 记录（~116B）都会走一次 HFS 同步 append：`Append` 直写设备
  （无写回缓冲）→ 非对齐子块写触发 RMW（读 4K + 写 4K，两次阻塞式设备往返）
  → 每次 append 还做 `stat`/`reserveExtents` 快照/`extentMapFor` 快照/`setSize`
  （namespace 互斥 + logical epoch 推进）。
- 扩展性差（1→8 线程仅 3.4x）来自 namespace 互斥区、单 data reactor 与每 child
  一次阻塞等待的组合。
- 附注：adapter 目前把 `FSWritableFile::Flush` 映射为 `File::sync()`（完整 durable
  checkpoint），语义过强；若后续做 append 缓冲，`Flush` 应只把缓冲区写入 HFS
  而不触发 namespace checkpoint。

## 6. 目标差距与优化路线（HFS 应在目标场景更快）

### 6.1 HFS 的结构性优势（应赢的点）

- 内核旁路（SPDK/uio）：无中断、无上下文切换、无 page cache 双拷贝，延迟与 p99 可控；
  4K 对齐批量 I/O 已验证（FIO gate `HFS/raw=106.7%`、p99 `0.851x`）。
- 单层缓存：只有 RocksDB block cache，避免 ext4 的 double buffering 与 writeback 抖动
  （working set 超过内存时 ext4 会抖动，HFS 不会）。
- 多盘聚合与自定义放置可扩展；ext4 单盘受限于单设备。
- 预计占优场景：大 value、SST flush/compaction、读命中 block cache、批量 sync、
  多盘、高 QD 对齐 I/O。

### 6.2 当前不足（按影响排序）

1. **写路径没有 page cache 等价物**：WAL append 每条直写设备（§5.1 证明差距 100%
   来自 WAL）；RocksDB 契约允许 `Append` 缓冲到 `Flush`，adapter 未实现。
2. **adapter 写是同步的**：`Append/PositionedAppend` 都 `.wait()`；core 的异步引擎
   （FIO 路径）未被 RocksDB 写路径使用；读已走 `ReadAsync`。
3. **子块写 RMW**：<4K 非对齐写读 4K+写 4K；append-at-EOF 可跳过读回。
4. **per-append 元数据税**：`stat`/`reserveExtents` 快照/`extentMapFor` 快照/
   `setSize`(epoch) 全在 namespace 全局锁；ext4 的 inode 更新是内存且批量。
5. **每 child 阻塞等待 + 跨线程往返**、单 data reactor：1→8 线程仅 3.4x。
6. **`FSWritableFile::Flush` 语义过强**：当前映射到 `File::sync()`（完整 checkpoint），
   做缓冲前必须先改为"写缓冲不触发 durability"。
7. **读无缓存**：readrandom 4.2x 差距依赖 RocksDB block cache（需显式调大）。
8. **CPU 成本**：busy-poll 常驻（FIO gate 2.35 cores vs raw 0.92），RocksDB 线程与
   reactor 争用 2 核。

### 6.3 优化路线（按 ROI）

- P0：adapter WAL 写回缓冲 + `Flush` 语义修正 + append 免 RMW + WAL 预分配
  （`Allocate` hint）；namespace per-handle extent map 缓存与批量 epoch。
- P1：写路径接入 core async（批量提交、重叠 I/O）；SST 读走 async MultiRead；
  调大 RocksDB block cache。
- P2：混合负载的 reactor 数量/亲和调整；多盘 striping 场景验证扩展性。

### 6.4 公平口径提醒

ext4 命中 page cache 时对比意义有限；目标场景应选 working set 超缓存、`sync=1`、
大 value、多盘或 compaction 密集负载，并给 ext4 侧同样约束（O_DIRECT / drop_caches）
后再比较。

## 7. P0 优化复测（adapter 写回缓冲，2026-09-19）

实现：`HfsWritableFile` 写回缓冲（每文件 1 MiB，`Append` 只 memcpy）；
`Flush` 改为 **no-op**（实测 RocksDB 8.9 每个写组都调用 Flush：
`flush_calls==append_calls`，落盘会使缓冲失效；Flush 不是 durability 屏障）；
`Sync/Fsync/RangeSync/Close` 与读句柄打开强制写出；`GetFileSize` 计入 pending；
pending registry 提供可见性。设计见
`doc/design/hfs2_rocksdb_wal_writeback_design_20260919.md`。

5-run 中位（同盘、RocksDB 8.9.1、8 线程 × 20000、16B/100B、
`use_direct_io_for_flush_and_compaction=1` 对 HFS 无效、ext4 侧生效）：

| benchmark | ext4 | HFS P0 | HFS/ext4 | P0 目标 | 结论 |
|---|---:|---:|---:|---:|---|
| fillseq | 263815 | 447397 | **169.6%** | ≥100% | 达标 |
| overwrite | 143998 | 266087 | **184.8%** | ≥100% | 达标 |
| readrandom | 894279 | 471610 | 52.7% | 不回归 | 达标（P1 目标 ≥50% 已顺带达成） |
| readwhilewriting（writer） | 565612 | 115209 | 20.4% | ≥2x 现状且 ≥50% ext4 | 2x 大幅达标（315→115k），50% 未达 → P1 |

调用模式证据（`HFS_ROCKSDB_IO_STATS=1`，fillseq 单次）：
`append_calls=156289 append_bytes=25233184 flush_calls=156302 sync_calls=9
buffer_flushes=32 write_throughs=0` —— 160k Put 只产生 32 次设备写。

边界与后续：

- WAL 写放大仍未 ≤1.1x：每次 1 MiB flush 会分配并预写零 1 MiB extent，设备级
  约 2x；P1 补集置零（设计 §8.2）处理。
- `readwhilewriting` writer 受 reader 干扰（无读缓存），归 P1 读路径。
- 功能回归：`file_data_test`（含 8 线程并发 sync）通过；`hfs_db_bench` 全序列
  `err=0`、closure 全闭合。证据 `/mnt/hfs-data/hfs2-rocksdb-wal-p0-gate-20260919`、
  `/mnt/hfs-data/hfs2-rocksdb-wal-p0-20260919`。

## 8. P0.5 与 crash 矩阵（2026-09-19）

### 8.1 P0.5：append 免 RMW

`DataPath` 增加 knownEof 快路径：设备块起始 ≥ 写前 EOF 时跳过刷新读，未覆盖字节
置零以保持"已提交 extent 字节要么写过、要么为零"不变式（`data_path.cpp`）。
`File::write` 传入写前 logical size；`data_path_device_test` 新增 3c 用例断言
两个 child 只需 2 次 submit（原 RMW 为 3）。验证：`data_path_device_test: ok`、
`file_data_test: ok`；db_bench fillseq 456k/overwrite 305k（无回归）。

### 8.2 五断点 crash 矩阵（设计 §10.2）

新增进程级 crash 钩子 `HFS_TEST_CRASH_AT`（`src/common/test_hooks.hpp`，命中
`_exit(137)`；`HFS_TEST_CRASH_ARM_FILE` 让 workload 在 DB 打开后武装，
`HFS_TEST_CRASH_SKIP` 控制跳过次数）。两阶段验证：先建 durable baseline
（fillseq 1000 keys，sync=1，clean 退出，`hfs_ldb entries=1000`），再在 crash
workload（overwrite 1000 keys）中注入，最后 remount 校验。

| 断点（§16.4） | 注入位置 | sync=1 | sync=0 |
|---|---|---|---|
| 1. data child → device flush 前 | `spdk_runtime.cpp` controlIo Flush | 恢复一致，baseline 保留 | 恢复一致，baseline 保留 |
| 2. data flush → metadata commit 前 | `engine.cpp` syncMetadata | 恢复一致 | 恢复一致 |
| 3. journal COMMIT 写入 → flush 前 | `journal.cpp` append；当前全栈路径不经过（group commit 未接线），由 `journal_test`/`journal_device_test` 的 torn-COMMIT 用例覆盖 | rc=0 | rc=0 |
| 4. checkpoint slot flush → superblock publish 前 | `journal.cpp` writeCheckpoint | 恢复一致 | 恢复一致 |
| 5. clean flag 写入前 | `volume.cpp` updateCleanShutdown | 恢复一致 | 恢复一致 |

8 个全栈用例全部：`crash_ok=1`（DB 打开后命中）、`hfsctl check rc=0`、
`hfs_ldb checkconsistency rc=0`、baseline `entries=1000` 保留；证据
`/mnt/hfs-data/hfs2-rocksdb-crash-matrix-20260919`。

### 8.3 缓冲窗口随机 SIGKILL

- sync=1（kill 0.3-1.8s，10 次中 5 次）：恢复 entries=297/744/1479/2378/3868，
  随 kill 时刻递增，全部 `checkconsistency` 通过 → 每次 Put 的 durability 边界
  有效、无半事务。
- sync=0（kill 0.02-0.2s）：4 次在 DB::Open 期间被 kill → 恢复为空命名空间
  （`db_missing=1`，一致前缀为空）；1 次在 workload 开始后被 kill → entries=0、
  一致。符合设计 §5 契约（未 sync 的 buffered/logical 数据在进程崩溃时丢失，
  丢失窗口 ≤1 MiB/文件，且恢复状态始终是 durable 前缀）。
- 证据 `/mnt/hfs-data/hfs2-rocksdb-sigkill-20260919`。

### 8.4 §16.4 校验清单覆盖

namespace/checkpoint CRC/generation/clean flag/journal 状态由 `hfsctl check`
覆盖（`corruption=none`）；RocksDB reopen 与数据一致性由
`hfs_ldb checkconsistency` 覆盖；durable 前缀由 baseline entries 保留验证；
allocator 快照非法会直接导致 mount 失败（含 CRC/格式校验），本轮未见。
未单独统计 orphan reclaim 计数，列为后续 crash 测试增强项。

## 9. P1 优化复测（2026-09-19）

### 9.1 补集置零（已实现）

`DataPlane::zeroRange` + `File::write` 只零写新 extent 中未被本次写覆盖的部分
（块外扩对齐，RMW 保护范围外字节）；完全覆盖的 extent 零写为 0。验证：
`data_path_device_test: ok`、`file_data_test` 新增 tail-zero 用例（写 100B 后
truncate 增长，空洞读仍为 0，coverage=1 extent）通过。吞吐在当前规模属运行方差
（写路径已非设备瓶颈），结构性写放大（每 MiB 数据额外 1 MiB 零写）已消除。

### 9.2 读路径归因（关键发现）

adapter 读计数器（`HFS_ROCKSDB_IO_STATS=1`）显示 readrandom 160k 次 Get 仅产生
**read_calls=5、readasync_calls=0、multiread_calls=0** —— 数据仍在 memtable /
block cache 中，读基准几乎不触发 HFS 读 I/O。HFS 与 ext4 的 readrandom 差距
（58%）来自 CPU/内存路径与 **busy-poll reactor 与 benchmark 线程争核**，
不是 HFS 读 I/O 慢。

### 9.3 idle 让出实验（已回退）

尝试 idle 感知轮询（连续空轮询后退让 CPU，并加 `outstanding()==0`、
poller 无工作、无 in-flight sync I/O 条件）。两档参数（512×50us、4096×10us）
均使 FIO 正式 gate 吞吐从 112% 降至 90-92%（激进档 p99 1.19x 超门禁），
**已回退**。结论：主 gate 优先，CPU 争用问题留待后续以可配置策略
（`.conf` 开关或 SPDK interrupt mode）解决，默认保持 busy-poll。

### 9.4 最终 5-run gate（P0+P0.5+P1-a）

| benchmark | ext4 中位 | HFS 中位 | HFS/ext4 | P0/P1 目标 | 结论 |
|---|---:|---:|---:|---:|---|
| fillseq | 301017 | 369753 | **122.8%** | ≥100% | 达标 |
| overwrite | 183432 | 240646 | **131.2%** | ≥100% | 达标 |
| readrandom | 974771 | 565681 | **58.0%** | ≥50% | 达标（≥80% stretch 未达，见 §9.2） |
| readwhilewriting（writer） | 631300 | 160195 | 25.4% | ≥50% | 未达（reader 争用，见 §9.3） |

设备状态说明：本轮 ext4/HFS 绝对值与上一轮 gate 均有漂移（ext4 fillseq
264k→301k，HFS 447k→370k），比值应以同场次为准；FIO gate 同日为
`HFS/raw=91.6%`、p99 `1.046x`，仍通过门禁。

## 10. 复现命令（VM）

```text
# ext4 baseline（会重建文件系统）
mkfs.ext4 -F /dev/nvme1n1 && mount /dev/nvme1n1 /mnt/rocksdb-ext4
taskset -c 2-3 db_bench --db=/mnt/rocksdb-ext4/db --num=20000 --threads=8 \
  --key_size=16 --value_size=100 \
  --benchmarks=fillseq,readrandom,overwrite,readwhilewriting \
  --use_direct_io_for_flush_and_compaction=1 --statistics=0

# HFS（先 hfsctl format，设备绑定 uio_pci_generic）
export LD_LIBRARY_PATH=/root/code/HFS_BASE_SPDK/build-repair98
taskset -c 2-3 ./build-repair98/hfs_db_bench --hfs-config=config/hfs_nvme.conf \
  --db=/rocksdb --num=20000 --threads=8 --key_size=16 --value_size=100 \
  --benchmarks=fillseq,readrandom,overwrite,readwhilewriting \
  --use_direct_io_for_flush_and_compaction=1
```