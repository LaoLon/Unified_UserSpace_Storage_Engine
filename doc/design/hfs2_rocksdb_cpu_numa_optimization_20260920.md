# HFS 2.0 RocksDB 读写全链路 CPU/NUMA 优化设计

日期：2026-09-20  
状态：单节点路径的 Phase A/B、DataPath 批处理、RMW 生命周期修复和 full-snapshot group commit 已落地；
多 NUMA 的 per-node DMA arena、remote-byte 采样和真实多 NUMA 验证暂缓，不阻塞其余开发  
适用范围：RocksDB 8.9.1 FileSystem adapter、HFS core、SPDK data reactor、真实 NVMe

本文基于当前工作树、RocksDB 对照报告、HFS 2.0 总体设计和 config/hfs_nvme.conf 编写。它不是“增加线程数”的调参清单，而是从请求生命周期、CPU 亲和、NUMA 内存、PCIe/NVMe 本地性、DMA、metadata 和 durability barrier 重新整理优化路线。

实现前若本方案改变 hfs2.0.md 已冻结的架构事实，必须先更新 hfs2.0.md，再开始代码修改。
当前第一阶段允许在单 node VM 上开发和回归；没有 NUMA 拓扑证据时，不宣称 local/remote
性能 gate 已通过。

## 1. 目标与当前判断

### 1.1 目标

HFS 的目标不是只在某一种 buffered write benchmark 上超过 ext4，而是在相同 RocksDB 版本、相同 workload、相同 CPU 预算、相同 durability 语义和相同缓存口径下，形成可重复的全链路优势：

1. 顺序写、小写、overwrite、WAL 和 compaction 写路径领先原生 RocksDB；
2. 冷缓存随机读、SST 读和读写混合负载领先原生 RocksDB；
3. 同等吞吐下 CPU 消耗更低，或同等 CPU 预算下吞吐更高；
4. p99/p999 延迟不因为 busy-poll、跨 NUMA 访问或 metadata checkpoint 抖动而恶化；
5. 多 NVMe 分布在不同 NUMA node 时，吞吐能够随本地设备和本地 CPU 近似扩展。

“全面超过”必须以 workload 条件为前提。warm-cache 下原生 RocksDB 可能直接命中 DRAM 中的 block cache 或 Linux page cache，NVMe bypass 不可能凭空超过内存访问。若产品要求 warm-cache 也必须领先，则必须明确引入 HFS 自己的读缓存，或者把目标改为“不增加缓存层但不明显回退”。当前 HFS 设计选择单层 RocksDB block cache，因此本文默认先要求冷缓存和超 cache working set 下领先。

### 1.2 当前基线

当前 2026-09-19 5-run 结果如下：

| workload | HFS/ext4 | 当前结论 |
|---|---:|---|
| fillseq | 122.8% | 已领先，主要受 adapter 写回缓冲帮助 |
| overwrite | 131.2% | 已领先 |
| readrandom | 58.0% | 未领先；实际读 I/O 很少，主要暴露 CPU/内存路径 |
| readwhilewriting | 25.4% | 未领先；reader 对 writer 造成明显干扰 |

报告还记录了：160K 次 readrandom Get 只产生约 5 次 HFS read，ReadAsync 和 MultiRead 未形成有效调用量。因此当前 readrandom 结果不能作为纯 NVMe 读能力结论，但它已经证明 HFS frontend/core 的 CPU 竞争和调用路径存在问题。

当前正式配置为单 data reactor：

    metadata_cpu = 2
    data_cpus = 3
    data_reactors = 1
    ring_depth = 1024
    client_depth = 32

这意味着 RocksDB benchmark 与 metadata/data reactor 在同一组 CPU 上运行。此前基线暴露的核心不足中，以下项目已经修复：

- 普通 RocksDB Read 现在是共享 async dispatcher 上的同步 facade：RocksDB 调用边界仍等待，core 路径使用 `Client::submitRead` 和 completion queue；
- MultiRead 在有界 dispatcher 中并发提交后统一 poll/reap，不再逐请求串行等待；
- ReadAsync 不再创建每请求 worker/ioWorker 线程；
- DataPath 已完成 bounded submit-then-reap，同时保留 RMW 的两阶段依赖；
- RMW 第二阶段会重新 arm wait state，避免复用第一阶段 `done=true` 导致提前返回、过早释放 DMA/bounce buffer；

当前仍需优化的部分包括：

- 同步 read/write 每次都重新获取 stat 和 extent map；
- Namespace 使用全局 mutex，syncMetadata 使用全局 durability barrier；
- RocksDB buffer 通常不属于 HFS DMA arena，存在 bounce copy；
- 当前 DMA/hugepage 设计没有明确的 per-NUMA ownership 和 remote fallback 统计。

