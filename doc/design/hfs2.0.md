# HFS 2.0：SPDK-native 数据库文件存储引擎

> 状态：重构目标设计，2026-09-16 起生效
>
> 本文是 HFS 2.0 的唯一目标架构。它不是当前 POSIX HFS 的增量优化说明，
> 也不承诺兼容旧镜像、旧 ABI 或旧文件语义。
>
> 重构输入：`doc/review/hfs2.0_design_review_20260916.md`；当前代码事实基线：
> `main` @ `74af418`。本文中的新模块和验收数字是目标设计，不表示已经实现。

## 1. 设计结论

HFS 2.0 是一个运行在应用进程内、直接管理 SPDK bdev/NVMe 的异步文件存储引擎。
首批 frontend 是：

```text
RocksDB 8.9.1 FileSystem adapter ─┐
                                   ├─ libhfs_core.so ─ SPDK bdev ─ NVMe
fio external ioengine ────────────┘
```

HFS 2.0 的核心目标不是继续扩展通用 POSIX，而是把数据库和 FIO 真正需要的能力收敛
到一条可验证的异步数据路径：

```text
frontend
  → HfsClient
  → HfsOp
  → bounded shard ring
  → per-shard actor
  → inode dependency scheduler
  → extent map
  → SPDK bdev
  → completion ring
  → frontend callback/event
```

最终产品只提供：

- C 语言 async-first core API；
- RocksDB 8.9.1 `FileSystem` adapter；
- FIO 3.42 external ioengine；
- `hfsctl`、format、check、dump 等管理工具。

最终产品明确不提供：

- `LD_PRELOAD`、POSIX fd/目录/cwd/权限兼容层；
- FUSE、Linux VFS、通用 mmap、native libaio/io_uring ABI；
- SPDK Blobstore 文件抽象；
- 旧 `hfs_lib_async_*`、`hfsAsync*`、registered-DMA 和显式 DMA allocation ABI；
- 多进程共享挂载、daemon、shared-memory 协议；
- 为旧 POSIX 行为保留的 ticket/range/fd compatibility path。

这些删除是产品边界，不是可选配置。

## 2. 取代关系与迁移原则

### 2.1 取代的文档和决策

本文在 HFS 2.0 实施阶段取代下列文档中与其冲突的目标性内容：

| 文档 | HFS 2.0 的处理 |
|---|---|
| `doc/design/hfs_product_architecture_decision.md` | 只保留为 HFS 1.x/POSIX 线的历史决策；HFS 2.0 以本文为准 |
| `doc/design/hfs_posix_only_architecture.md` | 只保留已实施的 HFS 1.x 边界；不作为 HFS 2.0 接口设计依据 |
| `doc/design/hfs_spdk_cpu_efficiency_optimization_design.md` | 其 operation、actor、scheduler、arena、parent completion 经验并入本文；旧实现细节不继承 |
| `doc/shard/hfs_cpu_efficiency_optimization_shard_plan.md` | 作为 shard-67 的实施记录；后续 HFS 2.0 按本文阶段推进 |
| `doc/design/hfs_async_model.md` | 仅抽取异步完成和资源所有权经验；旧导出 ABI 全部废弃 |
| `doc/design/libhfs_ld_preload_design.md` | 不再适用，删除 preload 目标 |
| 当前 Blobstore 元数据文档 | 只作为恢复/测试经验参考；HFS 2.0 不读取旧 Blobstore 镜像 |

同名文档中“当前已支持”的 HFS 1.x 结果必须标注版本，不能被写成 HFS 2.0 已实现。

### 2.2 旧实现与新实现并存

重构期间保留两条可运行但互不耦合的验证线：

```text
HFS 1.x frozen artifact ─ 旧 Blobstore 镜像 ─ 旧 POSIX/MariaDB/RocksDB baseline

HFS 2.0                ─ 新 raw-bdev 镜像 ─ 新 core/fio/RocksDB adapter
```

- HFS 1.x 冻结在迁移开始时的提交，用于回归对照，不再接受新功能；
- HFS 2.0 使用新的库名、头文件、镜像 magic 和测试目录；
- 两条线不能挂载同一 bdev，不能共享 metadata，也不能用兼容分支混合运行；
- 在 FIO、RocksDB 和恢复门禁全部通过前，不删除 HFS 1.x 的外部 baseline artifact；
- 门禁通过后，HFS 2.0 源码树删除旧 POSIX/Blobstore 代码，旧线只作为归档 artifact；
- 不提供在线旧镜像转换。需要保留旧数据时，使用旧线导出到普通文件，再由 HFS 2.0
  工具导入；导入工具不是 core 的兼容层。

### 2.3 反转理由和性能口径

反转 HFS 1.x POSIX-only 决策的原因是性能路径复杂度，而不是为了恢复旧 async 符号：

- 当前 4K～64K POSIX HFS/raw SPDK 只有约 63.4%～70.9% 的同口径吞吐；
- shard-67 已验证 operation lifetime、actor ring、inode scheduler、共享 DMA arena、
  parent completion 和 dirty epoch 是更适合新核心的结构；
- POSIX wrapper、fd、通用权限、mmap 和旧 Blobstore 层不能作为 HFS 2.0 的热路径税；
- HFS 2.0 的性能结论固定为 8 vCPU、真实 NVMe、256 MiB、O_DIRECT、trace/stats off、
  32 job、randrw 50/50、每项 10 秒、5 次 median，并与 raw SPDK 使用相同设备和队列口径。

目标是主基线 4K shared-file randrw 的 HFS/raw ≥85%，同时报告 IOPS、p50/p99、CPU%、
错误数和 submit/complete 守恒。没有完整口径的“快了多少”不作为设计证据。

## 3. 不变的边界和新的产品边界

### 3.1 进程模型

第一版仍是 in-process library，不启动 `hfsd`：

```text
RocksDB/fio
    │
    ├─ adapter/frontend thread
    │
    └─ libhfs_core.so
           ├─ metadata actor
           ├─ data actors
           └─ SPDK environment/reactors
```

一个 engine 独占一个 bdev；同一 bdev 的第二个进程通过进程锁收到 `EBUSY`。daemon、
跨进程 ring、权限租约、故障转移和共享挂载另立架构，不在 HFS 2.0 偷渡。

### 3.2 语义边界

