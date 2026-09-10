# HFS Registered DMA Async Ring Shard 开发计划

> 架构收口说明（2026-09-10）：本文件为历史计划。HFS 已收敛为 POSIX-only，
> 对外 registered-DMA、`hfs_lib_async_*` 和 async ring ABI 已删除；后续优化只在
> 完整 POSIX 路径内部复用 DMA pool、direct 检测和 bounded pipeline。

> **状态**：待开发
> **Shard**：shard-60
> **唯一技术路线**：固定深度 DMA buffer ring + direct async 多请求提交 + 固定完成队列
> **当前基线**：`main@e9adf77`，VM 内 SPDK 使用锁定 commit
> `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`

## 1. 决策

HFS 后续高性能路径只采用一条方案：由调用方显式取得 HFS DMA buffer，使用固定深度的
DMA ring 保持多个 outstanding request，通过 direct async 路径提交到 SPDK，并通过完成队列
回收和复用 slot。

普通 POSIX `malloc` buffer 继续保留现有 bounce fallback；`HFS_POSIX_DIRECT_ASYNC` 仅作为
默认关闭的实验桥接，不进入性能主路径。原因是普通 POSIX buffer 无法保证
`spdk_vtophys()` 成功，桥接仍需 copy，且 callback/condition wait 比当前同步 bounce 路径
更慢。该结论已经由 NVMe A/B 验证，不能再通过包装同步 POSIX API 来制造“异步零拷贝”。

## 2. 当前能力与缺口

### 2.1 已有能力

- `hfsDmaAlloc(size, alignment)` 使用 SPDK DMA allocator，返回可被 SPDK 直接访问的内存。
- `hfsRegisterBuffer()` 校验 `spdk_vtophys()` 并记录 buffer lifetime；未完成请求时
  `hfsUnregisterBuffer()` 返回 `EBUSY`。
- `hfs_lib_async_pread/pwrite()` 已能在 DMA-capable、对齐 buffer 上进入 worker-free
  direct async 路径。
- `hfsAsyncWait/Result/Release` 和 completion callback 已提供请求生命周期管理。
- 现有 direct-DMA 矩阵已经证明 depth=32 时 `copy_ops=0`、backend outstanding=32、
  submit/complete 闭合。

### 2.2 必须修复的缺口

1. 当前 public direct 判断要求请求至少包含 2 个 HFS unit；4 KiB 请求会落入兼容路径，
   需要让已确认 DMA-capable 的单 unit 请求也能走 direct async。
2. 调用方需要自行管理 buffer、request、完成和重提交，容易误用生命周期；需要提供一个
   HFS 原生 ring 封装，把固定 slot、注册、完成队列和销毁顺序集中管理。
3. 完成 callback 不能承担复杂业务逻辑或阻塞等待；需要固定大小的 MPSC completion queue，
   callback 只写入结果、释放 caller request 引用并通知消费线程。
4. 目前每个 direct request 仍会动态创建 request/context，并执行一次 runtime/range
   lock；ring 第一版先复用现有 direct API，随后在同一 shard 内增加 slot/context cache，
   消除 steady-state 的 per-request `calloc/free`。
5. ring 的数据完成不能等同于 durability 完成；`fsync` 必须作为独立 barrier，在所有相关
   slot 完成后通过 `hfs_lib_async_fsync()` 提交。

## 3. 目标架构

```text
HfsDmaRing
  ├─ N 个固定 slot
  │    ├─ SPDK DMA buffer
  │    ├─ registered range
  │    ├─ offset / length / operation
  │    ├─ HfsAsyncRequest*（仅 outstanding 期间有效）
  │    └─ slot state
  ├─ submit side
  │    └─ hfs_lib_async_pread/pwrite -> HFS direct async -> SPDK
  └─ completion side
       └─ data reactor callback -> MPSC completion ring -> caller poll/re-submit
```

一次完整请求的生命周期固定为：

```text
FREE
  -> FILL (write) / READY (read)
  -> SUBMITTED
  -> COMPLETED
  -> caller consumes result
  -> FREE
```

slot 在 `SUBMITTED` 状态期间禁止修改或释放 buffer；只有完成事件进入 completion queue 后，
消费线程才能调用 `hfsAsyncRelease()` 并重新填充/提交该 slot。

## 4. 对外 API 设计

在 `src/hfs_lib.h` 中新增 ring API，隐藏底层 `HfsAsyncRequest` 和注册表细节：