修复后的真实 NVMe sync=1 小矩阵（`num=20000`、`threads=4`、`value_size=1024`）为：fillseq
3103 ops/s、readrandom 24631 ops/s、overwrite 2991 ops/s；readwhilewriting 记录
writer 20000、reader 38050121，不能将总 ops/s 与其他 workload 直接横比。该轮退出码为 0，
未再出现 SST checksum mismatch；随后 `hfs_ldb checkconsistency` 检查 80000 entries 通过，
`hfsctl check` 报告 `corruption=none`。这证明本轮首先闭合了 correctness/lifetime 问题，
不等于已经达到“全面超过原生 RocksDB”的性能目标。

## 2. 性能模型

RocksDB 请求的完整路径应按以下链路测量，而不是只看最终 ops/s：

    RocksDB worker
      -> FileSystem adapter
      -> frontend submit queue
      -> Client / async slot
      -> shard actor
      -> extent planner
      -> DMA lease
      -> SPDK bdev submit
      -> NVMe completion
      -> shard completion
      -> adapter callback
      -> RocksDB worker

每个请求必须至少记录以下维度：

| 维度 | 说明 |
|---|---|
| submit CPU / NUMA | RocksDB worker 和 adapter 提交位置 |
| reactor CPU / NUMA | 实际处理请求的 SPDK reactor |
| device NUMA | NVMe PCIe root complex 所属 node |
| DMA NUMA | BufferLease 实际分配的 node |
| frontend wait | RocksDB 等待时间 |
| queue wait | adapter、ring、shard 和 backend 等待 |
| DMA wait | arena acquire、remote fallback、copy 时间 |
| device time | bdev submit 到 completion |
| metadata time | stat、extent map、reserve、commit、checkpoint |
| completion handoff | reactor 到 frontend 的通知和 reaping |
| remote bytes | 跨 NUMA node 的内存读写字节数 |

只有拆出这些时间，才能判断慢在 CPU、NUMA、core scheduler、DMA、设备还是 RocksDB 自身。

## 3. NUMA 事实采集与硬门禁

当前仓库没有足够证据说明验收 VM 的实际 NUMA 拓扑，因此不能预先假设 0000:0b:00.0 属于哪个 node。每次正式性能测试必须先采集并归档：

    lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
    numactl --hardware
    cat /sys/bus/pci/devices/0000:0b:00.0/numa_node
    readlink -f /sys/bus/pci/devices/0000:0b:00.0
    cat /sys/class/block/nvme0n1/device/numa_node
    cat /sys/devices/system/node/node*/cpulist
    cat /proc/cmdline

需要得到四个事实：

    N_device  = NVMe PCIe root complex 所在 NUMA node
    N_reactor = data reactor 实际运行 node
    N_dma     = SPDK hugepage / DMA arena 实际使用 node
    N_front   = RocksDB adapter/frontend worker 所在 node

正式 gate 的硬规则：

1. N_device 不可解析为有效 node 时，测试只能作为 topology probe，不能作为性能 gate；
2. 单盘 baseline 必须保证 data reactor、DMA arena 和 device 位于同一 node；
3. N_reactor != N_device 或 N_dma != N_device 时，必须单独标记为 remote 对照；
4. 不能只用进程级 taskset 宣称 NUMA 本地，必须确认线程和分配内存实际落点；
5. hugepage 总量满足不代表本地 node 有足够 hugepage，node-local shortage 必须返回明确的 Busy/NoMemory，并记录 fallback；
6. 任何 remote fallback 都必须有计数，不得把 remote DMA 混入 local baseline。

## 4. NUMA 拓扑设计

### 4.1 单 NVMe、单 NUMA node

单盘 baseline 的推荐布局是：

    N_device
     ├─ 1 个 metadata CPU
     ├─ 1..R 个 data reactor CPU
     ├─ RocksDB frontend/compaction CPU（有余量时优先同 node）
     └─ node-local SPDK hugepage / DMA arena

RocksDB frontend 与 data reactor 应使用同 node 的不同物理核心。若核心不足，不得让 frontend、metadata 和 data reactor 无限制 oversubscribe；应降低 benchmark worker 数，或者启用 hybrid polling，而不是简单增加线程。

### 4.2 多 NVMe、跨 NUMA node

多盘时采用 device-NUMA domain：

    NUMA node 0                 NUMA node 1
      data reactor 0              data reactor 1
      DMA arena 0                 DMA arena 1
      NVMe group 0                NVMe group 1
      local frontend queue        local frontend queue

每个 device group 应拥有：

- 本地 data reactor 集合；
- 本地 shard/ring；
- 本地 DMA arena class；
- 本地 completion queue；
- 本地统计和 backpressure 计数。

跨 node 的操作只允许出现在 control plane、mount/format/checkpoint 等低频 metadata 操作、明确标记的 remote fallback，以及跨设备复制或故障处理。

