# HFS 2.0：C++20 coroutine-first 的 SPDK-native 存储引擎

> 状态：clean-break 重构设计，2026-09-16 生效
>
> 评审输入：`doc/review/hfs2.0_design_review_20260916.md`
>
> 当前事实基线：clean-break 迁移后的工作区。本文描述的是新目标，不表示新 core 已实现。

## 1. 设计决策

HFS 2.0 不再是当前 HFS 的 C/Blobstore/POSIX 增量版本，而是从空的 `src/` 开始的
全新 C++20 存储引擎：

```text
RocksDB 8.9.1 FileSystem adapter ─┐
                                   ├─ libhfs_core.so ─ SPDK bdev set ─ NVMe pool
FIO 3.42 external ioengine ───────┘
```

核心运行模型：

```text
Frontend
  → hfs::Client
  → hfs::Task / hfs::Operation
  → bounded MPSC shard ring
  → shard coroutine actor
  → one inode dependency scheduler
  → raw-bdev extent planner
  → SPDK channel / NVMe device set
  → completion event
  → frontend callback or poll
```

本次重构的硬性结论：

1. 旧 `src/` 已整体改名为 `src_legacy_frozen/`，作为本地只读参考，不进入 HFS 2.0
   构建，也不再由 Git 跟踪；旧代码从 Git 中删除。
2. 新 `src/` 不兼容旧 C 源码；后续实现只使用 C++20、RAII、`std::span`、concepts、
   `std::stop_token` 和 coroutine。允许使用 coroutine，但禁止用 `std::future` 把每个
   I/O 变成隐藏 heap allocation 或阻塞线程。
3. 新 core 直接管理 SPDK bdev，不使用 Blobstore 文件抽象，不读取旧 Blobstore 镜像。
4. 对外只支持 RocksDB FileSystem、FIO ioengine 和新 C++ core；不恢复通用 POSIX 兼容。
5. 旧实现只作为冻结 A/B 和数据导出工具，不能以 compatibility flag 回流到新 core。
6. HFS 2.0 只接受统一的 `.conf` 配置文件；一个 HFS engine 可以独占并管理一个或多个
   NVMe/SPDK bdev，后续新增的 HFS 配置必须继续加入这个文件和版本化 schema。

## 2. 取代范围和冻结策略

### 2.1 与旧架构的关系

本文取代 HFS 1.x 的全部设计、性能、review、shard 和 task 文档；这些第一阶段材料已从
工作区清理，本文不依赖其文件存在。旧实现只允许从 Git 历史追溯，不能作为 HFS 2.0 的
输入或验收依据：

| 文档/内容 | HFS 2.0 处理 |
|---|---|
| 旧 POSIX/preload/fd/FUSE/mmap 产品线 | 完全删除；HFS 2.0 不提供兼容层 |
| 旧 C/Blobstore 实现 | 不直接复用；仅在新类型、所有权和错误模型下重写必要概念 |
| 旧 async/registered-DMA ABI | 删除旧导出符号；仅保留非阻塞语义和资源边界经验 |
| 旧 metadata/image layout | 删除旧格式；只重新设计 checksum、intent 和 recovery |
| 旧数据库兼容线 | 删除旧适配路径；HFS 2.0 仅按本文实现 RocksDB/FIO frontend |

### 2.2 旧 src 的实际处置

本次工作区处置如下：

```text
src/                  → src_legacy_frozen/
Git tracked old src   → 删除
src_legacy_frozen/    → .gitignore，保留本地冻结副本
new src/              → 由 Phase 0/1 创建 C++20 源码
```

`src_legacy_frozen/` 只能用于代码考古、差异阅读和旧线复现；禁止从其中复制旧的
`HfsFile`、fd 表、Blobstore handle、POSIX wrapper 或锁结构进入新实现。若旧算法可复用，
必须先用新的类型、所有权和错误模型重写，并在提交说明中标注来源。

本次转换提交只负责冻结旧源码和确定新架构，不伪造一个尚未实现的 HFS 2.0 build；因此
旧 Make/CMake source list 在 Phase 0 完成前不可视为有效构建入口。Phase 0 必须同时移除
旧的 `.c` 源文件引用、建立新的 `.cpp` target、固定 C++20 编译器/标准库版本，并让空壳
`Engine`/`Client`/`Task` 先通过结构化编译和生命周期测试。

迁移期间保留两条完全隔离的验证线：

```text
HFS 1.x frozen artifact ─ 旧 Blobstore 镜像 ─ 旧 POSIX/MariaDB/RocksDB baseline

HFS 2.0                 ─ 新 raw-bdev 镜像 ─ C++20 core/FIO/RocksDB adapter
```

两条线不能共享 device set 中的任何 bdev。HFS 2.0 不自动升级旧镜像；数据迁移只能是旧线
导出普通文件，再由 `hfsctl import` 按 `.conf` 声明的 device set 写入新镜像，import 工具
不属于 core compatibility path。

## 3. 产品边界

### 3.1 支持

- 单进程、单 engine，独占配置文件中声明的一个或多个 NVMe/SPDK bdev；
- C++20 coroutine-first core API；
- RocksDB 8.9.1 `FileSystem` adapter；
- FIO 3.42 external ioengine；
- 直接 SPDK bdev/NVMe 访问；
- 文件 namespace、目录、rename、delete、extent allocation、sync 和 crash recovery；
- direct buffer 和 bounded bounce buffer 两条数据路径；
- 当前进程内 read-after-write；明确的 `fsync`/`fdatasync` durable boundary。

### 3.2 永久删除

- `LD_PRELOAD`、POSIX fd/dup/fcntl/stdio/cwd/dirfd/permission wrappers；
- FUSE、Linux VFS、通用 mmap；
- native POSIX `read/write/pread/pwrite` 作为 core API；
- SPDK Blobstore、Blobstore cluster allocator 和 Blobstore metadata layout；
- `hfs_lib_async_*`、`hfsAsync*`、registered-DMA、`hfsRegisterBuffer`、`hfsDmaAlloc`；
- `HFS_POSIX_DIRECT_ASYNC`、`HFS_POSIX_QUEUE`、`HFS_POSIX_TICKET` 等旧开关；
- 多进程同时挂载同一 bdev、daemon、shared-memory IPC、跨进程锁和故障转移；
- 为通用 POSIX 重叠写、fd offset 或目录语义保留的 ticket/range compatibility path；
- MySQL/MariaDB/InnoDB native backend：v1 不做，数据库支持范围只有 RocksDB 8.9.1。

fork 保护仍可作为 core 的进程安全措施，但不再模拟 POSIX preload 行为：fork child
调用 core 直接得到 `ForkedProcess` 错误。

## 4. 总体架构

### 4.1 进程内组件