- 文件名、目录、rename、delete 和文件尺寸只服务于 frontend 的文件系统契约；
- 数据 I/O 使用 opaque file handle/file id，不在 hot path 解析路径；
- write 后本进程内立即可读；未 sync 的数据不承诺崩溃后可见；
- `fsync`/`fdatasync` 的持久化边界由 core 明确定义，不能为吞吐跳过设备 flush；
- sparse 区域读取为零；新 extent 在对外发布前必须已初始化或标记为未写入；
- 不提供 mmap；RocksDB 通过 Read/ReadAsync 取得数据；
- 不提供 HFS 1.x 的 fd、stdio、fork 兼容语义。

### 3.3 已知失败经验的硬性约束

HFS 2.0 必须把现有 bug 记录转成设计不变量，而不是等回归测试再次发现：

| 已知问题 | HFS 2.0 约束 |
|---|---|
| `doc/bug.md` #48：多个异步 callback 只唤醒一个 waiter，且丢失 bserrno | completion 使用明确的发布状态和 `wake-all`；parent 保存第一个错误及原始 backend errno，所有 child 都必须 drain |
| `doc/bug.md` #49：事务失败后内存 after-image、孤儿 Blob/占位 fd 未回滚 | raw-bdev transaction 只在 COMMIT durable 后 publish；失败恢复旧 snapshot，RESERVED extent 和未发布 handle 必须回收；HFS 2.0 不存在占位 fd |
| `doc/bug.md` #50：metadata transaction 写入后缺少最后的设备持久化屏障 | `data flush → metadata journal/stable image → journal flush → superblock/checkpoint flush → publish` 是不可变的 sync 顺序 |

任何新实现若无法证明这三条不变量，不能进入 frontend 验收。

## 4. 总体架构和所有权

### 4.1 组件

```text
include/hfs_core.h        稳定 C ABI、opaque 类型、状态和配置
src/core/                 engine、client、operation、handle
src/runtime/              SPDK env、reactor、ring、doorbell、shutdown
src/io/                   planner、DMA、bdev submit、completion、parent
src/metadata/             superblock、journal、checkpoint、namespace、inode
src/storage/              raw bdev、extent allocator、geometry
src/common/               pool、CRC、logging、stats、trace
adapters/rocksdb/         RocksDB 8.9.1 FileSystem adapter
fio/                      FIO 3.42 external ioengine
tools/                    format、check、dump、benchmark
```

单一所有权规则：

| 对象 | 唯一 owner | 其它线程的动作 |
|---|---|---|
| SPDK env、bdev descriptor | engine owner | 通过初始化/控制阶段访问 |
| SPDK io channel、submit | 所属 reactor actor | 通过 ring 投递 operation |
| shard ring head/tail | producer 短预约区 + actor consumer | 不持业务锁等待 ring |
| inode dependency index | inode 所属 shard actor | 插入/完成/唤醒都经 actor |
| namespace、journal、checkpoint | metadata actor | 通过 metadata request/epoch 交互 |
| DMA free map | shared arena owner | 只持有 `HfsBufferRef`，不跨层等待 |
| operation result/lifetime | caller ref + actor ref + parent ref | 引用计数决定释放 |

禁止以新 mutex 解决跨 owner 的问题。每个锁必须写出 owner、等待者和不会持有的资源。

### 4.2 SPDK 线程模型

HFS 不使用 `spdk_app_start` 作为应用主循环，也不让调用者线程直接驱动 SPDK。engine
启动时：

1. engine owner 线程调用 `spdk_env_init`，读取配置并打开 bdev descriptor；
2. 创建一个 metadata reactor 和 `R` 个 data reactor，每个 reactor 是一个 pthread；
3. 每个 pthread 创建自己的 `spdk_thread`，在本线程调用 `spdk_thread_poll`；
4. 每个 reactor 在自己的 SPDK thread 上取得并销毁自己的 `spdk_io_channel`；
5. 实际 bdev I/O 只在拥有对应 channel 的 actor 上提交；
6. caller 到 actor 只写固定 ring，空队列到非空队列时发一次 doorbell；
7. `spdk_thread_send_msg` 只用于 doorbell、启动、停止和低频控制，不用于每个请求；
8. stop 先关闭 ingress，再 drain ring/credit/operation，最后在 owner reactor 上卸载 bdev。

默认 CPU 规则：

```text
R = 1                         nproc == 1
R = 2                         2 <= nproc <= 8
R = min(4, max(2,nproc/4))    nproc > 8
```

`R` 是 data reactor 数，另有一个 metadata reactor。默认 metadata/owner 使用 CPU0，
data 使用 CPU1..R，frontend 不由 HFS 绑定。配置可以显式给出 reactor 数和 CPU 列表；
显式配置优先，但上限仍受 `max_data_reactors` 约束。8-vCPU 默认是 2 个 data reactor，
不因旧 shard-66 短测擅自扩大。

## 5. Core API 和 operation 生命周期

### 5.1 C ABI 原则

Core 不暴露结构体布局、pthread、SPDK 类型或 C++ 类型。所有公开类型是不透明句柄：

```c
typedef struct hfs_engine hfs_engine_t;
typedef struct hfs_client hfs_client_t;
typedef struct hfs_file hfs_file_t;
typedef struct hfs_op hfs_op_t;
typedef uint64_t hfs_file_id_t;
```

状态和结果：

```c
typedef enum hfs_status {
    HFS_OK = 0,
    HFS_EINVAL,
    HFS_ENOENT,
    HFS_EEXIST,
    HFS_ENOSPC,
    HFS_ENOMEM,
    HFS_EBUSY,
    HFS_EAGAIN,
    HFS_EIO,
    HFS_ECANCELED,
    HFS_ETIMEOUT,
    HFS_EFORK,
    HFS_ECORRUPT,
    HFS_ENOTSUP,
    HFS_ESTATE
} hfs_status_t;

typedef struct hfs_iovec {
    void *base;
    size_t len;
} hfs_iovec_t;

typedef struct hfs_result {
    hfs_status_t status;
    int backend_errno;
    size_t bytes;
    size_t completed_bytes;
    uint64_t request_id;
} hfs_result_t;

typedef void (*hfs_complete_fn)(hfs_op_t *, const hfs_result_t *, void *);
```

### 5.2 生命周期 API