```c
typedef struct HfsDmaRing HfsDmaRing;

typedef struct HfsDmaCompletion {
    uint32_t slot;
    ssize_t result;
    int error;
    void *context;
} HfsDmaCompletion;

int hfsDmaRingCreate(size_t depth, size_t bufferSize, size_t alignment,
                     HfsDmaRing **ringOut);
void *hfsDmaRingBuffer(HfsDmaRing *ring, uint32_t slot);
int hfsDmaRingSubmitRead(HfsDmaRing *ring, int fd, uint32_t slot,
                         off_t offset, size_t count, void *context);
int hfsDmaRingSubmitWrite(HfsDmaRing *ring, int fd, uint32_t slot,
                          off_t offset, size_t count, void *context);
int hfsDmaRingPoll(HfsDmaRing *ring, HfsDmaCompletion *events,
                   size_t capacity, uint32_t timeoutMs);
int hfsDmaRingDrain(HfsDmaRing *ring, uint32_t timeoutMs);
void hfsDmaRingDestroy(HfsDmaRing *ring);
```

约束如下：

- `depth` 是固定 outstanding 上限，创建后不动态扩容。
- `bufferSize` 和每次 `count` 必须是 HFS unit 的整数倍；`offset` 也必须对齐。
- `slot` 只能被提交一次；上一次 completion 被 `poll` 消费后才允许再次提交。
- `hfsDmaRingBuffer()` 返回的指针只属于 ring，不能调用 `free()` 或 `hfsDmaFree()`。
- `hfsDmaRingDestroy()` 内部先 drain，再 unregister，最后释放 DMA buffer 和 ring 对象。
- submit 返回 `EAGAIN` 时不得忙等；slot 保持 READY，由 caller 在已有 completion 后重试。

该 API 不改变普通 `read/pread/write/pwrite` 的 POSIX 语义，也不把普通 heap 指针转换成
DMA 指针。

## 5. 提交和完成规则

### 5.1 创建阶段

`hfsDmaRingCreate()` 必须按以下顺序执行：

1. 校验 depth、bufferSize、alignment，拒绝溢出和非 unit 对齐配置。
2. 为每个 slot 调用 `hfsDmaAlloc(bufferSize, alignment)`。
3. 为每个 slot 调用 `hfsRegisterBuffer()`；任何失败都按逆序 unregister/free。
4. 初始化固定大小的 completion queue、slot state 和统计计数。

### 5.2 提交阶段

- write：caller 先填充 `hfsDmaRingBuffer()`，然后提交；completion 前不能修改。
- read：提交前不需要初始化 buffer，completion 后 caller 才读取数据。
- 只调用 `hfs_lib_async_pread/pwrite()`；禁止在 ring 内调用同步 `pread/pwrite`。
- 首轮提交 depth 个不重叠 offset，之后每收到一个 completion 就补交一个新 offset，
  使 backend outstanding 稳定在目标深度。
- overlapping range 触发 `EAGAIN` 时保留 slot，不改变 offset 顺序；不通过忙轮询绕过
  `rangeGate`。

### 5.3 完成阶段

SPDK reactor 上的 callback 只做以下操作：

1. 把 `result/error/slot/context` 写入固定 MPSC completion queue。
2. 标记 slot 为 COMPLETED。
3. 调用 `hfsAsyncRelease(request)` 释放 caller reference。
4. 通过轻量 semaphore/futex 唤醒 poll 线程。

callback 不执行 `malloc/free`、文件元数据操作、同步等待或下一个请求的复杂准备；slot
复用和新请求提交统一由 caller poll 线程执行，避免拖慢 data reactor。

### 5.4 关闭阶段

```text
停止新提交
    -> hfsDmaRingDrain()
    -> 确认 outstanding == 0
    -> hfsUnregisterBuffer()（每个 slot）
    -> hfsDmaFree()（每个 slot）
    -> 销毁 completion queue/ring
```

任何 slot 仍为 SUBMITTED 时禁止卸载 HFS、关闭 blob 或释放 DMA memory。

## 6. 实现分解

### 6.1 shard-60.1：ring 核心和生命周期

- 新增 `src/hfs_dma_ring.[ch]`。
- 实现固定 slot、DMA allocation、registration、state machine 和 destroy/drain。
- 复用当前 `hfs_lib_async_*`，暂不修改 SPDK bdev 逻辑。
- 增加 `scripts/db/registered_dma_async_ring_smoke.sh`，覆盖 depth 1/4/32、读写校验、
  `EBUSY` unregister 和异常退出。

### 6.2 shard-60.2：completion queue 和 direct gate

- 增加固定大小 MPSC completion queue，禁止 steady-state 动态分配。
- callback 只入队，poll 线程负责释放 request 和复用 slot。
- 将 DMA-capable public async 请求的最小 direct 长度从 2 units 调整为 1 unit，确保
  4 KiB direct I/O 不再回退到 worker/bounce 路径。