```text
Application process
┌───────────────────────────────────────────────────────────┐
│ RocksDB adapter / FIO engine                              │
│        │                                                  │
│        └── hfs::Engine / hfs::Client                      │
│              ├── metadata actor coroutine                 │
│              ├── data shard actor coroutine × R            │
│              ├── shared DMA arena                          │
│              ├── operation/coroutine frame pools           │
│              └── SPDK environment and channels             │
└───────────────────────────────────────────────────────────┘
                              │
                        SPDK bdev set
                              │
                        NVMe device set
```

没有 hfsd。引入 daemon 将新增 IPC ABI、共享内存生命周期、跨进程 DMA、租约、权限、
取消和崩溃恢复协议，这些不属于本次 clean-break。

### 4.2 C++20 模块布局

```text
include/
  hfs/core.hpp              public C++20 API，opaque ownership boundary
  hfs/task.hpp              coroutine Task/awaitable contract
  hfs/types.hpp             result、status、handle、iovec
  hfs/config.hpp            versioned `.conf` schema and device-set types

src/
  core/engine.cpp           engine lifecycle and process lock
  core/client.cpp           client completion sink and submit
  core/file.cpp             file handle and metadata requests
  core/operation.cpp        operation state and intrusive lifetime
  core/task.cpp             coroutine frame/promise implementation
  config/parser.cpp         `.conf` parser, schema validation and device-set resolution
  runtime/spdk_runtime.cpp  SPDK env and reactor threads
  runtime/reactor.cpp       jthread + spdk_thread_poll actor
  runtime/shard_ring.cpp    bounded MPSC ring and doorbell
  io/io_planner.cpp         extent split、RMW、parent/child
  io/io_operation.cpp       DMA/bdev awaitables
  io/completion.cpp         parent completion and event dispatch
  io/dma_arena.cpp          shared on-demand arena
  metadata/superblock.cpp   geometry、A/B superblock
  metadata/journal.cpp     ring journal and CRC
  metadata/checkpoint.cpp   dual checkpoint and replay
  metadata/namespace.cpp    inode/directory/path index
  metadata/allocator.cpp    extent bitmap and free-run index
  metadata/scheduler.cpp    one inode dependency scheduler
  metadata/durability.cpp  flush、epoch、group commit
  storage/bdev.cpp          raw SPDK bdev wrapper
  common/crc.cpp             CRC32C
  common/log.cpp             logging
  common/trace.cpp           optional trace
  common/stats.cpp           optional counters

adapters/rocksdb/
  hfs_filesystem.cpp
  hfs_random_access_file.cpp
  hfs_writable_file.cpp
  hfs_sequential_file.cpp
  hfs_directory.cpp
  hfs_rocksdb_env.cpp

fio/
  fio_hfs.cpp               FIO C entrypoints implemented by C++20

tools/
  hfsctl.cpp
  hfs_db_bench.cpp
  hfs_ldb.cpp
```

新 `src/` 不编译 `src_legacy_frozen/`。CMake 和 Make 只枚举新 `.cpp` 文件，并开启
`-std=c++20`；不再维护旧 C source list。

## 5. C++20 coroutine 模型

### 5.1 public type

core 对外使用 C++ 类型，不暴露 SPDK、pthread、内部结构和 allocator：

```cpp
namespace hfs {

using FileId = std::uint64_t;
using DeviceId = std::uint32_t;
using RequestId = std::uint64_t;

enum class Status : std::uint8_t {
    Ok, InvalidArgument, NotFound, AlreadyExists, NoSpace, NoMemory,
    Busy, TryAgain, IoError, Cancelled, TimedOut, ForkedProcess,
    Corrupt, NotSupported, InvalidState
};

struct ResultInfo {
    Status status;
    int backend_errno;
    std::size_t bytes;
    std::size_t completed_bytes;
    RequestId request_id;
};

struct IoVec {
    std::span<std::byte> bytes;
};

template <typename T>
class Task;                         // lazy coroutine result

class Engine;
class Client;
class File;
class Operation;

} // namespace hfs
```

`Task<T>` 使用无异常 `Result<T>` 传播错误；core 默认 `-fno-exceptions -fno-rtti`，
adapter 是否启用 RTTI 由 RocksDB 构建决定。C++ ABI 只在锁定的 compiler/libstdc++
构建组合内保证，不能宣称跨发行版 binary compatibility。

### 5.2 Task 和 await point

每个数据 operation 是一个 coroutine frame + intrusive operation state。正常路径的
await 点只有：

```text
co_await ring.reserve(op)       // caller → shard slot; ring 满返回 Busy，不挂起
co_await dma.acquire(class)     // bounded buffer lease（先于 scheduler node）
co_await scheduler.admit(node)  // dependency ready
co_await spdk.submit(request)   // channel-owned async request
co_await parent.complete()      // child aggregation
```

ring 预约在 caller 线程完成且不挂起；预约失败以 `Busy` 结束 operation，不建立无界等待队列。
DMA lease 必须先于 scheduler node 取得，避免持依赖等待 buffer。

示意：

```cpp
Task<ResultInfo> File::read(std::span<IoVec> iov,
                            std::uint64_t offset,
                            std::stop_token stop) {
    auto op = co_await operationPool_.makeRead(*this, iov, offset, stop);
    auto slot = co_await ring_.reserve(op);
    auto buffer = co_await dma_.prepare(iov, op.length(), stop);
    auto dependency = co_await scheduler_.admit(op.range(), OperationKind::Read);
    auto result = co_await reactor_.submitRead(op, dependency, buffer);
    co_return result;
}
```

这是表达模型，不要求每个 awaiter 真的产生一个独立 heap 对象。要求：

- coroutine frame 从 bounded frame pool 或可审计的 custom allocator 取得；
- awaiter 只保存 operation 引用、索引或小型状态，不保存裸指针到短命局部变量；
- SPDK callback 只记录结果并在所属 actor 上 resume coroutine，不直接调用外部 C++；
- `co_await` 不等于阻塞，actor 线程只 poll 和 resume ready coroutine；
- operation 的 caller/actor/parent 引用分别释放，最后一个引用才销毁 frame；
- `Task` 被销毁前必须显式 `detach/await`，禁止遗留未消费 coroutine。

### 5.3 两种 frontend 消费方式

同一 core operation 提供两种完成出口，但同一 operation 只能选一种：

```cpp
// C++ adapter: callback continuation
client.submit(file.read(iov, offset, stop), callback);

// FIO adapter: event queue
auto handle = client.submit(file.read(iov, offset, stop));
client.poll(events, min_events, max_events, timeout);
```

RocksDB `ReadAsync` 使用 callback；FIO `getevents()` 使用 client completion queue。
同步 RocksDB 方法只在 frontend 线程等待 event，不阻塞 SPDK actor，也不把同步等待
包装成 core 的 blocking worker。

## 6. Runtime、reactor 和所有权

### 6.1 Runtime 启停

engine 启动顺序：