```c
hfs_status_t hfs_engine_create(const hfs_config_t *, hfs_engine_t **);
hfs_status_t hfs_engine_format(hfs_engine_t *, const hfs_format_options_t *);
hfs_status_t hfs_engine_mount(hfs_engine_t *);
hfs_status_t hfs_engine_stop(hfs_engine_t *);
hfs_status_t hfs_engine_destroy(hfs_engine_t *);

hfs_status_t hfs_client_attach(hfs_engine_t *, hfs_client_t **);
hfs_status_t hfs_client_detach(hfs_client_t *);

hfs_status_t hfs_open(hfs_client_t *, const char *path, uint32_t flags,
                      hfs_file_t **);
hfs_status_t hfs_create(hfs_client_t *, const char *path, uint32_t flags,
                        hfs_file_t **);
hfs_status_t hfs_close(hfs_file_t *);
hfs_status_t hfs_delete(hfs_client_t *, const char *path);
hfs_status_t hfs_rename(hfs_client_t *, const char *from, const char *to);
hfs_status_t hfs_allocate(hfs_file_t *, uint64_t offset, uint64_t length);
hfs_status_t hfs_truncate(hfs_file_t *, uint64_t size);
hfs_status_t hfs_stat(hfs_file_t *, hfs_stat_t *);
hfs_status_t hfs_sync(hfs_file_t *);
hfs_status_t hfs_datasync(hfs_file_t *);
```

create/open/rename/delete/allocate/truncate/sync 是 metadata API；它们可以在 v1 以同步
方式返回，但内部仍由 metadata actor 执行。数据 API 必须 async-first：

```c
hfs_status_t hfs_read_async(hfs_client_t *, hfs_file_t *,
                            const hfs_iovec_t *, size_t iovcnt,
                            uint64_t offset, hfs_complete_fn, void *, hfs_op_t **);
hfs_status_t hfs_write_async(hfs_client_t *, hfs_file_t *,
                             const hfs_iovec_t *, size_t iovcnt,
                             uint64_t offset, hfs_complete_fn, void *, hfs_op_t **);
hfs_status_t hfs_append_async(hfs_client_t *, hfs_file_t *,
                              const hfs_iovec_t *, size_t iovcnt,
                              hfs_complete_fn, void *, hfs_op_t **);
hfs_status_t hfs_cancel(hfs_op_t *);
hfs_status_t hfs_client_poll(hfs_client_t *, hfs_completion_event_t *,
                             size_t min_events, size_t max_events,
                             uint64_t timeout_us, size_t *returned);
```

callback 和 `hfs_client_poll` 二选一：一个 client 不得同时为同一个 operation 注册两种
完成出口。RocksDB 使用 callback，FIO 使用 poll/event。callback 在不持 core 锁的完成
dispatch 上下文执行；SPDK reactor 只发布结果，不调用外部 C++ 代码。

### 5.3 Operation 状态机

```text
NEW → READY → SUBMITTED → DONE
  │      │          │
  └→ WAIT_DEP ──────┘
  └→ ABORTED
```

`HfsOp` 至少包含：kind、file id、`[offset,end)`、iov 引用、shard、scheduler node、
parent 引用、结果、backend errno、caller/actor/parent 引用计数和可选诊断 sidecar。

- trace/stats off 路径从固定 operation pool 取得对象，不执行每请求 `calloc`；
- callback、poll、timeout、cancel、shutdown 都只释放自己的引用；
- timeout 只改变 caller 看到的状态，不能释放仍可能被 SPDK callback 使用的 operation；
- callback 错误必须保存 SPDK bdev 的原始错误，不能把所有错误折叠成 `EIO`；
- parent 完成前 child 结果写入 parent 聚合器，caller 只等待一次；
- fork child 不得使用继承的 engine/client/file/op，统一返回 `HFS_EFORK`。

## 6. Data plane

### 6.1 Shard ring 与背压

按 `hash(file_id) % shard_count` 选择 shard；同一文件的 scheduler、extent lookup、
append、resize 和 barrier 进入同一 shard。ring 为固定槽位的 MPSC producer / 单 actor
consumer：

1. producer 先取得 operation slot；
2. 预约 ring slot 并写入 `HfsOp *`；
3. 若从 empty 变为 non-empty，发送一次 doorbell；
4. actor 批量快照 head/tail，检查 dependency 和 backend credit；
5. actor 取得 DMA、提交 bdev request，callback 归还 credit 并发布完成；
6. ring 满、operation pool 满或 DMA cap 满时返回有界背压，不无限排队。

资源顺序固定为：

```text
operation slot → DMA buffer（需要时）→ inode dependency → backend credit → submit
```

任何状态都不能持有前一层资源等待后一层资源。ring 满时不持 inode dependency；
backend `-ENOMEM` 时回到 READY，带有次数上限的重试，不在 callback 中递归提交。

### 6.2 依赖调度

HFS 2.0 只保留一套 inode scheduler。它取代旧的 ticket、range stripe、range gate、
appendLock、writeGapLock 等兼容机制：

- scheduler 节点包含 `[start,end)`、读写类型、append/size/barrier 标志和 epoch；
- intrusive interval tree 的节点保存 `maxEnd`，冲突查询为 `O(log A + K)`；
- 不冲突的读可并行；重叠写、RMW、truncate、append 和 barrier 建依赖；
- completion 只检查受影响的后继，不广播扫描整张 active list；
- append/EOF gap 由 barrier/epoch 表达，不再新增专用锁；
- scheduler 只由所属 actor 修改，`HFS_ORDERING`、ticket/range/none 环境变量不进入新 ABI；
- 如需故障诊断的无序模式，只能是明确标记为不保证语义的内部测试构建，不能作为产品配置。

### 6.3 Read/write 和 extent planner

对齐请求按 extent 分段，连续且同属性的范围合并为一个 child；非对齐请求只生成必要
的 head/middle/tail RMW child。planner 输出：

```text
HfsOpBatch
  ├─ parent completion
  ├─ child[0..n)
  └─ buffer references
```

普通 read/write 不走 metadata actor；只有 cache miss 的 extent mapping 才能通过版本化
snapshot 或异步 metadata request 补齐，不能在 data actor 内等待全局 metadata 锁。

文件扩展要求：

- `end <= logical_size`：直接进入 scheduler/data path；
- `end > logical_size`：由同一 inode barrier 重新读取 EOF；
- `offset > old EOF`：先把 `[old EOF, offset)` 标记/写成零，再发布用户数据；
- 逻辑尺寸发布和 extent 可见性由同一 metadata epoch 完成；
- 用户数据写完但未 sync 时，当前进程可读，crash 后由 durable epoch 决定可见范围。

### 6.4 Parent completion

