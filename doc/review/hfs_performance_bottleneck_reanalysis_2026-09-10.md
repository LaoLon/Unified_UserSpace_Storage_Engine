# HFS 性能瓶颈再分析与唯一优化决策

> **分析日期**：2026-09-10  
> **对象**：HFS 完整 POSIX preload 路径与原生 SPDK FIO  
> **代码状态**：`main@ee6b0c4` 加当前工作区 tracepoint 扩展  
> **SPDK**：VM 内 Git 工作树，锁定 `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`  
> **数据目录**：VM `/mnt/hfs-data/hfs-performance-reanalysis-20260910`

## 1. 结论先行

当前 HFS 的主要问题不是 NVMe 服务能力，也不是 metadata 写回本身，而是 POSIX 同步边界把
每个 syscall 变成了“单请求提交 + 单请求等待”的小事务。32 个 FIO job 虽然可以同时进入
用户态，但默认只有一个数据 owner reactor 执行提交回调；每个请求还要持有 HFS 的 I/O
同步/区间保护并等待 completion，导致队列、锁和 DMA lease 的等待叠加。

本轮唯一推荐方案是：

> **将 POSIX 路径收敛为固定上限的 reactor-sharded bounded async submission queue，默认在
> 8 vCPU VM 使用 4 个 data reactor；POSIX 调用仍同步返回，但内部先排队并保持有界的多请求
> in-flight，按 inode/range ticket 保证顺序，metadata 以 epoch 合并提交。**

这不是重新暴露 async ABI，也不是只把 DMA pool 做大。它直接修复当前最重要的两个结构性
限制：owner 回调串行和每个 syscall 的同步提交边界；per-reactor pool 同时消除 8/16 KiB
class 的集中耗尽。

## 2. 复测口径

### 2.1 FIO 对照

| 项目 | HFS | 原生 SPDK |
|---|---|---|
| 入口 | `LD_PRELOAD=build/lib/libhfs.so`，完整 POSIX | `spdk_bdev` FIO engine |
| 后端 | `config/hfs_nvme.json`，SPDK bdev `Nvme0n1`，PCI BDF `0000:0b:00.0` | 同一 `Nvme0n1` |
| workload | `randrw`，50/50，`psync`，`O_DIRECT`，64 MiB，5 秒 | 同一 workload |
| 并发 | `numjobs=32` | `iodepth=32` |
| 块大小 | 4/8/16/32/64 KiB | 同左 |
| 统计 | 正式 FIO stats-off；归因样本另开 `HFS_STATS_TIMING=1` | FIO JSON |

正式对照命令由 `scripts/performance/fio_stage2_compare.sh` 执行。归因样本的 HFS 统计写入
`/mnt/hfs-data/hfs-performance-reanalysis-20260910/stats-*.txt`；trace 使用
`HFS_TRACE_SAMPLE=100`，因为高并发下固定 4096-event/thread ring 会溢出，trace 只作阶段
顺序和定性证据，计数以 stats/FIO JSON 为准。

### 2.2 Reactor A/B

为验证“多 reactor”是否真正有效，使用 8 KiB、32 FIO job、5 秒、同一 NVMe，分别设置
`HFS_DATA_REACTORS=1/2/4/8`。A/B 同时打开 stats timing，因此只比较相对变化，不与
stats-off 的绝对 FIO 表混合。

## 3. 当前 FIO 差距

| bs | HFS IOPS | 原生 SPDK IOPS | HFS/SPDK | HFS p99 读/写 (µs) | SPDK p99 读/写 (µs) |
|---:|---:|---:|---:|---:|---:|
| 4 KiB | 67,683.66 | 124,254.55 | 54.5% | 1,073 / 1,499 | 602 / 602 |
| 8 KiB | 66,343.86 | 113,444.51 | 58.5% | 1,466 / 1,974 | 643 / 651 |
| 16 KiB | 60,075.58 | 94,761.65 | 63.4% | 1,647 / 2,179 | 700 / 709 |
| 32 KiB | 52,490.80 | 74,363.53 | 70.6% | 1,401 / 1,942 | 799 / 799 |
| 64 KiB | 43,173.93 | 52,877.42 | 81.6% | 1,860 / 3,260 | 1,057 / 1,073 |