1. `Engine` 校验 `.conf`、hugepage 预算、进程锁和完整 device set；
2. owner `std::jthread` 初始化 SPDK env 并打开所有配置的 raw bdev descriptor；
3. 创建一个 metadata reactor 和 `R` 个 data reactor；
4. 每个 reactor 在自己的 `std::jthread` 中创建 `spdk_thread`，循环 `spdk_thread_poll`；
5. 每个 reactor 在本线程为每个 device 获得/销毁自己的 `spdk_io_channel`；
6. metadata actor 装载 superblock/checkpoint/journal；
7. engine mount 完成后才允许 client attach。

调用者线程不调用 `spdk_thread_poll`，不直接调用 bdev submit，不持有 SPDK channel。

### 6.2 Reactor 数量和 CPU

```text
R = 1                         nproc == 1
R = 2                         2 <= nproc <= 8
R = min(4, max(2,nproc/4))    nproc > 8
```

`R` 是 data shard 数，另有一个 metadata actor。默认 metadata CPU0、data CPU1..R，
frontend 不由 HFS 绑定。配置文件可显式指定 `data_reactors` 和 `data_cpus`，但上限
不能超过 `max_data_reactors`。默认不恢复旧的 `HFS_REACTOR_PIN` 环境变量。

`std::jthread` 的 `stop_token` 只用于控制线程停止；停止前必须关闭 ingress、drain
SPDK callback 和持久化 metadata，不能用 stop request 直接释放仍被 callback 使用的对象。

### 6.3 Doorbell 规则

data ring 是固定槽位 MPSC producer / single actor consumer：

- producer 只在短预约区分配 slot 并写入 `Operation*`；
- empty→non-empty 只发一次 doorbell；
- actor 批量快照 head/tail 并消费；
- `spdk_thread_send_msg` 只用于 doorbell、初始化、停止和低频 control；
- 不为每个 data request 发 SPDK message；
- ring 满返回 bounded `TryAgain/Busy`，不创建无界等待队列；
- actor 清空 ring 后再次检查 tail，再清 armed 位，避免 lost wake；
- shutdown 关闭 producer 后等待 ring、credit、operation 和 coroutine frame 全部闭合。

### 6.4 所有权矩阵

| 对象 | owner | 跨线程规则 |
|---|---|---|
| SPDK env/descriptor | engine owner | 只在 control phase 访问 |
| `spdk_io_channel`/bdev submit | reactor actor | 通过 ring 投递 |
| ring head/tail | producer reservation + actor | 不持业务依赖等待 |
| inode scheduler | file 所属 shard actor | 只有 actor 修改 index |
| namespace/journal/checkpoint | metadata actor | 由 metadata request/epoch 交互 |
| DMA arena free map | arena owner | 只传 `BufferLease` |
| coroutine frame/operation | intrusive refs | caller/actor/parent 各持一份 |
| frontend callback | client dispatcher | callback 时不持 core lock |

## 7. Operation、scheduler 和 data path

### 7.1 Operation state

```text
Created → Enqueued → WaitingDependency → Ready → Submitted → Completed
    │          │             │              │
    └──────────┴─────────────┴──────────────┴→ Cancelled/Failed
```

`Operation` 保存 file id、`[offset,end)`、读写 kind、iov 引用、shard、scheduler node、
DMA leases、parent、first error、completed bytes、retry count 和 coroutine continuation。
trace/profile 数据放 sidecar，不能膨胀 trace-off cache footprint。

资源预约顺序固定：

```text
operation frame → ring slot → DMA lease（需要时）→ scheduler node → backend credit → SPDK submit
```

任意资源不足都必须释放已经取得但不再需要的资源；不能持 scheduler node 等 ring、DMA
或 backend credit，不能在 callback 里递归提交。

### 7.2 唯一 inode dependency scheduler

新 core 只维护一个 scheduler，统一表达：

- 非重叠 read/read 并行；
- 重叠 write、RMW、append、truncate 和 size mutation 的依赖；
- EOF gap zeroing barrier；
- `sync`/`datasync` 的 durability epoch；
- extent reserve/commit 的 metadata barrier。

实现使用 intrusive interval tree，节点维护 `max_end`：

- admission 为 `O(log A + K)`，A 是 active operation 数，K 是实际冲突数；
- complete 只检查受影响的后继，不扫描 active 全表；
- scheduler 只由 inode 所属 actor 修改；
- 每个 file 由 `hash(file_id) % R` 固定到一个 data shard，该文件所有数据 operation
  （包括 read）都投递到该 shard，scheduler 只服务单 shard 内的顺序；
- metadata actor 只负责 namespace、extent reserve/commit 和 epoch 发布，不执行数据 I/O；
- 不保留 ticket/range/none 三套模式，不暴露 ordering 环境变量；
- 不新增 `appendLock`、`writeGapLock`、64-stripe lock 等专用锁。

RocksDB 的文件契约已经提供更强约束：SST 关闭后只读，WAL/MANIFEST 通常单 writer，
RandomAccessFile 可并行读。新 scheduler 利用这些约束，不重新实现通用 POSIX 多 writer。

### 7.3 Extent planner

planner 把一次逻辑 I/O 转成 parent 和有限 child：

```text
logical request
  ├─ aligned contiguous child
  ├─ head RMW child (only when needed)
  ├─ middle direct child
  └─ tail RMW child (only when needed)
```

连续同属性 extent 合并；最大 child 默认 1 MiB。数据路径不解析路径、不进入 metadata
actor；extent map 由 metadata actor 维护，data shard 持有 immutable/versioned snapshot，
metadata epoch 推进后整体发布新 snapshot。v1 不做 extent map 换出/回填（见 §13.3）。

扩展写规则：

1. `end <= logical_size` 直接 admission；
2. `end > logical_size` 生成同一 inode 的 size barrier，重新确认 EOF；
3. `offset > old EOF` 先将 gap 物化为零或明确 zero extent，再写用户数据；
4. 数据和 extent state 完成后更新 logical epoch；
5. sync 时才把 epoch 推进为 durable epoch。

### 7.4 Parent completion

parent 保存 pending child count、first error、completed bytes、child bitmap 和引用：

- 未提交 child 的 acquire/start 失败必须补偿 pending；
- child callback 只更新 parent，不唤醒多个 frontend waiter；
- 最后一个 child 通过 release store 发布 parent，并 wake-all；
- read 统一 copy-out 后释放 lease，write 统一 release；
- 一个 frontend operation 只能有一个 completion sink；
- timeout/cancel 不提前销毁 SPDK callback 仍可见的 frame。

## 8. DMA 和资源上限

### 8.1 Buffer lease

`BufferLease` 表示 caller buffer 或 shared arena slice：

```cpp
class BufferLease {
public:
    BufferLease(BufferLease&&) noexcept;
    BufferLease& operator=(BufferLease&&) noexcept;
    ~BufferLease();
    std::span<std::byte> span() const noexcept;
};
```