一个逻辑请求只对 caller 暴露一个 parent completion。parent 保存 pending、first error、
completed bytes、child 状态 bitmap 和 operation 引用；child callback 不唤醒 caller。

- 所有 child submit/acquire 失败都要补偿 pending；
- read 在 parent 完成后统一 copy-out，再释放所有 buffer；
- write 在 parent 完成后批量释放 buffer；
- 任一 child 的 backend error 传播到 parent，但所有 child 仍完成 drain；
- cancel/timeout 不绕过未完成 child 的 callback；
- parent/child 在 success、error、shutdown、fork 测试中必须闭合。

## 7. DMA 和内存预算

### 7.1 Buffer contract

`HfsBufferRef` 有两种来源：

1. caller buffer：通过 `spdk_vtophys`、对齐和长度检查后可 direct；
2. shared arena slice：caller 不可 direct 时用于 bounce/staging。

不提供 registered buffer API，不要求 frontend 一次分配一块可 DMA 的完整
`orig_buffer`。FIO 的大 buffer 被按最大 1 MiB child 分段；RocksDB 的异步 buffer 在
callback 前保持有效。

### 7.2 Shared arena

arena 是 mount 级共享、按需增长的 bounded pool：

```text
4 KiB class   small I/O / metadata edge
16 KiB class  common database I/O
64 KiB class  large database I/O
1 MiB class   extent child / fallback staging
```

默认 `dma_arena_mb=512`，配置上限不能超过 hugepage 预算；class 只有首次需求才分配。
共享 bitmap/free map 可先作为最小实现，只有 profile 证明 central owner 是瓶颈时才加
local magazine。arena 满时返回 `HFS_EAGAIN` 或由 frontend 看到有界 busy，不分配无界 heap。

### 7.3 资源预算和预检

engine 启动前计算：

```text
hugepage_required ≥ ceil((SPDK baseline + dma_cap + op_pool + reactor_state)
                          × 1.15 / hugepage_size)
```

预检失败返回清晰错误并打印 required/available，不在运行中把系统盘或根盘当作隐式
交换区。FIO caller memory 不计入 HFS arena cap，但必须计入 frontend 进程的外层 RSS
预算。8-vCPU 验收默认使用 VM `vm.nr_hugepages=1024`，正式数据使用真实 NVMe。

## 8. Metadata、namespace 和 allocator

### 8.1 Namespace

HFS 2.0 的 namespace 是持久 inode + directory entry：

- inode id 64 位单调分配，handle 另带 generation，不能把旧 handle 解析到新对象；
- directory entry 按 name hash 加速查找，同时维护排序 cursor 供 `GetChildren`；
- 路径解析只发生在 metadata actor；数据 operation 只带 file handle/file id；
- `rename` 是单一 journal transaction，源 entry 和目标 entry 不允许中间可见；
- 目录 fsync 通过 directory inode 的 metadata epoch 持久化；
- 删除先从 namespace 摘除，再延迟回收 extent；仍被打开的 file handle 可以继续完成已有 I/O，
  但不会重新出现在路径查找中；
- 超大目录使用分片 hash/有序迭代器，禁止每次 `GetChildren` 扫描全 namespace。

mount 时加载 checkpoint，再 replay journal；内存预算按以下项目核算：

```text
inode table + directory entries + path/name bytes + extent index + allocator index
```

`max_inodes`、`max_namespace_bytes` 和 mount 时间目标必须配置并验收；超过预算时 create
返回 `HFS_ENOMEM`，不能悄悄退回磁盘随机查找。

### 8.2 Extent 和 allocator

第一版固定 1 MiB data extent，metadata block 按设备 geometry 对齐。文件的 extent map
只保存 logical extent 到 physical extent 的映射和状态：

```text
FREE → RESERVED → INITIALIZED → COMMITTED
```

- allocator 只由 metadata actor 修改；data actor 请求一个 bounded extent lease；
- 内存中使用 two-level bitmap 加 free-run index，分配采用 next-fit，释放相邻范围合并；
- 单次预分配默认最多 64 个 extent（64 MiB），超过则拆成多个 transaction；
- 没有满足长度的连续 run 时可分段分配；完全没有空间返回 `HFS_ENOSPC`；
- `RESERVED` 在 crash replay 时回收，未 `COMMITTED` 的 extent 不进入可见 map；
- 只有 `INITIALIZED`/`COMMITTED` extent 才能读；未初始化区域按零返回或先完成初始化；
- 物理回收可在后台执行，但不能阻塞 caller 持有的 scheduler node；
- TRIM/unmap 不是 v1 的必需接口，不以“unmap 读零”替代实际零填充保证。

### 8.3 RocksDB preallocation

RocksDB 的 `PrepareWrite()`/`Allocate()` hint 只在 adapter 声明 direct I/O 时生效。HFS
2.0 v1 选择 `use_direct_io() == true`，所以 adapter 必须正确实现：

- 对齐检查和 direct/bounce 两条路径；
- `PrepareWrite` 到 HFS `hfs_allocate` 的 64 MiB window；
- `Allocate` 只预留空间，不改变逻辑文件尺寸；
- `PositionedAppend` 的单 writer 顺序；
- window 耗尽时以 metadata request 批量补充，data actor 不持锁等待；
- 预分配失败映射为 NoSpace/IOStatus，不返回假成功。

## 9. On-disk 格式和恢复

### 9.1 Clean-break 镜像

HFS 2.0 直接拥有整个 bdev，不使用 Linux partition，不使用 Blobstore。旧镜像一律
不可挂载；`hfsctl format` 是破坏性命令，必须要求明确确认或使用空设备标志。

布局以 device block 为单位：

```text
LBA 0..N
┌──────────────────────────────┐
│ Superblock A/B reserved area │ 8 MiB, two independent copies
├──────────────────────────────┤
│ Journal ring                 │ configurable, default 256 MiB
├──────────────────────────────┤
│ Checkpoint A/B region        │ configurable metadata capacity
├──────────────────────────────┤
│ Allocation bitmap/index      │ persistent allocator state
├──────────────────────────────┤
│ Data extents                 │ remaining capacity
└──────────────────────────────┘
```

`meta_start/meta_blocks` 指 checkpoint 和 allocator 区域；journal 单独由
`journal_start/journal_blocks` 描述；`checkpoint_a/b` 是该区域内的两个 generation
槽位，不能把 checkpoint LBA 误写成 metadata region 起点。

