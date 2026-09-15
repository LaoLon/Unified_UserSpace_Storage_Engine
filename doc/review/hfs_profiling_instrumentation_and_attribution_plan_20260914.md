# HFS 最小侵入 Profiling / Instrumentation 与性能归因方案

日期：2026-09-14  
对象：当前 `main`（`add0722`）  
目的：定位 HFS 相比 raw SPDK 约 30%～37% 性能损失的实际软件开销归属。  
本轮范围：设计 profiling 和统计落点，不改变 HFS 数据面架构，不在 I/O hot path 输出日志。

## 1. 先给出边界和目标

当前已知的 FIO 现象来自 `doc/performance/hfs_final_performance_baseline_20260913.md`：HFS/raw SPDK 在 4 KiB～64 KiB NVMe randrw 基线约为 63.4%～70.9%。这个比例只说明端到端结果，不说明差距由 copy、ticket、message、completion、futex 还是 SPDK/设备 latency 构成。

本方案的目标不是先改架构，而是为一次“逻辑 POSIX I/O”建立可闭合的时间线：

```text
T0 application enters hfs_pread/pwrite
T1 fd/open-file lookup complete
T2 range/ticket ready
T3 buffer/staging ready
T4 request enqueue complete
T5 data/owner reactor dequeue
T6 SPDK submit call
T7 SPDK completion callback
T8 copy-out/completion processing complete
T9 application wait returns / thread is woken
T10 POSIX API returns
```

每个 stage 需要至少输出 `count`、`total cycles`、`average`、`p50`、`p95`、`p99`、`max`。此外必须能回答：

* 实际有多少请求走 DMA direct、多少走 staging copy；
* ticket 是否真的发生冲突和等待；
* enqueue/dequeue、owner/data reactor message 是否形成提交串行点；
* 应用线程是否因为 futex/condvar 睡眠；
* HFS 自己产生了多少 request allocation/free、SPDK outstanding 和 queue depth；
* fsync/FLUSH 是否进入测试；durability coordinator 的 batch 实际有多大。

## 2. 当前代码中已经存在的统计基础

现有实现并不是从零开始，`src/hfs_stats.h`/`src/hfs_stats.c` 已经有一套“默认关闭、进程退出统一写快照”的基础设施：

* `hfsStatsEnabledState`、`hfsStatsFastEnabledState`、`hfsStatsTimingState` 作为开关；`HFS_STATS_PATH`/`HFS_STATS=1` 控制是否输出。
* `HfsStatsShard` 按线程懒注册，记录锁、runtime、I/O、DMA、metadata 和 durability 的局部累计值；`hfsStatsAggregateShard` 在退出时汇总。
* 现有计数包括 `spdk_submit/spdk_complete`、`hfsStatsIoOutstanding`/peak、data reactor submit/complete、queue credit/depth/drain、ticket admit/ready/complete/dependency wait、DMA acquire/wait/direct、copy bytes、durability batch/epoch/flush 等。
* 现有锁 timing 使用 `hfsStatsNowNs()` 和每线程 `HfsStatsLockFrame`，不存在每次锁操作都访问全局 mutex 的设计。

因此新增 profiling 应复用此生命周期和 per-thread shard，而不是引入一个新的全局统计锁或在每个 I/O 结束时 `printf`。现有统计仍有明显缺口：没有覆盖 T0～T10 的请求级 stage timestamp，没有 stage p50/p95/p99/max，没有明确的 owner/data message 总数、futex sleep、spin ticks、request 生命周期计数，也没有完整的 durability batch size 分布。

## 3. 最小侵入设计

### 3.1 两个开关、三种成本等级

新增统计应保持以下语义：

| 模式 | 建议开关 | hot path 行为 | 用途 |
|---|---|---|---|
| off | 默认，无 `HFS_PROFILE` | 一个廉价的 branch，不能调用 clock/分配/锁 | 正式性能基线 |
| counter | `HFS_PROFILE=counter` 或现有 `HFS_STATS=1` | per-thread/per-reactor 本地计数，必要的 TSC 采样 | A～G 归因的主体 |
| timing | `HFS_PROFILE=timing` | 保存一次请求的 TSC 时间戳，更新固定 histogram | stage latency、p50/p95/p99 |