普通 RocksDB read/write 热路径不得因为一个全局 round-robin 把请求随机投递到远端 reactor。调度应优先按 device_id -> numa_node -> shard 绑定，只有本地资源耗尽时才 bounded fallback。

### 4.3 内存与 DMA ownership

当前设计中的 shared DMA arena 需要演进为 NUMA-aware arena：

    Arena[node][size_class]
      ├─ 4 KiB / 16 KiB read buffers
      ├─ 64 KiB / 256 KiB buffers
      ├─ ≤1 MiB write/compaction buffers
      └─ reserved control/checkpoint buffers

分配规则：

1. 首选请求所属 device node；
2. 其次选择同 socket 的 node；
3. 最后才允许跨 socket fallback；
4. fallback 需要独立计数，并受上限约束；
5. 不允许在 I/O 热路径执行无界 NUMA migration 或普通 heap fallback；
6. 每个 arena class 记录 local hit、remote hit、busy、high watermark 和释放延迟。

RocksDB 传入的 buffer 通常不是 HFS arena-owned。短期内可以保留 bounded bounce fallback，但必须把 copy 和 buffer node 计入性能报告。中期应评估 adapter-side DMA pool 或 RocksDB kFSBuffer/buffer-provider 路径，减少读写双拷贝。

## 5. CPU 亲和与调度策略

### 5.1 角色分离

CPU 角色不应混用：

| 角色 | 责任 | NUMA 要求 |
|---|---|---|
| metadata CPU | namespace、journal、checkpoint、control | 优先 N_device，不得与 data reactor 共核 |
| data reactor | SPDK poll、backend submit/complete、shard pump | 必须靠近对应 NVMe |
| adapter frontend | RocksDB FileSystem、batch、callback/reap | 优先对应 device node，避免与 reactor 共核 |
| compaction worker | SST flush/compaction 产生的读写 | 与其主要 device group 同 node |
| benchmark/system | workload、SSH、日志、监控 | 不占用 data/metadata 保留核 |

推荐的 CPU 集合关系：

    frontend_cpus ∩ data_cpus      = ∅
    metadata_cpus ∩ data_cpus      = ∅
    benchmark_cpus ∩ reserved_cpus = ∅

如果验收环境只有 2 个 CPU，不应同时运行 8 个 RocksDB worker、metadata busy-poll 和 data busy-poll，然后把结果解释为设备能力。此时必须使用独立的低并发 baseline，或者启用可验证的 sleep/interrupt 模式。

### 5.2 Hybrid polling

默认 busy-poll 适合高 QD、持续 I/O，但不适合 RocksDB 读请求大量命中 block cache 的阶段。建议引入配置化 polling policy：

    active      ：存在 in-flight I/O 或最近 completion，短窗口 spin
    yield       ：连续空轮询超过阈值，有限 pause/yield
    sleep       ：长时间无 I/O，使用 bounded sleep 或 SPDK interrupt
    re-enter    ：新请求或 completion 到达后立即恢复 active

策略必须按 data reactor 独立统计：

- active/idle/yield/sleep 时间；
- completion latency；
- reactor CPU utilization；
- RocksDB frontend CPU utilization；
- HFS/raw IOPS 和 p99。

不能用固定的“空转 N 次再 sleep”作为最终方案。之前 idle 实验导致 FIO gate 下降，说明需要用请求到达率、in-flight 数和 completion 延迟动态选择，而不是全局固定阈值。

### 5.3 读写公平性

读请求不能无限占满 data reactor，也不能让 WAL 写请求被 SST read 饿死。每个 NUMA domain 建议使用有界配额：

    WAL / metadata write    ：保留最小提交配额
    foreground read         ：高优先级但有最大并发
    SST compaction read     ：可降级
    background flush/write  ：可降级

调度器必须报告每类请求的 queue wait 和 starvation 次数。readwhilewriting 的目标不是让 reader 无限增加 ops，而是保证 writer 和 reader 都达到各自的目标。

## 6. RocksDB 读路径优化

### 6.1 取消每请求线程

当前普通 Read 是对共享异步路径的同步 facade；ReadAsync/普通 Read 共用以下路径：

    RocksDB ReadAsync
      -> adapter per-frontend request queue
      -> owning frontend thread / reactor
      -> Client::submitRead()
      -> async shard service
      -> callback/reap

要求：

1. 一个请求最多使用已有 frontend/reactor 线程，不创建两个临时线程；
2. io_handle 的生命周期覆盖 SPDK completion 和 RocksDB callback；
3. AbortIO 只标记取消，不能提前释放 DMA、request 或 scratch；
4. callback 不在 SPDK actor 上执行阻塞操作；
5. 每个 NUMA domain 的 submit queue 和 completion queue 保持本地。

### 6.2 MultiRead 批处理

当前 MultiRead 已采用以下有界并发流程；物理相邻合并和 NUMA 分组仍是后续优化：