- 统计 `ring_submit/ring_complete/ring_reject/ring_depth_peak`，并与已有
  `io_direct_ops/io_copy_ops` 对齐。

### 6.3 shard-60.3：steady-state allocation 消除

- 为 ring slot 关联可复用的 direct request/context storage。
- 让 direct async steady-state 不再为每个 request 执行 `calloc/free`；失败路径仍允许
  使用一次性错误对象，但不能污染成功热路径。
- 保留 runtime/range lock 语义和 metadata publish 顺序，不通过扩大锁范围换取吞吐。

### 6.4 shard-60.4：基准和产品接入边界

- 新增 async ring benchmark，覆盖 4/8/16/32/64 KiB、QD 1/4/8/16/32、read/write/randrw。
- 使用同一 `Nvme0n1` 对比原生 SPDK；Aio/Malloc 只用于 correctness 和回归，不冒充 NVMe。
- 记录 `copy_ops=0`、有效 outstanding、submit/complete、range wait/hold、p50/p99/p99.9。
- 更新 `doc/design/fio_baseline_notes.md` 和 `doc/performance/` 报告，明确 async ring 与
  普通 POSIX/FIO preload 不是同一 API 口径。

## 7. 正确性和性能验收

### 7.1 必须通过的正确性条件

- depth 1/4/8/16/32 连续 3 轮读写校验通过，错误数为 0。
- 所有 direct ring 样本 `io_copy_ops=0`，`hfsAsyncSubmit == hfsAsyncComplete`。
- backend outstanding 最终回到 0，`hfsDmaRingDestroy()` 无 UAF、SIGSEGV 或泄漏。
- `hfsUnregisterBuffer()` 在未完成 request 时返回 `EBUSY`，完成后成功。
- overlap range、`EAGAIN`、cancel、close、fork child、SIGKILL recovery 不破坏现有契约。
- Make、CMake Release、既有 POSIX/fd/metadata/database smoke 全部通过。

### 7.2 性能验收条件

- QD32 的有效 backend outstanding 至少达到 28，稳定样本达到配置深度。
- direct submit/complete 闭合，copy 为 0，completion queue 不溢出。
- 4/8/16 KiB QD32 相比当前同步 POSIX HFS 基线，吞吐至少提升 20%；若未达到，必须由
  trace 明确指出剩余主导瓶颈，不能把结果称为优化完成。
- 4/8/16 KiB 的 p99 不得因 ring 引入新的固定级别排队；64 KiB p99 不得比基线恶化。
- callback/queue 的额外 CPU 开销在 QD1 下不超过 direct baseline 的 5%。

## 8. 明确不做的事情

- 不再扩展 `HFS_POSIX_DIRECT_ASYNC`，它不是零拷贝方案。
- 不改写全局 `malloc()`，不对未知 heap/mmap buffer 做伪注册。
- 不把 POSIX 同步 `pread/pwrite` 包装成 callback+condition wait 来冒充异步队列。
- 不在 ring 内使用 `hfs_lib_async_preadv/pwritev` 作为 direct 零拷贝接口；当前 vector
  public wrapper 尚未保证 registered-DMA direct，先使用每 slot 一个连续 DMA buffer。
- 不为了达到 QD32 而放宽 overlapping write、RMW、fsync 和 metadata publish 语义。

## 9. 回滚和观测

- ring API 是新增能力，不改变默认 POSIX 路径；异常时调用方直接回退到已有 async API。
- 保留 `HFS_IO_PIPELINE_DEPTH`，用于限制单请求 window 的 child depth；ring depth 不得绕过
  该上限造成无限 outstanding。
- `HFS_STATS_TIMING=1` 只用于归因；正式吞吐使用 stats-off。
- 每轮结果必须保存 commit、SPDK commit、bdev、CPU 亲和、buffer kind、QD、copy/direct
  计数和错误日志。
- 新发现的生命周期、completion、unregister 或 shutdown bug 立即写入 `doc/bug.md`。

## 10. 完成定义

shard-60 只有在以下条件全部满足后才能标记完成：

1. ring API、completion queue、4 KiB direct gate 和 request/context reuse 已合入 `main`。
2. VM 上 Make/CMake、ring smoke、既有全量 smoke 和错误注入全部通过。
3. NVMe 矩阵证明 `copy_ops=0`、QD32 outstanding 稳定、submit/complete 闭合，并达到
   性能门槛或给出可复现的剩余锁/队列证据。
4. `doc/performance/`、`doc/design/fio_baseline_notes.md`、本计划和产品支持边界已更新。
5. 独立提交后在 `main` 上复核 Git 状态、VM 构建和原始测试结果。