通过 `spdk_vtophys`、对齐和长度检查可以 direct 的 caller buffer 不复制；其它 buffer
使用 arena bounce。没有 registered-DMA public API。FIO/RocksDB 的大 buffer 按 ≤1 MiB
child 分段，不要求整块 `orig_buffer` 可 DMA。

### 8.2 Shared arena

mount-level shared arena 按需增长，默认 class：

```text
4 KiB   metadata edge / small I/O
16 KiB  common database I/O
64 KiB  large database I/O
1 MiB   extent child / fallback staging
```

默认 `dma_arena_mb=512`，所有 class、operation frame、ring 和 SPDK baseline 都计入
hugepage 预算。arena 达到 cap 返回 `TryAgain`/`Busy`；不在热路径执行无界 `new`，不把
根盘或 swap 当作 fallback。central bitmap 是第一实现；只有 profile 证明需要时才加
bounded local magazine。

### 8.3 Hugepage preflight

```text
required = ceil((SPDK baseline + DMA cap + frame pool + ring + metadata reserve)
                × 1.15 / hugepage_size)
```

启动时比较 required 与 available，失败时报告两者并拒绝 mount。正式 VM 仍按项目口径
使用 8 vCPU、`vm.nr_hugepages=1024`、真实 NVMe `Nvme0n1` 的单盘 baseline；多 NVMe
配置的性能结果必须额外报告设备数量、设备角色和 placement policy。MALLOC/AIO 只能用于
确定性故障注入，不替代性能结论。

## 9. Raw-bdev 磁盘格式

### 9.1 Clean-break 镜像

HFS 2.0 完整拥有配置文件中声明的全部 raw bdev；每个设备都拥有自己的物理 geometry
和 allocator，多个设备共同组成一个逻辑 HFS volume：

```text
Superblock A/B       8 MiB reserved
Journal ring         default 256 MiB
Checkpoint A/B       configurable metadata region
                     （namespace/inode/extent map/allocator bitmap 快照）
Data extents         remaining capacity
```

旧 Blobstore magic、旧 metadata header 和 HFS 1.x image 都拒绝 mount。`hfsctl format`
是破坏性命令，必须显式 `--yes` 和空设备/确认检查；不做在线转换。

### 9.2 Superblock 和 geometry

```cpp
struct Hfs2Superblock {
    std::uint64_t magic;
    std::uint32_t format_version;
    std::uint32_t feature_flags;
    std::uint32_t device_id;
    std::array<std::byte, 16> uuid;
    std::uint64_t generation;
    std::uint64_t device_blocks;
    std::uint32_t sector_size;
    std::uint32_t metadata_block_size;
    std::uint64_t journal_start;
    std::uint64_t journal_blocks;
    std::uint64_t checkpoint_a_start;
    std::uint64_t checkpoint_b_start;
    std::uint64_t checkpoint_blocks;
    std::uint64_t data_start;
    std::uint64_t data_blocks;
    std::uint64_t checkpoint_generation;
    std::uint64_t journal_head;
    std::uint64_t journal_tail;
    std::uint64_t mount_epoch;
    std::uint8_t clean_shutdown;
    std::uint32_t crc32c;
};
```

实际 layout 由 geometry 计算并写入 superblock；`checkpoint_a/b` 是 checkpoint region
内的两个槽位，不与 metadata region 起点混用。A/B 写入顺序为：写非当前副本 → 对相关
device flush → 发布 generation。设计不假设多 sector 原子写；每个 record 都有长度、序号、
CRC 和 commit marker。

### 9.3 Journal 和 checkpoint

journal 是固定 block 对齐 ring：

```text
BEGIN(txid, generation, record_count, crc)
RECORD...
COMMIT(txid, generation, tx_crc)
```

只有完整 BEGIN/records/COMMIT 且 CRC 正确才 replay。record 类型至少包括：
`CREATE`、`LINK`、`UNLINK`、`RENAME`、`ALLOC`、`RESERVE`、`FREE`、`SET_SIZE`、
`SET_TIMES`、`TRUNCATE` 和 `CHECKPOINT`。

- 50% 使用率触发 checkpoint，80% 使用率优先同步 checkpoint；
- journal 空间不足时先 checkpoint；checkpoint 失败则在 bounded timeout 后返回 NoSpace/Busy；
- checkpoint 写 inactive slot 的完整 snapshot（namespace/inode/extent map/allocator
  bitmap），flush 后更新 superblock；
- superblock durable 后才推进 journal tail；
- 撕裂/未知/超长 record 丢弃并记录 corruption evidence，不静默 replay。

### 9.4 Extent 和 allocator

第一版每个 data device 固定 1 MiB extent，全部 device 的 allocator 由 metadata actor 独占：

```text
FREE → RESERVED → INITIALIZED → COMMITTED
```

- two-level bitmap + free-run index；next-fit 分配，相邻 free run 合并；
- allocator bitmap 不原地更新，只作为 checkpoint snapshot 的一部分写入 inactive slot；
  free-run index 在 mount 时由 bitmap 重建，checkpoint 之后的分配状态由 journal replay 恢复；
- 单次预分配最多 64 extent（64 MiB），更多空间拆成多个 transaction；
- 没有足够 run 时可分段，否则返回 NoSpace；
- RESERVED 在 replay 时回收；非 COMMITTED extent 不可见；
- new extent 暴露前必须被初始化为零或完成用户数据 durable；
- 释放可异步回收，但不能持 frontend scheduler 等 allocator；
- TRIM/unmap 不是 v1 必需接口，不能用“设备 unmap 读零”替代逻辑零语义。

### 9.5 Multi-NVMe volume

配置文件中的 `data_devices` 构成一个逻辑 namespace 的数据池。每个 extent descriptor
必须保存 `device_id`、起始 block、block 数量和 extent state；一个逻辑 I/O 可以拆成多个
device-local child，但 child 不能跨设备提交。默认 placement 为 `capacity_weighted`，
也可配置为 `round_robin`；v1 不实现 RAID、纠删码或隐式复制。

`metadata_devices` 保存同一个 volume UUID 的 superblock、journal 和 checkpoint。配置
中的 metadata device 全部必须可打开，metadata transaction 只有在所有 required metadata
device 完成 flush 后才发布 generation；v1 不支持降级挂载。`data_devices` 可以包含
metadata device，也可以只承担数据；所有声明的 device 都必须由同一个 Engine 独占，不能
被另一 HFS 实例重复配置。

journal 在每个 metadata device 上镜像相同的 transaction stream；事务的 commit point
是全部副本。recovery 以所有 metadata device 的最小已提交 txid 为回放终点，超出该点的
tail 丢弃（roll back），不做单盘前滚；journal 损坏、CRC 不一致或设备缺失时 fail closed。

