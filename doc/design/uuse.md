> 2026-10-04：项目由 HFS 更名为 UUSE（unified user-space storage engine）；本文更名后仍是当前唯一事实源，历史评审/性能文档保留 HFS 时期的名称与证据路径。

# UUSE 2.0：C++20 coroutine-first 的 SPDK-native 存储引擎

> 状态：clean-break 重构设计，2026-09-16 生效
>
> 评审输入：`doc/review/hfs2.0_design_review_20260916.md`
>
> 当前事实基线：clean-break 迁移后的工作区。本文描述的是新目标，不表示新 core 已实现。
>
> 2026-09-24：新增 Phase 7（MySQL 8.0.46 InnoDB frontend，§20）；只做加法，不改变
> §11/§12 的 RocksDB/FIO 契约、默认值和正式性能口径。审计依据
> `doc/review/hfs2_mysql_innodb_audit_20260924.md`，执行计划
> `doc/shard/hfs2.0_mysql_innodb_shard_plan.md`。

## 1. 设计决策

UUSE 2.0 不再是当前 UUSE 的 C/Blobstore/POSIX 增量版本，而是从空的 `src/` 开始的
全新 C++20 存储引擎：

```text
RocksDB 8.9.1 FileSystem adapter ──┐
FIO 3.42 external ioengine ────────┼─ libuuse_core.so ─ SPDK bdev set ─ NVMe pool
MySQL 8.0.46 InnoDB os_file seam ──┘   （Phase 7，§20）
```

核心运行模型：

```text
Frontend
  → uuse::Client
  → uuse::Task / uuse::Operation
  → bounded MPSC shard ring
  → shard coroutine actor
  → one inode dependency scheduler
  → raw-bdev extent planner
  → SPDK channel / NVMe device set
  → completion event
  → frontend callback or poll
```

本次重构的硬性结论：

1. 旧 `src/` 已整体改名为 `src_legacy_frozen/`，作为本地只读参考，不进入 UUSE 2.0
   构建，也不再由 Git 跟踪；旧代码从 Git 中删除。
2. 新 `src/` 不兼容旧 C 源码；后续实现只使用 C++20、RAII、`std::span`、concepts、
   `std::stop_token` 和 coroutine。允许使用 coroutine，但禁止用 `std::future` 把每个
   I/O 变成隐藏 heap allocation 或阻塞线程。
3. 新 core 直接管理 SPDK bdev，不使用 Blobstore 文件抽象，不读取旧 Blobstore 镜像。
4. 对外只支持 RocksDB FileSystem、FIO ioengine 和新 C++ core；不恢复通用 POSIX 兼容。
5. 旧实现只作为冻结 A/B 和数据导出工具，不能以 compatibility flag 回流到新 core。
6. UUSE 2.0 只接受统一的 `.conf` 配置文件；一个 UUSE engine 可以独占并管理一个或多个
   NVMe/SPDK bdev，后续新增的 UUSE 配置必须继续加入这个文件和版本化 schema。

## 2. 取代范围和冻结策略

### 2.1 与旧架构的关系

本文取代 HFS 1.x 的全部设计、性能、review、shard 和 task 文档；这些第一阶段材料已从
工作区清理，本文不依赖其文件存在。旧实现只允许从 Git 历史追溯，不能作为 UUSE 2.0 的
输入或验收依据：

| 文档/内容 | UUSE 2.0 处理 |
|---|---|
| 旧 POSIX/preload/fd/FUSE/mmap 产品线 | 完全删除；UUSE 2.0 不提供兼容层 |
| 旧 C/Blobstore 实现 | 不直接复用；仅在新类型、所有权和错误模型下重写必要概念 |
| 旧 async/registered-DMA ABI | 删除旧导出符号；仅保留非阻塞语义和资源边界经验 |
| 旧 metadata/image layout | 删除旧格式；只重新设计 checksum、intent 和 recovery |
| 旧数据库兼容线 | 删除旧适配路径；UUSE 2.0 仅按本文实现 RocksDB/FIO frontend |

### 2.2 旧 src 的实际处置

本次工作区处置如下：

```text
src/                  → src_legacy_frozen/
Git tracked old src   → 删除
src_legacy_frozen/    → .gitignore，保留本地冻结副本
new src/              → 由 Phase 0/1 创建 C++20 源码
```

`src_legacy_frozen/` 只能用于代码考古、差异阅读和旧线复现；禁止从其中复制旧的
`UuseFile`、fd 表、Blobstore handle、POSIX wrapper 或锁结构进入新实现。若旧算法可复用，
必须先用新的类型、所有权和错误模型重写，并在提交说明中标注来源。

本次转换提交只负责冻结旧源码和确定新架构，不伪造一个尚未实现的 UUSE 2.0 build；因此
旧 Make/CMake source list 在 Phase 0 完成前不可视为有效构建入口。Phase 0 必须同时移除
旧的 `.c` 源文件引用、建立新的 `.cpp` target、固定 C++20 编译器/标准库版本，并让空壳
`Engine`/`Client`/`Task` 先通过结构化编译和生命周期测试。

迁移期间保留两条完全隔离的验证线：

```text
HFS 1.x frozen artifact ─ 旧 Blobstore 镜像 ─ 旧 POSIX/MariaDB/RocksDB baseline

UUSE 2.0                 ─ 新 raw-bdev 镜像 ─ C++20 core/FIO/RocksDB adapter
```

两条线不能共享 device set 中的任何 bdev。UUSE 2.0 不自动升级旧镜像；数据迁移只能是旧线
导出普通文件，再由 `uusectl import` 按 `.conf` 声明的 device set 写入新镜像，import 工具
不属于 core compatibility path。

## 3. 产品边界

### 3.1 支持

- 单进程、单 engine，独占配置文件中声明的一个或多个 NVMe/SPDK bdev；
- C++20 coroutine-first core API；
- RocksDB 8.9.1 `FileSystem` adapter；
- FIO 3.42 external ioengine；
- MySQL 8.0.46 InnoDB frontend（Phase 7，经 fork 的 `os_file` seam，见 §20）；
- TiKV frontend（TiDB 的本地存储层；Phase 8，经 `components/file_system`/
  `RocksDBEnv` seam，见 §21）；
- 直接 SPDK bdev/NVMe 访问；
- 文件 namespace、目录、rename、delete、extent allocation、sync 和 crash recovery；
- direct buffer 和 bounded bounce buffer 两条数据路径；
- 当前进程内 read-after-write；明确的 `fsync`/`fdatasync` durable boundary。

### 3.2 永久删除

- `LD_PRELOAD`、POSIX fd/dup/fcntl/stdio/cwd/dirfd/permission wrappers；
- FUSE、Linux VFS、通用 mmap；
- native POSIX `read/write/pread/pwrite` 作为 core API；
- SPDK Blobstore、Blobstore cluster allocator 和 Blobstore metadata layout；
- `uuse_lib_async_*`、`uuseAsync*`、registered-DMA、`uuseRegisterBuffer`、`uuseDmaAlloc`；
- `UUSE_POSIX_DIRECT_ASYNC`、`UUSE_POSIX_QUEUE`、`UUSE_POSIX_TICKET` 等旧开关；
- 多进程同时挂载同一 bdev、daemon、shared-memory IPC、跨进程锁和故障转移；
- 为通用 POSIX 重叠写、fd offset 或目录语义保留的 ticket/range compatibility path；
- MySQL/MariaDB/InnoDB 的 POSIX 兼容层（`LD_PRELOAD`/FUSE/fd wrapper、Aria/MyISAM
  数据路径）：不提供；MySQL 8.0.46 只能经 fork 的 `os_file` seam 接入（Phase 7，§20）。
- TiDB/TiKV 的 POSIX/FUSE 接入、TiFlash、TiKV encryption-at-rest、S3/cloud FS、
  mmap 读写、`tikv-ctl` 直连 UUSE 数据目录：不提供；TiKV 只能经
  `components/file_system` seam 接入（Phase 8，§21）。

fork 保护仍可作为 core 的进程安全措施，但不再模拟 POSIX preload 行为：fork child
调用 core 直接得到 `ForkedProcess` 错误。

## 4. 总体架构

### 4.1 进程内组件

```text
Application process
┌───────────────────────────────────────────────────────────┐
│ RocksDB adapter / FIO engine                              │
│        │                                                  │
│        └── uuse::Engine / uuse::Client                      │
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

没有 uused。引入 daemon 将新增 IPC ABI、共享内存生命周期、跨进程 DMA、租约、权限、
取消和崩溃恢复协议，这些不属于本次 clean-break。

### 4.2 C++20 模块布局

```text
include/
  uuse/core.hpp              public C++20 API，opaque ownership boundary
  uuse/task.hpp              coroutine Task/awaitable contract
  uuse/types.hpp             result、status、handle、iovec
  uuse/config.hpp            versioned `.conf` schema and device-set types

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
  uuse_filesystem.cpp
  uuse_random_access_file.cpp
  uuse_writable_file.cpp
  uuse_sequential_file.cpp
  uuse_directory.cpp
  uuse_rocksdb_env.cpp

fio/
  fio_uuse.cpp               FIO C entrypoints implemented by C++20

tools/
  uusectl.cpp
  uuse_db_bench.cpp
  uuse_ldb.cpp
```

新 `src/` 不编译 `src_legacy_frozen/`。CMake 和 Make 只枚举新 `.cpp` 文件，并开启
`-std=c++20`；不再维护旧 C source list。

## 5. C++20 coroutine 模型

### 5.1 public type

core 对外使用 C++ 类型，不暴露 SPDK、pthread、内部结构和 allocator：

```cpp
namespace uuse {

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

} // namespace uuse
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