1. 校验请求和 scratch buffer；
2. 按 device、NUMA domain、物理 offset 排序；
3. 合并相邻且不超过最大 I/O 的范围；
4. 一次提交 bounded batch；
5. 完成后按原始 request 顺序填充结果；
6. 对失败请求保留独立 status。

批量大小应受 client_depth、DMA arena 和 RocksDB caller contract 限制。不得为了合并而无限等待后续请求。

### 6.3 DataPath 并行提交

当前 DataPath 已按以下流程执行：

    plan children
      -> acquire bounded DMA leases
      -> submit all ready children
      -> poll/reap completions
      -> first-error cancellation / drain
      -> parent completion

要求：

- 同一个 parent 的 child 并发度受 max_inflight_children 限制；
- 不持有 Namespace mutex 等待设备 I/O；
- data reactor 内完成 submit 和 completion，不跨 NUMA 传递临时 buffer；
- partial failure 必须等待已提交 child 进入 terminal state；
- parent completion 只发布一次。

### 6.4 读缓存口径

默认不新增 HFS page cache，以免与 RocksDB block cache 形成双缓存。但验收必须拆成三类：

| 类型 | 目的 |
|---|---|
| warm-cache | 衡量 RocksDB/adapter CPU 路径；不应宣称 NVMe 优势 |
| cold-cache | 衡量真实 device read、DMA、queue 和 p99 |
| working set > cache | 衡量长时间运行下的缓存抖动和 NUMA locality |

若产品硬性要求 warm-cache 也超过原生 RocksDB，则另立“可选 HFS read cache”设计，不得在当前单层 cache 结论下偷换口径。

## 7. RocksDB 写路径优化

### 7.1 保留现有写回缓冲，但增加 per-NUMA 和 file-role 策略

当前 1 MiB adapter write-back 已使 fillseq/overwrite 超过 ext4。后续应增加：

- WAL、MANIFEST、SST 使用不同 flush threshold；
- 每个 writable file 的 buffer 与其 device node 绑定；
- 大写直接进入本地 DMA pool，避免临时 heap；
- WAL append cursor 与 extent reservation 合并；
- Allocate hint 转化为连续 extent reservation，不在每次 flush 中重新扫描。

Flush 仍不能误当成 durable barrier。Sync/Fsync/Close 的 durability 语义必须保持和 hfs2.0.md §10.2 一致。

### 7.2 Group commit

syncMetadata 当前是全局串行 barrier。目标是把多个同步请求合并成一个 durable epoch：

    多个 File::sync()
      -> 一个 epoch coordinator
      -> 一次 per-device data flush
      -> 一次 dirty metadata checkpoint
      -> publish durable epoch
      -> 唤醒该 epoch 的全部 waiter

必须避免：

- 每个 WAL sync 扫描全部 inode；
- 每个 WAL sync 写完整 namespace snapshot；
- 不同 NUMA domain 重复 flush 同一设备；
- frontend 在等待 group commit 时占用 data reactor。

### 7.3 Metadata 增量化

当前同步 read/write 路径每次重新获取 stat 和 extent map。建议引入：

- per-file immutable extent snapshot；
- inode metadata version；
- append-only 文件的 cached logical EOF；
- dirty inode set；
- checkpoint 只序列化 dirty metadata；
- namespace global epoch 只作为失效 key，不作为每次 I/O 的完整 snapshot 触发器。

RocksDB 的 WAL、MANIFEST 和已关闭 SST 具有明确访问角色，可以利用单 writer/只读约束降低锁竞争，但不得恢复通用 POSIX 多 writer 语义。

## 8. NUMA-aware 配置提案

以下是目标配置形态，不代表当前 parser 已支持，实施前必须加入 .conf schema 和 fail-closed 校验：

    [runtime]
    data_reactors = 2
    data_cpus = 4,5
    metadata_cpus = 6
    frontend_cpus = 7,8
    ring_depth = 1024
    client_depth = 64

    [numa]
    policy = local-first
    device_numa = auto
    dma_numa = auto
    frontend_numa = device-local
    allow_remote_dma = false
    remote_fallback = bounded
    polling = hybrid

    [limits]
    max_inflight_children = 32
    max_read_batch = 32
    max_group_commit_wait_us = 200

配置校验规则：

1. data_cpus 数量必须与 data_reactors 一致，或显式给出每 reactor 的 CPU 列表；
2. CPU 必须 online、不可重复、不可与 metadata/frontend 保留集重叠；
3. device_numa=auto 解析失败时拒绝正式性能模式；
4. allow_remote_dma=false 时本地 arena 不足应返回 Busy/NoMemory，不允许静默跨 node；
5. 多 device group 必须声明完整 device set，不通过临时环境变量追加设备；
6. 配置文件输出实际解析后的 CPU、NUMA、DMA 和 device mapping，供证据归档。

## 8.1 Frontend dispatcher ownership