`hfsctl format`、`check`、`dump` 和 `import` 都以同一个 `.conf` 为入口，并对 device set
逐盘执行 geometry/UUID/CRC 检查。设备缺失、重复 bdev、sector/metadata geometry 不兼容
或 volume UUID 不一致时拒绝 mount；设备容量可以不同，但必须满足各自 layout 的最小容量。
设备故障返回可定位的 `IoError`，不得静默把数据迁移到未声明的设备。

## 10. Durability 和故障恢复

### 10.1 文件状态

每个 inode 保存 `logical_size`、`durable_size`、extent map、mtime/ctime、link count 和
当前 metadata epoch。logical update 对当前进程立即可见；durable epoch 只在完整 flush
和 metadata commit 后推进。crash 后不把未 durable 的 logical tail 伪装成已提交文件。

### 10.2 sync 顺序

`datasync`：

```text
1. 记录目标 data epoch，阻止新的相关 barrier 越过它
2. drain file data operations
3. device flush，保证 user data 和 extent initialization
4. 写 SET_SIZE/extent-state metadata transaction
5. journal flush
6. checkpoint/superblock flush（需要时）
7. 发布 durable epoch，唤醒 waiter
```

`sync` 还包括文件时间戳、namespace 和 directory metadata。不可改变的顺序是：

```text
data submit → data drain → data device flush
  → metadata intent/commit → journal flush
  → checkpoint/superblock flush → publish durable epoch
```

这明确避免“metadata transaction 写完却没有最终设备屏障”的旧问题，也保证 metadata
不会先指向未 durable 的用户数据。

### 10.3 Crash replay

- committed transaction replay；不完整 transaction 丢弃；
- 内存 after-image 只有在 commit durable 后才 publish；失败时恢复旧 snapshot；
- RESERVED extent 回收，unlinked-but-open file 的 extent 进入 orphan reclaim；
- 已写但未被 durable metadata 指向的数据不可见并可回收；
- checksum、generation、namespace、extent bitmap 和 file size 必须相互守恒；
- mount 发现不可修复 corruption 返回 Corrupt，保留现场给 `hfsctl check`，不强行 mount。

## 11. RocksDB 8.9.1 adapter

### 11.1 接入原则

adapter 是独立 C++20 shared library，core 与 RocksDB 只通过 C++ header contract 编译
连接；core 不包含 RocksDB header。由于 C++ ABI 依赖编译器，adapter 与 core 必须使用
锁定的 RocksDB 8.9.1、compiler 和 libstdc++ 构建组合。

Debian stock `db_bench` 不提供 CLI 注入 `DBOptions::env`，所以验收使用：

```cpp
// RocksDB 8.9.1 的 DBOptions::env 是裸 Env*，RocksDB 不接管所有权；
// env 必须由调用方持有到 DB 关闭。
auto env = hfs::rocksdb::NewHfsEnv(hfs_engine);
Options options;
options.env = env.get();
DB* db = nullptr;
Status s = DB::Open(options, db_path, &db);
```

实际工具为自编译 `hfs_db_bench`；`hfs_ldb` 同样显式设置 HFS Env。不能把 stock 工具
的输出写成 HFS 已接管。

### 11.2 8.9.1 必需接口

严格以 VM `/usr/include/rocksdb/file_system.h` 和 `env.h` 的 8.9.1 头文件为准：

| 8.9.1 契约 | HFS 2.0 实现 |
|---|---|
| `FSSupportedOps::kAsyncIO` | 声明并实现 |
| `FSSupportedOps::kFSBuffer` | v1 不声明；它只适用于 non-direct MultiRead |
| `kFSPrefetch` | 不存在，禁止声明 |
| `FSRandomAccessFile::ReadAsync(FSReadRequest&, ..., void** io_handle, IOHandleDeleter*, IODebugContext*)` | 按完整真实签名实现 |
| `FileSystem::AbortIO()` | 必须实现，遵守 core cancel boundary |
| `GetFileModificationTime` | 返回 inode mtime |
| `FSDirectory::Fsync/FsyncWithDirOptions` | 提交 directory metadata epoch |
| `LockFile/UnlockFile` | 进程级 RocksDB lock lease |
| `LinkFile` | link-count transaction；若版本/场景不支持则明确返回 NotSupported |
| `FSSequentialFile::Skip/InvalidateCache` | 必须实现；invalidate 可为正确的 no-op |
| `FSWritableFile::PrepareWrite/Allocate` | direct I/O 下映射 extent preallocation |
| `PositionedAppend` | WAL/MANIFEST 单 writer 顺序 |

v1 选择 `use_direct_io() == true`，因此 RocksDB preallocation hint 生效；adapter 必须
实现对齐检查、bounce fallback、`PrepareWrite`、`Allocate`、`PositionedAppend`、
`RangeSync`、`Sync/FSync`、`Close` 和目录 fsync。ReadAsync 的 `FSReadRequest`、iov、
`io_handle` 必须活到 callback 返回；之后才调用 `IOHandleDeleter`。v1 不声明 `kFSBuffer`，
不把 HFS DMA lease 借给 `fs_scratch`。

### 11.3 文件类型

```text
SST/table       → RandomAccessFile，可并发 Read/ReadAsync
WAL             → WritableFile，单 writer，PositionedAppend + Sync
MANIFEST        → append/rename，metadata epoch
CURRENT/OPTIONS → write/close/rename
LOCK            → LockFile/UnlockFile lease
```

不实现通用 POSIX 多 writer；利用 RocksDB 自身的 file role 和访问约束降低 scheduler 复杂度。

## 12. FIO 3.42 adapter

### 12.1 加载方式和 options

FIO engine 编译为 C++20 shared library，但只在 FIO 注册入口使用 `extern "C"`：

```text
fio --ioengine=external:/absolute/path/fio-hfs.so job.fio
```

`ioengine_ops.options` 必须登记自定义 options，并设置 `option_struct_size`：

```text
hfs_config=/path/hfs2.conf
hfs_direct=1
hfs_preallocate=1
hfs_client_depth=32
```

engine 直接调用 C++ `Engine/Client/File`，不通过 POSIX open/read/write。`open_file`
负责 create/open/allocate，并设置 `FIO_NOEXTEND` 或完成等价的 engine-owned extend，
避免 `setup_files()` 用 POSIX ftruncate 绕过 HFS allocator。

### 12.2 FIO lifecycle

- `queue()` 产生 coroutine operation，保存 `io_u` 引用直到 completion；
- `iomem_alloc/iomem_free` 从 shared arena 提供整块 DMA `orig_buffer`；arena 无法容纳时
  回退到 ≤1 MiB child bounce，并把 copy 计入统计；
