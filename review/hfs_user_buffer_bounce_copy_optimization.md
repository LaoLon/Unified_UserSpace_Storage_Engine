# HFS 用户 buffer Bounce Copy 与冗余 I/O 优化评审

> **评审日期**：2026-09-09
> **评审范围**：普通 POSIX/preload buffer 进入 HFS 数据面后的 copy、DMA lease、SPDK 请求和相关放大路径
> **基线提交**：`4054111`
> **参考结果**：`/mnt/hfs-data/performance-stage3-nvme-fio-qd32`

## 1. 评审结论

当前 HFS 的主要问题不是“每个 FIO I/O 被重复提交了很多次”，而是普通用户 buffer 无法被
SPDK 直接 DMA 时，单个逻辑 I/O 被迫经过一条较重的兼容路径：

```text
用户 buffer
   │
   ├─ spdk_vtophys 失败
   │
   ├─ 获取 reactor-local DMA lease
   ├─ write: memcpy(user → DMA)
   ├─ 跨线程 batch queue
   ├─ SPDK blob I/O
   ├─ completion / waiter
   └─ read: memcpy(DMA → user)
```

这条路径当前还叠加了 buffer registry 查询、pool mutex、range/sync lock、completion waiter
和延迟 metadata publish。QD32 NVMe 实测结果如下：

| Block size | HFS IOPS | raw SPDK IOPS | HFS/raw | HFS p99 读/写 |
|---:|---:|---:|---:|---:|
| 4 KiB | 46,523 | 117,821 | 39.5% | 2.57 / 6.39 ms |
| 8 KiB | 26,932 | 108,364 | 24.9% | 3.10 / 20.32 ms |
| 16 KiB | 18,957 | 91,602 | 20.7% | 2.02 / 27.92 ms |
| 32 KiB | 17,914 | 72,022 | 24.9% | 0.92 / 13.43 ms |
| 64 KiB | 15,870 | 52,123 | 30.5% | 43.25 / 68.68 ms |

本轮 HFS 使用 `numjobs=32`，raw SPDK 使用 `iodepth=32`，两者访问同一块 SPDK NVMe
`Nvme0n1`，错误数为 0。当前最值得做的不是简单增大 DMA pool，而是按照以下顺序减少
额外工作：

1. 让可验证的 aligned buffer 直接进入 SPDK，消灭双向 memcpy。
2. 对已经由 HFS 自己拥有的 DMA buffer，跳过无意义的 registry 查询和生命周期加锁。
3. 让 bounce request 尽量一次提交、一次完成，避免 edge RMW、metadata 和 zero fallback
   产生不必要的附加请求。
4. 将 copy、lease、锁和 completion 的生命周期拆开，避免一个慢请求把后续请求全部挡住。

## 2. 当前数据路径拆解

### 2.1 Aligned 且 DMA-capable

`hfsIoReadAtLocked()`/`hfsIoWriteAtLocked()` 在 `src/hfs_io.c:1833-1842` 和
`src/hfs_io.c:2002-2011` 首先检查 offset、count 和 `hfsIoBufferDmaCapable()`。
判断成功后，路径进入 `hfsIoSubmitDirect()`（`src/hfs_io.c:917`）：

```text
caller buffer → data reactor channel → spdk_blob_io_read/write → completion
```

理想情况下，一个 logical I/O 只有一个 SPDK data request，copy 次数为 0。当前限制是：

- `spdk_vtophys()` 必须覆盖整个连续 buffer，不能只映射其中一页；
- buffer 必须完整 unit 对齐，不能带边缘 RMW；
- 同步路径仍然在 `hfsIoSubmitDirect()` 中等待 completion；
- count 超过单个 DMA class 最大尺寸时，direct path 会拆成多个 chunk 并逐个等待。

因此，direct path 已经减少了 copy，但还没有完全达到 raw SPDK 的“连续 outstanding queue”
模型。

### 2.2 Aligned 但普通用户 buffer