RocksDB worker 不能直接调用 HFS 的 Client async API。当前 Client 对 submitRead、submitWrite、poll 和 close 都有 frontend-thread ownership 约束，因此 adapter 必须引入明确的 dispatcher：

    RocksDB worker threads
      -> per-HfsContext MPSC submit queue
      -> one frontend dispatcher thread
      -> one owning Client
      -> Client::submitRead / Client::poll
      -> completion queue
      -> RocksDB callback handoff

规则如下：

1. 一个 HfsContext 的 Client 只能由一个 dispatcher owner 调用 submit、poll、AbortIO 和 close；
2. RocksDB worker 只写入 bounded MPSC queue，不持有 Client frontend slot；
3. dispatcher 不执行设备 I/O 和长时间阻塞回调，只负责 admission、poll、reap 和 callback handoff；
4. 每个异步请求有明确状态：Accepted、Submitted、Completed、Aborted、Reaped；
5. IOHandleDeleter 只有在 core operation terminal、callback 已结束且 request buffer 不再使用后才能回收状态；
6. AbortIO 之前未提交的请求可以直接取消；已经提交的请求必须等待 backend completion 后再释放 DMA 和 request；
7. callback 只执行一次，超时不能伪造 completion，也不能提前释放 RocksDB 的 FSReadRequest；
8. Close 顺序固定为：停止 ingress、排空 submit queue、取消未提交请求、等待已提交请求 terminal、reap completion、关闭 Client。

如果同一进程存在多个 NUMA frontend queue，应使用每个 queue 一个 Client/dispatcher，而不是多个线程共享一个 Client。每个 dispatcher 的 callback 必须回到 RocksDB 允许的线程语义，不能假设 RocksDB callback 一定可以在 SPDK reactor 上执行。

### 8.1.1 Pending write visibility

adapter 的 pending write-back buffer 必须纳入 async read ordering：

- async read submit 前，先确认同一文件的 pending buffer 已 flush 到 HFS logical layer，或者由 adapter 提供 overlay read；
- 读取已有 FSRandomAccessFile 时，不能只依赖“打开 read handle 时 flush”；
- GetFileSize、Read、Rename、Delete、Close 必须共享同一 file-role ordering；
- writer buffer flush 与 async read submit 之间必须建立 happens-before；
- Close 需要等待所有 reader callback terminal，不能只等待 writable file buffer 清空。

短期实现建议采用“同文件 pending buffer flush 后才允许 async read submit”。overlay read 可作为后续优化，但不能在没有一致性模型时直接引入。

## 8.2 Cross-NUMA parent/child operation

文件 shard ownership 和 device NUMA ownership 是两个不同维度，不能用一个 hash 同时解决：

    file_id -> owner shard / metadata ordering
    child.device_id -> device NUMA domain / local reactor

一个逻辑请求可能跨多个 extent、设备和 NUMA node。正确的执行模型是：

    owner shard
      -> capture one immutable extent snapshot
      -> build all device-local children
      -> reserve all required resources
      -> dispatch each child to its device-local reactor
      -> aggregate completion back at owner shard
      -> publish one parent result

要求：

1. parent operation 只有一个 owner，负责 request lifetime、first error、completed bytes 和 final publish；
2. child 可按 device NUMA domain 分发，但不能直接修改 frontend completion state；
3. 在提交第一个 child 前，先完成所有 child 的 bounded resource admission；资源不足时整体返回 Busy/NoMemory，避免部分提交；
4. 如果允许 remote fallback，必须是整个 operation 的显式策略，不允许 child 随机跨 node；
5. parent 在所有已提交 child terminal 后才能释放 snapshot、DMA lease 和 request state；
6. device-local completion 必须通过 bounded owner queue 返回，不能直接跨线程调用 RocksDB callback。

### 8.2.1 Same-file dependency rules

DataPath 的 submit-then-reap 只适用于不存在冲突的 child。必须保留以下依赖：

- 同一 inode 的重叠逻辑 write 必须按 file scheduler 顺序执行；
- 同一 device block 的两个 partial write/RMW 必须串行或先合并；
- 不同 extent、不同 device block 的 child 可以并行；
- write 与 truncate、rename、delete 之间必须建立 metadata dependency；
- data child 完成后才能执行 setSize、extent publish 和 durability staging；
- read 可以与不重叠 write 并行，但不能读取尚未发布的 extent snapshot。

因此，DataPath 并行化不能绕过现有 inode dependency scheduler。同步 File API、RocksDB async API 和 FIO async API 必须使用同一个 file ordering contract。

### 8.2.2 MultiRead 的合并边界

adapter 只知道 RocksDB 的逻辑 offset，不能自行假设物理连续。MultiRead 应采用两阶段：

1. adapter 按同一文件、逻辑 offset 和 request deadline 形成 bounded batch；
2. core 根据 extent snapshot 将 batch 拆成 device-local child，并在 core 内完成物理相邻范围合并。