// FIO adapter: fixed-size async event queue
CompletionToken token{slot, generation};
auto handle = client.submitRead(file, iov, offset, token);
client.poll(events, min_events, max_events, timeout);
```

RocksDB `ReadAsync` 使用 callback；FIO `getevents()` 使用 client completion queue。
同步 RocksDB 方法只在 frontend 线程等待 event，不阻塞 SPDK actor，也不把同步等待
包装成 core 的 blocking worker。event queue 出口返回 move-only `IoHandle`，它持有
operation 的 frontend intrusive ref；completion 使用 `slot + generation + request_id`
值标识，不把 `io_u*`、pending slot 或其它 frontend 裸指针跨线程传递。固定覆盖读写的
`submitRead/submitWrite` 返回前校验 logical size，并把 FileHandle、iovec descriptor、
token、sink 和按请求裁剪的 extent snapshot 复制到 bounded operation state，不能在
frontend 返回后访问调用栈上的临时 span。

RocksDB adapter 提供两种 frontend 消费方式，同一 `UuseContext` 同时只能启用其中一种，
由 `[rocksdb] worker_direct_reads` 控制（默认 `true`）：

1. worker-owned frontend channel（默认）：每个 worker 线程最多拥有一个 Client，
   frontend owner 就是该线程；该线程自行 `submitRead`/`poll`/`close`，直接发布到
   data-reactor ring，不经过共享 dispatcher。该模式的性能证据、生命周期修复和验收
   记录见 `doc/review/hfs2_rocksdb_read_latency_optimization_20260923.md`。
2. 共享 dispatcher（回落路径，`worker_direct_reads = false`）：RocksDB worker 只向
   bounded MPSC submit queue 投递请求，dispatcher 在自己的 owner thread 上完成
   submit、poll、reap 和 callback handoff。dispatcher 关闭顺序为停止 ingress、取消
   未提交请求、等待已提交请求 terminal、reap completion，最后在 owner thread 调用
   `close`。direct channel 建链失败时也必须回落到该路径。

`worker_direct_reads` 只作用于 RocksDB adapter 的随机读；诊断时可用环境变量
`UUSE_ROCKSDB_DIRECT_WORKER_READS=0/1` 做进程级覆盖（env 优先于配置文件）。

completion wake 只是低延迟提示而不是正确性条件；只要存在已提交但未完成的 I/O，
消费者必须以有界短周期继续 poll，避免唤醒合并/丢失时让 RocksDB worker 永久睡眠。
为支持这一生命周期，Client 允许在没有 outstanding request、IoHandle 或 open File 时
一次性转移 frontend owner；转移后禁止多个线程共享同一个 Client 的 async facade。
共享 dispatcher 的 per-request 等待状态（含 completion condition variable）只能由
dispatcher 线程释放；worker 必须通过 deferred retire 交还 handle，禁止在 dispatcher
可能仍在执行 completion notify 时释放 state，否则会破坏 condvar 内部状态并造成永久挂死。

worker-owned channel 必须遵守以下生命周期规则，否则不得进入正式构建：

- channel 条目只能持有 `UuseContext`/`Engine` 的非拥有引用，不得用 `shared_ptr` 把
  engine 生命周期延长到 thread-local 析构；UUSE context 的销毁必须发生在 RocksDB
  后台线程静默之后的主线程上，且不得在后台线程上执行 `Engine::stop()`。
- adapter 维护 direct channel registry；`UuseContext` 析构（主线程）先关闭 registry，
  再在 engine 存活时 retire 全部 channel（销毁 client、open File 和 metadata lease），
  然后停止 dispatcher，最后 `Engine::stop()`。registry 关闭后新建链必须回落共享
  dispatcher，并记录计数和日志。
- 拥有线程退出时在自己的线程上 close 自己的 channel；被 context 抢先 retire 的
  thread-local 析构必须是 no-op，不得再访问 engine/namespace。
- retire 时仍有 outstanding request 或未关闭 File 属于契约违例：必须记录错误并
  失败关闭，不得无界等待。

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

SPDK env 和 subsystem 是进程级一次性初始化：运行时单例在第一次 mount 时完成 env/
subsystem 初始化并常驻，Engine 的 mount/stop 只在 EnvReady 与 Active 之间切换 descriptor、
reactor 和 channel。配置中的 bdev 由内存生成的 `bdev_nvme_attach_controller` JSON 创建；
调用 `spdk_subsystem_load_config` 前必须把 RPC 状态切到 `SPDK_RPC_RUNTIME`，否则 RUNTIME
方法会被静默跳过。channel 释放经线程消息延迟执行，销毁 reactor 线程前必须 poll 排空。

### 6.2 Reactor 数量和 CPU

```text
R = 1                         nproc == 1
R = 2                         2 <= nproc <= 8
R = min(4, max(2,nproc/4))    nproc > 8
```

`R` 是 data shard 数，另有一个 metadata actor。`metadata_cpus` 是必填的显式 CPU 列表，
首元素用于 metadata reactor pin、EAL main core 和 `data_cpus=auto` 时的 CPU 基准；
`data_reactors`、`data_cpus` 和 `frontend_cpus` 可显式指定或 auto，frontend 不由 UUSE
强制绑定。当前正式开发口径以单 NUMA node 为准，runtime
保留 topology-aware 选择和确定性 fallback，但多 NUMA node 的本地 DMA/跨 node 性能
验证暂缓，不作为单节点功能验收门槛。`data_reactors` 上限不能超过
`max_data_reactors`；默认不恢复旧的 `UUSE_REACTOR_PIN` 环境变量。data reactor 的
polling policy 由 `[numa].polling` 控制，默认 hybrid。

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

data reactor 的空闲等待（hybrid/interrupt）必须可被 doorbell 唤醒，不得使用无唤醒源的
`sleep_for`：

- 每个 data reactor 持有一个 nonblocking `eventfd`；空闲路径先清 eventfd、置 idle 标记、
  重查 poller/ring，再用 `ppoll` 做有界等待；
- producer 只在 ring `empty→non-empty` 且该 reactor 处于 idle 等待时写一次 eventfd；
  reactor 未睡时不产生每请求 syscall；
- doorbell 只是唤醒提示，`ppoll` 超时兜底仍是正确性边界；等待期间 stop 请求和 SPDK
  thread message 的处理延迟不超过该超时；
- reactor 醒来后重新进入完整 poll 循环（`spdk_thread_poll`、retry/completion drain、
  poller pump），doorbell 不改变 ring 的 MPSC 事实源地位。

metadata reactor 使用同一 eventfd 等待协议，语义是 control-path：

- 只在没有 SPDK 事件、没有在途 control I/O 时进入有界等待（hybrid 1 ms、interrupt
  100 µs、active 不休眠）；
- 任何线程向 metadata reactor `spdk_thread_send_msg` 后必须写它的 eventfd；
- 在途 control I/O（journal/checkpoint/format 读写）期间不得休眠，保证 control 提交
  不被等待延迟；
- 等待不改变 control I/O 的阻塞语义：调用者仍在自己的 condition variable 上等待完成；
- 找不到唤醒源时 1 ms 超时兜底，不允许无界等待。

SPDK callback 不直接 `scheduler.complete`，也不在 callback 栈中递归提交 RMW 下一阶段或
下一笔 operation。callback 只释放 `spdk_bdev_io`、记录 child status，并把内嵌 ready node
放入 reactor-local actor queue；actor 主循环负责 phase 推进、scheduler complete 和 frontend
completion publish。每个 Client 为每个 shard 维护一个 bounded SPSC completion ring；
producer 是该 shard actor，consumer 是 Client frontend。Linux 用 nonblocking eventfd 作为
唤醒提示，ring 才是完成事实源，producer 不获取 frontend mutex 或执行堆分配。

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
- SPDK child callback 只记录结果并排入 actor ready queue，不直接更新 scheduler 或 frontend；
- actor continuation 更新 parent；最后一个 child 通过 release store 发布 parent，并只唤醒
  operation 选定的唯一 completion sink；
- read 统一 copy-out 后释放 lease，write 统一 release；
- 一个 frontend operation 只能有一个 completion sink；
- timeout/cancel 不提前销毁 SPDK callback 仍可见的 frame。

SPDK submit 返回 `-ENOMEM` 时，child 在同一 reactor/channel 注册
`spdk_bdev_queue_io_wait`；wait callback 只把 child 排入 actor ready queue，由 actor 重试。
operation 已经被 ring 接受后不得把该错误倒退映射为 frontend submit Busy。

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

UUSE 2.0 完整拥有配置文件中声明的全部 raw bdev；每个设备都拥有自己的物理 geometry
和 allocator，多个设备共同组成一个逻辑 UUSE volume：

```text
Superblock A/B       8 MiB reserved
Journal ring         default 256 MiB
Checkpoint A/B       configurable metadata region
                     （namespace/inode/extent map/allocator bitmap 快照）