结论是请求变大后固定开销被摊薄，但 64 KiB 写 p99 仍是 raw 的约 3 倍；因此不能只用
“copy 字节占比”解释差距。8/16 KiB 是资源 class 和用户态排队最容易同时饱和的工作点。

## 4. 统计证据与根因

### 4.1 P0：POSIX 同步边界没有形成稳定的多请求提交窗口

当前路径大致为：

```text
FIO pread/pwrite
  -> hfsIoReadAt()/hfsIoWriteAt()
  -> 取 inode sync/range 保护
  -> 获取 DMA lease
  -> hfsIoStartRequest()
  -> runtime batch queue
  -> owner/data reactor callback
  -> SPDK blob I/O
  -> hfsIoWaitRequest() / futex completion
  -> copy-out、release lease、释放锁
```

当前 stats 的关键值：

| bs | runtime queue peak | runtime queue wait | runtime inflight peak | io pipeline peak |
|---:|---:|---:|---:|---:|
| 4 KiB | 30 | 22.746 s aggregate | 1 | 1 |
| 8 KiB | 16 | 5.205 s aggregate | 1 | 1 |
| 16 KiB | 16 | 4.528 s aggregate | 1 | 1 |

这里的 `runtime_inflight_peak` 是 runtime callback 执行并发度，不是 NVMe backend outstanding
计数；它稳定为 1 说明 owner callback 本身仍是串行执行。`dma_active_peak` 可以达到 32，
说明多个 POSIX 线程确实同时占用了请求资源，但请求无法通过一个长期存在的提交窗口摊薄
通知、callback 和 waiter 成本。现有 batch 只合并通知，不改变同步 syscall 的等待边界。

### 4.2 P0/P1：I/O 同步/区间保护持有时间过长

`lock_io` 统计聚合了 I/O range 保护和 inode sync 写侧保护。平均值如下：

| bs | wait count | wait 总时间 | 平均 wait | hold count | hold 总时间 | 平均 hold | contention |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 KiB | 352,390 | 28.84 s | 81.8 µs | 352,334 | 129.92 s | 368.8 µs | 94,866 |
| 8 KiB | 337,070 | 33.04 s | 98.0 µs | 337,014 | 125.31 s | 371.8 µs | 89,009 |
| 16 KiB | 303,043 | 32.70 s | 107.9 µs | 302,987 | 125.70 s | 414.9 µs | 80,835 |

这些是跨线程的 aggregate，不能直接当作单请求延迟，但 wait/hold 与 FIO p99 同步增长，
说明保护范围覆盖了真实 I/O 生命周期，而不是只保护 inode 指针查找。当前应把“排队和
后端提交”移出不必要的独占区；语义上仍需保留同一 inode/range 的顺序 ticket。

### 4.3 P0/P1：8/16 KiB DMA class 集中耗尽

| bs | DMA acquire | DMA wait | wait 比例 | wait 总时间 | active peak | pool class |
|---:|---:|---:|---:|---:|---:|---:|
| 4 KiB | 352,326 | 0 | 0% | 0 | 32 | class 0 (32 slots) |
| 8 KiB | 337,006 | 131,133 | 38.9% | 32.39 s | 17 | class 1 (16 slots) |
| 16 KiB | 302,979 | 124,258 | 41.0% | 33.97 s | 17 | class 1 (16 slots) |

`dma_exhaust=0` 只表示没有返回失败；大量请求仍被 condition wait 阻塞。单纯把全局 pool
做大可以降低这个症状，但会继续把 lease 绑定在同步 syscall 上，无法解决 owner queue 和
lock hold，因此只能作为 bounded queue 的内部容量参数，不能作为独立方案。

### 4.4 P1：普通 POSIX buffer 几乎全部走 bounce，但 copy 不是当前第一主因

归因 stats 中三个块大小均为：

```text
io_direct_ops=0
io_direct_bytes=0
io_aligned_ops≈io_read_ops+io_write_ops
io_unaligned_ops=0
io_rmw_ops=0
```

trace 采样的 copy 阶段 p50 为亚微秒级（约 0.1～0.6 µs），而 FIO 请求 p50 为数百 µs。
因此用户 buffer 不可直接 DMA 是稳定成本，但当前高并发差距主要来自 lease 等待、runtime
排队、锁持有和 completion 边界；继续优化 copy 不能替代提交队列改造。仍应保留一次必要的
write copy-in/read copy-out，不能为追求吞吐破坏普通 POSIX buffer 生命周期。