core 必须保留：

- sparse hole 的逻辑零语义；
- extent 边界；
- 不同 device 的 child 边界；
- 每个原始 request 的独立 status；
- partial completion 和 first-error 规则。

## 8.3 Durability 与 checkpoint 格式兼容边界

Group commit 和增量 checkpoint 必须拆成两个阶段，不能同时作为一个优化直接落地。

### 8.3.1 第一阶段：full snapshot group commit

第一阶段保持现有 full checkpoint format：

    多个 sync waiter
      -> 选择最大目标 epoch
      -> drain 目标 epoch 之前的 data
      -> per-device data flush
      -> prepare durable epoch
      -> 写完整 namespace/inode/extent/allocator snapshot
      -> checkpoint flush
      -> superblock publish
      -> publish durable epoch
      -> 唤醒目标 epoch 不超过已发布 epoch 的 waiter

max_group_commit_wait_us 只是 coalescing window，不是 durability timeout。窗口结束后必须继续等待完整 durable barrier；成功返回必须意味着 durable epoch 已发布。

失败规则：

- data flush 失败：本批全部 waiter 返回同一设备错误；
- metadata prepare 失败：不发布 durable epoch；
- checkpoint 或 superblock flush 失败：rollback staged durable state，所有受影响 waiter 返回错误；
- 新 logical write 可以进入更高 epoch，但不能被错误地包含到更低目标 epoch 的成功承诺中；
- shutdown 时必须先停止新 barrier，再排空当前 group commit。

### 8.3.2 第二阶段：incremental checkpoint

dirty metadata checkpoint 需要新的 on-disk contract，至少包含：

- base full checkpoint txid/generation；
- dirty object identifiers；
- object-level payload and CRC；
- allocator dirty runs；
- journal sequence range；
- new snapshot/checkpoint format version；
- mount 时的 base + incremental replay order；
- torn incremental checkpoint 的丢弃和回退规则；
- 旧镜像拒绝挂载或显式迁移策略。

在上述格式和 replay 规则完成前，文档中的“checkpoint 只序列化 dirty metadata”只能作为后续设计方向，不能作为 Phase D 的直接实现项。

## 8.4 NUMA resource enforcement 与 shutdown

仅增加 device_numa、dma_numa 配置不能保证 DMA 本地。当前 DmaArena 是单 arena，SPDK allocator 也没有显式 socket 参数，因此必须先扩展资源模型：

1. 每个 NUMA node 拥有独立 DmaArena 和 size class；
2. SPDK hugepage allocation 明确指定 socket，或在启动时验证每 node 的 hugepage 数量；
3. local arena 不足时按 admission policy 返回 Busy/NoMemory，不在已提交 child 中途迁移；
4. control、checkpoint、journal buffer 需要独立保留额度，不能被 RocksDB read/write 耗尽；
5. BufferLease 必须携带 node/domain identity，释放时回到原 arena；
6. 统计 local hit、same-socket hit、remote fallback、allocation failure 和 peak usage；
7. NUMA topology 不可解析、device node 不可确定或 local hugepage 不足时，正式性能模式 fail closed。

frontend_cpus 不应被 HFS 配置假装控制。当前总体架构规定 frontend 不由 HFS 绑定，因此该字段应改为外部观测/校验信息，或者新增显式 frontend affinity registration API。metadata_cpu 也必须和 NUMA node 绑定关系明确，不能只记录 CPU 编号。

### 8.4.1 Reactor wake and shutdown

hybrid polling 的 sleep/yield 必须有受控唤醒机制：

- 有 in-flight bdev I/O 时不能使用无条件普通 sleep；
- 使用 sleep 只能在没有 active I/O，或 bdev interrupt/wakeup contract 已确认时；
- 新 ring ingress、SPDK message、completion 和 stop request 都必须能够唤醒 reactor；
- stop 前先关闭 ingress，再 drain owner queue、child completion、SPDK message 和 DMA lease；
- clear poller、spdk_put_io_channel 和 thread destroy 必须在所属 reactor 完成 bounded drain 后执行；
- sleep 模式必须记录最大 wake latency，不能用默认 FIO 通过作为唯一证明。

## 8.5 Per-file buffer 的正确路由

writable file buffer 不能静态绑定一个 device node，因为同一个逻辑文件可能跨多个 device extent。正确做法二选一：

1. 使用逻辑文件级 buffer，flush 时根据 extent snapshot 拆成多个 device/NUMA child；
2. 使用 per-device/per-node sub-buffer，并由一个 file-local sequence coordinator 保证逻辑 offset 顺序。

无论采用哪种方式，都必须保证：

- append 跨 extent 时不乱序；
- 某个 child 失败时其它 child 的 logical/durable 状态可判定；
- buffer flush 不绕过 file scheduler；
- Close、Rename、Delete 能等待所有 sub-buffer terminal；
- pending registry 记录的是逻辑文件状态，而不是单个 node 的局部状态。