这是 FIO `psync` 和大多数普通数据库 POSIX buffer 的实际路径。`spdk_vtophys()` 失败后，
HFS 进入 `hfsIoReadAtLocked()`/`hfsIoWriteAtLocked()` 的 bounce pipeline：

| 操作 | 必要步骤 | 当前额外成本 |
|---|---|---|
| 写 | 获取 DMA lease → copy 用户数据 → 提交一次 SPDK write | 至少一次 `memcpy`、pool mutex、completion waiter |
| 读 | 获取 DMA lease → 提交一次 SPDK read → copy 到用户 buffer | 至少一次 `memcpy`、pool mutex、completion waiter |
| 多 child | 每个 child 独立 lease 和 `HfsIoRequest` | lease 生命周期与 copy 绑定，不能快速归还 |

写 copy 位于 `src/hfs_io.c:2097-2099`，读 copy 位于 `src/hfs_io.c:1969-1970`。这类
aligned 请求通常没有额外的 data read/write，但用户 buffer 与 DMA buffer 之间的 copy、
registry 查询和同步等待会在每个 I/O 上重复发生。

### 2.3 非对齐读写

非对齐路径不是简单的“多做了一次 copy”，而是需要保护 unit 外部内容：

- 读：边缘 unit 必须先读完整 unit，再从 lease copy 所需字节到用户 buffer；
- 写：边缘 unit 必须执行 read-modify-write，读取完整 unit，修改局部字节，再写回完整
  unit；实现位于 `hfsIoModifyPartial()`（`src/hfs_io.c:1782-1804`）；
- 中间完整 unit 才能走普通 aligned pipeline。

因此，非对齐边缘的 read+write 是 POSIX 数据保持语义要求的必要请求，不能通过“直接把用户
buffer 传给 SPDK”简单消除。可以消除的是重复获取 lease、重复 copy 和相邻边缘操作的重复
读写。

### 2.4 文件增长、metadata 和 zero fallback

这些不是每个稳定 aligned FIO 请求都必然发生，但会放大数据库和首次写入：

- `hfsIoWriteAt()` 每次先调用 `hfsResizeTo()`（`src/hfs_io.c:2215`），但
  `hfs_file.c:461-497` 的 `allocatedClusters` fast check 可以在容量已覆盖时避免真正的
  resize request；首次跨 cluster 写入仍可能产生 blob resize/分配操作。
- `hfsPublishWriteEndFast()`（`src/hfs_lib.c:555-577`）已把不扩展写的 timestamp 更新
  延迟到 runtime，避免每次写都立即获得 metadata write lock；扩展写、truncate 和 fsync
  仍有必要的 metadata 工作。
- punch hole 的 aligned 路径先 unmap，再尝试 WRITE_ZEROES，失败后再用 DMA 写零。后端已经
  保证 deallocated read 返回零时，这个 zero fallback 就是多余的物理写请求。

评审重点应放在“稳定 aligned I/O 不再触发上述附加操作”，而不是把必要的 RMW 或 durability
barrier 误判为冗余。

## 3. 主要性能瓶颈

### 3.1 DMA 判定和 direct 命中率过低

`hfsIoBufferDmaCapable()`（`src/hfs_io.c:884-889`）对普通调用方地址调用
`spdk_vtophys()`。当前 HFS 有 `hfsRegisterBuffer()`/`hfsUnregisterBuffer()` API，但它是
实验性显式接口，要求应用自己提供经过 SPDK DMA 地址验证的连续内存；FIO 默认 buffer 和
普通 libc heap 并不会自动使用它。

结果是：

1. 普通 POSIX I/O 的 direct hit rate 接近 0；
2. 每个请求都进入 DMA pool；
3. 读写分别保留一次反向/正向 copy；
4. direct path 的异步 child fan-out 对普通 preload workload 不生效。

这也是 HFS 与 raw SPDK 的根本分叉：raw plugin 可以要求/分配 DMA-capable buffer，而
LD_PRELOAD 只能接收调用方已经传入的地址，不能在系统调用返回前替换调用方内存。