### 4.5 P2（FIO）/P1（数据库 fsync）：metadata 不是本轮 FIO 的主瓶颈

以 8 KiB 为例：

```text
meta_read_lock=164,227
meta_write_lock=345
lock_meta_contention=321
lock_meta_wait_ns=37.0 ms aggregate
lock_meta_hold_ns=44.7 ms aggregate
```

metadata lock 的平均等待/持有约为数百纳秒，远低于 `lock_io` 的数十至数百微秒。FIO
steady-state 主要是已存在文件的 aligned I/O，不应先扩大 metadata cache 或删除 POSIX
事务。数据库的 WAL、`fdatasync/fsync` 仍可能放大 metadata/barrier 成本，但那是下一步
在新提交队列上验证 durability epoch 合并的问题。

### 4.6 P1：completion/futex 通知按 request 重复

每个 child 仍单独产生 completion state、futex wake 和 waiter；在高并发 trace 中
`completion_wake` 数量显著高于可用于无损统计的采样 submit 记录。这个通知本身通常不是
最大 CPU 项，但它把每个 request 的尾延迟固定加到 POSIX 边界，并延长 DMA lease 与锁的
持有时间。提交队列必须允许多个 request 在同一批次进入 SPDK，再按 ticket 完成和批量
回收，不能把 callback + condvar 包装再复制一层。

## 5. Reactor A/B 证明了正确方向和边界

8 KiB、32 job 的结果：

| `HFS_DATA_REACTORS` | 总 IOPS | p99 读/写 (µs) | runtime queue peak | inflight peak | DMA wait | 结论 |
|---:|---:|---:|---:|---:|---:|---:|---|
| 1 | 66,144 | 1,466 / 1,991 | 21 | 1 | 130,929 | 当前默认路径 |
| 2 | 67,722 | 1,466 / 1,925 | 16 | 2 | 123,266 | 仅增加一个并行 owner，收益很小 |
| 4 | **87,580** | **995 / 1,384** | 28 | 3 | **2,297** | 当前 VM 的最佳点，+32.4% |
| 8 | 51,708 | 2,277 / 3,097 | 30 | 4 | 22 | 过度并行，CPU/队列争用反而恶化 |

4 reactor 同时降低 DMA wait 和 p99，说明资源按 reactor 分片确实有效；8 reactor 反而
恶化，证明“无限增加 reactor/queue depth”不是方案。默认值应受 CPU 上限约束，8 vCPU
环境不应盲目使用 8 个 data reactor。

## 6. 为什么此前局部修复没有消除差距

1. **batch dispatcher** 合并了 `spdk_thread_send_msg` 通知，但 POSIX caller 仍在每个
   syscall 结束前等待，无法形成跨 syscall 的持久 in-flight window。
2. **无锁 freeMask** 消除了无竞争 acquire 的 mutex 往返，但 8/16 KiB class 只有 16 个
   slot；当 active 达到上限后，condition wait 仍是硬阻塞。
3. **普通 buffer → direct async bridge** 增加 callback、completion 和 condition wait，
   A/B 已比同步 bounce 慢约 15%～30%；它不是正确的 POSIX 优化方向。
4. **metadata/持久化修复** 提升了正确性和数据库恢复能力，但 FIO 数据显示 metadata
   lock 时间不是 steady-state 主导项；先优化 metadata 会错过 I/O 提交边界。
5. **增加 reactor 数量但没有 bounded policy** 会在 8 reactor 样本中放大调度和队列争用；
   需要固定上限、每 reactor 独立资源和可观测的 backpressure。

## 7. 唯一推荐优化策略

实施 **POSIX Reactor Queue Engine**，具体采用以下不可拆分的组合：

```text
POSIX read/write/pread/pwrite
        │
        ├─ 一次 buffer classify；不可 DMA 时只做一次 staging copy
        ├─ inode/range ticket（只约束需要顺序的操作）
        └─ hash(blobId, range) -> data reactor queue
                          │
                 bounded in-flight credits
                          │
                 batch drain -> SPDK channel/qpair
                          │
                 completion ticket -> batch recycle/wake
                          │
                 POSIX caller 在自己的 ticket 上返回
```