建议继续允许 `HFS_PROFILE_SAMPLE=N`：`N=1` 表示全量记录，`N=100` 表示按 parent/request ID 每 100 个请求采样一次。counter 类指标不采样；stage latency 在高并发运行中按 N 采样，以避免统计本身改变队列和调度。

### 3.2 时间源

* 首选 `spdk_get_ticks()`，只做寄存器/TSC 读取，不在请求 hot path 调用 `clock_gettime`。
* 启动或 `hfsStatsInit()` 时读取一次 `spdk_get_ticks_hz()`；报告阶段把 ticks 转成 ns/us。
* 每个 stage 只保存一个 `uint64_t` tick。差值在 T10 的应用线程完成，直方图更新也由该线程执行，避免 reactor 与应用同时更新同一个统计桶。
* 如果当前 SPDK 版本没有可用的 ticks-hz API，才回退到已有 `hfsStatsNowNs()`；回退路径必须在快照中输出 `clock_source=monotonic_ns`，不能把 ns 标签误写成 cycles。

### 3.3 请求级上下文

建议增加一个仅用于 profiling 的小结构（可以放在 `src/hfs_io.h` 或独立 `src/hfs_profile.h`）：

```c
typedef struct HfsIoProfile {
    uint64_t parentId;
    uint64_t ticks[11];       /* T0..T10, zero means not observed */
    uint64_t bytes;
    uint32_t reactorId;
    uint16_t flags;           /* read/write/direct/staging/ticket/owner/data */
    uint16_t childCount;
} HfsIoProfile;
```

`HfsIoProfile` 由 `hfs_lib_pread/pwrite` 的调用栈创建，作为可选指针沿内部 `hfsIoReadAt/hfsIoWriteAt` 传到 `HfsIoRequest`。它不改变请求所有权和完成协议；因为 POSIX API 在 `hfsIoWait*` 后才返回，stack profile 在最后一个 child completion 前始终有效。pipeline 的多个 child 只共享一个 parent profile，另用 child counters 记录实际 SPDK submit/completion 数。

不建议把 profile 放到全局 map：那会引入哈希、分配和锁，直接污染要测量的路径。

### 3.4 直方图和 p50/p95/p99

每个线程/每个 reactor 的 `HfsStatsShard` 增加：

```text
HfsLatencyStat {
    count
    totalTicks
    maxTicks
    hist[64]
}
```

`hist[64]` 采用 log2 ticks 桶，更新只需一次比较/移位和一次数组加法；统计退出时先合并所有 shard，再按累计桶计算 p50/p95/p99。报告中的分位点是对应桶上界，必须同时输出 `hist_kind=log2_upper_bound`。

若某次实验需要精确分位点而不接受桶上界，可设置 `HFS_PROFILE_EXACT=1`，使用每 stage 固定大小的 reservoir（例如每 shard 4096 个样本）离线排序；这只用于低 QD 或短时间验证，不作为正式 32/64 QD 吞吐基线。

### 3.5 cacheline 和汇总原则

* 每个应用线程使用自己的 `HfsStatsShard`，每个 data reactor 使用自己的 reactor shard；将高频统计 shard 按 64-byte 对齐，避免不同 producer 写入同一 cacheline。
* 不在 I/O hot path 访问 `g_hfsStatsShardLock`；线程第一次注册时才获取一次注册锁。
* completion 线程只写 request profile 的 T7 和 per-reactor counter；应用线程在 wait 返回后完成阶段差值并更新自己的 latency shard。completion 与应用之间已有 completion state release/acquire，可以保证这些时间戳的可见性。
* 进程结束或显式 `hfsStatsFini()` 时一次性写 KV/TSV 快照；不在 reactor、SPDK callback 或 syscall hook 中输出文本。

## 4. T0～T10 的具体插桩落点