### 3.2 HFS 自己拥有的 DMA buffer 仍然经过 registry 查询

`hfsIoStartRequest()`（`src/hfs_io.c:704-735`）对所有非 vector request 调用
`hfsIoBufferAcquire()`。该函数在 `src/hfs_io.c:793-838` 获取全局
`g_hfsIoBufferLock`，遍历注册范围；即使 request buffer 是 HFS 自己从 pool 获得的 DMA
buffer、从未注册过，也会进入一次 mutex/链表查询。

这个查询的原始目的，是防止 async direct I/O 在用户调用 `hfsUnregisterBuffer()` 或释放
内存前完成；对 HFS-owned lease 不需要这个保护。当前实现把两种生命周期混在一个字段里，
因此普通 bounce I/O 付出了与 direct registration 相关的额外锁成本。

**建议**：在 `HfsIoRequest` 增加明确的 buffer ownership：

```c
typedef enum {
    HFS_IO_BUFFER_BORROWED,     /* caller-owned; registry ref may be needed */
    HFS_IO_BUFFER_DMA_LEASE,    /* HFS-owned; lease already pins lifetime */
    HFS_IO_BUFFER_VECTOR        /* iovec lifetime handled by caller/request */
} HfsIoBufferKind;
```

`HFS_IO_BUFFER_DMA_LEASE` 直接跳过 `hfsIoBufferAcquire()`。对 caller-owned buffer，再保留
registry ref；这样既不改变生命周期语义，也能从普通 bounce 热路径移除一次全局 mutex。

### 3.3 DMA pool 获取和 copy 生命周期耦合

`hfsIoAcquire()`（`src/hfs_io.c:509-555`）在 class 无空闲 buffer 时等待条件变量；
`hfsIoRelease()`（`src/hfs_io.c:581-616`）在 completion 和 read copy/write 生命周期
结束后才归还 lease。当前模型会产生两个问题：

- 读请求已经收到 SPDK completion，但如果仍等整个 pipeline 才执行 `memcpy(DMA→user)`，
  lease 释放被推迟；
- 请求在等待 lease 时可能已经持有 inode/range reservation，pool 枯竭会反过来放大锁竞争。

64 KiB QD32 的 HFS p99 读/写达到 43/69 ms，而 raw SPDK 约 1.1 ms，说明这种资源等待和
同步等待的叠加远大于 NVMe 本身的 completion 延迟。

### 3.4 每个 logical I/O 的提交/完成粒度过细

`hfsIoStartRequest()` 最终通过 owner/data reactor 的 batch dispatcher 提交一个
`HfsIoRequest`。batch 已经消除了“每个请求各发送一次 SPDK thread message”的早期问题，
但以下开销仍然按 request 重复：

- 一个 `HfsRequestResult` completion state；
- 一次 request-level callback；
- 一次 DMA lease acquire/release；
- 一次 waiter 唤醒；
- bounce 写入的 copy 或 bounce 读取的 copy。

因此，batch dispatcher 解决的是通知放大，不是数据 copy、lease 或 completion 粒度问题。
要继续逼近 raw SPDK，需要以 parent batch 为单位聚合 child completion，同时确保每个
child 的错误、range 和 buffer 生命周期仍可追踪。

### 3.5 非对齐和特殊操作中的请求放大

以下放大是语义要求或后端能力缺失导致，应该分别处理：

| 场景 | 典型放大 | 是否可直接消除 |
|---|---|---|
| aligned read/write | 1 个 data request + 1 次方向性 copy | copy 可通过 DMA direct 消除 |
| 单边非对齐 read | 完整 unit read + edge copy | read 不能省，copy 可在专用边缘 buffer 中合并 |
| 单边非对齐 write | 完整 unit read + 完整 unit write | 读写都由 RMW 语义要求；可合并相邻操作 |
| 首次扩展写 | resize/cluster 分配 + data write | 可通过 fallocate/preallocation 摊平 |
| punch hole | unmap + write zeroes 或 DMA zero write | 取决于 bdev 的 zero-after-unmap 能力 |
| fsync | data completion + metadata commit + bdev flush | durability 要求；只能 group commit |