- bounded ring/operation/DMA 满时返回 `FIO_Q_BUSY`，不丢请求；
- `getevents(min,max,timeout)` 调用 `client.poll`，满足 FIO min/max 语义；
- `event()` 写回 `io_u->error`、`resid` 和完成字节；
- `FIO_NOEXTEND`、`fio --verify`、direct/bounce、iodepth 必须在测试中覆盖；
- engine close 前 drain 全部 io_u、coroutine frame、File 和 Client。

## 13. Namespace 和文件 API

### 13.1 C++ API 草案

```cpp
namespace hfs {

class Engine {
public:
    static Result<std::unique_ptr<Engine>> create(const Config&);
    Task<Result<void>> format(FormatOptions);
    Task<Result<void>> mount();
    Task<Result<void>> drain();
    Task<Result<void>> stop();
};

class Client {
public:
    static Result<std::shared_ptr<Client>> create(Engine& engine,
                                                  const ClientOptions&);
    Task<Result<void>> close();
    Task<Result<std::shared_ptr<File>>> open(std::string_view path,
                                             OpenFlags flags);
    Task<Result<void>> remove(std::string_view path);
    Task<Result<void>> rename(std::string_view from, std::string_view to);
    PollResult poll(std::span<CompletionEvent>, std::size_t min,
                    std::size_t max, std::chrono::microseconds timeout);
};

class File {
public:
    Task<Result<ReadResult>> read(std::span<IoVec>, std::uint64_t offset,
                                  std::stop_token = {});
    Task<Result<WriteResult>> write(std::span<const IoVec>, std::uint64_t offset,
                                    std::stop_token = {});
    Task<Result<WriteResult>> append(std::span<const IoVec>,
                                     std::stop_token = {});
    Task<Result<void>> allocate(std::uint64_t offset, std::uint64_t length);
    Task<Result<void>> truncate(std::uint64_t size);
    Task<Result<void>> datasync();
    Task<Result<void>> sync();
};

} // namespace hfs
```

`std::shared_ptr<File>` 只用于 frontend 的长寿命 handle，不用于每个 child；data path
使用 intrusive `OperationRef`、pool 和 `BufferLease`。路径 API 只进 metadata actor，
文件数据使用 file id/handle。

### 13.2 Namespace 规则

- inode id 单调分配，handle 带 generation，旧 handle 不能解析到新 inode；
- directory entry 由 name hash 查找、ordered cursor 遍历；
- rename 是单一 journal transaction，源/目标不产生中间可见状态；
- delete 先摘 namespace，再延迟回收 extent；打开的 File 可完成已有 operation；
- directory fsync 提交 directory metadata epoch；
- `max_inodes`、`max_namespace_bytes` 超限返回 NoMemory，不退回磁盘全表扫描；
- 大目录必须按 shard/hash 维护，`GetChildren` 不扫描全部 namespace。

### 13.3 内存元数据模型

v1 采用常驻内存元数据，不做分页或换出：

- mount 时从 latest checkpoint + journal replay 重建全部活跃元数据：inode 表、
  namespace/directory index、per-inode extent map、各 device 的 allocator bitmap
  和 free-run index；
- extent map 每 inode 一个 versioned vector，由 metadata actor 串行修改并递增 epoch；
  data shard 只读 immutable snapshot，不持锁查找；
- 内存预算由 `max_inodes`、`max_namespace_bytes` 和每 inode ≤1 KiB 目标约束，
  超限返回 NoMemory，不退回磁盘全表扫描；
- 大文件 extent 数受 `max_extents_per_file` 限制，超限返回 NoSpace；
- checkpoint snapshot 是各 device extent map/allocator 的唯一磁盘权威副本，journal
  记录增量。

## 14. 错误、取消和关闭

### 14.1 错误模型

所有 SPDK callback 都传播原始 backend errno；parent 聚合 first error 和 completed bytes，
但等待其它 child drain。常见 frontend 映射：

| Core status | RocksDB | FIO |
|---|---|---|
| `NoSpace` | `IOStatus::NoSpace` | `ENOSPC` |
| `TryAgain/Busy` | `IOStatus::Busy()`（可 `SetRetryable(true)`） | `FIO_Q_BUSY`（submit） |
| `NoMemory` | `IOStatus::IOError()` | `ENOMEM` |
| `IoError` | IO error + backend errno | backend errno 或 `EIO` |
| `Cancelled` | Aborted | `ECANCELED` |
| `TimedOut` | TimedOut | `ETIMEDOUT` |
| `Corrupt` | Corruption，终止 engine | `EIO` |

所有 waiter 都 wake；错误不能只唤醒一个等待者，也不能在 parent 处丢掉 bserrno。
RocksDB 8.9.1 没有 `IOStatus::TryAgain`；`ReadAsync` 对 `Busy` 的处置（回退同步读或上层重试）
必须在 Phase 5 用 exact-version 测试确认。

### 14.2 Retry/cancel

- only transient `TryAgain`/backend `-ENOMEM` retry；默认最多 3 次；
- retry 不持 scheduler、DMA 或 metadata lock；
- ring、frame、arena 满时立即 bounded busy；
- submit 前 cancel 从 scheduler 删除 operation；
- submit 后 cancel 只标记状态，等待 SPDK callback 完成，不能伪造 backend abort；
- timeout 只结束 frontend 等待，不释放 callback 仍需要的 coroutine frame；
- coroutine cancellation 由 `std::stop_token` 传播到 awaiter，必须经过统一 state machine。

### 14.3 Teardown/fork/multiprocess

```text
reject new attach/submit
  → close producer rings
  → cancel/remove pending scheduler nodes
  → drain backend credit and SPDK callbacks
  → flush journal/checkpoint/superblock
  → write clean_shutdown
  → destroy channels in owning jthread
  → join reactor threads
  → close bdev and process lock
```

活跃 client/operation 未 drain 时 `stop()` 返回 Busy，除非调用显式 drain API。SIGKILL
不保证 clean flag，下次 mount 必须 recovery。第二进程打开任一已配置 bdev 或相同 volume
返回 Busy；fork child 所有 core API 返回 ForkedProcess。

## 15. 配置、ABI 和构建

### 15.1 `.conf` 配置（唯一入口）

HFS 2.0 使用版本化的 INI-like `.conf` 格式，不再使用 HFS JSON 配置或散落的
`HFS_*` 环境变量。engine、SPDK、device set、metadata、reactor、DMA、限流、恢复和
frontend 的所有 HFS 配置都必须写入同一个文件；命令行只负责传入配置文件路径，例如
`hfsctl --config=/path/hfs2.conf`。未知 key、重复 device 或 schema 不兼容必须拒绝启动，
不能静默忽略。

仓库当前已有的 `config/*.json` 文件属于冻结的 HFS 1.x 配置，不是 HFS 2.0 的兼容输入；
Phase 0 创建 `.conf` parser 和新 target 后，正式命令只能接受 `.conf`。