### 9.2 Superblock

```c
struct Hfs2Superblock {
    uint64_t magic;
    uint32_t format_version;
    uint32_t feature_flags;
    uint8_t  uuid[16];
    uint64_t generation;
    uint64_t device_blocks;
    uint32_t sector_size;
    uint32_t metadata_block_size;
    uint64_t journal_start;
    uint64_t journal_blocks;
    uint64_t checkpoint_a_start;
    uint64_t checkpoint_b_start;
    uint64_t checkpoint_blocks;
    uint64_t allocator_start;
    uint64_t allocator_blocks;
    uint64_t data_start;
    uint64_t data_blocks;
    uint64_t checkpoint_generation;
    uint64_t journal_head;
    uint64_t journal_tail;
    uint64_t mount_epoch;
    uint8_t  clean_shutdown;
    uint32_t crc32c;
};
```

`format_version`、device geometry、UUID 和 CRC 必须检查。A/B superblock 写入顺序为：
写非当前副本 → device flush → generation 发布；mount 选择 generation 最大且 CRC 正确
的副本。设计不假设多 sector 写入原子；journal/checkpoint 每个 record 都有长度、序号、
CRC 和 commit marker，撕裂记录自动丢弃。

### 9.3 Journal

journal 是固定 block 对齐的环形日志。一个 transaction 为：

```text
BEGIN(txid, generation, record_count, crc)
RECORD...
COMMIT(txid, generation, tx_crc)
```

- CRC 覆盖 header、所有 record 和 commit 之前的 payload；
- 只有看到完整 BEGIN、records、COMMIT 且 CRC 正确才 replay；
- `CREATE`、`LINK/UNLINK`、`RENAME`、`ALLOC/RESERVE`、`FREE`、`SET_SIZE`、`SET_TIMES`、
  `TRUNCATE`、`CHECKPOINT` 是 metadata record 类型；
- 普通已分配 extent 内的用户数据不写 HFS data WAL；RocksDB 自己的 WAL 是普通文件数据；
- journal 使用率达到 50% 触发 checkpoint；超过 80% 时优先同步 checkpoint；
- journal 没有足够连续空间时，metadata actor 先执行 checkpoint；checkpoint 无法完成时，
  阻塞最多 `metadata_wait_timeout_ms`，然后返回 `HFS_ENOSPC`/`HFS_EBUSY`；不丢弃事务。

checkpoint 为双槽：写入非活动槽的完整 inode/namespace/allocator snapshot，flush 后再
更新 superblock generation；只有 superblock 成功 durable 才推进 journal tail。mount 失败
不能清空旧 checkpoint，必须返回 `HFS_ECORRUPT` 并保留现场供 `hfsctl check` 读取。

### 9.4 Sync 和 crash 顺序

`hfs_datasync(file)`：

```text
1. 拒绝新 data op 或记录目标 durable epoch
2. drain 该文件在途 data operation
3. device flush（确保用户 data/新 extent 初始化完成）
4. 提交必要的 SET_SIZE/extent state transaction
5. journal flush
6. 必要时写并 flush superblock/checkpoint
7. 发布 durable epoch，返回 frontend
```

`hfs_sync(file)` 在上述基础上包含文件 metadata、时间戳和 namespace 关联修改。任何
metadata commit 都不能先于其指向的数据 durable；不能出现“先 device flush、后写 journal
但不再 flush”这种不可恢复顺序。crash 后：

- 完整 committed transaction replay；
- 未提交的 after-image 丢弃并回滚内存状态；
- RESERVED extent 回收为 FREE；
- 已写但未被 durable metadata 指向的数据成为不可见 orphan，之后可回收；
- 所有 waiter 收到明确的恢复/错误状态，不遗留占位 handle 或孤儿 namespace entry。

## 10. RocksDB 8.9.1 adapter

### 10.1 接入方式

adapter 是独立 C++ shared library，依赖 RocksDB 8.9.1，核心只依赖 C ABI。使用
`NewCompositeEnv(std::shared_ptr<FileSystem>)` 或自定义 driver 注入 `DBOptions::env`。
Debian 自带 `db_bench` 没有通过 CLI 注入 `DBOptions::env` 的选项，因此验收必须使用：

- 自编译的 `hfs_db_bench`，在创建 DB 前设置 `DBOptions.env`；或
- 一个明确设置 `DBOptions.env = NewCompositeEnv(hfs_fs)` 的 custom driver；
- 同理构建 `hfs_ldb` 做 `checkconsistency`，不能声称普通 stock `db_bench` 已使用 HFS。

### 10.2 FileSystem 8.9.1 契约

以 VM 内 RocksDB 8.9.1 的头文件为唯一 API 依据，禁止使用不存在的名称或枚举：

| RocksDB 8.9.1 项目 | HFS 2.0 决策 |
|---|---|
| `FSSupportedOps::kAsyncIO` | v1 声明并实现 |
| `FSSupportedOps::kFSBuffer` | v1 不声明；它只适用于 non-direct MultiRead |
| `kFSPrefetch` | 不存在，不得声明 |
| `FSRandomAccessFile::ReadAsync(...)` | 按实际签名实现，包含 `FSReadRequest`、options、callback、`cb_arg`、`io_handle`、`IOHandleDeleter` 和 debug context |
| `FileSystem::AbortIO()` | 必须实现，取消只对 core 仍可取消的 operation 生效 |
| `GetFileModificationTime` | 必须实现并从 HFS inode mtime 返回 |
| `FSDirectory::Fsync/FsyncWithDirOptions` | 必须实现，映射目录 metadata epoch |
| `FileSystem::LockFile/UnlockFile` | 必须实现 RocksDB lock 生命周期和错误传播 |
| `FileSystem::LinkFile` | 实现硬链接所需的 inode link-count transaction；不支持时必须返回明确的 `NotSupported`，不得伪造复制文件 |
| `FSSequentialFile::Skip/InvalidateCache` | 必须实现；cache invalidate 在 core 为 no-op，但保留正确状态 |
| `FSWritableFile::PrepareWrite/Allocate` | direct I/O 下必须实现为 extent preallocation |
| `PositionedAppend` | WAL/MANIFEST 单 writer 必须实现并保持顺序 |

`FSReadRequest`、iov 和 `io_handle` 在 ReadAsync callback 之前必须保持有效；callback
结束后才调用 `IOHandleDeleter`。v1 不把 HFS DMA buffer 直接借给 RocksDB `fs_scratch`；
如果将来启用，必须用 `std::unique_ptr<void, std::function<void(void*)>>` 的自定义
deleter，并把 buffer 引用绑定到 request 生命周期。