对于本轮 aligned `randrw`，主要差距不是 RMW，而是普通 buffer 的 bounce copy、request
同步等待和 pool/lock 资源占用。优化时不能拿非对齐的 correctness fallback 去替代主路径
收益。

## 4. 优化方案

### 4.1 第一阶段：不改变 ABI 的低风险热路径优化

#### A. 明确 buffer ownership，移除 bounce request 的 registry lock

改动位置：`src/hfs_io.h`、`src/hfs_io.c:704-735`。

- request 增加 `bufferKind` 或 `ownsBuffer`；
- DMA lease 申请成功后标记 `HFS_IO_BUFFER_DMA_LEASE`；
- `hfsIoStartRequest()` 只对 caller-owned direct buffer 调用 registry acquire；
- pool-owned buffer 的释放路径只依赖 lease，不再扫描全局注册表。

预期收益：不减少 copy 字节数，但减少每个 bounce I/O 的全局 mutex 和链表遍历，尤其适合
QD32 多线程场景；风险低，不改变数据和 async lifetime。

#### B. 为空 registry 增加无锁 fast path

即便保留统一 ownership，也应维护 `_Atomic uint32_t g_hfsIoRegisteredCount`：

- count 为 0 时直接返回 NULL，不获取 `g_hfsIoBufferLock`；
- count 非 0 时再进入当前查找逻辑；
- registered direct path 仍通过 active ref 防止 unregister 早于 completion。

这能覆盖绝大多数普通 preload/FIO 进程，同时保留显式注册 API 的安全语义。

#### C. 把 read copy 提前到 child completion

当前同步 bounce read 在 pipeline 中等待全部 child 后才集中 copy（`src/hfs_io.c:1962-1975`）。
可将目标用户地址和相对偏移放入 request，child completion 完成后立即执行：

```text
SPDK read completion
   → DMA → user copy
   → release lease
   → parent pending--
```

这不会减少 copy 字节数，但能显著缩短 lease 持有时间，避免一个 slow child 把同一个
reactor/class 的所有 buffer 占满。必须保证同步调用仍在父请求完成前返回，并通过 caller
buffer registration 保证 async buffer 生命周期。

#### D. 写路径按 child 预留、copy、提交

当前 write pipeline 会先为整批 child 预留 lease，再依次 copy/submit。应保留 bounded
window，但当下一个 lease 不能获得时，不要继续持有已准备但尚未提交的 range reservation。
推荐顺序：

1. 计算 child range；
2. 获取该 child lease；
3. copy 用户数据；
4. 提交 SPDK request；
5. 才进入下一个 child。

这不会减少 write copy，但能减少“已占 lease、尚未提交”的空转时间和 pool head-of-line
blocking。

### 4.2 第二阶段：让普通 workload 能够使用 direct DMA

#### A. 提供明确的 HFS DMA allocator，而不是全局替换 malloc

不建议在 LD_PRELOAD 中全局重载 `malloc()`：HFS 自己、glibc、MariaDB、日志和 socket
路径都可能调用 malloc，强行返回 SPDK DMA memory 会扩大碎片、NUMA 和 fork 风险。

建议提供独立 ABI：

```c
void *hfsDmaAlloc(size_t length, size_t alignment);
int hfsDmaFree(void *buffer);
int hfsRegisterBuffer(void *buffer, size_t length);
int hfsUnregisterBuffer(void *buffer);
```

数据库/FIO 专用适配器可以显式使用该 allocator；普通 libc buffer 继续安全 fallback。
这样可以把“高性能 ABI”与“POSIX 兼容 ABI”分开，不破坏未知应用的内存语义。