下面的文件和函数是当前代码中的实际落点。表中的“建议动作”是统计动作，不是数据面架构改造。

| 点 | 当前代码位置 | 定义和记录内容 | 执行线程 |
|---|---|---|---|
| T0 | `src/hfs_lib.c:hfs_lib_pread`、`hfs_lib_pwrite` 入口；`src/hfs_lib.c:hfs_lib_read/write` 可作为隐式 offset 补充 | 创建 `HfsIoProfile`，写入 parentId、bytes、read/write、T0 | application |
| T1 | `src/hfs_lib.c:hfs_acquire_file` 返回后 | `hfsFdAcquire` 已完成、`HfsOpenFile`/inode 已取得；写 T1，并累加 fd fast/slow | application |
| T2 | `src/hfs_io.c:hfsIoReadAt`/`hfsIoWriteAt` 中 `hfsPosixTicketWaitReady` 返回后；ticket disabled 时对应 `hfsIoRangeLock` 成功后 | 写 T2；记录 ticket enabled、pending dependency、conflict/wait | application |
| T3 | `src/hfs_io.c:hfsIoReadAtLocked`/`hfsIoWriteAtLocked`，lease/direct 判定完成处 | direct：DMA capability 判定完成；staging：`hfsIoAcquire` 完成，写请求 copy-in 完成；记录 direct/staging 和 copy bytes | application |
| T4 | `src/hfs_runtime.c:hfsRuntimeSubmitDataBatch`/`hfsRuntimeSubmitBatch` 返回后；fallback `hfsRuntimeSubmitData` 也要标记 | queue 链接成功或 owner/data message 已提交；记录 enqueue、owner/data message、queue depth、fallback | application |
| T5 | `src/hfs_runtime.c:hfsRuntimeDispatchDataBatch`/`hfsRuntimeDispatchBatch` 取出 item 的位置 | 写 T5；记录 dequeue、queue wait ticks、batch size、reactor id | owner/data reactor |
| T6 | `src/hfs_io.c:hfsIoStart` 调 `spdk_blob_io_read/write/readv/writev` 紧邻位置 | 写 T6；记录 SPDK-visible submit 和 outstanding++；不能声称这是 NVMe doorbell | owner/data reactor |
| T7 | `src/hfs_io.c:hfsIoDone` 入口；控制面对应 `hfsMetaOpCb`/`hfsSpdkBdevFlushCb` | 写 T7；记录 completion、outstanding--、SPDK rc、reactor id | SPDK channel thread |
| T8 | `src/hfs_io.c:hfsIoWaitReadPipeline` copy-out 和 `hfsIoWaitPipeline` 完成后 | read 在最后一次 copy-out 后；write 在 child completion/lease release 后；写 T8 | application |
| T9 | `src/hfs_runtime.c:hfsRuntimeCompletionWait` futex wait 返回处；ticket/durability cond wait 返回处另记 sleep | 写 T9，记录 futex/cond sleep count、sleep ticks；不要试图从 callback 推断线程已被调度 | application |
| T10 | `hfs_lib.c:hfs_lib_pread/pwrite` cleanup、`hfsFdRelease` 和 errno/result 已确定、真正 return 前 | 写 T10，计算 T0～T10 差值，更新所有 percentile 和逻辑请求 count | application |

注意：T9 的严格含义是“应用线程从等待原语返回”，不是 callback 发出 wake 的时刻。callback wake 可另记 `wakeup_issued`；调度延迟是 `T9 - T7` 的一部分。

## 5. 指标清单与当前代码落点

### 5.1 必须新增或补齐的请求指标