### 10.3 文件类型映射

```text
SST / immutable table  → RandomAccessFile，read/readAsync，可并行
WAL                    → WritableFile，单 writer，PositionedAppend，Sync
MANIFEST/CURRENT       → append/rename/lock，metadata transaction
OPTIONS/INFO           → ordinary writable file，close 后只读
LOCK                   → LockFile/UnlockFile 的进程级 lease
```

RocksDB `WritableFile` 通常由一个线程使用；HFS 不为它重新实现通用 POSIX 多 writer
语义。`RandomAccessFile` 的并发 ReadAsync 是高并发主路径。

## 11. FIO 3.42 external ioengine

### 11.1 加载和 option

支持无需安装到系统目录的方式：

```text
fio --ioengine=external:/absolute/path/fio-hfs.so <job.fio>
```

`fio-hfs.so` 在 `ioengine_ops.options` 注册全部自定义选项，并正确设置
`option_struct_size`：

```text
hfs_config=/path/hfs2.json
hfs_volume=/path-or-volume-name
hfs_direct=1
hfs_preallocate=1
hfs_client_depth=32
```

engine 不调用 POSIX open/read/write 访问 HFS 数据，而是用 core `hfs_open/create` 取得
handle。`setup_files()` 的默认 extend 不能绕过 allocator：`open_file` 负责 create+
allocate，并设置 `FIO_NOEXTEND` 或完成等价的 engine-owned extend；`size=`、`offset`、
`filename` 的映射必须写入 HFS namespace。

### 11.2 submit/getevents/event

- `queue()` 创建一个 `HfsOp`，保存 `io_u` 指针直到完成；
- `FIO_Q_BUSY` 只表示有界 ring、operation pool 或 DMA 暂满，不能丢请求；
- `getevents(min,max,timeout)` 使用 `hfs_client_poll`，返回数满足 FIO min/max 语义；
- `event()` 取得对应完成事件并设置 `io_u->error`、`resid`、`error`, `bytes`；
- backend errno、`HFS_ENOSPC`、`HFS_EAGAIN` 和 `HFS_ECANCELED` 必须保留到 FIO；
- `fio --verify` 在 HFS data path 上运行，用于确认 bounce/direct 两条路径；
- engine close 前必须 drain 所有 io_u，再释放 file handle/client。

## 12. 错误、重试、取消和退出

### 12.1 错误传播

一个 child 失败时：保存原始 backend errno，标记 parent first error，继续等待其它
child，最后把映射后的 core status 送回 frontend。所有等待者必须被唤醒，不能只唤醒
一个 waiter。

| Core | RocksDB `IOStatus` | FIO |
|---|---|---|
| `HFS_ENOSPC` | `NoSpace` | `ENOSPC` |
| `HFS_EAGAIN`/`HFS_EBUSY` | `TryAgain`/busy error | `FIO_Q_BUSY`（submit 阶段） |
| `HFS_ENOMEM` | `MemoryLimit` 或 IO error | `ENOMEM` |
| `HFS_EIO` | IO error，附 backend errno | backend errno 或 `EIO` |
| `HFS_ECANCELED` | Aborted | `ECANCELED` |
| `HFS_ETIMEOUT` | TimedOut | `ETIMEDOUT` |
| `HFS_ECORRUPT` | Corruption | `EIO`，同时终止 engine |

`EINVAL`、`ENOENT`、`EEXIST`、`ENOTSUP` 等语义错误不得重试。

### 12.2 背压和 backend retry

所有队列均有上限：client depth、ring depth、backend credit、operation pool 和 DMA
arena cap。满时：

- submit 尚未取得 operation 时返回 `HFS_EAGAIN`；
- 已取得 operation 但未入 ring 时释放引用后返回 busy；
- backend `-ENOMEM` 只在 actor 内以最多 3 次、带界限 delay 的 READY retry 处理；
- retry 不持 inode dependency、DMA lease 或 metadata lock；
- 超过上限返回原始错误；绝不无限增长 retry queue。

### 12.3 Teardown 和 fork

正常 stop 顺序：

```text
reject new client/op
  → close ingress rings
  → complete/cancel pending scheduler nodes
  → drain backend credit and callbacks
  → flush metadata/journal/checkpoint
  → write clean_shutdown superblock
  → destroy channels on owning reactors
  → join reactor threads
  → close bdev and release process lock
```

有活跃 client、operation 或未完成 durability 时，`hfs_engine_stop` 返回 `HFS_EBUSY`，
除非调用显式的 drain-and-stop。SIGTERM 可触发 bounded drain；SIGKILL 不保证 clean flag，
下次 mount 必须走 recovery。`pthread_atfork` 只用于把 child 标记为 fork child；子进程
任何 core API 都返回 `HFS_EFORK`，不尝试重新使用继承的 SPDK thread/channel。

## 13. 配置和管理工具

### 13.1 最小配置

```json
{
  "bdev_name": "Nvme0n1",
  "config_path": "config/hfs2_nvme.json",
  "metadata_block_size": 4096,
  "journal_mb": 256,
  "checkpoint_mb": 512,
  "extent_mb": 1,
  "data_reactors": 0,
  "metadata_cpu": 0,
  "data_cpus": [],
  "ring_depth": 1024,
  "client_depth": 32,
  "dma_arena_mb": 512,
  "op_pool_count": 4096,
  "metadata_wait_timeout_ms": 5000,
  "backend_retry_limit": 3
}
```

`data_reactors=0` 表示使用本文件的 nproc 规则。CPU 列表长度必须与实际 reactor 数
一致。实际 VM 验收固定 `config/hfs_nvme.json`、`Nvme0n1`、PCI BDF `0000:0b:00.0`；
MALLOC/AIO 只用于 deterministic fault injection，不得替代正式 NVMe 结论。

### 13.2 工具

```text
hfsctl format --config ... --yes
hfsctl check --config ... [--repair-orphans]
hfsctl dump-super --config ...
hfsctl dump-journal --config ...
hfsctl dump-tree --config ... --path ...
```

`check` 默认只读；repair 必须单独确认并记录修改的 generation。format、repair 和
device flush 错误必须打印 bdev 名、geometry 和 generation，但不打印密码/敏感配置。

## 14. 可观测性