具体决策：

- 默认 data reactor 数为 `min(4, max(1, nproc/2))`，8 vCPU VM 默认 4；保留
  `HFS_DATA_REACTORS` 仅用于实验和故障回退。
- 每个 reactor 同时拥有 SPDK channel、DMA pool class 和提交队列；不让所有 8/16 KiB
  lease 争用同一个 pool/condition。
- queue 使用 MPSC 入队、单 reactor 消费；一次 drain 提交一批 request，credit 到达上限
  时让 caller 有界等待，不 busy-spin、不无限 malloc。
- ticket 保存 fd/open-file description、inode、range、sequence 和用户 buffer staging
  状态；普通 POSIX API 仍同步返回，外部不新增 async ABI。
- 对不重叠 range 允许并行；同一 inode 的 overlapping write、truncate、rename、size
  publish 通过 sequence/ticket 保证 read-after-write 和 POSIX 可见顺序。
- completion 在 reactor 侧先做状态快照、回收 DMA lease 和锁保护，再唤醒 ticket；减少
  caller 与 owner 之间的长临界区。
- metadata dirty epoch 与 data ticket 解耦；`fsync/fdatasync` 只等待目标 epoch，leader
  执行必要 metadata commit 和设备 flush，同一 epoch 的 waiter 不重复 flush。

该方案把 copy、POSIX 语义、请求排队、SPDK 并行度和 metadata durability 分开处理，能够
直接覆盖本轮证据中的 P0/P1 瓶颈；扩大 DMA pool、增加锁粒度或单独重写 metadata 都只是该
方案的内部参数，不作为独立路线。

## 8. 验收指标

### 8.1 性能

- 8 vCPU VM 上 8 KiB、32 job：相对当前 `HFS_DATA_REACTORS=1`，总 IOPS 至少提升 25%，
  目标不低于 82k IOPS；p99 读不高于 1.2 ms、写不高于 1.6 ms。
- 16 KiB、32 job：总 IOPS 至少提升 20%，写 p99 不高于当前 2.18 ms。
- 4/32/64 KiB 不得比当前 stats-off 基线下降 5% 以上；64 KiB 写 p99 必须有可解释的
  queue/lock 改善，不能仅吞吐上升。
- `dma_wait_ns`、`runtime_queue_wait_ns`、`lock_io_wait_ns` 分别输出 per-reactor，能
  明确区分 pool、队列和锁的主导项。

### 8.2 正确性与生命周期

- 所有 FIO、POSIX contract、fd lifecycle、metadata transaction、MariaDB/MySQL 和
  RocksDB smoke 继续 `errors=0`。
- `spdk_submit=spdk_complete`、DMA `acquire=release`、ticket pending=0；SIGKILL recovery
  和 `ldb checkconsistency` 不回退。
- 同一 fd 的 shared offset、`O_APPEND`、overlap write、read-after-write、`fsync` 顺序
  保持现有契约；fork child 不得进入父进程 queue。

### 8.3 可观测性

- trace-off 是正式性能口径；trace-on 仅做归因。
- 1 秒、`HFS_TRACE_SAMPLE=1000` 的高并发样本要求 `trace_dropped=0`；若仍溢出，先降低
  采样或缩短 workload，不用有损 trace 计数宣称 request 放大。
- 新 trace 至少能关联 `queue_enter → submit → complete → ticket_wake`，并按 reactor、
  lock class、DMA pool class 和 wake reason 分组。

## 9. 限制与后续边界

- 本轮 SPDK 通过 PCIe BDF 直接 attach controller，`ls /dev/nvme*` 为空不等价于没有
  NVMe bdev；结果以 SPDK config、plugin 和同一 bdev 为准。
- stats timing 和 trace-on 会增加观测开销；本报告只把 stats-off FIO 用于绝对对照，
  把 timing/trace 样本用于归因。
- 普通 POSIX buffer 的必要 copy 不会因本 shard 消失；只有调用方主动提供可验证 DMA
  buffer 才能走零 copy，这不属于当前 POSIX 兼容层的强制前提。
- 多进程共享同一 HFS mount、mmap、native libaio/io_uring 仍不在当前支持边界，不能通过
  增加 reactor 偷换成已支持能力。