## 9. 实施阶段

### 9.0 当前实现进度（2026-09-20）

- 已完成：schema-version 1 的可选 `[numa]` 策略解析和 fail-closed 校验，缺省配置保持
  单 node 兼容行为；
- 已完成：SPDK runtime 启动时读取 Linux sysfs 的 online node/CPU、PCI device NUMA
  node 并输出 topology snapshot；`local-only` 在拓扑不可读或 device node 未解析时拒绝
  启动，单 node VM 的 `local-first` 保持兼容；
- 已完成：RocksDB adapter 为每个 HfsContext 建立一个 bounded frontend dispatcher，
  ReadAsync 和 MultiRead 不再创建每请求阻塞 worker；
- 已完成：Client frontend ownership 一次性转移、请求 timeout/AbortIO/terminal drain
  生命周期收口；
- 已完成：真实 `Nvme0n1` 上 `hfs_rocksdb_smoke`、`hfs_db_bench` 的 fillseq/readrandom/
  overwrite/readwhilewriting 小矩阵、`hfs_ldb checkconsistency` 和 `hfsctl check`；
  VM CTest `26/26` 通过；
- 已完成：DataPath 在首次提交前完成 bounded DMA admission，随后批量 submit/reap，
  仅 Head/Tail RMW child 进入第二阶段；对齐 bounce child 不再误走 RMW；
- 已完成：runtime 采集 CPU→node placement，按可解析的 device node 选择 data reactor，
  node 不可解析时使用可审计的确定性 fallback；`active`/`hybrid`/`interrupt` polling
  policy 已进入 data reactor loop；配置增加 `metadata_cpus`/`frontend_cpus` 并拒绝显式
  data/frontend 与 metadata CPU 重叠；
- 暂缓：per-node DMA arena 和真实 local/remote DMA、remote bytes、frontend CPU 采样；
  当前验收 VM 为单 NUMA node 且 PCI `numa_node=-1`，这些项目不作为本轮单节点验收门槛；
- 已完成：并发 `sync/fsync` 的 full-snapshot group commit。以
  `metadata.max_group_commit_wait_us` 为 coalescing window，冻结最大目标 epoch，
  一次完成 data flush、checkpoint、superblock publish 和 durable epoch 发布；窗口内
  的 waiter 共享结果，窗口外的更高 epoch 自动进入下一批；

### Phase A：拓扑和指标先行

- 增加启动时 NUMA topology snapshot；
- 输出 device node、CPU→node placement、reactor 选择和 fallback；
- 为每个 I/O 增加 submit/complete CPU 和 node 采样；
- 增加 local/remote DMA、arena busy、copy bytes、queue wait 计数；
- 不改变默认调度策略，先建立当前版本的 NUMA baseline。

完成条件：同一 workload 能区分 local、remote、CPU oversubscription 三种结果。

### Phase B：真正的 RocksDB async read（基本路径已完成）

- [x] adapter 去除每请求 worker/ioWorker；
- [x] 连接 Client::submitRead、completion queue、AbortIO；
- [x] 实现并验证真实 MultiRead batch；
- [x] DataPath 改为 bounded submit-then-reap，并保留 RMW 两阶段依赖；
- [x] 在真实 NVMe 上验证 RocksDB smoke、sync=1 小矩阵和 SST consistency；
- [ ] 增加 read-only SST 大批量、多 extent、跨 node 对照测试。

完成条件：ReadAsync、MultiRead 调用量与 workload 规模匹配，不能再出现 160K Get 只有个位数底层读且无法解释的情况。

### Phase C：NUMA-local arena 和 reactor（暂缓）

- per-node DMA arena；
- device-NUMA reactor mapping（代码已具备，双 node 证据待补）；
- .conf 的 NUMA schema、CPU overlap 校验和启动日志；
- hybrid polling（代码已具备，CPU utilization/p99 对照待补）；
- remote fallback bounded gate。

完成条件：local baseline 的 remote bytes 接近零；remote 对照下降可解释；arena shortage 不发生静默迁移。
该阶段需要多 NUMA VM，当前不阻塞单节点实现。

### Phase D：metadata 和 durability（单节点已收尾可落地项）

- extent snapshot cache；
- append reservation/cursor；
- dirty inode checkpoint；
- full-snapshot epoch group commit（已完成）；
- per-device flush 合并；
- 并发 sync=1、crash matrix、reopen 和 allocator consistency 回归。

当前完成条件：并发 sync=1 的 barrier 不重复执行同一批 flush/checkpoint，且 durable/recovered
语义不回退。dirty inode checkpoint、extent snapshot cache 和增量 checkpoint 仍受新的
on-disk format 约束，不能在当前格式上伪装完成。

### Phase E：compaction 和多盘扩展（单节点范围）