| 指标 | 记录方式 | 当前相关代码 |
|---|---|---|
| fast/slow path requests | `path_fast/path_slow` counter；至少区分 fd、DMA、queue、resize | `hfsFdAcquire`、`hfsIoAcquire`、`hfsResizeTo`、runtime submit |
| DMA direct ratio | `direct_ops/direct_bytes` | 已有 `hfsStatsRecordIoDirect`，需和 parent count 关联 |
| staging ratio | `staging_ops/staging_bytes` | `hfsIoReadAtLocked`/`hfsIoWriteAtLocked` 的 lease 分支 |
| copy bytes/ops | copy-in、copy-out 分开计数 | 已有 `hfsStatsRecordCopy`，需增加方向字段 |
| ticket conflict | admission 扫描中 `pendingDependencies>0` 的次数和依赖总数 | `hfs_ticket.c:hfsPosixTicketAdmit` |
| ticket wait count/time | 仅真正进入 cond wait 时计数；不能把 ready ticket 误算成 wait | `hfsPosixTicketWaitReady` |
| reactor enqueue/dequeue | 每个 batch item 一次；并区分 queue/fallback | `hfsRuntimeSubmit*`、`hfsRuntimeDispatch*` |
| owner/data message | 以 `spdk_thread_send_msg` 目标 reactor 分类 | `hfsRuntimeSendMessage` 调用者/submit 函数 |
| futex/cond sleep | futex `WAIT` syscall 成功一次计一次；EAGAIN 不算 sleep；cond wait 入口/返回配对 | `hfsRuntimeCompletionWait`、ticket/durability/pool cond |
| spin wait time | 当前应用路径没有 spin；reactor `spdk_thread_poll` 不计作应用 spin | 只在实验 F 打开后由专门 wait wrapper 记录 |
| request allocation/free | 区分 stack logical request 与 heap allocation | `HfsIoRequest`/`HfsRuntimeMessage`/vector bounce/durability batch |
| SPDK outstanding | submit 时 +1，completion 时 -1，记录 peak | 已有 `hfsStatsIoOutstanding` |
| queue depth | enqueue/dequeue 即时值和 peak，按 reactor | 已有 `hfsStatsQueueDepthPeak`，需补平均/分布 |
| submit/completion per second | snapshot 中记录开始/结束 tick；输出 SPDK-visible submits/s | `hfsStatsSpdkSubmits/completions` |
| fsync/FLUSH | count 和 latency | `hfs_lib_fsync/fdatasync`、`hfsDeviceFlush` |
| durability batch distribution | 固定 1、2、3、4、8、16、32、64、>64 桶 | `hfsDurabilityWorker` 取出 queue 后 |

### 5.2 现有统计可以直接复用的字段

以下字段已经在 `src/hfs_stats.h` 中存在，不应重复实现：

* `hfsStatsSpdkSubmits`、`hfsStatsSpdkCompletions`、`hfsStatsIoOutstanding`、`hfsStatsIoOutstandingPeak`；
* `hfsStatsQueueCreditWaits[]`、`hfsStatsQueueDepthPeak[]`、`hfsStatsQueueDrainBatches[]`、`hfsStatsQueueDrainItems[]`；
* `hfsStatsTicketAdmits`、`hfsStatsTicketDependencyWaits`、`hfsStatsTicketDependencyWaitNs`、`hfsStatsTicketWakes`；
* `hfsStatsDmaAcquires`、`hfsStatsDmaWaits`、`hfsStatsDmaFastAcquires`、DMA active peak；
* `hfsStatsDurabilityBatches`、batch requests/max、epochs、metadata commits、flushes、fdatasync fast；
* `hfsStatsRecordCopy`、`hfsStatsRecordIoDirect`、`hfsStatsRecordRmw` 和 lock wait/hold hist。

新增代码只需补齐方向、分支和 latency 维度，避免把已有 counter 改成昂贵的全局原子统计。

## 6. 对现有函数的最小改动边界

### 6.1 `hfs_lib.c`

只增加可选 profile 创建、T0/T1/T10 和结果收口：

1. `hfs_lib_pread`/`hfs_lib_pwrite` 入口检测 profile 开关并创建 stack context。
2. `hfs_acquire_file` 返回后写 T1。
3. 在所有早退分支统一走 cleanup，确保失败请求也有 T10 和 status；不能只统计成功请求。
4. 将 profile 指针传给 `hfsIoReadAt`/`hfsIoWriteAt` 的内部调用，`hfs_lib_read/write` 复用同一个 helper。
5. `hfsPublishWriteEnd` 不新增设备 I/O，只记录是否走 fast metadata overlay 或 write-lock fallback。