```conf
[hfs]
schema_version = 1
instance_name = hfs2
volume_uuid = auto
process_lock = exclusive

[spdk]
env_driver = uio
hugepages = 1024

[storage]
metadata_devices = nvme0,nvme1
data_devices = nvme0,nvme1
placement = capacity_weighted
metadata_replication = 2
require_all_devices = true
metadata_block_size = 4096
journal_mb = 256
checkpoint_mb = 512
extent_mb = 1
prealloc_extents = 64
max_extents_per_file = 65536

[device.nvme0]
bdev_name = Nvme0n1
pci_bdf = 0000:0b:00.0
role = metadata,data
weight = 1
required = true

[device.nvme1]
bdev_name = Nvme1n1
pci_bdf = 0000:0c:00.0
role = metadata,data
weight = 1
required = true

[runtime]
data_reactors = 0
max_data_reactors = 4
metadata_cpu = 0
data_cpus = auto
ring_depth = 1024
client_depth = 32

[limits]
dma_arena_mb = 512
operation_pool = 4096
coroutine_frame_pool = 4096
metadata_wait_timeout_ms = 5000
backend_retry_limit = 3

[metadata]
max_inodes = 1000000
max_namespace_bytes = 1073741824
checkpoint_trigger_percent = 50
checkpoint_force_percent = 80

[observability]
trace = false
stats = false
```

`metadata_devices` 和 `data_devices` 使用 `[device.<id>]` 中的逻辑 ID；设备可以只承担
data，也可以同时承担 metadata。只配置一个 NVMe 时仍使用完全相同的 schema。正式单盘
baseline 的配置文件为 `config/hfs_nvme.conf`，设备为 `Nvme0n1`、PCI BDF
`0000:0b:00.0`；多盘测试必须列出实际 device set，不能通过临时环境变量追加设备。
engine 用 `[device.*] pci_bdf` 生成 SPDK/DPDK allowlist 并写入 `spdk_env_opts`；`[spdk]`
只描述 env 驱动和 hugepage 预算，不通过环境变量注入。

后续 HFS 配置统一按 section namespace 添加到该 `.conf` 并提升 `schema_version`，例如
`[recovery]`、`[cache]`、`[frontend.rocksdb]` 和 `[frontend.fio]`；不得另建第二个配置
文件来绕过 schema 校验。`data_reactors=0` 使用 nproc 规则，`data_cpus` 必须匹配 R。

### 15.2 C++ ABI

```text
libhfs_core.so       C++20 core
libhfs_rocksdb.so    RocksDB 8.9.1 adapter
fio-hfs.so           FIO external engine
hfsctl               format/check/dump/import
```

- public headers 只暴露 C++20 standard-library types 和 HFS types，不暴露 SPDK；
- `HFS_CORE_ABI_MAJOR=2`，构建包记录 compiler、libstdc++ 和 RocksDB 版本；
- 默认 visibility hidden，仅 public header 标记 `HFS_API` 的符号导出；version script
  导出 `hfs::*` 公共 C++ 符号和 `extern "C"` 工厂/版本符号，保证
  `libhfs_rocksdb.so`/`fio-hfs.so` 在同一 ABI 组合内可链接；
- 不承诺跨 toolchain 的 C++ binary compatibility；若未来需要其它语言，再新增薄的
  C ABI，不把它倒灌为 core 主 API；
- core `-std=c++20 -fno-exceptions -fno-rtti`；adapter/fio 按各自依赖单独编译；
- Make/CMake 必须都能从新 `src/` 构建，旧 `src_legacy_frozen/` 不在 source list；
- `hfs_db_bench`/`hfs_ldb` 只在 RocksDB 8.9.1 开发头可用时构建，core target 不包含
  RocksDB header。

### 15.3 代码处置清单

| 旧内容 | 处理 |
|---|---|
| `src_legacy_frozen/` | 本地冻结、Git ignore、只读参考 |
| `hfs_op`、actor ring、parent completion | 提取概念，C++20 重新实现 |
| inode scheduler interval algorithm | 提取算法，合并为唯一 C++ scheduler |
| shared DMA arena、dirty epoch、durability order | 提取设计，接到新 raw-bdev owner model |
| stats/trace/log/CRC | 复用经过验证的观测思想，重写 C++接口 |
| `hfs_runtime.c`、`hfs_io.c`、`hfs_meta*.c` | 不直接复用，按新 owner/extent/journal 重写 |
| `hfs_preload.c`、`hfs_fd.*`、`hfs_lib.*` | 删除 |
| `hfs_ticket.*`、range lock、POSIX lock/gate | 删除；只提取必要 interval 算法 |
| Blobstore glue、blob handle、旧 metadata layout | 删除 |
| old preload map/env/async ABI | 删除 |

## 16. 测试与验收

### 16.1 结构门禁

- trace/stats off 不执行 per-request unbounded allocation；
- coroutine frame、OperationRef、BufferLease、ring slot、parent/child 全部闭合；
- 没有持 scheduler 等 DMA/queue credit；
- callback 不持 frontend/core lock，不直接调用外部 C++；
- `spdk_submit == spdk_complete`；backend errno 可见；NVMe 可释放；
- shutdown、cancel、timeout、fork child 不产生 UAF、double resume 或 orphan frame。

### 16.2 FIO 功能和性能

功能矩阵：

```text
iodepth: 1, 8, 32, 64, 128
numjobs: 1, 2, 4, 8
rw:      read, write, randread, randwrite, randrw
bs:      4K, 16K, 64K, 1M
```

正式性能固定：8 vCPU、真实 NVMe BDF `0000:0b:00.0`、`Nvme0n1`、O_DIRECT、256 MiB、
shared-file、randrw 50/50、4K/8K/16K/32K/64K、32 jobs、10 秒、5-run median、
trace/stats off。raw 对照使用同一设备的 SPDK bdevperf 或官方 fio SPDK plugin，固定
相同 bs/iodepth/jobs。报告 HFS/raw、read/write p50/p99、CPU%、RSS、errors、
submit/complete、journal bytes/op。

多 NVMe 验收至少覆盖单盘、双盘和配置中全部 data device 三种 device set；每次记录
`metadata_devices`、`data_devices`、placement、每盘容量和每盘 submit/complete。任一
配置设备不可用、未被正确独占或数据落到了未声明设备时，功能验收失败。

主 gate：4K shared-file randrw 的 HFS/raw ≥85%，CPU ≤500%（8 vCPU 口径），errors=0，
submit/complete 闭合，HFS p99 不高于 raw 基线的 110%。如果结构/恢复门禁失败，即使
IOPS 更高也不接受。

### 16.3 RocksDB 验收

由 custom driver 注入 HFS Env：

```text
hfs_db_bench --hfs-config=config/hfs_nvme.conf \
  --db=/hfs2/rocksdb \
  --benchmarks=fillseq,readrandom,overwrite,readwhilewriting \
  --num=20000 --threads=8 --use_direct_io_for_flush_and_compaction=1

hfs_ldb --hfs-config=config/hfs_nvme.conf \
  checkconsistency --db=/hfs2/rocksdb
```