- SST flush/compaction 使用当前单节点 data reactor；
- 每 device group bounded queue；
- 大 I/O DMA ring 和批量 completion；
- 多 NVMe 单节点 striping；跨 node striping 暂缓；
- 故障和降级边界保持 fail-closed。

完成条件：单 node、双 node、多盘结果能按 local device bandwidth 和 CPU 预算扩展，不出现全局 reactor/metadata 锁把多盘收益吃掉。

## 10. 性能验证矩阵

### 10.1 对照原则

必须使用同一 RocksDB 8.9.1、同一 logical workload、同一 key/value 分布、同一 CPU 预算和同一设备。ext4 与 HFS 必须分别报告：

- warm-cache；
- cold-cache；
- working set 大于内存；
- sync=0；
- sync=1；
- compaction on/off；
- 读写线程比例；
- CPU 和 NUMA mapping。

不能把 HFS custom benchmark 和 stock db_bench 的不同线程语义混为同一 gate。readwhilewriting 必须共享 workload 定义，并分别报告 writer throughput、reader throughput、总 throughput 和公平性。

### 10.2 必测 workload

    fillseq
    overwrite
    readrandom
    readwhilewriting
    large sequential write/read
    SST flush
    compaction
    WAL sync-heavy
    WAL sync=0 buffered
    WAL sync=1 durable

### 10.3 建议 gate

以下是目标门槛，不是当前已通过结果：

| 指标 | 目标 |
|---|---|
| sync=0 fillseq/overwrite | HFS ≥ ext4 的 120% |
| sync=1 写入 | HFS ≥ ext4 的 110% |
| cold-cache random read | HFS ≥ ext4 的 120% |
| readwhilewriting writer | HFS ≥ ext4 的 110%，且 reader 不低于 ext4 的 100% |
| compaction throughput | HFS ≥ ext4 的 120% |
| p99 | HFS 不高于 ext4 的 110% |
| CPU efficiency | HFS ops/core 不低于 ext4 的 120% |
| 单节点 baseline | errors=0、completion 闭合、durable/recovered 一致；NUMA 指标暂不纳入本轮 gate |
| correctness | errors=0、completion 闭合、crash/reopen 一致 |

warm-cache 读不直接使用“必须超过 120%”作为设备 gate。若要满足产品层面的全面领先，必须先引入并单独验收 HFS read cache。

## 11. 风险与停止条件

### 11.1 主要风险

- NUMA-local arena 可能导致某个 node 内存不足；
- 增加 reactor 可能放大 metadata/global lock 竞争；
- hybrid polling 可能降低高 QD FIO 吞吐；
- adapter async bridge 可能引入 callback、AbortIO 和 buffer lifetime 错误；
- group commit 可能改变 RocksDB Sync/Fsync/Close 的错误返回边界；
- 多 node striping 可能把 namespace 和 checkpoint 变成新的全局瓶颈；
- remote fallback 若无上限，会把 NUMA 问题隐藏成吞吐抖动。

### 11.2 立即停止条件

出现以下任意情况，停止性能调优，回到正确性和生命周期修复：

- remote buffer/fallback 无法准确统计；
- data child、completion、DMA lease 或 callback 不闭合；
- AbortIO 后仍有 late callback 写入释放对象；
- crash/reopen 不能恢复到最后 durable epoch；
- sync=1 返回成功但重启后数据缺失；
- 为了性能改变 .conf 以外的正式设备口径；
- 使用 Malloc/Aio 或旧 HFS 1.x 接口替代真实 NVMe 验收；
- 只通过增加 benchmark 线程掩盖 CPU oversubscription；
- 只报告总吞吐，不报告 CPU、p99、remote bytes 和错误数。

## 12. 结论

当前 HFS 已经证明 adapter 写回缓冲可以让部分 sync=0 写 workload 超过原生 RocksDB，并已修复
RMW completion 生命周期和 RocksDB async read 的核心闭环；但冷缓存读性能、严格持久化的完整
crash/reopen 矩阵、compaction 和多 NUMA node 场景仍没有证明全面领先。

当前 VM 无法提供多 NUMA 拓扑时，最优先的工程顺序是：

    修复 RMW/async read 生命周期
      -> 单节点 sync=1 correctness 回归
      -> 冷缓存/直接读公平性能矩阵
      -> hybrid polling/QoS
      -> full-snapshot metadata group commit
      -> 单节点 compaction/多盘扩展
      -> 公平矩阵和正式 gate

多 NUMA 的 topology/per-node DMA/reactor 设计和代码保留，但移出当前完成条件；待 VM
具备多 NUMA 后再单独补齐 remote-byte、local arena 和跨 node striping 证据。

其中，NUMA 不是最后的部署调参，而是从 DMA arena、reactor、frontend queue 到 completion 的 ownership 设计。只在测试命令前加 numactl，而不改变 HFS 内部 buffer、队列和 reactor 归属，不能解决当前问题。