### 6.2 `hfs_io.c`

只增加 T2/T3/T8 和 child T4～T7 的时间戳/flags：

* `hfsIoReadAt`/`hfsIoWriteAt` 负责 ticket/range marker；
* `hfsIoReadAtLocked`/`hfsIoWriteAtLocked` 负责 direct/staging、copy 方向和 child count；
* `hfsIoStartRequest` 只负责把 parent profile 指针或 child timestamp 槽位挂到已有 `HfsIoRequest`；
* `hfsIoStart` 和 `hfsIoDone` 只读写一次 tick 并更新已有 SPDK counter；
* `hfsIoWaitReadPipeline`/`hfsIoWaitPipeline` 在 copy/release 结束处写 T8。

不改变 `HfsIoRequest` 的所有权、channel 选择、pipeline depth 或 ticket 语义。

### 6.3 `hfs_runtime.c`

在已有 queue 的 enqueue/dequeue 点补计数和可选 tick：

* `hfsRuntimeSubmitDataBatch`/`hfsRuntimeSubmitBatch`：T4、owner/data message、enqueue path；
* `hfsRuntimeDispatchDataBatch`/`hfsRuntimeDispatchBatch`：T5、dequeue、queue wait、batch size；
* `hfsRuntimeCompletionWait`：T9、futex sleep；
* `hfsRuntimeSendMessage`：只做目标类型计数，不在该全局锁内取时间戳。

### 6.4 `hfs_ticket.c`、`hfs_fd.c`、`hfs_durability.c`

* `hfs_ticket.c`：区分 conflict admission 和真正 `pthread_cond_wait`，已有 dependency wait ns 不能代表所有 ticket admission latency。
* `hfs_fd.c`：区分 bucket 无竞争快速 acquire、bucket mutex 实际等待和 generation/ref 重试；现有 fd lock counter 只说明加锁次数，不说明 retry/等待比例。
* `hfs_durability.c`：worker 取 queue 时记录实际 batch size histogram；`hfsDurabilityRequestSubmit` 记录 cond sleep；保留现有 epoch/batch/flush counters。

## 7. 统计开销控制

### 7.1 不能做的事

* 不在 `hfsIoDone`、`hfsRuntimeDispatch*` 或 preload hook 中 `printf`、`fprintf` 或写文件。
* 不为每个 request `malloc` 一个 profile；使用调用栈对象和已有 request 生命周期。
* 不在所有阶段都使用 `clock_gettime`；这会把测量开销混入 T0～T10。
* 不在每个阶段用全局 atomic 更新 percentile；只写 profile tick，T10 在 owner application shard 汇总。
* 不把应用线程的 futex wait 与 SPDK reactor 的 polling 混称为 spin。

### 7.2 允许的开销

profile 关闭时只保留一次静态布尔判断。counter 模式允许每个请求少量 relaxed atomic 或本地 shard 加法；timing 模式允许每个 T 点一次 `spdk_get_ticks()` 和一次 `uint64_t` store。高并发 timing 运行必须使用采样；正式 IOPS/BW 比较使用 profile off。

## 8. A～G 实验矩阵

### 8.1 实验定义

| 实验 | 数据路径 | 目的 | 约束 |
|---|---|---|---|
| A | raw SPDK | 给出同设备、同 workload 的软件基准 | 不加载 `libhfs.so`，使用 raw SPDK fio engine |
| B | 完整 HFS | 当前 POSIX preload 实际路径 | 默认 ticket、queue、staging/direct 判定全部开启 |
| C | HFS + DMA-safe buffer | 只观察 bounce copy 是否造成差距 | 必须确认 buffer 被 `spdk_vtophys` 判定为可 DMA；否则不能称 C |
| D | HFS + 临时 bypass range ticket | 观察 ticket admission/conflict 成本 | 只允许无重叠 randread/randwrite；不能用于有写写/读写冲突的 workload |
| E | HFS + 临时 bypass owner reactor | 观察 owner message/控制面是否在数据路径中 | 只作为诊断开关；不能跳过必须由 owner 执行的 metadata/blob control 操作 |
| F | HFS + polling/spin completion | 观察 futex/scheduler 延迟 | 只允许短时低 QD 诊断；必须单独记录 spin ticks/CPU |
| G | HFS + C+D+E+F | 估计多个可移除项的上限叠加 | 不是可提交的正式模式，结果只用于上限归因 |