#### B. 注册范围与 request 生命周期绑定

现有 `hfsRegisterBuffer()` 已经验证整个范围通过 `spdk_vtophys()` 后可以走 direct async，
但注册表是线性链表。建议：

- 使用按地址排序的 interval tree 或分段 radix index；
- 在 request 创建时取得一个 immutable registration handle；
- completion 后释放 active ref；
- unregister 只删除可见范围，仍有 active request 时返回 `EBUSY`；
- fork child 继续拒绝进入父进程 SPDK runtime。

这样既可以避免每个 I/O 重复调用 `spdk_vtophys()`，也能让多个相邻 I/O 共享一次 buffer
classification。

#### C. direct path 使用 bounded async window

当前同步 `hfsIoSubmitDirect()` 是“submit 一个 chunk，然后 wait 一个 completion”。对于
注册 DMA 的大块 aligned I/O，应复用 `hfsIoAsyncStartBatch()` 的 parent/child 模型：

- 每个 child 直接引用 caller DMA buffer 的对应区间；
- depth 由 `HFS_IO_PIPELINE_DEPTH` 控制；
- parent 只在所有 child 完成后返回；
- write metadata publish 只在 parent 成功时执行一次。

这样可以在不增加 copy 的前提下把 direct path 的 outstanding 行为拉近 raw SPDK。

### 4.3 第三阶段：减少不可避免的 copy 和请求数量

#### A. full-unit middle 与 edge RMW 分离

非对齐请求必须拆成：

```text
edge prefix  → RMW
full middle  → direct/bounce pipeline
edge suffix  → RMW
```

当前实现已经具备这个结构，但可以进一步：

- 对相邻 edge request 在同一 worker/window 内合并，避免对同一个 unit 反复 read-modify-write；
- 只在 copy 所需字节范围内做用户 buffer copy，不复制整个 unit 到用户空间；
- 将同一个 unit 的 pending RMW 串行化，避免两个写各自读旧 unit 后互相覆盖。

合并只能在 async queue 或明确的 per-fd write-behind 模式下进行；同步 `write()` 返回前
不能把尚未提交的数据留在内存里，否则会改变 POSIX 可见性。

#### B. read/write vector 使用一次 SGL 提交

`hfsIoSubmitVector()` 已在所有 iovec segment 都 DMA-capable 时尝试
`spdk_blob_io_readv/writev`，但普通 heap iovec 会整体 fallback。建议为注册 DMA buffer
提供 DMA iovec/SGL 构造器：

- 不再把每个 iovec 复制到单独的临时大 buffer；
- 一个 logical `readv/writev` 对应一个 SPDK vector request；
- 非 DMA segment 只对该 segment fallback，不让整个 vector 退化。

这能减少“一个 vector 被拆成多个同步 `hfsReadAt()`/`hfsWriteAt()`”的请求放大。

#### C. 预分配减少 resize 请求

对已知工作集先执行 `fallocate()`/`posix_fallocate()`，让 `allocatedClusters` 在数据面热身
阶段一次性建立。随机写的稳态路径只做 data I/O，不在首次命中更高 LBA 时触发扩容。

文件系统内部仍需保留 `hfsResizeTo()` 的正确性检查；优化目标是让数据库/FIO 能显式选择
“容量已承诺”的文件，从而把 resize request 从每个 workload 的热路径移到初始化阶段。

#### D. 后端能力化 punch zero

为 bdev 增加“unmap 后读取是否保证 zero”的能力查询：

- 能力确认时：aligned punch 只提交 unmap；
- 能力未知/不保证时：保留 write-zero fallback；
- edge unit 仍用 RMW zero，保证非对齐范围语义。

这只影响 punch hole，不应混入普通 FIO aligned 结果，但能避免数据库清理阶段出现额外写
请求和介质放大。

## 5. 推荐实施顺序与预期收益