## 10. 结论

当前 HFS 与原生 SPDK 的差距，本质是“同步 POSIX 语义在用户态被实现成单请求串行提交”，
而不是某一个 `memcpy` 或 metadata 函数太慢。唯一值得继续投入的路线是 bounded、多
reactor、ticket 化的内部提交队列；它可以在不牺牲 POSIX 语义的前提下，把 owner queue、
DMA class 和 completion waiter 从单请求固定成本变成可控的批量成本。

## 11. Shard-62 验证后的归因修正

Shard-62 已将内部 reactor queue、bounded credit 和 SPDK message admission 完成，并在
同一 NVMe bdev 上用 32-job FIO 验证：8K HFS `72.0K` IOPS、原生 SPDK `112.2K`，HFS/raw
为 `64.2%`，所有错误为 0。queue depth 32/64/128 A/B 均只有约 `71.5K～72.7K` IOPS，
所以当前差距不是 queue capacity 不足。

stats-on 归因样本（8K、32 jobs、5 s）为：

```text
lock_io_wait_ns       = 30.95 s
lock_io_hold_ns       = 126.82 s
lock_io_contention    = 95,711
io_copy_ops           = 365,549
io_copy_bytes         = 2.99 GiB
queue_credit_waits    = 0
io_backend_outstanding_peak = 32
spdk_submit/complete  = 365,705/365,705
```

这说明优化顺序需要修正为：

1. 先把普通 POSIX aligned I/O 的 range lock 从“持有到同步 completion”改为 ticketized
   reservation/ordering，保持 overlap write、read-after-write、`O_APPEND` 和 fsync 语义；
2. 再做普通 buffer 的 copy-elision（只有可验证 DMA buffer 才能零拷贝；不能为了性能放宽
   POSIX buffer 语义）；
3. queue depth、reactor 数和 metadata steady-state 不再作为当前第一优先级。

因此 Shard-62 的提交队列实现已完成，82K IOPS 性能门槛仍属于后续 range-ticket 与
copy-elision shard，不能把当前 72K 样本误写成已达标。

## 12. Shard-63 ticket 验证后的归因更新

Shard-63 已将普通 POSIX read/write 接入 inode-local ticket admission：短临界区只建立
overlap 依赖，aligned steady-state 不再让 `rangeStripes` 覆盖 SPDK completion；非对齐请求
仍由一个 ticket 包住现有 RMW，控制操作继续走已有 exclusive sync barrier。

专项测试闭合了 `ticket_admit/ready/complete`、依赖等待和 SPDK submit/complete；MariaDB、
RocksDB、POSIX contract、fd lifecycle、metadata recovery、trace 和 runtime stress 均通过。
NVMe 5 秒矩阵（HFS_DATA_REACTORS=4）为 HFS/raw：4K `83,959/123,325`、8K
`80,604/111,129`、16K `75,120/94,309`、32K `59,571/73,771`、64K `44,311/52,712`
IOPS；8K stats-off 10 秒重复为 `88,403/88,577/88,629`，p99 读写约 `0.83–0.85 ms`。

stats-on 8K 归因样本显示：

```text
ticket_admit/complete       = 442820/442820
ticket_pending_peak         = 32
ticket_dependency_waits     = 1183
ticket_dependency_wait_ns   = 262568670
lock_io_wait_count          = 64
lock_io_hold_count          = 8
lock_io_hold_ns             = 247538246
io_backend_outstanding_peak = 32
spdk_submit/complete        = 443010/443010
io_copy_ops                 = 442820
io_copy_bytes               = 3.38 GiB
```

这组数据确认原先 `lock_io_wait_ns/lock_io_hold_ns` 的 30.95/126.82 秒级热点已经消失，
并发窗口和后端 completion 已闭合；性能剩余差距不是 queue credit 或 backend outstanding
不足。当前第一主因转为普通 POSIX buffer 的一次 bounce copy、fd/metadata 轻量同步、
每请求 ticket/condition/futex 生命周期和 data-reactor 消息往返。下一阶段应围绕“copy
批量化 + caller 提交批量化”做测量型优化，不能把 registered-DMA 专用接口强行套到普通
数据库 buffer，也不应继续无上限增大 reactor/queue。