D/E/F 的开关应是实验构建或环境变量，并默认不存在/关闭；它们不能影响正式 POSIX 语义。若某个开关改变了 ordering、owner 数据一致性或 completion 语义，该实验结果必须标记为 invalid，不能与 B 做性能百分比相减。

### 8.2 workload 组合

```text
block size: 4K, 16K, 64K
rw: randread, randwrite
QD: 1, 8, 32, 64
每项：至少 3 次 warm-up 后正式运行 30 s，记录 3 次 median；另保留一次 timing run
```

每个组合输出：

* IOPS、BW；
* fio avg latency、p50、p99；
* CPU utilization（user/system/total，按 HFS application、reactor、SPDK 进程可分组时分别记录）；
* `hfs_profile.tsv`：T0～T10 stage histogram 和 per-request counters；
* `hfs_stats` 快照：queue、ticket、DMA、SPDK outstanding、fsync/flush、durability batch；
* workload metadata：HFS commit、SPDK commit、NVMe BDF、reactor 数、queue depth、buffer allocation mode。

### 8.3 命令模板

HFS B 的基线可以复用 `scripts/performance/fio_stage2_compare.sh` 的 HFS 分支，但默认配置必须改为项目记忆规定的 NVMe `config/hfs_nvme.json`/`HFS_BDEV_NAME=Nvme0n1`，结果目录放 `/mnt/hfs-data/<run>`。示意：

```bash
mkdir -p /mnt/hfs-data/profile/B-4k-qd32
HFS_STATS_PATH=/mnt/hfs-data/profile/B-4k-qd32/hfs-stats.tsv \
HFS_PROFILE=timing HFS_PROFILE_SAMPLE=1 \
HFS_CONFIG_PATH=config/hfs_nvme.json HFS_BDEV_NAME=Nvme0n1 \
LD_PRELOAD=build-cmake/libhfs.so \
fio --name=rw --ioengine=psync --thread=1 --numjobs=32 \
    --rw=randwrite --bs=4k --iodepth=1 --size=256m \
    --direct=1 --runtime=30 --time_based=1 \
    --filename=/hfs/fio/rw.dat --output-format=json \
    --output=/mnt/hfs-data/profile/B-4k-qd32/fio.json
```

这里的 `psync + numjobs` 用于 HFS POSIX 路径；A raw SPDK 应使用 `ioengine=spdk_bdev` 和对应 `iodepth=QD`，不能把 HFS 的 `numjobs` 和 raw SPDK 的 `iodepth` 当作同一个并发模型。C～G 只在 HFS 命令中替换临时诊断开关，并将开关值写入 `environment.txt`。

## 9. 报告格式

### 9.1 阶段 latency 表

每个 workload/实验一行，字段如下：

| case | bs | rw | qd | stage | count | total_cycles | avg_cycles | p50_cycles | p95_cycles | p99_cycles | max_cycles | avg_us | p99_us |
|---|---:|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| B | 4K | randwrite | 32 | T0-T1 | … | … | … | … | … | … | … | … | … |
| B | 4K | randwrite | 32 | T1-T2 | … | … | … | … | … | … | … | … | … |
| B | 4K | randwrite | 32 | T2-T3 | … | … | … | … | … | … | … | … | … |
| B | 4K | randwrite | 32 | T3-T4 | … | … | … | … | … | … | … | … | … |
| B | 4K | randwrite | 32 | T4-T5 | … | … | … | … | … | … | … | … | … |
| B | 4K | randwrite | 32 | T5-T6 | … | … | … | … | … | … | … | … | … |
| B | 4K | randwrite | 32 | T6-T7 | … | … | … | … | … | … | … | … | … |
| B | 4K | randwrite | 32 | T7-T8 | … | … | … | … | … | … | … | … | … |
| B | 4K | randwrite | 32 | T8-T9 | … | … | … | … | … | … | … | … | … |
| B | 4K | randwrite | 32 | T9-T10 | … | … | … | … | … | … | … | … | … |