必须覆盖 create/write/close/reopen、SST read、WAL sync、flush/compact、rename/delete、
normal restart、SIGKILL recovery、跨进程顺序 reopen 和 checkconsistency。Stock db_bench
没有注入 HFS Env 时，其结果无效。

### 16.4 Crash 和参考模型

至少在以下位置故障注入：

1. data child 完成但 device flush 前；
2. data flush 完成但 metadata commit 前；
3. journal COMMIT 写入但 journal flush 前；
4. checkpoint inactive slot flush 后、superblock publish 前；
5. clean flag 写入前。

每个断点检查 namespace、extent bitmap/state、logical/durable size、orphan reclaim、
generation、CRC 和 RocksDB reopen。ext4 上运行同一逻辑 workload 对照语义，不把 ext4
性能当作 HFS 目标。

## 17. 非功能目标、风险和停止条件

### 17.1 非功能目标

| 项目 | 目标 |
|---|---|
| mount | 1,000,000 active inode、256 MiB journal 的 8-vCPU VM，除 SPDK 初始化外 ≤10 s；replay 追加 ≤2 s |
| metadata actor | create/rename/set-size/commit microbenchmark ≥20,000 ops/s，报告 journal bytes/op |
| metadata memory | inode/namespace/extent index 默认 ≤1 KiB/active inode，不含实际 name bytes 和 DMA arena |
| data latency | 4K 主口径 p50 ≤0.5 ms、p99 ≤1.5 ms；64K p99 ≤3 ms |
| resource | DMA arena ≤512 MiB，frame/ring/retry/metadata 均有界，无 swap、无泄漏 |
| recovery | 正常重启、SIGKILL 和所有 crash 断点在 mount budget 内恢复，状态守恒 |

checkpoint region 必须容纳 namespace/inode/extent map/各 device allocator bitmap 的
序列化快照；1,000,000 inode 目标下 `checkpoint_mb` 默认值需在 Phase 1 验证，容量不足
时 mount 必须报错而不是截断。

### 17.2 风险与缓解

| 风险 | 缓解 |
|---|---|
| metadata actor 瓶颈 | epoch/group commit、批量 extent transaction，单独报告 metadata ops/s |
| coroutine frame 膨胀 | frame pool、await state size audit、trace-off allocation test |
| C++ ABI 漂移 | 锁定 compiler/libstdc++/RocksDB，启动版本检查 |
| direct IO 对齐差异 | exact RocksDB 8.9.1 compile test、bounded bounce fallback |
| hugepage 不足 | preflight、DMA cap、分段 FIO buffer，不隐式扩大 arena |
| queue/backpressure | 有界 ring、FIO busy、RocksDB Busy、有限 retry |
| recovery 错误 | A/B、CRC、commit marker、crash matrix、hfsctl check |
| 旧功能回流 | 新库名、新 magic、新 source tree，不编译 frozen src |

### 17.3 立即停止条件

任何以下情况都停止性能测试并回到结构/正确性修复：

- data mismatch、sparse read 非零、RMW 丢数据；
- metadata corruption、generation/CRC 不一致、journal replay 错误；
- parent/child、coroutine frame、DMA lease、ring slot 泄漏或 double resume；
- `spdk_submit != spdk_complete`、backend errno 丢失、NVMe 无法释放；
- RocksDB/FIO error mapping、cancel、AbortIO、directory fsync 不完整；
- 旧 Blobstore、POSIX fd 或 preload 代码重新进入 HFS 2.0 build。

## 18. 实施顺序

### Phase 0：新工程和 ABI

- `src/` 使用 C++20 新目录结构；
- 建立 `Task/Result/Engine/Client/File` 和 versioned build；
- 建立严格的 `.conf` parser、schema version 和多-device set resolver；
- Make/CMake 删除旧 C source list，建立 C++20 target 和编译标志；
- 实现 process lock、geometry preflight 和空 engine。

验收：新 core 能独立编译；不链接 Blobstore/preload/fd；旧 artifact 仍可单独复现。

### Phase 1：raw bdev 和格式

- SPDK env/reactor/channel owner；
- 打开、校验并逐盘 format 配置文件声明的 NVMe device set；
- format、A/B superblock、geometry、journal/checkpoint；
- `hfsctl format/check/dump/import`；
- torn record、journal full、旧 magic rejection。

### Phase 2：coroutine data plane

- `Task` frame pool、OperationRef、MPSC shard ring、actor doorbell；
- inode interval scheduler、extent planner、parent completion；
- shared DMA arena、direct/bounce、read/write/append；
- capacity-weighted/round-robin placement 和跨 device parent/child completion；
- C++ unit/ASAN/TSAN 结构测试。

### Phase 3：namespace、allocator、durability

- inode/directory/name index；
- 1 MiB extent allocator、RESERVED/INITIALIZED/COMMITTED；
- logical/durable epoch、datasync/sync、crash replay；
- metadata replica flush、设备故障 fail-closed 和多盘 recovery；
- fault injection 和 ext4 reference model。

### Phase 4：FIO frontend

- FIO options、setup_files/FIO_NOEXTEND、queue/getevents/event；
- verify、direct/bounce、iodepth/numjobs 功能矩阵；
- raw SPDK comparator 和 5-run performance gate。

### Phase 5：RocksDB frontend

- 8.9.1 FileSystem exact contract；
- ReadAsync/AbortIO、direct IO、preallocation、PositionedAppend；
- custom `hfs_db_bench`/`hfs_ldb`、WAL/flush/compact/recovery。

### Phase 6：收口和归档

- 所有 gate 通过后删除新树中最后的旧兼容残留；
- `src_legacy_frozen/` 保持本地忽略或单独归档，不回到 product build；
- 更新架构索引、性能基线、支持边界和迁移说明；
- HFS 2.0 才能作为正式产品线，旧 HFS 1.x 只保留归档 artifact。

## 19. 最终定位

> HFS 2.0 是一个 C++20 coroutine-first、运行在应用进程内、直接管理 SPDK bdev/NVMe
> 的数据库文件存储引擎。它以 per-shard actor、单一 inode dependency scheduler、
> bounded DMA arena、extent allocator 和可恢复 raw-bdev metadata 为核心，为 RocksDB
> 8.9.1 和 FIO 3.42 提供明确的异步文件契约。它不是通用 POSIX 文件系统，不使用
> Blobstore，也不兼容 HFS 1.x 的 src、镜像、fd ABI 或 preload 路径。

HFS 2.0 的第一条实现原则是：旧代码可以冻结，但不能以兼容性名义继续污染新架构；
第二条原则是：coroutine 只用于表达非阻塞状态机，所有资源、错误、恢复和完成边界都
必须可计数、可回放、可验证。