Data extents         remaining capacity
```

旧 Blobstore magic、旧 metadata header 和 HFS 1.x image 都拒绝 mount。`uusectl format`
是破坏性命令，必须显式 `--yes` 和空设备/确认检查；不做在线转换。

### 9.2 Superblock 和 geometry

```cpp
struct UuseSuperblock {
    std::uint64_t magic;
    std::uint32_t format_version;
    std::uint32_t feature_flags;
    std::uint32_t device_id;
    std::uint32_t replica_id;
    std::uint32_t replica_count;
    std::uint32_t reserved0;
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

`*_start`/`*_blocks` 均以 `metadata_block_size` 为单位，`device_blocks` 是设备容量换算后的
metadata block 数。Superblock slot A 位于偏移 0，slot B 位于 4 MiB（region 共 8 MiB）。
`replica_id` 是设备在 metadata 副本中的序号，`replica_count` 等于配置的
`metadata_replication`；两者在单盘 format 时也必须写入并校验。
CRC32C 覆盖 `crc32c` 字段之前的全部字节。

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

每个 journal slot 是一个 metadata block：BEGIN 槽携带 txid 与 record_count，RECORD 槽
携带固定 payload（type/object_id/arg0/arg1），COMMIT 槽携带 tx_crc（txid 与全部 record
payload 的 CRC32C）。slot 序号单调递增，物理位置 = `journal_start + (sequence %
journal_blocks)`；superblock 的 `journal_head`/`journal_tail` 保存 slot 序号。slot 与
checkpoint 头部都带 volume UUID，旧世代内容被视为 ring 结束而不是 corruption。
checkpoint 槽头部记录 txid、sequence（checkpoint 时的 head）、generation 和 CRC；mount
选择 txid 最大且校验通过的 checkpoint，并从其 sequence 继续 replay。replay 遇到
sequence/txid 不连续的 slot 视为陈旧内容或 journal 结尾；遇到非空但校验无效的 slot 或
commit CRC 不匹配则记录 corruption evidence 并停止。提交一个事务后，记录立即应用到
内存状态机；format 会清零 journal 头部与两个 checkpoint 槽的首块，保证新镜像从空
journal 开始。

- 50% 使用率触发 checkpoint，80% 使用率优先同步 checkpoint；
- journal 空间不足时先 checkpoint；checkpoint 失败则在 bounded timeout 后返回 NoSpace/Busy；
- checkpoint 写 inactive slot 的完整 snapshot（namespace/inode/extent map/allocator
  bitmap），flush 后更新 superblock；
- superblock durable 后才推进 journal tail；
- 撕裂/未知/超长 record 丢弃并记录 corruption evidence，不静默 replay。

已实施的记录路径（Shard-107）：`SET_SIZE`/`ALLOC`/`FREE`/`SET_TIMES`/`TRUNCATE` 由对应
mutation 直接 stage（`arg1` 打包 extent run：device/start/count），barrier 在数据 flush
之前把整笔事务（BEGIN+records+COMMIT）作为**一次连续写**追加，随后同一次设备 flush
一并落盘。

**Shard-9.5 更新（2026-10-12）**：`CREATE`/`LINK`/`UNLINK`/`RENAME` 形状变化改为
**可回放记录**（append-only 类型 `CreateInode=15`/`RemoveEntry=16`/`RenameEntry=17`/
`LinkEntry=18`/`FreeInode=19`）：`RecordPayload.reserved1` 携带变长扩展长度，扩展
（同一 4 KiB slot 内）承载 1–2 个名字与记录专用标量（create 的 mtime/ctime、rename 的
被替换 inode 与"运行时是否已释放"标志）；replay 精确复刻 live mutation（含 allocator
与 free-slot 决策），因此 barrier 常规走 journal 事务 + delta checkpoint，
全量 base 只在 ring 使用率到达 `checkpoint_force_percent`（80%）或记录不可打包时写。
journal 压缩（`checkpoint_required`）仍保留为兜底：无记录可打包的 mutation 与
`UUSE_TEST_FORCE_CHECKPOINT` 测试钩子都会强制下一 barrier 走全量快照。
replay 侧 `ALLOC` 按 checkpoint 中的 allocator 状态把 run 从 FREE/RESERVED/INITIALIZED
逐级推进到 COMMITTED（`markReserved` → `markInitialized` → `commit`），`FREE` 幂等。

异步 journal writer（write-behind：后台线程追加、barrier 只 flush）已评估并**回退**：
实测 1809.90 vs 1926–1958 TPS（约 −5.8%），因为 barrier 仍要等 append 的设备往返，
额外线程与唤醒开销是净损失。回退后保留两项加固：Journal 公开入口（append/checkpoint/
replay/flush/stats）内部串行化，供并发调用者使用；checkpoint 失败时必须
`requireCheckpoint()`——本次尝试已消费 staged 记录，快照失败后下一次 barrier 必须重试。

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
被另一 UUSE 实例重复配置。

journal 在每个 metadata device 上镜像相同的 transaction stream；事务的 commit point
是全部副本。recovery 以所有 metadata device 的最小已提交 txid 为回放终点，超出该点的
tail 丢弃（roll back），不做单盘前滚；journal 损坏、CRC 不一致或设备缺失时 fail closed。

`uusectl format`、`check`、`dump` 和 `import` 都以同一个 `.conf` 为入口，并对 device set
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
data submit → data drain
  → metadata prepare（staged durable sizes）
  → checkpoint 路径：checkpoint 记录 + superblock 一次批量写（defer flush）
     journal 路径：BEGIN+records+COMMIT 一次连续写（defer flush）
  → 每设备一次 device flush（覆盖 user data + journal slots/checkpoint + superblock）
  → publish durable epoch
```

这明确避免“metadata transaction 写完却没有最终设备屏障”的旧问题，也保证 metadata
不会先指向未 durable 的用户数据：单次 flush 之前掉电则 data 与 metadata 一起回退到
上一个 checkpoint，flush 之后三者同时 durable。写入顺序只保留一次设备屏障，
消除 checkpoint/superblock 各自的 flush 往返（2026-09-27 barrier 合并）。

并发 `sync/fsync` 使用 full-snapshot group commit：第一个 waiter 成为 leader，在
`metadata.max_group_commit_wait_us` 窗口内合并更高目标 epoch；该窗口是纯延迟项，每次
barrier 都已真实落盘，因此 MySQL 配置取 0（实测 +9%），需要合并时由并发 caller 自然
join；窗口结束后只执行一次
全设备 data flush、namespace checkpoint 和 superblock publish，所有不超过冻结目标的
waiter 共享同一个成功或失败结果。该窗口只用于合并请求，不是 durability timeout；
增量 dirty-object checkpoint 仍未改变当前 on-disk format，留待后续格式版本。

写路径的四层可见性（2026-09-19，RocksDB adapter 写回缓冲）：

```text
buffered  = adapter 写回缓冲内（每文件 ≤1 MiB）；仅本进程；进程崩溃丢失
logical   = File::write 成功（数据已提交设备、namespace 内存态更新）；同进程可见；进程崩溃丢失
durable   = File::sync() 成功（上述 1-7 顺序完成并发布 durable epoch）；跨进程/机器崩溃保留
recovered = mount replay 后状态；不变量 recovered ⊆ durable，无半事务
```

RocksDB 回调映射：`Append`=buffered；`Flush`=logical（**不** durable）；
`Sync/Fsync`=durable。UUSE 的 logical 层不跨进程崩溃（namespace 内存态），
因此需要进程崩溃不丢的写入必须 `sync=1`；这与 ext4 page cache 的差异属已知
边界（`doc/design/hfs2_rocksdb_wal_writeback_design_20260919.md` §5）。

### 10.3 Crash replay

- committed transaction replay；不完整 transaction 丢弃；
- 内存 after-image 只有在 commit durable 后才 publish；失败时恢复旧 snapshot；
- RESERVED extent 回收，unlinked-but-open file 的 extent 进入 orphan reclaim；
- 已写但未被 durable metadata 指向的数据不可见并可回收；
- checksum、generation、namespace、extent bitmap 和 file size 必须相互守恒；
- mount 发现不可修复 corruption 返回 Corrupt，保留现场给 `uusectl check`，不强行 mount。

### 10.4 checkpoint 增量与停顿（P2-2）

> 评审：`doc/review/hfs2_incremental_checkpoint_review_20260929.md`。
> 现状：`Namespace::saveSnapshot` 持 namespace 全局锁完成全量序列化，由 barrier
> 线程在 checkpoint 路径调用（设备写已在锁外）。目标：1M active inode 下把
> checkpoint 的 stop-the-world 从 O(N) 编码降为 O(N) 冻结拷贝（(a)，不改格式），
> 并在下一个格式版本实现 O(dirty) 增量（(b)）。

**(a) 冻结视图 + 锁外编码（格式版本不变）**

- `Namespace::freezeCheckpointViewLocked(txid)` 在锁内把所有 checkpoint 需要的
  状态捕获为扁平 `CheckpointView`：header（txid/logicalEpoch/durableEpoch/
  namespaceBytes）、inode 标量数组（含 entries/extents 的 begin/count）、
  目录项（name+child）、extent 描述符、`freeSlots`、allocator 位图字节；
- `snapshotTxid_` 在冻结时发布；
- `encodeCheckpointView(view, out)` 在锁外按现有字段顺序编码，磁盘格式与
  `kNamespaceSnapshotVersion` 不变；
- 一致性：view 是一次性深拷贝，冻结后的 mutation 不影响它；失败路径沿用
  `rollbackPreparedDurableEpoch` + `requireCheckpoint`；
- 内存：峰值 ≈ view + 编码缓冲（同阶），编码后 view 立即释放；
- 度量：新增 `syncCheckpointFreeze` 计数与 `ChkptFreeze` trace，报告冻结/编码
  占比；`syncCheckpointSnapshot` 保持为总时间。

**(b) journal-backed 增量 checkpoint（格式版本 +1）**

- checkpoint 有两种记录：**full base**（全量快照，沿用 (a) 的冻结/编码）与
  **delta**（仅 header + `DeltaPayload{base slot/txid/sequence}`，不写快照）；
- journal 就是 delta：delta 只把当前 `head` 记入 checkpoint，`tail` 保持在上一个
  base 的 sequence；恢复 = 装载 base 快照 + 从 base txid 起 replay 现有 journal
  （无需 per-object dirty 跟踪）；
- **shape 变更约束**：journal 记录格式（objectId/arg0/arg1）不能携带名字，
  Create/Link/Unlink/Rename 不写 journal（原设计由全量快照覆盖）；因此 shape 变更
  置 `checkpointRequired` 并强制下一次 checkpoint 写 **full base**，delta 只覆盖
  无 shape 变更的窗口（SetSize/Alloc/Free/Truncate/SetTimes 可 replay）。
  **Shard-9.5 修订（2026-10-12）**：该约束已解除——`RecordPayload.reserved1` +
  变长扩展让 CreateInode/RemoveEntry/RenameEntry/LinkEntry/FreeInode 携带名字并
  可 replay（见 §9.3 更新），shape 变更不再强制 full base；`checkpointRequired`
  仅作为不可打包 mutation 的兜底；
- 写 full base 的时机：无有效 base、窗口 `head - baseSequence` ≥ 75% ring、或
  ring 将满（append 内部触发）；其余 checkpoint 写 delta；
- 写槽位：始终写**非 base**槽位，保证 base 槽位在任何 delta 之后仍有效；
- 触发节奏：`needsCheckpoint()` 改为“自上次 checkpoint 起的 append 量 ≥
  triggerPercent% ring”（不再是 tail 占用率，因为 delta 不推进 tail）；
- 发布顺序：checkpoint 记录 → superblock → 一次设备 flush；失败时 base/delta
  状态不变（`tail` 不推进）；
- 兼容：`CheckpointHeader.kind` 字段 + `formatVersion=2`；v1 槽位按 full 读取，
  旧卷可被新二进制 mount；新卷对旧二进制 fail-closed。

## 11. RocksDB 8.9.1 adapter

### 11.1 接入原则

adapter 是独立 C++20 shared library，core 与 RocksDB 只通过 C++ header contract 编译
连接；core 不包含 RocksDB header。由于 C++ ABI 依赖编译器，adapter 与 core 必须使用
锁定的 RocksDB 8.9.1、compiler 和 libstdc++ 构建组合。

Debian stock `db_bench` 不提供 CLI 注入 `DBOptions::env`，所以验收使用：

```cpp
// RocksDB 8.9.1 的 DBOptions::env 是裸 Env*，RocksDB 不接管所有权；
// env 必须由调用方持有到 DB 关闭。
auto env = uuse::rocksdb::NewUuseEnv(uuse_engine);
Options options;
options.env = env.get();
DB* db = nullptr;
Status s = DB::Open(options, db_path, &db);
```

实际工具为自编译 `uuse_db_bench`；`uuse_ldb` 同样显式设置 UUSE Env。不能把 stock 工具
的输出写成 UUSE 已接管。

### 11.2 8.9.1 必需接口

严格以 VM `/usr/include/rocksdb/file_system.h` 和 `env.h` 的 8.9.1 头文件为准：

| 8.9.1 契约 | UUSE 2.0 实现 |
|---|---|
| `FSSupportedOps::kAsyncIO` | 声明并实现 |
| `FSSupportedOps::kFSBuffer` | 声明（Shard-111，2026-09-29）；读文件非 direct，FS 用 arena 对齐 `fs_scratch`，见本节读侧 zero-copy 段 |
| `kFSPrefetch` | 不存在，禁止声明 |
| `FSRandomAccessFile::ReadAsync(FSReadRequest&, ..., void** io_handle, IOHandleDeleter*, IODebugContext*)` | 按完整真实签名实现 |
| `FileSystem::AbortIO()` | 必须实现，遵守 core cancel boundary |
| `GetFileModificationTime` | 返回 inode mtime |
| `FSDirectory::Fsync/FsyncWithDirOptions` | 提交 directory metadata epoch |
| `LockFile/UnlockFile` | 进程级 RocksDB lock lease |
| `LinkFile` | link-count transaction；若版本/场景不支持则明确返回 NotSupported |
| `FSSequentialFile::Skip/InvalidateCache` | 必须实现；invalidate 可为正确的 no-op |
| `FSWritableFile::PrepareWrite/Allocate` | `Allocate` 映射 extent preallocation；`PrepareWrite` 为 no-op |
| `PositionedAppend` | 保留直写实现；RocksDB 8.9 写侧 `use_direct_io()==false` 时不调用 |

读侧 v1 选择 `use_direct_io() == true`（`FSRandomAccessFile`）：adapter 必须实现对齐
检查与 bounce fallback；目录 fsync 由 `FSDirectory::Fsync` 承担。ReadAsync 的
`FSReadRequest`、iov、`io_handle` 必须活到 callback 返回；之后才调用
`IOHandleDeleter`。v1 不声明 `kFSBuffer`，不把 UUSE DMA lease 借给 `fs_scratch`。

写侧 v1 选择 `use_direct_io() == false`（2026-09-19 决策 B，见
`doc/design/hfs2_rocksdb_wal_writeback_design_20260919.md`）：**所有可写文件**
（WAL/MANIFEST/SST）在 adapter 内做写回缓冲（每文件 ≤1 MiB）：
`Append` 只写入缓冲；`Flush` 为 no-op（实测 RocksDB 每个写组都调用 Flush，落盘会使
缓冲失效；Flush 不是 durability 屏障），缓冲由阈值、`Sync/Fsync`、`RangeSync`、
`Close` 和读句柄打开强制写出；只有 `Sync/Fsync/RangeSync/Close` 调用
`File::sync()` 触发 durable epoch；`use_direct_io_for_flush_and_compaction`
对 UUSE 无效，基准口径需声明。`PositionedAppend`/`Allocate`/`PrepareWrite` 保留实现。

读侧 zero-copy（Shard-111，2026-09-29 实施；评审见
`doc/review/hfs2_rocksdb_kfsbuffer_review_20260929.md`）：RocksDB 9.11.2 的
`BlockFetcher` 在 direct 读下读入自分配的 `AlignedBuf`，`memory_allocator` 只覆盖
读后的 `heap_buf_`，不能消除设备路径 bounce。读侧 zero-copy 因此走 kFSBuffer 契约：

- `UuseFileSystem::SupportedOps` 声明 `kAsyncIO | kFSBuffer`；
- 读文件（`UuseRandomAccessFile`）`use_direct_io() == false`，`BlockFetcher` 走
  `MultiRead` 的 `fs_scratch` 分支（顺序文件保持 direct，其调用方自带 scratch）；
- `MultiRead`/`ReadAsync` 在 `scratch == nullptr && len != 0` 时先按 512 对齐
  span（RocksDB 只在 direct 时自行对齐），≤1 MiB 时用
  `Engine::acquireDmaBuffer`（class 池）分配 arena buffer，写 `fs_scratch`
  （deleter 捕获 engine + size 归还 arena），读完后 `result` 指向调用方原始
  偏移；>1 MiB（预取 readahead）或 arena 不可用时回落 `new char[len]`，
  设备路径自动 bounce，功能不回退；
- core `SpdkRuntime::isDmaCapable` 要求 dword 对齐（SPDK PRP 要求），非对齐
  候选一律 bounce（2026-09-29 修复：kFSBuffer 的非对齐预取偏移曾导致设备提交
  `not dword aligned` 失败）；
- RocksDB 仍会做一次 `fs_scratch→heap_buf_` 的内部 memcpy（BlockContents 所有权），
  收益 = 去掉 UUSE arena bounce lease + copy-out；
- 预取路径（`FilePrefetchBuffer::FSBufferDirectRead`）同样取 `fs_scratch`，但
  >1 MiB 的 readahead 走 heap 回落（bounce），基准口径必须声明；
- 验收：`read_direct_user_children > 0`、copy-out 归零、大工作集 readrandom 同会话
  A/B 无回退、校验和与 `uusectl check corruption=none` 通过；实测 post/pre
  ops=1.0016、p50=0.982、p99=1.016（`uuse_rocksdb_kfsbuffer_20260929.md`）。

### 11.3 文件类型

```text
SST/table       → RandomAccessFile，可并发 Read/ReadAsync
WAL             → WritableFile，单 writer，buffered Append + Sync（§11.2 写侧语义）
MANIFEST        → append/rename，metadata epoch
CURRENT/OPTIONS → write/close/rename
LOCK            → LockFile/UnlockFile lease
```

不实现通用 POSIX 多 writer；利用 RocksDB 自身的 file role 和访问约束降低 scheduler 复杂度。

## 12. FIO 3.42 adapter

### 12.1 加载方式和 options

FIO engine 编译为 C++20 shared library，但只在 FIO 注册入口使用 `extern "C"`：

```text
fio --ioengine=external:/absolute/path/fio-uuse.so job.fio
```

`ioengine_ops.options` 必须登记自定义 options，并设置 `option_struct_size`：

```text
uuse_config=/path/uuse.conf
uuse_direct=1
uuse_preallocate=1
uuse_client_depth=32
uuse_file_per_job=0
```

engine 直接调用 C++ `Engine/Client/File`，不通过 POSIX open/read/write。`open_file`
负责 create/open/allocate，并设置 `FIO_NOEXTEND` 或完成等价的 engine-owned extend，
避免 `setup_files()` 用 POSIX ftruncate 绕过 UUSE allocator。

### 12.2 FIO lifecycle

- `queue()` 产生 async operation，pending slot 保存 move-only `IoHandle` 和 `io_u`
  引用直到 `event()`；协议层仍区分 `Reserved/Submitted/Completed/Reported` 边界，
  但当前 FIO adapter 将其压缩为 `Free→Submitted→Reaped→Free`：`Reaped` 表示
  completion 已被 `getevents()` 取出但 `event()` 尚未交还 `io_u`，因此仍禁止复用
  slot；每次从 `Free` 重新提交递增 generation；
- `iomem_alloc/iomem_free` 从 shared arena 提供整块 DMA `orig_buffer`，是 all-or-nothing
  契约：engine 按 bs×iodepth×jobs 预检 arena 容量，无法完整提供时 FIO 初始化失败并明确
  报错，不做半量 fallback；不使用 `iomem_alloc` 的普通 FIO buffer 按 ≤1 MiB child bounce
  并计入 copy 统计；`io_u->xfer_buf` 是 allocation interior pointer 时用
  `ownsRange(ptr,len)` 校验，不能只匹配 allocation base；
- FIO engine 必须声明 `FIO_RAWIO | FIO_NODISKUTIL | FIO_DISKLESSIO`，因为 `filename`
  是 UUSE namespace 路径而不是宿主机文件；setup 阶段强制 `thread=1`，避免 fork 后复用
  process-shared Engine。FIO 可能在 engine `init` 前调用 `get_file_size`，该阶段只能使用
  显式 `size` 或安全返回，不能访问尚未建立的 Client；
- `queue()` 三态返回：成功 `FIO_Q_QUEUED`、临时资源不足 `FIO_Q_BUSY`、参数或永久错误
  `FIO_Q_COMPLETED` + `io_u->error`；只有 client credit、operation pool 或 ring reservation
  在 publish 前失败才能返回 `FIO_Q_BUSY`。publish 后 planner/DMA/backend/metadata 错误必须
  产生唯一异步 terminal event，不能伪装成可重试 queue Busy；
- `getevents(min,max,timeout)` 调用 `client.poll`，满足 FIO min/max 语义；
- `ClientOptions` 可注入 frontend-owned completion wait strategy；core 仍统一负责
  adaptive spin、ring rearm/rescan、deadline 和 completion drain，外部策略只提供
  prepare/epoch/waitSince，不能绕过 Client poll 生命周期；
- `getevents()` 用 slot、generation 和 request id 校验 token 并写回 `io_u->error/resid`；
  `event()` 返回 `io_u` 后才释放 pending slot；
- 同一 shared file 的 open/preallocate 使用 process-level single-flight：一个 owner 执行
  allocate，全部 waiter 复用同一校验结果；
- `uuse_file_per_job=0` 保持正式 shared-file 口径；设置为 `1` 时，FIO adapter 为每个
  worker 在原始路径后追加稳定的 `.job-<thread_number>` 后缀，使每个 worker 获得独立
  UUSE file 和 file-owned shard。该选项只用于多 file 拓扑归因/对照，不改变默认 shared
  file gate，也不把同一 file 的请求跨 shard 迁移；
- `FIO_NOEXTEND`、`fio --verify`、direct/bounce、iodepth 必须在测试中覆盖；
- cleanup 告警超时不得提前释放 FIO memory；engine close 前必须继续 drain 全部 callback、
  token、IoHandle、io_u、coroutine frame、File 和 Client。

## 13. Namespace 和文件 API

### 13.1 C++ API 草案

```cpp
namespace uuse {

class Engine {
public:
    static Result<std::unique_ptr<Engine>> create(const Config&);
    Task<Result<void>> format(FormatOptions);
    Task<Result<void>> mount();
    Task<Result<void>> drain();
    Task<Result<void>> stop();
};

struct ClientOptions {
    std::uint32_t maxOpenFiles = 0; /* 0 = only the namespace limit applies */
    std::uint32_t maxOutstanding = 32;
};

struct CompletionToken {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
};

struct CompletionEvent {
    std::uint64_t requestId = 0;
    CompletionToken token{};
    Status status = Status::Ok;
    int backendErrno = 0;
    std::uint64_t completedBytes = 0;
};

struct PollResult {
    Status status = Status::Ok;
    std::size_t count = 0;
};

class IoHandle {
public:
    IoHandle(IoHandle&&) noexcept;
    IoHandle& operator=(IoHandle&&) noexcept;
    IoHandle(const IoHandle&) = delete;
    IoHandle& operator=(const IoHandle&) = delete;
    ~IoHandle();
    bool valid() const noexcept;
    std::uint64_t requestId() const noexcept;
    CompletionToken token() const noexcept;
    void reset() noexcept;
};

enum class OpenFlags : std::uint32_t {
    None      = 0,
    Create    = 1U << 0, /* create when missing */
    Exclusive = 1U << 1, /* AlreadyExists when present */
    Directory = 1U << 2,
    Truncate  = 1U << 3, /* reset logical size when present */
};

struct DirectoryEntry {
    std::string name;
    std::uint64_t inodeId = 0;
    std::uint32_t generation = 0;
    bool directory = false;
};

struct DirectoryPage {
    std::vector<DirectoryEntry> entries;
    std::string nextCursor; /* empty when iteration is complete */
};

struct FileStat {
    std::uint64_t inodeId = 0;
    std::uint32_t generation = 0;
    bool directory = false;
    std::uint64_t logicalSize = 0;
    std::uint64_t mtimeNs = 0;
    std::uint64_t ctimeNs = 0;
};

class Client {
public:
    static Result<std::shared_ptr<Client>> create(Engine& engine,
                                                  const ClientOptions& = {});
    Task<Result<void>> close();
    Task<Result<std::shared_ptr<File>>> open(std::string_view path,
                                             OpenFlags flags = OpenFlags::None);
    Task<Result<void>> remove(std::string_view path);
    Task<Result<void>> rename(std::string_view from, std::string_view to);
    /* Bounded directory iteration: ordered by name, cursor is the last name of
     * the previous page (empty = start); GetChildren never scans other
     * directories. */
    Task<Result<DirectoryPage>> getChildren(std::string_view path,
                                            std::string_view cursor,
                                            std::size_t maxEntries);
    /* Path-based metadata lookup without opening a handle. */
    Task<Result<FileStat>> stat(std::string_view path);
    /* Creates every missing directory component; existing directories are
     * accepted and a non-directory component fails closed. */
    Task<Result<void>> createDirectories(std::string_view path);
    Result<IoHandle> submitRead(const std::shared_ptr<File>&, IoVec,
                                std::uint64_t offset, CompletionToken);
    Result<IoHandle> submitWrite(const std::shared_ptr<File>&, IoVec,
                                 std::uint64_t offset, CompletionToken);
    PollResult poll(std::span<CompletionEvent>, std::size_t min,
                    std::size_t max, std::chrono::microseconds timeout);
    std::size_t outstanding() const noexcept;
    std::size_t maxOutstanding() const noexcept;
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
    Result<FileStat> stat() const noexcept;
};

} // namespace uuse
```

`std::shared_ptr<File>` 只用于 frontend 的长寿命 handle，不用于每个 child；data path
使用 intrusive `OperationRef`、pool 和 `BufferLease`。路径 API 只进 metadata actor，
文件数据使用 file id/handle。

### 13.2 Namespace 规则

- path 是绝对路径，按 `/` 分段；v1 不解析 `.`/`..`，不支持符号链接；regular inode
  支持同一 namespace 内的 hard link（`link count` 用于 unlink/orphan 生命周期），供
  RocksDB adapter 的 `LinkFile` contract 使用；
- inode id 单调分配，handle 带 generation，旧 handle 不能解析到新 inode；
- directory entry 由 name hash 查找、ordered cursor 遍历；
- rename 是单一 journal transaction，源/目标不产生中间可见状态；
- delete 先摘 namespace，再延迟回收 extent；打开的 File 可完成已有 operation；
- directory fsync 提交 directory metadata epoch；
- `max_inodes`、`max_namespace_bytes` 超限返回 NoMemory，不退回磁盘全表扫描；
- 大目录必须按 shard/hash 维护，`GetChildren` 不扫描全部 namespace；
- `stat(path)` 不要求打开 handle；`createDirectories` 逐段创建且幂等，遇到非目录
  组件 fail closed（Phase 7 起供 InnoDB adapter 使用）。

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
- ring acceptance 前的 retry 不持 scheduler、DMA 或 metadata lock；
- ring、frame、arena 满时立即 bounded busy；
- ring publish 后的 arena 暂时不足停在 bounded Preparing FIFO，不持 scheduler node；所有
  child bounce/RMW lease 必须 batch 全取或全不取；
- backend `-ENOMEM` 使用同 channel `spdk_bdev_queue_io_wait`，此时 operation 保持已取得的
  scheduler admission、lease 和引用；
- submit 前 cancel 从 scheduler 删除 operation；
- submit 后 cancel 只标记状态，等待 SPDK callback 完成，不能伪造 backend abort；
- `Client::poll` timeout 只结束 frontend 等待；core operation timeout 进入 Draining，等待
  已提交 child 返回后才发布 TimedOut，不释放 callback 仍需要的 frame/buffer；
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

frontend（RocksDB adapter）的关闭路径必须先 flush adapter 写缓冲：`Close` 执行
flush + `File::sync()`；未 sync 的 buffered/logical 数据在进程崩溃时丢失，由 WAL
replay 恢复到最后一个 durable epoch（§10.2 四层语义）。

Client 生命周期固定为 `Open→Closing→Closed`。Closing 后拒绝新 submit；outstanding、queued
completion、IoHandle 或 open File 任一非零时 close 返回 Busy 但保持 Closing。operation
使用 pool-backed intrusive completion-sink 引用，Client facade 保留最后一个 frontend 引用
到 Closed，禁止在 reactor callback 上析构 Client/File 高层对象或提前销毁 completion ring。

## 15. 配置、ABI 和构建

### 15.1 `.conf` 配置（唯一入口）

UUSE 2.0 使用版本化的 INI-like `.conf` 格式，不再使用 UUSE JSON 配置或散落的
`UUSE_*` 环境变量。engine、SPDK、device set、metadata、reactor、DMA、限流、恢复和
frontend 的所有 UUSE 配置都必须写入同一个文件；命令行只负责传入配置文件路径，例如
`uusectl --config=/path/uuse.conf`。未知 key、重复 device 或 schema 不兼容必须拒绝启动，
不能静默忽略。

仓库当前已有的 `config/*.json` 文件属于冻结的 HFS 1.x 配置，不是 UUSE 2.0 的兼容输入；
Phase 0 创建 `.conf` parser 和新 target 后，正式命令只能接受 `.conf`。

```conf
[uuse]
schema_version = 1
instance_name = uuse
volume_uuid = auto
process_lock = exclusive
process_lock_path = /var/run/uuse/uuse.lock

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
metadata_cpus = 0
data_reactors = 0
max_data_reactors = 4
data_cpus = auto
frontend_cpus = auto
ring_depth = 1024
client_depth = 32

[rocksdb]
worker_direct_reads = true

[numa]
policy = local-first
device_numa = auto
dma_numa = auto
frontend_numa = device-local
allow_remote_dma = false
remote_fallback = bounded
polling = hybrid

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
max_group_commit_wait_us = 200

[observability]
trace = false
trace_sample = 1
trace_dir = /mnt/uuse-data
stats = false
```

`metadata_devices` 和 `data_devices` 使用 `[device.<id>]` 中的逻辑 ID；设备可以只承担
data，也可以同时承担 metadata。只配置一个 NVMe 时仍使用完全相同的 schema。正式单盘
baseline 的配置文件为 `config/uuse_nvme.conf`，SPDK bdev 名称为 `Nvme0n1`；PCI BDF
必须按当前 VM 的控制器拓扑填写，多盘测试必须列出实际 device set，不能通过临时环境变量
追加设备。2026-09-25 当前 VM 的单盘验证目标为序列号 `VMware NVME_0002` 的 20 GiB
NVMe，PCI BDF `0000:13:00.0`，此时内核设备名为 `/dev/nvme0n1`。内核 `nvmeXn1`
编号可随启动或绑定变化，验证前必须用 sysfs 的控制器 address 和序列号核对目标盘，
确认未挂载、无 holder，再绑定给 SPDK；历史 `0000:0b:00.0` 性能结果不直接移用到新盘。
该 VM 为 6 vCPU；2026-09-25 在 `0000:13:00.0` 上完成 SPDK/UUSE 编译与完整
CTest `26/26`，其中设备测试使用真实 NVMe。该结果只证明当前功能回归通过，不继承旧
8 vCPU 性能 gate 的吞吐结论。
engine 用 `[device.*] pci_bdf` 生成 SPDK/DPDK allowlist 并写入 `spdk_env_opts`；`[spdk]`
只描述 env 驱动和 hugepage 预算，不通过环境变量注入。

`[numa]` 是当前实现的可选兼容 section；缺省时等价于 `local-first`、`auto` 拓扑、禁止
静默 remote DMA、bounded fallback 和 hybrid polling。当前阶段已在 SPDK runtime 启动时
读取 Linux sysfs 的 online node/CPU、CPU→node placement 和 PCI device NUMA node，输出
topology snapshot；可解析 device node 时按本地 CPU 选择 data reactor，无法解析时使用
确定性的 device-index fallback；data loop 已支持 `active`/`hybrid`/`interrupt` 三种
polling policy。`local-only` 在拓扑不可读或 device node 未解析时 fail closed。尚未宣称
已经完成 per-node hugepage/DMA 分配、remote-bytes 采样或多 node 证据；多 NUMA gate
暂缓，不阻塞当前单节点功能和性能开发。后续 UUSE 配置统一按 section namespace
添加到该 `.conf` 并提升 `schema_version`，例如 `[recovery]`、`[cache]`、
`[frontend.rocksdb]` 和 `[frontend.fio]`；不得另建第二个配置文件来绕过 schema 校验。
`data_reactors=0` 使用 nproc 规则，`data_cpus` 必须匹配 R。

### 15.2 C++ ABI

```text
libuuse_core.so       C++20 core
libuuse_rocksdb.so    RocksDB 8.9.1 adapter
fio-uuse.so           FIO external engine
uusectl               format/check/dump/import
```

- public headers 只暴露 C++20 standard-library types 和 UUSE types，不暴露 SPDK；
- `UUSE_CORE_ABI_MAJOR=2`，构建包记录 compiler、libstdc++ 和 RocksDB 版本；
- 默认 visibility hidden，仅 public header 标记 `UUSE_API` 的符号导出；version script
  导出 `uuse::*` 公共 C++ 符号和 `extern "C"` 工厂/版本符号，保证
  `libuuse_rocksdb.so`/`fio-uuse.so` 在同一 ABI 组合内可链接；
- 不承诺跨 toolchain 的 C++ binary compatibility；若未来需要其它语言，再新增薄的
  C ABI，不把它倒灌为 core 主 API；
- core `-std=c++20 -fno-exceptions -fno-rtti`；adapter/fio 按各自依赖单独编译；
- Make/CMake 必须都能从新 `src/` 构建，旧 `src_legacy_frozen/` 不在 source list；
- `uuse_db_bench`/`uuse_ldb` 只在 RocksDB 8.9.1 开发头可用时构建，core target 不包含
  RocksDB header。

### 15.3 代码处置清单

| 旧内容 | 处理 |
|---|---|
| `src_legacy_frozen/` | 本地冻结、Git ignore、只读参考 |
| `uuse_op`、actor ring、parent completion | 提取概念，C++20 重新实现 |
| inode scheduler interval algorithm | 提取算法，合并为唯一 C++ scheduler |
| shared DMA arena、dirty epoch、durability order | 提取设计，接到新 raw-bdev owner model |
| stats/trace/log/CRC | 复用经过验证的观测思想，重写 C++接口 |
| `uuse_runtime.c`、`uuse_io.c`、`uuse_meta*.c` | 不直接复用，按新 owner/extent/journal 重写 |
| `uuse_preload.c`、`uuse_fd.*`、`uuse_lib.*` | 删除 |
| `uuse_ticket.*`、range lock、POSIX lock/gate | 删除；只提取必要 interval 算法 |
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

2026-09-18 旧 VM 的正式性能口径固定：8 vCPU、真实 NVMe BDF `0000:0b:00.0`、`Nvme0n1`、O_DIRECT、256 MiB、
shared-file、randrw 50/50、4K/8K/16K/32K/64K、合计 QD=32（UUSE 8 jobs × iodepth 4，
raw 1 job × iodepth 32）、10 秒、5-run median、trace/stats off。2026-09-18 优化复测前
的 32 jobs × iodepth 1 口径保留为历史对比，不再作为主 gate 形状。
2026-09-29 gate 形状修订（依据 `doc/performance/hfs2_8j4_shape_attribution_20260929.md`
和 `doc/review/hfs2_gate_shape_revision_review_20260929.md`）：原口径把 UUSE 的
8 jobs × QD4 与 raw 的 1 job × QD32 跨形状相比，但 shape sweep 证明 raw 在同一
8j4 形状自身塌陷约 43%（113.9k → 65.0k IOPS，p99 3.46 ms），UUSE 反而快约 26%；
因此正式 gate 改为**同形状对照**：主形状为双侧 1 job × QD32（raw 可持续的形状），
扩展性形状为双侧 8 jobs × QD4，两个形状分别判定并同时报告 UUSE/raw 与 p99 比值。
raw 在 8j4 形状的塌陷作为已知边界记录，不用跨形状比值给任一侧加分。
UUSE 正式 `.conf`
的 reactor/CPU 必须与测试 cpuset 对齐（`config/uuse_nvme.conf`：`metadata_cpus=0`、
`data_reactors=1`、`data_cpus=1`、`frontend_cpus=3`，当前 VM 为 6 vCPU），避免越界 pin
覆盖 `taskset`。raw 对照使用同一设备
的 SPDK bdevperf 或官方 fio SPDK plugin，固定相同 bs/iodepth/jobs。报告 UUSE/raw、
read/write p50/p99、CPU%、RSS、errors、submit/complete、journal bytes/op。

多 NVMe 验收至少覆盖单盘、双盘和配置中全部 data device 三种 device set；每次记录
`metadata_devices`、`data_devices`、placement、每盘容量和每盘 submit/complete。任一
配置设备不可用、未被正确独占或数据落到了未声明设备时，功能验收失败。

主 gate（2026-09-29 起为同形状对照）：4K shared-file randrw 在**同一 jobs×iodepth
形状**下 UUSE/raw ≥85%，CPU ≤500%（8 vCPU 口径），errors=0，submit/complete 闭合，
UUSE p99 不高于 raw 基线的 110%。主形状 1 job × QD32，扩展性形状 8 jobs × QD4，
两个形状分别判定、同时报告。当前同会话结果（2026-09-29，6 vCPU VM、真实 `Nvme0n1`）：
1j32 UUSE/raw = 90.6%、p99 比值 1.00；8j4 UUSE/raw = 120.8%、p99 比值 0.19
（raw 在该形状 p99 3.46 ms）。如果结构/恢复门禁失败，即使 IOPS 更高也不接受。

### 16.3 RocksDB 验收

由 custom driver 注入 UUSE Env：

```text
uuse_db_bench --uuse-config=config/uuse_nvme.conf \
  --db=/uuse/rocksdb \
  --benchmarks=fillseq,readrandom,overwrite,readwhilewriting \
  --num=20000 --threads=8 --use_direct_io_for_flush_and_compaction=1

uuse_ldb --uuse-config=config/uuse_nvme.conf \
  checkconsistency --db=/uuse/rocksdb
```

`uuse_db_bench` prints a compact RocksDB-style result line by default and suppresses
routine UUSE lifecycle/checkpoint messages; pass `--verbose` when detailed runtime
diagnostics are required. `UUSE_LOG_LEVEL=error` can also be used to keep the
benchmark output quiet in scripted runs.

必须覆盖 create/write/close/reopen、SST read、WAL sync、flush/compact、rename/delete、
normal restart、SIGKILL recovery、跨进程顺序 reopen 和 checkconsistency。Stock db_bench
没有注入 UUSE Env 时，其结果无效。

### 16.4 Crash 和参考模型

至少在以下位置故障注入：

1. data child 完成但 device flush 前；
2. data flush 完成但 metadata commit 前；
3. journal COMMIT 写入但 journal flush 前；
4. checkpoint inactive slot flush 后、superblock publish 前；
5. clean flag 写入前。

每个断点检查 namespace、extent bitmap/state、logical/durable size、orphan reclaim、
generation、CRC 和 RocksDB reopen。ext4 上运行同一逻辑 workload 对照语义，不把 ext4
性能当作 UUSE 目标。

### 16.5 Tracepoint 和性能归因设计

trace 的目标是把“总吞吐上不去”分解到具体层级，避免再次出现“只知道掉了 40%，不知道
时间花在 ring、scheduler、DMA 还是设备”。tracepoint 必须覆盖 frontend → client →
ring → actor → scheduler → DMA → planner → bdev → completion → frontend 的完整链路。

设计原则：

- 默认 off；off 时热路径只读 per-reactor 的 cached bool，不做 per-event atomic/RMW、
  不分配、不取锁；开启/关闭只在 control phase 切换；
- 每个 event 写定长二进制 record，只含标量，不做格式化和字符串/指针编码；
  格式化与聚合全部离线完成；
- 每个 reactor/client 有独立 SPSC trace ring + 后台 writer；ring 满或 writer 落后时
  计数 `trace_dropped` 并丢弃，绝不阻塞 I/O；
- `trace_sample=N` 只作用于高频点（submit/complete/copy），低频点（journal/checkpoint/
  flush/mount/recovery）必须全量；
- trace 不得改变 I/O 顺序、资源生命周期或 `spdk_submit == spdk_complete`；trace 数据
  不参与恢复；
- event id 稳定，只增不改；record 带 `format_version` 和 TSC，TSC 使用同 CPU invariant
  rdtsc，并在 session 开始写 wall-clock 校准点；manifest 同时记录 TSC 频率和
  source→OS thread 映射；
- 正式性能数字必须 trace off；归因跑使用采样或短时全量。

record（示意）：

```cpp
struct TraceRecord {
    std::uint16_t event_id;
    std::uint16_t format_version;
    std::uint32_t shard;      // reactor/client id
    std::uint64_t tsc;
    std::uint64_t arg0;       // offset / bytes / request id ...
    std::uint64_t arg1;
    std::uint64_t arg2;
    std::uint64_t arg3;       // status / errno / device id ...
}; // 48 B
```

点位清单（按链路顺序）：

| 层 | tracepoint | 记录内容 | 归因用途 |
|---|---|---|---|
| frontend | `FE_SUBMIT` / `FE_REAP` | file_id, op, offset, len, client | 入口吞吐、大小分布、调用方 |
| client | `CQ_PUSH` / `CQ_REAP` | req_id, status | 完成到 reaper 的等待 |
| ring | `RING_RESERVE` / `RING_DOORBELL` / `RING_BUSY` | slot, items, depth | 入队背压和唤醒批量 |
| actor | `ACTOR_BATCH` | shard, dequeue 数量 | actor 消费批量和空转 |
| scheduler | `SCHED_WAIT` / `SCHED_ADMIT` / `SCHED_DONE` | inode, range, kind, wait_tsc | 依赖等待、重叠冲突 |
| DMA | `DMA_ACQUIRE` / `DMA_BUSY` / `DMA_RELEASE` | class, wait_tsc, arena_bytes | arena 容量与等待 |
| planner | `EXTENT_PLAN` | children, direct/bounce, device_id | extent 拆分与 RMW 成本 |
| bdev | `BDEV_SUBMIT` / `BDEV_COMPLETE` | device_id, len, status | 设备延迟与 QD |
| parent | `PARENT_COMPLETE` | pending, first_error | child 聚合与错误传播 |
| copy | `COPY_IN` / `COPY_OUT` | bytes, tsc | bounce 成本与比例 |
| metadata | `META_TX_BEGIN` / `META_TX_COMMIT` / `JOURNAL_APPEND` / `JOURNAL_FLUSH` | tx 项数, bytes | metadata actor 吞吐与写放大 |
| checkpoint | `CHKPT_BEGIN` / `CHKPT_END` / `CHKPT_BYTES` | generation, bytes | checkpoint 长尾停顿 |
| durability | `FSYNC_BEGIN` / `FSYNC_END` / `EPOCH_PUBLISH` | file_id, epoch | sync 分段耗时 |
| recovery | `RECOVERY_BEGIN` / `RECOVERY_END` | checkpoint generation, replay tx | mount 时间分解 |
| error | `BACKEND_ERR` / `RETRY` / `BUSY_REJECT` / `TRACE_DROP` | errno, retry count | 失败与背压分布 |

Repair-94 追加不改变既有点位语义的请求关联点：
`REQUEST_SHARD_SUBMIT/COMPLETE` 的参数为 `(request_id, file_id, child_index, bytes)`，
`REQUEST_BDEV_SUBMIT` 为 `(request_id, child_index, offset_blocks, blocks)`，
`REQUEST_BDEV_COMPLETE` 为 `(request_id, child_index, errno, bytes)`，
`REQUEST_PARENT_COMPLETE` 为 `(request_id, bytes, errno, child_count)`。这些点位使用
append-only event id，并按 request id 成对采样，离线工具可据此计算 frontend、parent、
shard 和 bdev 分段延迟；旧的 `BDEV_*` 和 `ShardBackend*` 点位保持原参数布局。

Shard-9.5 追加（同样 append-only，旧点位参数布局不变）：

- `SYNC_PHASE = 60`：一次 barrier 分段耗时，参数 `(phase, epoch, elapsed_ns, extra)`；
  phase 取值 `1=join_wait, 2=window, 3=staged_take, 4=journal_append,
  5=prepare_durable, 6=require_full, 7=snapshot, 8=ckpt_write, 9=flush,
  10=publish`（`src/common/trace.hpp` 的 `SyncPhaseId`）；
- `SYNC_BATCH = 61`：一次 barrier 服务的 sync 批次，参数
  `(served_syncs, target_epoch, staged_records, checkpoint_kind)`，kind `0=none,
  1=delta, 2=full`；
- `CHKPT_FREEZE.arg3` 由 0 改为 namespace-lock freeze 纳秒数（原字段未使用）。

归因流程不变：`stats` 先定位层级 → `trace_sample` 拉取分段分布 → 结构修复 →
trace off 重跑正式门禁；MinIO 侧实例见
`doc/performance/minio_shard95_attribution.md`。

关键延迟链（每段独立统计 p50/p99，不允许只报总数）：

```text
T0 FE_SUBMIT → T1 RING_RESERVE → T2 ACTOR_BATCH → T3 SCHED_ADMIT
→ T4 DMA_ACQUIRE → T5 BDEV_SUBMIT → T6 BDEV_COMPLETE → T7 PARENT_COMPLETE
→ T8 CQ_REAP
```

派生指标：

```text
enqueue     = T1 - T0      // frontend 提交到 ring
actor_queue = T2 - T1      // ring 等待
sched_wait  = T3 - T2      // inode 依赖等待
dma_wait    = T4 - T3      // arena lease 等待
submit      = T5 - T4      // planner + bdev submit
device      = T6 - T5      // SPDK/NVMe 延迟
complete    = T7 - T6      // parent 聚合
reap        = T8 - T7      // 完成到 frontend 消费
```

常驻计数（stats，trace off 也可报告）：

- per-shard：outstanding、ring high watermark、actor busy/idle、busy reject；
- per-device：submit/complete/QD/延迟、errno；
- scheduler：active、dependency waits、pending peak；
- DMA：arena bytes/high watermark、per-class acquire、busy；
- metadata：tx/op、journal bytes/op、checkpoint bytes/时间、group commit 批量；
- frontend：ReadAsync/Poll/AbortIO、FIO busy、copy bytes、bounce/direct ratio；
- error：backend errno 分布、retry、`trace_dropped`。

输出与工具：

- session 目录含 `trace-manifest.json`（format version、wall-clock、TSC 频率、
  source thread 映射、recorded/dropped/closed）和 `trace-<shard>.bin`；默认写
  `trace_dir`，正式 VM 为 `/mnt/uuse-data`；
- 离线工具解析 record、校验 manifest 与文件记录数一致、合并多 shard 时间线，输出
  frontend/parent/shard/bdev 的 p50/p99 和 per-request 分段，不在线格式化；
- 固定归因流程：`stats` 先定位层级 → `trace_sample` 拉取分段分布 → 结构修复 →
  trace off 重跑正式门禁。

门禁：

- trace off 时无 per-request allocation、无 per-event atomic，`trace_enabled` 分支可被
  branch predictor 稳定预测；
- trace on 时不改变 `spdk_submit == spdk_complete`，不引入 UAF/double resume；
- 采样运行必须检查 `trace_dropped`；全量短跑要求 `trace_dropped == 0`。

### 16.6 TiKV 验收

TiKV frontend（Phase 8，§21）按以下矩阵验收：

- 功能：最小 PD+TiKV 启动、TiKV 写负载与重启持久、kill -9 恢复（无已提交写丢失）、
  跨进程顺序 reopen；SQL/DDL/事务/备份矩阵按执行计划 Shard-119；
- 崩溃：在 data child 完成、RocksDB WAL sync 前后、MANIFEST/atomic flush 与目录
  fsync 边界注入，检查 TiKV 重启可恢复、`uusectl check` `corruption=none`；
- 无宿主落盘：strace 校验 `storage.data-dir` 内文件（db/raft/snap/import/lock）全部
  落 UUSE namespace；宿主白名单外出现 datadir 文件即失败；
- 禁用项：encryption/cloud/mmap/第二实例/`tikv-ctl` 探针必须 fail-closed；
- 回归：每个 shard 出口执行 shard 计划 §0.3 的 L1–L4 门禁；RocksDB/MySQL 正式口径
  回退 >5% 即失败。

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
| recovery 错误 | A/B、CRC、commit marker、crash matrix、uusectl check |
| 旧功能回流 | 新库名、新 magic、新 source tree，不编译 frozen src |
| worker-owned channel 生命周期 | §5.3 非拥有引用 + registry 主线程 retire；单测覆盖线程退出、context 析构与建链回落 |
| shared frontend 偶发丢唤醒 | 有界 poll + 压力重复回归；真实 NVMe 复现时按 bug 记录 |
| 跨会话性能环境漂移 | 同会话交错 A/B、native 首尾校准；跨会话数字不得直接比较 |
| MySQL `os_file` seam 遗漏绕过点 | 审计清单 + 每 shard strace 校验；datadir 文件必须落 UUSE |
| `File::sync` 无 data-only barrier | 组提交先行；任何 core 改动必须通过 RocksDB L4 无回退门禁 |
| MySQL frontend 回归影响 RocksDB | shard 出口 L1–L4 门禁；默认值/ABI 冻结，MySQL 独立 `.conf` |
| MySQL 提交路径剩余差距 | 已定位为 barrier 的设备往返（耐久性必需）+ SPDK 轮询；异步 journal writer 与 per-file sync 均实测回退（见 baseline 台账），不重复尝试 |
| TiKV Rust FFI 生命周期/UB | 显式 C ABI + opaque handle + 版本检查；ASAN/TSAN 用例 |
| TiKV 双 RocksDB 实例 + 高线程数 | per-thread registry 上限 + 共享回落 + 计数（复用 §20.4 机制） |
| TiKV 绕过点遗漏 | 审计清单 + 每 shard strace 校验；datadir 文件必须落 UUSE |
| TiKV punch hole 不支持 | 按审计 fail-closed 或显式降级并记录，禁止静默 no-op |
| TiKV 与 3FS 实验室资源冲突 | 独立盘与时段约束；不占用已声明盘 |
| TiKV 上游 rebase 冲突 | 改动限定 FS 工厂 + shim + 独立 patch 目录；版本冻结 |

### 17.3 立即停止条件

任何以下情况都停止性能测试并回到结构/正确性修复：

- data mismatch、sparse read 非零、RMW 丢数据；
- metadata corruption、generation/CRC 不一致、journal replay 错误；
- parent/child、coroutine frame、DMA lease、ring slot 泄漏或 double resume；
- `spdk_submit != spdk_complete`、backend errno 丢失、NVMe 无法释放；
- RocksDB/FIO error mapping、cancel、AbortIO、directory fsync 不完整；
- MySQL datadir 文件落到宿主 OS、MySQL 路径 data mismatch 或崩溃恢复失败；
- RocksDB/FIO 正式口径回退超过 5%（Phase 7 shard 门禁失败）；
- 旧 Blobstore、POSIX fd 或 preload 代码重新进入 UUSE 2.0 build；
- TiKV 路径 data mismatch、崩溃恢复失败（store 无法启动或丢失已提交写）；
- TiKV datadir 文件落到宿主 OS、或禁用项静默降级。

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
- `uusectl format/check/dump/import`；
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
- custom `uuse_db_bench`/`uuse_ldb`、WAL/flush/compact/recovery。

### Phase 6：收口和归档

- 2026-09-17：Shard-79 至 Shard-84 已完成主要实现并通过对应的单盘真实 NVMe
  最小回归，但 data-flush 崩溃断点、完整 RocksDB/FIO 错误与取消矩阵仍未闭合；
  Shard-85 已完成单盘 workload 矩阵与 fail-closed 配置校验，性能 gate 与多盘故障
  组合仍未完成。验收 VM 现提供两块独立控制器：`Nvme0n1`（`0000:0b:00.0`）和
  `Nvme1n1`（`0000:13:00.0`）；双盘 `.conf` 的 format、mount、metadata replica
  检查及双盘 FIO verify 已通过。双盘完整矩阵、metadata 故障组合和性能 gate 仍是
  硬门禁，不以 Malloc/Aio 或重复 BDF 代替。真实双盘验收还暴露并修复了生命周期
  superblock generation 逐盘递增导致的副本漂移；RocksDB smoke 中“12 字节写入后
  free space 必须立即减少”断言与 extent 粒度统计不一致的问题已修正，当前 free-space
  reclaim 与双盘 `uusectl check` 均通过。
  缺失 BDF、合法但落后 generation、双槽 CRC 损坏三类双盘 fail-closed 探针也已通过；
  双盘完整 workload 矩阵和性能 gate 仍未完成。
  双盘回归期间还修复了控制 flush 的块/字节单位错误、镜像 superblock 未应用目标盘
  geometry，以及 mirrored journal 持有临时 geometry 引用导致的悬空引用；修复后
  双盘 RocksDB smoke 与 `uusectl check` 均通过。
  双盘 `uuse_db_bench` 的 fillseq/readrandom/overwrite/readwhilewriting、SIGKILL 后
  checkconsistency 与 `uusectl check` 也已通过；完整错误/取消矩阵和性能 gate 仍未闭合。
  双盘 FIO 完整 workload 矩阵已收集到 320 项 PASS、80 项 RESOURCE_BOUNDARY、0 项
  功能失败；结果归档在 `/mnt/uuse-data/uuse-fio-matrix-dual-20260917.log`。
  Shard-79 另以 `UUSE_TEST_FAIL_FLUSH_DEVICE` 在真实 NVMe 的 `file_api_test` 中覆盖
  data child 完成后、device flush 提交前的 `datasync` 失败边界；注入仅在显式测试
  环境变量下生效，清除后同一数据可完成 datasync/sync，不把该顺序注入等同于掉电
  或其余四个 crash 断点。
  File API 的同步 `read/write` 也已尊重 pre-cancelled `stop_token`，在提交数据面前
  返回 `Cancelled`；in-flight 取消仍只由 RocksDB async `AbortIO` 契约负责。
  DataPath 已修正为按请求轮询已启动的 data reactors，不再把所有数据 I/O 固定到
  reactor 0；真实 NVMe FIO smoke 回归通过，性能 gate 仍以正式 raw 对照为准。
  2026-09-18 按统一 CPU 2-3 和真实 `Nvme0n1` 完成 Shard-81 raw/UUSE 各五次采集；
  raw 中位 5151.390 IOPS，但 UUSE external engine 每次仅完成 8 KiB，总请求量未形成
  可证明的 32 jobs 有效并发，UUSE/raw=0.000019，性能 gate 结构性失败。raw 仅有 aggregate p99，
  读写分离 p99 与 submit/complete 尚未闭合；不得据此宣称性能 gate 通过。证据见
  `doc/performance/hfs2_shard81_formal_gate_20260918.md`。
  随后 Shard-81 的 core-native async backend、FIO `queue/getevents/event`、close/drain
  和真实 NVMe 生命周期已闭合，但最新五次正式 gate 的 UUSE/raw 中位数为 61.38%，
  read/write p99 约为 raw 的 4.9 倍，性能门禁仍未通过。后续 Shard-86~92 的 trace、
  completion、FIO topology、shard affinity 和 reactor 热路径执行计划见
  `doc/review/hfs2_shard86_92_async_performance_plan_20260918.md`；该计划不放宽本节
  §16 的正式门禁。
  2026-09-18：Shard-86 已落地请求级 trace 基础：固定 48B record、每 producer 独立
  SPSC ring、后台 writer、`trace-manifest.json`（含 wall-clock 校准、recorded/drop/closed）
  和 `uuse_trace_dump` 离线统计工具；现行 Client、completion notifier/queue、actor-local
  bdev 路径已补齐 submit/complete/reap/notify/wait/spin 观测。真实 NVMe 32 worker
  trace-on 短跑 `queued=reaped`、`outstanding=0`、`token_mismatch=0`、`dropped=0`；
  trace 默认关闭，Shard-81 正式性能 gate 仍按 trace/stats off 执行。
  2026-09-18：Shard-87 将 completion ring 的 wake-arm 竞态改为清除后二次检查，
  并将固定 1024 次 active spin 改为 32~1024 的自适应预算；真实 NVMe 短跑平均预算约
  32.1、trace drop=0、completion closure 完整。notify/wait/read 仍接近一 IO 一次，
  该拓扑问题明确移交 Shard-88，不把 spin 优化误记为 notifier batching 完成。
  2026-09-18：Shard-88 为外部 frontend 增加窄化的 `ClientOptions::completionWake` 唤醒
  回调和 `AsyncCompletionQueue::pushWithWakeHint()`；配置该回调的 Client 不创建自己的
  eventfd。FIO adapter 维护 process-wide endpoint registry，但每个 worker/Client 独占一个
  futex endpoint，callback 只唤醒拥有该 Client 的 worker，不得使用广播唤醒；endpoint 数量
  超限时回退到 per-Client eventfd。endpoint 只在 Client close/drain 完成后释放，保证回调
  context 生命周期安全。真实 NVMe 32 jobs/q1 的 randrw 短跑约 65.7k IOPS，closure 为
  `queued=terminal=published=reaped`、`outstanding=0`、`token_mismatch=0`；`ctest`
  23/23 和同步 `uuse_async=0` 回归通过。该结果仍低于 raw 75% 中间目标，Shard-81 正式
  性能 gate 继续保持未通过，后续由 Shard-89/90 处理 file-shard/reactor 热点。
  2026-09-18：Shard-89 追加 `ShardMap`、`ShardPump`、`ShardBackendSubmit`、
  `ShardBackendComplete` tracepoint。真实 `Nvme0n1`、2 data reactors、32 jobs/q1
  shared-file trace-on sample=64 的 `fileId=2 → shard=0`、backend submit/complete
  和 pump work 证据确认 shard 0 是唯一实际工作的 file owner；shard 1 的 pump work
  与 high-watermark 均为 0，不能把它计入有效利用率。现有 actor-local backend API 要求
  在目标 reactor 调用，不能只修改 file hash 将 child 随机迁移；同一 file 的 ordering、
  cancellation、late completion 边界继续保持单 owner，跨 reactor dispatch 留给后续
  runtime 设计。该观测跑 closure 完整、trace dropped=0，未改变正式数据路径。
  2026-09-18：Shard-90 将 actor-local data I/O 的热路径线程归属校验改为
  `spdk_get_thread()` 加 data-reactor thread-local marker，移除每 IO 的
  `syscall(SYS_gettid)`；启动阶段仍保留 `osTid` 供诊断 API 和测试使用。data-reactor
  poller 改用一个带 admission 位和 in-flight reader 计数的 reactor-owned atomic 状态字，
  正常 loop 不再获取 `pollerMutex`，clear/stop 通过 C++20 `atomic::wait/notify_all` 等待
  reader quiescence 后才释放 callback/context。真实 NVMe 32 jobs/q1 strace 中 gettid
  从约 65582 次降至 46 次，closure 与 token/outstanding 均闭合；VM ctest 23/23 以及
  连续 10 次 runtime stop/restart 回归通过。该优化未改变 actor-local ownership，Shard-81
  正式性能 gate 仍保持未通过。
  2026-09-18：Shard-91 为每个 Client 预分配 bounded free-slot stack 和固定容量
  requestId open-addressing lookup，正常 slot reserve/reap 不再线性扫描或每 IO 分配；
  tombstone 保持 probe chain，索引异常时保留线性完整性回退。generation/token 校验、
  late completion、credit release、close Busy 和 slot reuse 语义不变。新增 32-slot
  真实 NVMe file API 回归、VM ctest 23/23 及 32 jobs/q1 randrw closure 全部通过；
  单次 smoke 约 63.8k IOPS，仅作为实现验证，不关闭 Shard-81 正式性能 gate。
  2026-09-18：Shard-92 在真实 `Nvme0n1` 上完成 trace/stats off 的 raw/UUSE 五次正式
  gate。raw SPDK 使用 `config/uuse_nvme.json` 作为 plugin 输入，UUSE 使用正式
  `config/uuse_nvme.conf`；两侧固定 CPU 2-3、4 KiB `randrw` 50/50、256 MiB、10 秒和
  32 有效 outstanding。raw 中位数 `109474.152585 IOPS`，UUSE 中位数
  `76893.931820 IOPS`，UUSE/raw=`70.239%`；UUSE 读/写 p99 中位数为
  `3227.648/3227.648 us`，raw 为 `643.072/643.072 us`，p99 约 `5.019x`。
  五次 UUSE 的 accepted/terminal/published、child submit/complete、queued/reaped
  均闭合，outstanding、token mismatch 和 errors 均为 0，驱动为 `uio_pci_generic`、
  hugepages 为 1024。因此生命周期和环境 gate 通过，但 85% 吞吐及 110% p99 性能门禁
  失败；Shard-81 性能仍未完成。详细证据见
  `doc/performance/hfs2_shard92_formal_gate_20260918.md`。
  2026-09-18：Repair-94 扩展 trace format v2，加入 TSC 频率校准、source→thread
  映射、manifest 记录完整性校验和请求关联 tracepoint；`uuse_trace_dump` 现在在
  记录数、source id、closed 状态不一致时失败，并输出 frontend/parent/shard/bdev
  分段 p50/p99。该修复只增强归因证据，不改变 trace-off 性能口径，也不关闭正式 gate。
  2026-09-18：RocksDB `ReadAsync` 已实现 `IOOptions::timeout` 的前端等待语义；超时
  返回 `TimedOut` 后，后台 I/O 继续排空，句柄析构时才回收 worker 和 buffer，且不会
  在 callback 后写回 request result。DataPath 的 `-ETIMEDOUT`、`-ECANCELED`、
  `-EAGAIN` 和 `-ENOSPC` 也分别映射为 core 的 TimedOut、Cancelled、Busy 和 NoSpace；
  RocksDB FileSystem 输出参数空指针统一返回 InvalidArgument。上述边界已由真实
  `Nvme0n1` 回归覆盖。2026-09-18 的联合回归已闭合 Shard-79/83/84 的可重复
  错误、超时、取消组合矩阵，包括超时后 AbortIO、取消抢先于超时、callback-started
  边界、8 句柄批量取消及 RocksDB flush 错误映射；证据见
  `doc/review/hfs2_error_timeout_cancel_matrix_20260918.md`。真实掉电等价、性能
  gate 和完整 stock RocksDB 兼容性仍保持未完成边界。
  2026-09-18：Repair-93～98 完成一轮异步性能修复收口。Release `fio-uuse` 构建已
  移除默认 `-O0 -g`；completion wait、request lookup/reap、trace v2 关联和 FIO
  per-job 文件拓扑均完成实现。VM Release 全量 CTest `24/24`、runtime stop/restart
  `10/10` 通过；真实 `Nvme0n1` 的 per-job trace 产生 14 条 `ShardMap`，覆盖 7 个
  file id 和两个 shard，manifest `closed=true`、`dropped=0`。随后 raw/UUSE 各五次
  正式采集均无错误，UUSE 的 accepted/terminal/published、child submit/complete、
  queued/reaped、outstanding 和 token mismatch 全部闭合；但中位 UUSE/raw 吞吐为
  `60.5303%`，read/write p99 比值为 `5.525x`，仍未达到 `85%` 和 `110%` 门槛。
  该结果确认生命周期和环境边界已修复，但不关闭 Shard-81 性能 gate；完整证据见
  `doc/performance/hfs2_repair98_final_matrix_20260918.md`。
- 所有 gate 通过后删除新树中最后的旧兼容残留；
- `src_legacy_frozen/` 保持本地忽略或单独归档，不回到 product build；
- 更新架构索引、性能基线、支持边界和迁移说明；
- UUSE 2.0 才能作为正式产品线，旧 HFS 1.x 只保留归档 artifact。

### Phase 7：MySQL 8.0.46 InnoDB frontend（Shard-99 起）

- 设计前置：本文 §3.1/§3.2/§17/§20 更新完成后才能动代码；
- 基座：VM 内 MySQL 8.0.46 fork + 独立 `config/uuse_mysql.conf`；先固化 RocksDB/FIO
  基线作为回归对照；
- InnoDB `os_file` seam（handle 表、路径路由、同步门面）、审计的 7 处绕过点、
  模拟 AIO（native AIO 关闭）；
- per-thread Client registry 与停机顺序、功能矩阵、性能与 durability 评估、收口；
- 每个 shard 出口执行 RocksDB L1–L4 无回退门禁；执行计划见
  `doc/shard/hfs2.0_mysql_innodb_shard_plan.md`。

### Phase 8：TiKV frontend（Shard-113 起）

- 设计前置：本文 §3.1/§3.2/§16/§17/§21 更新完成后才能动代码；
- 基座：VM 内 TiKV 8.x LTS fork（tag/commit 与 Cargo.lock 锁定的 rust-rocksdb
  依赖冻结）+ 独立 `config/uuse_tidb.conf`；先固化 CTest/FIO/RocksDB/MySQL 基线
  作为回归对照；
- `components/file_system`/`RocksDBEnv` seam：C ABI shim、Rust `UuseFileSystem`、
  路径路由与宿主白名单、绕过点修复、写/恢复路径；
- per-thread Client registry 与停机顺序、功能矩阵、性能与 durability 评估、收口；
- 每个 shard 出口执行 RocksDB/MySQL L1–L4 无回退门禁；执行计划见
  `doc/shard/uuse_tidb_shard_plan.md`。

## 19. 最终定位

> UUSE 2.0 是一个 C++20 coroutine-first、运行在应用进程内、直接管理 SPDK bdev/NVMe
> 的数据库文件存储引擎。它以 per-shard actor、单一 inode dependency scheduler、
> bounded DMA arena、extent allocator 和可恢复 raw-bdev metadata 为核心，为 RocksDB
> 8.9.1 和 FIO 3.42 提供明确的异步文件契约。它不是通用 POSIX 文件系统，不使用
> Blobstore，也不兼容 HFS 1.x 的 src、镜像、fd ABI 或 preload 路径。

2026-09-24 起，Phase 7 在同一 core 上增加 MySQL 8.0.46 InnoDB frontend（§20）：它以
fork 的 `os_file` seam 接入，不提供 POSIX 兼容层，也不改变 RocksDB/FIO 的契约与
性能口径。

2026-10-09 起，Phase 8 在同一 core 上增加 TiKV frontend（§21）：它以 TiKV
`components/file_system`/`RocksDBEnv` seam 接入，不提供 POSIX 兼容层，也不改变
RocksDB/FIO/MySQL 的契约与性能口径。

UUSE 2.0 的第一条实现原则是：旧代码可以冻结，但不能以兼容性名义继续污染新架构；
第二条原则是：coroutine 只用于表达非阻塞状态机，所有资源、错误、恢复和完成边界都
必须可计数、可回放、可验证。

## 20. MySQL 8.0.46 InnoDB frontend（Phase 7）

> 审计依据：`doc/review/hfs2_mysql_innodb_audit_20260924.md`；
> 执行计划：`doc/shard/hfs2.0_mysql_innodb_shard_plan.md`。
> 本节只定义 Phase 7 的接入边界与契约，不修改 §11/§12 的 RocksDB/FIO 契约、默认值、
> 配置 schema 与性能口径。

### 20.1 定位与范围

- MySQL 8.0.46 的 InnoDB 以 UUSE 作为 datadir 存储，接入方式为 fork MySQL 并在
  InnoDB `os_file` 文件层实现 UUSE backend；不是 POSIX 兼容层，core 不新增
  POSIX `read/write/pread/pwrite` 入口；
- 单进程、单 engine、bdev 独占语义与 §14.3 相同：第二实例、跨进程或同盘共享返回
  `Busy`；fork child 返回 `ForkedProcess`，`--daemonize` 被禁止；
- RocksDB/FIO 路径、`config/uuse_nvme.conf` 与默认值不被修改；MySQL 使用独立
  `config/uuse_mysql.conf`。

### 20.2 os_file seam 契约

seam 位于 `storage/innobase/os/os0file.cc`（审计 §5.1）：

- `os_file_t`/`pfs_os_file_t` 由 fd 改为 handle 索引，映射到 `uuse::File`；不使用
  host fd，`OS_FILE_CLOSED` 由 handle 表表示；
- 路径路由：`--datadir`/`Fil_path` 前缀 → UUSE namespace；socket、pid、error log、
  slow log、`--tmpdir`、binlog 使用宿主绝对路径，不进入 UUSE；
- 必须覆盖的 seam 组：create/close/read/write/pread/pwrite/fsync/flush/get_size/
  set_size/truncate/exists/status/delete/rename/create_directory/scan_directory/lock/
  copy/`SyncFileIO::execute`；
- `os_file_set_nocache` 与 `posix_fadvise` 为 no-op（UUSE 已是 direct、无 host 缓存）；
  `os_file_punch_hole` 不支持，必须 fail closed；
- `os_file_get_status` 返回合成的 size/mode/mtime；UUSE 无权限模型；
- 审计列出的 7 处直接 I/O 绕过点必须在 Phase 7 内关闭，不得把 datadir 文件静默
  落到宿主 OS；
- native AIO（`AIO::linux_dispatch`/`os_aio_linux_handler`）在 v1 关闭
  （`innodb_use_native_aio=OFF`），模拟 AIO 统一收敛到 seam；AIO bridge 属二期。
- Shard-106：seam 的 `read/write` 增加 worker-direct 快路径——File 仍由进程级 owner
  client 持有，调用线程用自己的 per-thread client `submitRead/submitWrite` 后在本线程
  `poll` 收完成；仅用于文件内读与 fixed-overwrite 写（增长写、前端受限或 submit 失败时
  回落同步门面，ABI 与 `File::sync` 语义不变）。为此 `Client::submitRead/submitWrite`
  的 file 归属校验从"同一 client"放宽到"同一 engine"，即 §20.4 的 owner-file 模型：
  槽位、credit 与完成队列仍属于提交方 client。计数器
  `async_reads/async_writes/async_fallbacks` 随 `shutdown complete` 行输出；完整
  AIO bridge（跨线程提交/完成回调）仍属二期。

### 20.3 Durability 契约

- `os_file_fsync`/`os_file_flush` → `File::sync`；`sync` 与 `datasync` 同为 namespace 级
  durable barrier（强于 per-file fsync），ABI 不变；
- barrier 按 §10.2 顺序执行：logical epoch 前进时先 prepareDurableEpoch → 写 checkpoint 记录与
  superblock（defer flush），随后每个设备下发一次 device flush，最后 publishDurableEpoch；
  未前进时只做 device flush。单次 flush 覆盖其提交前所有已完成写，checkpoint 不再自带
  flush；
- data 覆盖由 `SpdkRuntime` 的 per-device `writtenSeq`/`flushedSeq` 判定：data 写完成推进
  `writtenSeq`；`controlFlush` 在提交前采样 `writtenSeq`，成功后把 `flushedSeq` 推进到
  采样值。设备 flush 仅当 `flushedSeq >= writtenSeq`（上次 flush 后无新完成写）才可跳过，
  且 `File::sync` 返回时必须满足该条件；
- 只有 metadata 未变且 data 已覆盖时 barrier 才可早退；任何 write→fsync→掉电路径都不得
  跳过 data flush，原地覆盖写（不改变 logical size）同样必须落盘；
- data-only `datasync`（不发 checkpoint）是本契约的第一阶段；实施不得改变 `File::sync`
  行为与 ABI，且必须通过 RocksDB L4 无回退门禁；
- checkpoint 记录与 superblock 以一次 `JournalDevice::writeBatch` 提交（每个 range 仍是
  独立的设备命令，但并发在飞、只付一次唤醒），默认实现回退为逐条写；
- 诊断：`UUSE_DIAG_SYNC_STATS=1` 时 `Engine::stop()` 打印
  calls/early/joiners/barriers/flushes/flush_skips/checkpoints 与各相位 avg/max
  （barrier/device_flush/ckpt_snapshot/meta_write/ckpt_flush），
  用于核对 flush 覆盖比例与 commit 路径成本拆分；
- 恢复：UUSE journal replay 与 InnoDB redo 双层恢复；kill -9 断点后 datadir 的
  namespace/extent/size 必须守恒，`uusectl check` 无 corruption。

### 20.4 线程与生命周期

- InnoDB 后台线程（page cleaner、log writer、AIO handler、purge、DDL 等）使用
  per-thread `uuse::Client`：拥有线程自建自关；被 context 抢先 retire 的 thread-local
  析构必须为 no-op；
- 打开的 `File` 由进程级 owner client 持有，线程退出时不携带打开文件，保证
  per-thread client 可干净 retire；
- 停机顺序：停新提交 → drain outstanding → close Client → 停 dispatcher →
  `Engine::stop()`；任一层失败 fail-closed，不半量回落；
- 资源上限：线程数 × `maxOutstanding` × 队列容量按配置校验；建链失败回落共享
  dispatcher 并计数/日志；
- `Engine::stop()` 在仍有 client/open file/outstanding 时返回 `Busy`，由 adapter
  有界重试；
- 信号：SPDK/DPDK 线程以空信号掩码运行，进程级 SIGTERM/SIGINT/SIGHUP/SIGQUIT
  会被投递到这些线程；seam 必须在 mysqld 的 `start_signal_handler()` 之后安装转发
  handler（`my_init_signals` 会重置 SIGHUP/SIGTERM/SIGUSR1），把信号
  `pthread_kill` 到 `sig_handler` 线程，使 mysqld 的 `sigwait` 路径触发正常停机。

### 20.5 配置与构建

- MySQL 使用独立 `.conf`（`config/uuse_mysql.conf`），不得修改
  `config/uuse_nvme.conf` 的正式口径；
- 2026-09-25 当前 VM 的 MySQL 验收使用 `VMware NVME_0002`（`0000:13:00.0`）
  和 VM 本地 `/home/uuse` 结果目录；历史 `0000:0b:00.0` 测量只作为旧 VM 记录。
  性能对照必须在同一 VM、同一设备和同一会话重新采集。
- MySQL fork 在 VM 内通过 Git 获取与维护；UUSE core 不引用 MySQL 源码，fork 不反向
  依赖 UUSE 内部头文件；补丁集中在 seam 与 §20.2 的绕过点；
- ABI：Phase 7 只做加法；破坏性变更升 `coreAbiMajor` 并单独评审。

### 20.6 禁用项（fail-closed）

native AIO、页压缩/punch hole、clone、MyISAM/Aria 数据路径、`--daemonize`、
`--innodb-directories` 指向宿主、多实例共享 datadir、把 MySQL 数据文件落到宿主 OS。

### 20.7 验收

- MySQL：initialize/bootstrap、启停、SQL/事务/DDL（含 online）、binlog、逻辑备份、
  跨进程 reopen、kill -9 恢复、`mysqlcheck`；
- UUSE：`uusectl check`、closure（submit==complete）、无 handle/frame/lease 泄漏；
- 设备 I/O 归因：MySQL 使用的同步 `DataPath` 提交也计入 SPDK data bdev 读写
  次数和字节；只在独立的 `stats=true` 诊断运行中记录，正式吞吐门禁保持
  `stats=false`。该计数反映 SPDK 接受的 data I/O，不包含 metadata/journal I/O；
- 回归：RocksDB/FIO L1–L4 无回退门禁（CTest、FIO smoke、readrandom 同会话 A/B <5%、
  正式 8j4 gate）；具体矩阵见 shard 计划 §0.3/§2。

2026-09-25 当前 VM 的 Shard-104/105 结果见
`doc/performance/hfs2_mysql_shard104_105_20260925.md`：功能与两处 SIGKILL
恢复通过；同机重建改动前 core 的五组 8j4 对照未见本次统计改动造成
>5% 吞吐回退。UUSE/raw 中位数约 65.2% 和 p99 约 114.5% 仍未达到
§16.2 总体性能目标，完整进程级 CPU gate 本轮未采集。

### 20.8 前端 zero-copy DMA（buffer pool 来自 UUSE arena）

> 设计评审：`doc/review/hfs2_zero_copy_frontend_review_20260929.md`（P2-1）。
> 目标：消除 InnoDB 页读写的 bounce + copy-in/copy-out，使 `ownsRange` 与
> `spdk_vtophys` 对 buffer pool 直接成立。

- seam 增加 `uuse_vfs::allocDmaBuffer(size)` / `freeDmaBuffer(ptr)`，包装
  `Engine::acquireDmaBuffer`/`releaseDmaBuffer`；
- `buf_pool_t::allocate_chunk` 优先从 arena 申请整块 chunk；失败（bridge 未激活、
  预算不足或非 hugepage 环境）回落到 `ut::malloc_large_page_withkey`，行为不变；
- UUSE-backed chunk 在 `deallocate_chunk` 走 arena 释放，并跳过 `madvise_dont_dump`/
  `madvise_dump` 与 `srv_numa_interleave` 的 `ut::large_page_low_level_info` 路径
  （这些路径只对 ut 自身分配有效）；
- `config/uuse_mysql.conf` 的 `dma_arena_mb` 必须覆盖 buffer pool 与在途 bounce 的总和，
  preflight 按总和校验；当前验证口径为单 NUMA node；
- 释放顺序：InnoDB 必须在 engine stop 前归还全部 UUSE chunk；bridge 未激活时
  `freeDmaBuffer` 为 no-op，arena 析构负责兜底释放，不 double free；
- 验收：`read_direct_user_children > 0`、copy-out 字节显著下降、`uusectl check`
  `corruption=none`、MySQL 功能/恢复矩阵与 §16.3 门禁不回退。

## 21. TiKV frontend（Phase 8）

> 审计依据：`doc/review/uuse_tikv_storage_audit_YYYYMMDD.md`（Shard-114 产出）；
> 执行计划：`doc/shard/uuse_tidb_shard_plan.md`。
> 本节只定义 Phase 8 的接入边界与契约，不修改 §11/§12/§20 的 RocksDB/FIO/MySQL
> 契约、默认值、配置 schema 与性能口径。

### 21.1 定位与范围

- TiDB 的持久化层是 TiKV；TiDB server 与 PD 是独立进程，不进入 UUSE 边界。TiKV 的
  本地存储（kv RocksDB、默认 raft-engine、`db/`、`raft-engine/`、`snap/`、`import/`
  与 `LOCK`）以 UUSE 作为文件存储。接入面按审计
  `doc/review/uuse_tikv_storage_audit_20261009.md` 分为两处：
  （a）RocksDB 侧在 C++ 层实现 `rocksdb::FileSystem`，经 crocksdb FFI 构造 `Env`
  并注入 `engine_rocks` 的共享 env（rust-rocksdb 无通用 Rust Env trait，不存在可
  直接实现的 `RocksDBEnv`）；（b）raft-engine 侧实现 Rust `FileSystem` trait 并替换
  `ManagedFileSystem` 的 base FS。两者都经 `tikv/shim` C ABI 调 UUSE core；不是
  POSIX 兼容层，core 不新增 POSIX `read/write/pread/pwrite` 入口，也不修改 RocksDB
  本体（仅新增 FFI 构造）；
- 单进程、单 engine、bdev 独占语义与 §14.3 相同：第二实例、跨进程或同盘共享返回
  `Busy`；fork child 返回 `ForkedProcess`；
- RocksDB/FIO/MySQL 路径、`config/uuse_nvme.conf`/`uuse_mysql.conf` 与默认值不被修改；
  TiKV 使用独立 `config/uuse_tidb.conf`；
- 版本冻结：TiKV 8.x LTS（Gate 0 冻结 tag/commit 与 Cargo.lock 锁定的 rust-rocksdb
  依赖）；具体 crate/文件/函数名以 Shard-114 审计（冻结版本）为准。

### 21.2 RocksDB Env / raft-engine FileSystem seam 契约

seam 分为 RocksDB Env 与 raft-engine FileSystem 两处（审计
`doc/review/uuse_tikv_storage_audit_20261009.md` §2/§3）：

- UUSE 侧提供 `tikv/shim` 的 C ABI（opaque engine/client/file/dir handle、
  Status/errno 映射、版本与 ABI 检查）；TiKV 侧不直接依赖 C++ 头；
- RocksDB 侧：C++ `rocksdb::FileSystem` 覆盖 metadata/stat、open（read/write/append/
  direct）、read/write、sync/range sync、truncate、allocate、rename、link、lock、
  目录与文件属性；crocksdb 新增 FFI `Env(fs)` 构造，`engine_rocks::get_env` 按
  `storage.uuse.*` 选择 UUSE base env；kv 与 raftdb 共享该 env；WAL/MANIFEST/
  CURRENT/OPTIONS 的写、rename、atomic flush、link 与目录 fsync 必须经 UUSE；
  `File::sync` 触发 durable epoch；
- raft-engine 侧：Rust `FileSystem`（`create/open/delete/rename/reuse/new_reader/
  new_writer` + `Handle.sync/file_size` + `WriteExt.allocate/truncate`）经 shim 调
  UUSE；`ManagedFileSystem` 的 base 按配置选择；
- 缺失项按 Shard-114 审计在 core 侧纯加法补齐（Shard-115）；`punch_hole` 一期
  fail-closed 或显式降级并计数；
- `LOCK` 文件与 TiKV `data-dir/LOCK` 必须与 UUSE 进程锁共同保证第二实例 fail-closed，
  不允许宿主锁绕过；
- mmap 读写、encryption-at-rest、S3/cloud FS 一期禁用；配置出现即失败关闭。

### 21.3 路径路由与宿主白名单

- `storage.data-dir`（含 `db/`、`raft-engine/`（默认）或 `raft/`、`snap/`、`import/`、
  `LOCK`、panic mark 与占位文件）→ UUSE namespace；
- 宿主白名单：日志、status/监控输出、socket/pid、备份/导入的输出目录（显式配置，
  不默认进 UUSE）；白名单之外不得出现 datadir 文件落到宿主 OS；
- 每个 shard 出口用 strace 校验 datadir 无宿主落盘；审计列出的绕过点必须在 Phase 8
  内关闭，不得把 datadir 文件静默落到宿主。

### 21.4 线程与生命周期

- 复用 §20.4 的 per-thread Client registry（惰性创建、weak 注册、线程退出自关、
  上限回落共享并计数）；文件由进程级 owner client 持有，工作线程用自己的 client
  `submitRead/submitWrite` 后本线程 `poll`；
- 信号：SPDK/DPDK 线程以空信号掩码运行，必须在 TiKV 信号处理注册后安装转发
  （SIGTERM/SIGINT；MySQL Shard-103 先例），保证 TiKV 正常停机路径不被默认动作杀死；
- 停机顺序：停新提交 → drain outstanding → close Client → 关 handle →
  `Engine::stop()`；任一层失败 fail-closed；
- `tikv-server` 不作为 daemon/fork 运行；fork child 的 core 调用返回 `ForkedProcess`。

> 实现注记（Shard-118，2026-10-10）：
> - RocksDB adapter 侧已按上款实现 worker-direct per-thread client（DirectWorkerEntry
>   池 + dispatcher），沿用 MySQL Shard-106/108 机制；
> - raft-engine shim 侧一期采用「共享 client + 专用 frontend 线程」：client 由
>   engine state 自带的线程创建与关闭（core 要求 `Client::close` 在 frontend thread
>   调用，而最后句柄可能由任意线程释放）；路径操作由调用线程经 mutex 串行执行，
>   文件数据操作经 `File` 协程在调用线程 `wait()` 自驱动。per-thread registry 对
>   raft-engine 路径操作（open/remove/rename/exists 低频）收益有限，推迟到 Shard-120
>   按路径操作竞争数据决定是否引入；
> - 信号：实测 TiKV 8.5 用 `signal_hook` 注册进程级 handler（专用 signal 线程），
>   SIGTERM 走 `signal_handler.rs` graceful shutdown，SPDK 线程空掩码不影响，
>   无需额外转发（矩阵日志证据 `receive signal 15, starting graceful shutdown...`）。
> - 关闭同步（Shard-118）：TiKV 正常写路径为异步 fsync（raft 日志按 header `sync_log`
>   默认 false；kvdb apply `need_sync = sync_log_hint` 普通写 false），ext4 靠进程退出后的
>   页缓存兜底；UUSE 无页缓存，未 sync 的尾部随进程退出丢失。压缩状态经 admin 同步写
>   落盘后，kvdb applied 回退到压缩点之前 → 重启报
>   `entry at apply index ... doesn't exist`（宿主 ext4 同负载对照通过）。修复：
>   （a）`run_impl` 不再把 engines `take()` 进信号线程（改为 clone，信号线程只用于
>   统计 dump），`TiKV::stop()` 因此能访问 engines；
>   （b）`TiKV::stop()` 在 `raft_server.stop()` 后依次 `KvEngine::sync()`（kvdb WAL）
>   与 `RaftEngine::sync()`（raft 日志）；
>   （c）raft-engine `Engine::sync()` 因 `write` 对空 batch 提前返回而是空操作，
>   改为直连 `pipe_log.sync(LogQueue::Append)`；
>   （d）apply 批次强制同步（`need_sync = !disable_wal`）：UUSE 上异步 apply 写入
>   无法在进程退出后存活，持久 applied 必须跟上 raft 日志压缩进度。
>   补丁：`tikv/patches/apply-uuse-shutdown-sync.py`、
>   `tikv/patches/apply-raft-engine-sync-fix.py`、
>   `tikv/patches/apply-uuse-apply-sync.py`。kill -9 仍可能丢未 sync 的尾部
>   （与 ext4 掉电语义对齐），优雅退出已闭合；apply 同步的吞吐代价由 Shard-120
>   量化后再决定是否收敛为可配置项。

### 21.5 禁用项（fail-closed）

encryption-at-rest、S3/cloud FS、mmap 读写、多进程共享 bdev、`tikv-ctl` 直连 UUSE
数据目录（一期）、把 TiKV 数据文件落到宿主 OS。禁用项配置出现时必须在启动阶段报错，
不得静默走宿主路径或降级。

### 21.6 验收

- 功能/崩溃/无宿主落盘/禁用项矩阵按 §16.6 执行；
- 每个 shard 出口执行 `doc/shard/uuse_tidb_shard_plan.md` §0.3 的 L1–L4 无回退门禁；
- 执行计划、shard 编号与停止条件见 `doc/shard/uuse_tidb_shard_plan.md`。

> **实现状态（Shard-121 收口，2026-10-10）**：Phase 8（Shard-113…121）已完成。
> 功能矩阵 26/26、并发 closure 5/5、生命周期用例 6/6（ASAN/TSAN 干净）、
> CTest 29/29；TiKV 同盘性能 A/B（同步窗口化终态）点读 98.4%、混合 tps 93.7%、
> p99 102.4%/109.5% 全部通过（trace 归因、三变体对比与 RPO 边界见
> `doc/performance/uuse_tidb_baseline_20261009.md` §6）；RocksDB/FIO 门禁无回退。
> 支持范围、禁用项、已知边界（fast-reorg ingest、kill -9 语义、错误面、MySQL
> 冒烟延后等）与补丁栈见 `tikv/README.md` 与 `tikv/patches/README.md`；
> 实现过程中发现并修复的真实缺陷见 `doc/bug.md` #48–#50。