| 阶段 | 主要改动 | 直接减少的工作 | 预期收益 | 风险 |
|---|---|---|---|---|
| P0 | buffer ownership、空 registry fast path | 每 I/O 一次 registry mutex/扫描 | 降低 CPU 和 queue wait | 低 |
| P0 | completion 内提前 read copy/release lease | lease 持有时间、pool HOL | 降低 64 KiB p99 | 中 |
| P1 | DMA allocator + registration handle | aligned read/write 双向 copy | 提升 direct hit rate | 中 |
| P1 | direct async bounded window | direct path 串行 wait | 提升 QD32 吞吐 | 中 |
| P1 | per-reactor freelist/credit | pool mutex、cond broadcast | 降低 pool backpressure | 中 |
| P2 | RMW edge 合并、vector partial fallback | 重复 edge read/write、vector 拆分 | 降低非对齐/IOV 放大 | 高 |
| P2 | preallocation、punch capability | resize/zero fallback 请求 | 降低数据库初始化和清理成本 | 中 |

建议不要一次性修改所有层。先做 P0，确认性能改善来自 registry/lease 生命周期，再做 DMA
allocator，最后才调整 RMW 和 vector 语义。每一步都要保持现有 `submit=complete`、
`lease acquire=release` 和数据校验闭合。

## 6. 验证矩阵

### 6.1 主性能矩阵

```text
backend:        SPDK NVMe Nvme0n1（固定 PCI BDF）
workload:       randread / randwrite / randrw(50/50)
block size:     4K / 8K / 16K / 32K / 64K
concurrency:    QD1 / QD32
buffer:         普通 malloc / registered DMA / HFS DMA allocator
stats:          stats-off 主结果，stats-on 归因结果
```

必须同时记录：

- `io_copy_bytes`、`io_copy_ops`、`io_direct_bytes`、direct hit rate；
- `dma_acquire/release`、`dma_wait`、`dma_wait_ns`、`dma_exhaust`、active peak；
- runtime queue wait、owner/data reactor submit/complete；
- range lock wait/hold、RMW 次数、resize 次数；
- read/write p50/p99/p99.9 和错误数。

### 6.2 预期验收门槛

1. registered DMA 的 aligned I/O：`io_copy_ops=0`，SPDK data request 数与 logical I/O 数
   一致；
2. 普通 malloc 的 aligned bounce I/O：每个方向最多一次 copy，不再额外触发 registry mutex
   热路径；
3. 4 KiB QD32 HFS/raw IOPS 比例先从 39.5% 提升到 50% 以上，再继续优化；
4. 64 KiB QD32 HFS p99 相比当前 43/69 ms 至少下降 30%；
5. 非对齐 RMW、truncate、punch、fsync、SIGKILL recovery 和数据库 smoke 无回退；
6. 所有请求生命周期计数严格闭合：`submit=complete`、`acquire=release`、
   `parent pending=0`。

## 7. 最终判断

当前 HFS 的 bounce copy 是“普通 POSIX buffer 与 SPDK DMA 要求之间的兼容成本”，不能仅靠
增加 reactor 或扩大 queue depth 消除。最有效的路径是分层处理：

1. 对 caller-owned buffer 做一次可靠的 DMA classification；
2. 对 DMA-capable buffer 走 direct async，完全消除用户/DMA 双向 copy；
3. 对不可 DMA 的 buffer 保留 bounce，但跳过 HFS-owned lease 的 registry 锁，并在 completion
   处尽早 copy/release；
4. 对非对齐和特殊操作只保留语义必需的 RMW、resize、metadata 和 zero 请求；
5. 将剩余 request 聚合、pool backpressure 和 lock 生命周期纳入同一个 bounded pipeline。

这套方案既不会牺牲当前 POSIX 数据正确性，也不会把 LD_PRELOAD 强行变成全局 DMA allocator。
最终产品形态仍应把高性能 direct DMA 作为显式 async/libhfs ABI，把普通 preload 作为兼容
fallback；二者共享同一份 SPDK data reactor、range 和 metadata 正确性协议。