`total_cycles` 是所有样本 ticks 差值的总和；`avg_cycles=total/count`。转换到 us 时使用同一运行中记录的 `ticks_hz`，不同 case 不能混用 host wall-clock 估算。

### 9.2 请求结构表

```text
logical_io_count
spdk_child_count
fast_path_count / slow_path_count
direct_count / staging_count
copy_in_ops / copy_in_bytes
copy_out_ops / copy_out_bytes
ticket_admit / ticket_conflict / ticket_wait / ticket_wait_ticks
reactor_enqueue / reactor_dequeue
owner_message / data_message
futex_sleep / cond_sleep / spin_ticks
request_create / request_destroy / heap_alloc / heap_free
spdk_submit / spdk_complete / outstanding_peak
queue_depth_avg / queue_depth_p99 / queue_depth_peak
fsync_count / flush_count
durability_batch_hist[1,2,3,4,8,16,32,64,>64]
```

### 9.3 CPU 和设备核对

HFS 的统计只能准确表示“SPDK-visible submit/completion”，不能声称直接读到了 NVMe doorbell/CQ。报告中应使用两个名字：

* `spdk_submit/s`、`spdk_complete/s`：来自 HFS callback 边界；
* `nvme_submit/s`、`nvme_complete/s`：只有采集 SPDK/NVMe driver PMU 或 bdev 级数据后才能填写，否则写“无法从当前 HFS 代码确定”。

CPU utilization 必须来自外部系统统计（例如 `pidstat`/`perf stat`/VMware vCPU 统计），不能用 HFS 的 ticks 总和伪造 CPU 百分比。

## 10. Bottleneck attribution 归因方法

### 10.1 端到端守恒关系

对单 child、无重叠、无超时的典型请求，先检查：

```text
T10 - T0
≈ (T1-T0) + (T2-T1) + (T3-T2) + (T4-T3)
  + (T5-T4) + (T6-T5) + (T7-T6)
  + (T8-T7) + (T9-T8) + (T10-T9)
```

多 child pipeline 需要同时报告 `logical_io_count` 和 `spdk_child_count`；父请求时间不能简单除以 child 数。时间线不闭合时优先标记 sample lost/early return，而不是把差值归到某个瓶颈。

### 10.2 最终 attribution 表模板

| 类别 | 直接证据 | 主要字段 | attribution 计算 | 结论门槛 |
|---|---|---|---|---|
| POSIX/fd | T0-T1、fd wait/hold | `T1-T0`、fd fast/slow/retry | `(T1-T0)/(T10-T0)` | B 比 A 增加且 fd bucket wait 与之同步 |
| ticket/range | T1-T2、conflict/wait | `ticket_conflict`、`ticket_wait_ticks` | `(T2-T1)/(T10-T0)`；D-B 差值作交叉验证 | D 只在无冲突 workload 有效 |
| buffer copy | T2-T3、copy bytes | direct/staging、copy in/out | `copy_ticks/(T10-T0)`；C-B 与 copy bytes/s 对照 | C 必须被 `spdk_vtophys` 证实 |
| reactor queue/message | T3-T5 | enqueue/dequeue、owner/data msg、queue wait | `(T5-T3)/(T10-T0)`；E-B 观察上限 | queue wait 与 queue depth 必须同时上升 |
| SPDK/device | T6-T7 | submit/completion、outstanding | `(T7-T6)/(T10-T0)` 与 A 对比 | HFS 与 A 的该段接近时不能归为 HFS 软件 |
| completion/wakeup | T7-T10 | futex/cond sleep、T9/T10 | `(T10-T7)/(T10-T0)`；F-B 分解 sleep/spin | F 的 CPU 代价必须单独报告 |
| durability | fsync/flush | fsync count、flush count、batch hist | 按 fsync 请求独立统计，不混入 steady-state FIO | 没有 fsync 的 FIO 行填 N/A |
| request/allocator | create/free、heap alloc/free | request counters | `alloc_ticks/(T10-T0)`；只归 HFS-owned allocation | 不把 libc/SPDK 内部 malloc 猜成 HFS |