默认 trace/stats 都关闭。trace-on 只能写 `/mnt/hfs-data/<run>/` 或用户显式指定目录，
不能把大文件默认写到根盘 `/var/tmp`。

必须能够观测：

```text
op_alloc/release/fallback
ring_reservation/doorbell/items/backpressure
scheduler_probe/conflict/dependency_wait
dma_bytes/high_watermark/lease_wait
extent_alloc/reserve/commit/free
parent_children/parent_wait/first_error
metadata_journal/checkpoint/flush/publish
spdk_submit/complete/backend_errno
```

trace-off 热路径不能为这些计数执行额外 atomic。诊断必须首先检查：
`trace_dropped=0`、`spdk_submit=spdk_complete`、parent/child 闭合、operation/arena 无泄漏。

## 15. 非功能目标

以下是 HFS 2.0 的验收目标，不是当前 HFS 1.x 的已达成绩：

| 项目 | 目标 |
|---|---|
| mount | 在 8-vCPU VM、1,000,000 个 active inode、256 MiB journal 的 checkpoint 上，除 SPDK 初始化外 ≤10 s；journal replay 追加 ≤2 s |
| metadata actor | 单 metadata actor 对 create/rename/set-size/commit 达到 ≥20,000 ops/s 的 microbenchmark，且报告 journal bytes/op |
| memory | `inode + namespace + extent index` 按预算运行，默认不超过 1 KiB/active inode（不含 name/path 实际字节和 shared DMA arena）；超限返回 ENOMEM |
| data latency | 主 FIO 4K 口径 p50 ≤0.5 ms、p99 ≤1.5 ms；64K p99 ≤3 ms；所有结果必须附带 CPU% |
| resource | 默认 DMA arena ≤512 MiB，operation/ring/metadata 资源均有上限；无 swap、无无界 retry、无泄漏 |
| recovery | 正常重启、SIGKILL 和五个 crash 注入断点都能在规定 mount 预算内恢复，且 checksum/extent/namespace 守恒 |

## 16. 构建、ABI 和源码收敛

### 16.1 构建产物

```text
libhfs_core.so       C core，默认 hidden visibility
libhfs_rocksdb.so    RocksDB 8.9.1 C++ adapter
fio-hfs.so           FIO 3.42 external engine
hfsctl               format/check/dump tool
```

core 用 CMake 和 Make 双构建；SPDK 版本由 VM 内 Git checkout 固定，Windows 工作区不
同步 SPDK 源码。C core 与 C++ adapter 分开编译，core 不包含 RocksDB 头文件。

ABI 规则：

- `HFS_CORE_ABI_MAJOR=2`，不导出内部结构；
- public C symbols 通过 version script 显式导出，默认 hidden；
- `HFS_CORE_2.0` 只为新 API 服务，不提供旧 `hfs_lib_*`/async alias；
- adapter 和 fio engine 对 core ABI 做启动时版本检查；
- ABI 变更必须增加 major 或在新符号组中实现，不能静默改结构布局。

### 16.2 复用、重写、删除清单

| 当前模块/能力 | HFS 2.0 处理 | 说明 |
|---|---|---|
| `src/hfs_op.[ch]` | 复用后扩展 | 作为 operation lifetime 基础，改为公开 opaque core 的内部实现 |
| `src/hfs_runtime.c` | 参考重写 | 保留自建 pthread + SPDK thread poll 经验，重写为 reactor/ring owner |
| actor ring、parent completion | 复用设计，重写边界 | 保留 shard-67 的守恒和生命周期原则，不保留 POSIX adapter |
| inode interval scheduler | 复用算法，重写 API | 成为唯一 scheduler；去掉 ticket/range compatibility 名称 |
| shared DMA arena | 复用设计，重写接口 | core `HfsBufferRef`，不暴露 registered-DMA ABI |
| dirty set/epoch、durability 顺序 | 复用设计 | 转换到 raw-bdev journal/checkpoint 实现 |
| stats、trace、logging、CRC | 复用并瘦身 | 只保留 core 需要的事件和错误信息 |
| `src/hfs_io.c` | 大幅重写 | 只抽取 direct/RMW/parent 的验证经验，改为 extent planner+bdev |
| `src/hfs_meta*.c` | 大幅重写 | 只抽取 checksum、intent、epoch、恢复测试经验，不保留 Blobstore布局 |
| `src/hfs_durability.c` | 参考重写 | 保留 group commit/flush 顺序，不保留 POSIX fd 入口 |
| `src/hfs_file.c` | 删除后重写 | 新 `hfs_file` 是 opaque handle，不是 Blobstore file |
| `src/hfs_preload.c` | 删除 | 不再编译、不再导出 preload 符号 |
| `src/hfs_fd.[ch]` | 删除 | 无 fd、dup、stdio、cwd、目录句柄 |
| `src/hfs_lib.[ch]` | 删除 | 新 API 不围绕 read/write/pread/pwrite |
| `src/hfs_ticket.[ch]` | 删除 | 只提取 interval scheduler 可复用算法；不保留 ticket API |
| `src/hfs_range_lock.[ch]` | 删除 | 已不属于当前主线，也不在新架构恢复 |
| SPDK Blobstore glue | 删除 | HFS 2.0 直接调用 bdev |
| `hfs_preload.map`、POSIX env 开关 | 删除 | 不允许旧兼容 ABI 回流 |
| mmap/fork/POSIX permission wrappers | 删除 | fork 只保留 core 的进程保护，不保留 preload 语义 |

“复用”只表示提取经过验证的算法、测试口径和故障经验；如果旧代码的数据结构、锁、
ABI 或 Blobstore 假设与新 owner 模型冲突，直接删除后重写。

## 17. 开发阶段和切换门禁

### Phase 0：冻结旧线和新 ABI

- 冻结 HFS 1.x commit、性能基线和数据库 baseline artifact；
- 建立 `include/hfs_core.h`、ABI version script、Make/CMake 双构建；
- 建立 raw-bdev `hfs2` format/mount skeleton；
- 明确旧镜像不可用，format 必须显式确认。

验收：新 core 不链接 Blobstore/preload/fd；旧线仍能运行原有回归。

### Phase 1：raw bdev、superblock、journal、checkpoint

- 直接打开 SPDK bdev，读取 sector geometry；
- 实现 A/B superblock、journal record/CRC、双 checkpoint 和 clean flag；
- 实现 crash replay、journal full、tear record fault injection。

验收：format/mount/reopen/check、断电恢复和旧镜像拒绝。

### Phase 2：runtime、ring、op、DMA、extent allocator

- 实现 reactor pthread/`spdk_thread_poll`、per-thread channel 和 shard ring；
- 实现 HfsOp、parent、scheduler、shared arena；
- 实现 extent reserve/initialize/commit 和 direct/bounce read/write。

验收：native API unit test、ring/scheduler/arena 守恒、4K/16K/64K raw I/O、无泄漏。

### Phase 3：FIO frontend

- 实现 external engine、options、setup_files/FIO_NOEXTEND、queue/getevents/event；
- 覆盖 verify、iodepth=1/8/32/64/128、numjobs=1/2/4/8；
- 建立 raw SPDK comparator 和固定 baseline harness。

验收：读写正确、错误映射正确、FIO iodepth 真正生效，主基线达到 HFS/raw ≥85%。

### Phase 4：RocksDB 8.9.1 frontend

- 实现 FileSystem 全部必需接口和 ReadAsync exact signature；
- direct IO、PrepareWrite/Allocate、PositionedAppend、Directory Fsync、LockFile；
- 构建 `hfs_db_bench` 和 `hfs_ldb`，显式注入 `DBOptions.env`。

验收：fillseq/readrandom/overwrite/readwhilewriting/flush/compact/WAL、重启和
`ldb checkconsistency`。

### Phase 5：恢复、性能和删除旧线

- data flush、metadata commit、journal flush 三个 crash 边界注入；
- ext4 同 workload 参考模型对照；
- 5-run median、CPU/RSS、p50/p99、错误/守恒报告；
- 所有新 frontend 通过后，从 HFS 2.0 源码树删除旧 POSIX/Blobstore 实现；
- 更新产品文档，旧 POSIX 文档只留历史和迁移说明。

## 18. 验收标准

### 18.1 结构门禁

- trace/stats off 无每请求 heap、pthread primitive init/destroy 或全表扫描；
- one logical I/O 对应一个 parent、有限 child，caller 只 wait 一次；
- ring reservation/doorbell、scheduler、DMA lease、extent、operation 引用全部闭合；
- 没有持 dependency 等 DMA/queue credit，也没有 callback 反向取得 caller lock；
- `spdk_submit == spdk_complete`，backend errno 可追踪，NVMe 可释放。

### 18.2 FIO 固定口径

正式性能：VM 真实 NVMe `0000:0b:00.0`、8 vCPU、256 MiB、O_DIRECT、trace/stats off、
shared-file、randrw 50/50、4K/8K/16K/32K/64K、32 jobs、10 秒、5 次 median；raw 侧
使用同设备的 SPDK bdev engine，使用等价 QD。报告 HFS/raw、IOPS、read/write p50/p99、
CPU%、RSS、errors、submit/complete。

功能矩阵：

```text
iodepth: 1, 8, 32, 64, 128
numjobs: 1, 2, 4, 8
rw:      read, write, randread, randwrite, randrw
bs:      4K, 16K, 64K, 1M
```

每个组合至少验证错误为零和 `fio --verify`；性能门禁不以 trace-on 数字通过。

### 18.3 RocksDB 和恢复

使用 custom driver：

```text
hfs_db_bench --hfs-config=config/hfs2_nvme.json \
  --db=/hfs2/rocksdb --benchmarks=fillseq,readrandom,overwrite,readwhilewriting \
  --num=20000 --threads=8 --use_direct_io_for_flush_and_compaction=1

hfs_ldb --hfs-config=config/hfs2_nvme.json \
  checkconsistency --db=/hfs2/rocksdb
```

验收必须包含：新建、写入、close、正常重启、跨进程 reopen（一次一个进程）、flush、
compact、delete、WAL sync、SIGKILL recovery、checkconsistency。跨进程同时 mount 同一
bdev 仍必须被拒绝。

crash 注入至少覆盖：

1. data child 完成但 device flush 前；
2. data flush 完成但 metadata transaction commit 前；
3. journal commit 写入但 journal flush 前；
4. checkpoint 写 inactive slot 后、superblock publish 前；
5. clean shutdown 前的任意退出。

每个断点都要检查 namespace、extent 状态、logical/durable size、孤儿回收、RocksDB
reopen 和 checksum。ext4 上相同 workload 作为语义参考，不把 ext4 性能当作 HFS 目标。

## 19. 风险和停止条件

| 风险 | 缓解 |
|---|---|
| metadata actor 成为瓶颈 | dirty epoch/group commit、批量 extent transaction；单独报告 metadata ops/s |
| 全量 metadata 内存过大 | max_inodes/namespace bytes、mount time budget、create 返回 ENOMEM |
| direct IO 对齐和 RocksDB 版本差异 | 固定 8.9.1 头文件、adapter compile test、bounce fallback |
| journal 写放大 | data 不写 HFS WAL、metadata batch、阈值 checkpoint、报告 journal bytes/op |
| hugepage 不足 | 启动预算检查、DMA cap、FIO 分段，不隐式扩大 arena |
| bdev 错误/队列满 | 原始 errno、有限 retry、FIO busy、RocksDB TryAgain |
| scheduler/parent UAF | 引用计数、fault injection、shutdown/fork sanitizer 检查 |
| 旧功能回流 | 新库名/新镜像 magic/新 ABI，HFS 2.0 不编译旧模块 |

立即停止条件：任何数据错误、sparse 非零、metadata corruption、recovery 失败、
parent/child 不闭合、operation/DMA 泄漏、submit/complete 不相等、NVMe 无法释放，或
RocksDB/FIO 错误映射不完整。性能提升不能抵消结构或正确性门禁失败。

## 20. 最终定位

> HFS 2.0 是一个面向 RocksDB 和 FIO 的 SPDK-native 用户态文件存储引擎。它在应用
> 进程内直接管理 NVMe，使用异步 C core、raw-bdev metadata/extent layout、per-shard
> actor、单一 inode dependency scheduler 和 bounded DMA arena，为数据库 frontend
> 提供明确的文件、异步 I/O 和持久化契约；它不是通用 POSIX 文件系统，也不兼容 HFS 1.x
> 的 Blobstore 镜像、fd ABI 或 preload 路径。

HFS 2.0 只有在接口、磁盘格式、frontend、恢复和固定性能口径全部闭环后才切换为产品线。
在此之前，旧 HFS 只作为冻结对照，不得把两种架构拼接成新的“兼容模式”。