### 10.3 归因规则

* 只有同时满足“阶段 latency 上升”和“对应事件计数/bytes 上升”才把差距归到该类别。
* C、D、E、F 是实验上限，不是可以直接相加的正式收益；G 只表示组合上限，可能存在交互效应。
* 如果 A 与 B 的 T6-T7 接近，但 B 的 T0-T6/T7-T10 变长，损失应归软件路径；如果 T6-T7 本身在 B 显著变长，需保留 SPDK queue/device contention 的可能，不能只归 HFS wrapper。
* 如果 direct ratio 很高但 C-B 几乎没有变化，copy 不是当前主要损失；如果 staging ratio 高且 T2-T3/copy ticks 随 block size 成比例增长，copy 才有代码和实测双重证据。
* ticket conflict=0 但 T1-T2 很高，说明 admission/list/mutex 本身而不是 ordering wait；D 的结果用于区分两者。
* `spdk_submit == spdk_complete` 是完整性检查，不表示 latency 没有排队；必须看 outstanding 和 T4-T7。

## 11. 期望输出的最终表（填实测数据后）

下面是最终文档应包含的单页摘要，当前只给出字段，不伪造数值：

| bs | rw | qd | A raw SPDK IOPS | B HFS IOPS | B/A | C-B copy delta | D-B ticket delta | E-B owner/message delta | F-B wakeup delta | G upper bound | 主归因 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 4K | randread | 1 | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| 4K | randread | 8 | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| 4K | randread | 32 | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| 4K | randread | 64 | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| 4K | randwrite | 1 | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| 16K | randread | 32 | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| 16K | randwrite | 32 | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| 64K | randread | 32 | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| 64K | randwrite | 32 | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |

`TBD` 必须在实际 A～G 矩阵完成后替换；不能把当前 63.4%～70.9% 端到端基线直接填到某个软件阶段。

## 12. 不确定性和禁止过度解读

1. 当前 HFS 源码可以观测 SPDK API submit/completion，但不能单独观测 NVMe controller doorbell、completion queue 或设备内部 latency。
2. “request allocation/free”在 scalar 数据路径中主要是 stack 生命周期，不应误写成 malloc/free；只有 `HfsRuntimeMessage`、vector bounce、metadata 和 durability batch 的 HFS-owned heap allocation 可以直接计数。
3. `spdk_thread_poll` 是后台 reactor polling，不等于应用线程 spin completion。F 实验的 spin 必须由独立 wait wrapper 明确实现并记录。
4. ticket disabled、owner bypass 或 polling 开关可能改变语义，任何违反 POSIX ordering/durability 的结果只能作为诊断上限，不能作为正式支持数据。
5. p50/p95/p99 如果来自 log2 histogram，是桶上界近似；正式报告必须写清 hist kind 和 sample rate。
6. profiling 开启后的吞吐不得与 profile-off 基线混作同一性能结论；A～G 至少各保留一组 profile-off 正式结果和一组 timing 归因结果。

## 13. 本轮交付结论

这套方案不引入新的 I/O engine、reactor、queue 或 POSIX 语义，只把当前调用链已有的边界变成可闭合的 T0～T10 统计。它优先回答“损失发生在哪个时间段”，再用 C/D/E/F/G 做受控交叉验证，最后用 `spdk_submit/completion`、queue、ticket、copy、futex、durability counters 检查归因是否自洽。

在完成上述 instrumentation 和 A～G 矩阵前，不能仅凭当前 63%～71% 基线断言“copy 一定是最大瓶颈”或“ticket 一定是最大瓶颈”。这正是本 profiling 阶段需要避免的推测。

