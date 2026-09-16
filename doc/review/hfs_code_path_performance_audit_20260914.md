# HFS 代码级 I/O 性能审计

日期：2026-09-14  
审计对象：当前 `main`（`add0722 docs: consolidate final performance baseline`）  
审计范围：HFS 源码、当前构建入口和已提交的性能基线。  
审计约束：本次只做代码审计，不修改 C/C++ 代码，也不提出改造方案。凡是不能由当前源码确定的事实，明确标记为“无法从当前代码确定”。

## 0. 结论摘要

当前 FIO 基线的现象是：HFS 相对于 raw SPDK 的 4 KiB～64 KiB 随机混合 I/O 约为 63.4%～70.9%。这个比例来自 `doc/performance/hfs_final_performance_baseline_20260913.md`，测试口径是 HFS POSIX preload、raw SPDK FIO、同一 NVMe、HFS `numjobs=32`、raw SPDK `iodepth=32`。该文档给出了结果，但不能单独证明每一项软件开销的时间占比。

从当前代码可以确定的结构性差异是：

1. HFS 的 FIO 请求首先通过 LD_PRELOAD 和 POSIX 语义层，随后每个标量 `pread/pwrite` 仍然是同步等待一个 SPDK 子请求；raw SPDK 不经过这些层。
2. 默认开启的 inode-local ticket scheduler 对每个标量 I/O 都做 ticket admission、冲突扫描和 completion；完全不重叠的范围没有依赖关系，但仍共享 inode runtime 的 ticket mutex、链表和计数器。
3. 普通非 DMA-safe 用户缓冲区走固定 DMA pool。典型 4 KiB 对齐写有一次 user buffer → DMA buffer 的 `memcpy`，读有一次 DMA buffer → user buffer 的 `memcpy`；DMA-capable 且满足对齐条件时才可直接把用户缓冲区交给 SPDK。
4. data reactor 使用受 mutex 保护的批队列和一条 `spdk_thread_send_msg` 通知，completion 在同一个 SPDK data reactor 上执行，再用 Linux futex 唤醒应用线程。应用线程本身没有 busy-spin；它在 completion 前睡眠。
5. `pread/pwrite` 在进入数据面前都要通过 fd bucket、open-file 引用和全局 metadata 读锁查 inode。`pwrite` 还会执行 `hfsResizeTo` 以及写后元数据/timestamp 发布。
6. `fsync/fdatasync` 走单独的 durability coordinator。它可以在约 200 µs 窗口内把最多 64 个请求编成同一 epoch，但某次运行是否实际形成 batch 不能仅由代码确定。

下面的章节逐项给出文件、函数、结构和真实调用关系。

## 1. 实际总体架构

### 1.1 代码对应的端到端图

```text
Application (FIO / MySQL / RocksDB)
        |
        |  ELF symbol interception
        v
src/hfs_preload.c
  open/open64/openat/fortify variants
  read/write/pread/pwrite/readv/writev/preadv/pwritev
  fsync/fdatasync/dup/close/fstat...
        |
        v
src/hfs_lib.c  -- POSIX semantic layer
  hfs_lib_open/close/read/write/pread/pwrite
  hfs_acquire_file -> hfsFdAcquire
  global g_metaLock + path/inode lookup
        |
        v
src/hfs_fd.c / src/hfs_fd.h
  g_fdSlots[65536] -> HfsFdSlot
  HfsOpenFile (open-file description)
  HfsFile -> inodeId/blobId/HfsInodeRuntime
        |
        v
src/hfs_meta.c / src/hfs_meta_index.c
  Hfs::inodes / Hfs::metaIndex
  hfsMetaFindById / hfsMetaFindByName
  metadata v4 intent/stable image
        |
        v
src/hfs_inode_runtime.c / src/hfs_ticket.c / src/hfs_io.c
  HfsInodeRuntime
  64 range stripes, syncLock, ticketLock/ticket list
  HfsPosixIoTicket
  HfsIoRequest + HfsIoLease
        |
        v
src/hfs_runtime.c
  owner reactor (SPDK thread 0)
  data reactors (SPDK threads 1..N)
  HfsRuntimeDataReactor::batchHead/batchTail
  hfsRuntimeSubmitDataBatch -> spdk_thread_send_msg
        |
        v
src/hfs_io.c / src/hfs_file.c / src/hfs_meta.c
  hfsIoStart -> spdk_blob_io_read/write/readv/writev
  hfsDoSync/hfsDeviceFlush -> spdk_bdev_flush
        |
        v
SPDK blobstore / bs_dev / bdev channel
        |
        v
NVMe bdev (default deployment: Nvme0n1)
        |
        v
SPDK callback on owning SPDK thread
  hfsIoDone / hfsMetaOpCb / hfsSpdkBdevFlushCb
        |
        v
HfsCompletion / durability request completion
  futex wake (data/control I/O)
  pthread_cond_signal (ticket/durability/pool wait)
        |
        v
Application thread returns POSIX result
```

### 1.2 各层的实际对象

* `src/hfs.h` 的 `Hfs` 持有 SPDK `blobstore`、`bsDev`、`bdevDesc`、owner `channel`、`dataChannels[]`、inode 表、`metaIndex`、`rangeLocks[256]`、inode runtime registry 和 durability coordinator。
* `src/hfs.h` 的 `HfsFile` 保存 blob、`inodeId`、所属 `Hfs`、`HfsInodeRuntime` 和文件级 offset。
* `src/hfs_fd.h` 的 `HfsOpenFile` 是 POSIX open-file description：保存共享 offset、flags、引用计数、`ioLock`、inode 属性快照以及 orphan 处理状态。
* `src/hfs_inode_runtime.h` 的 `HfsInodeRuntime` 是每 inode 的并发状态：`syncLock`、`resizeLock`、`appendLock`、64 个 `rangeStripes`、兼容 `rangeGate`、`ticketLock` 和 ticket 链表。
* `src/hfs_io.c` 的 `HfsIoRequest` 是一次后端 I/O 的请求对象，包含 `HfsRequestResult`、嵌入式 `HfsRuntimeBatchItem`、buffer/iovec、offset/units、channel 和 data reactor 信息。
* `src/hfs_runtime.c` 的 `HfsRuntimeDataReactor` 是每个 data reactor 的线程、SPDK thread、batch mutex、链表队列、queue depth 和 in-flight credit。

## 2. 4 KiB `pread` 完整调用链

以下以“文件已存在、offset/length 按 unit 对齐、请求落在文件范围内、默认 ticket 开启、默认 data reactor 队列开启”为典型路径。用户 buffer 是否 DMA-safe 不能仅由 HFS 代码确定，所以先列非 DMA-safe 普通路径，再列 direct 分支。

### 2.1 非 DMA-safe 普通 buffer 路径

```text
pread()
 -> src/hfs_preload.c:hfs_pread / large-file alias
 -> src/hfs_lib.c:hfs_lib_pread
 -> hfs_acquire_file
 -> src/hfs_fd.c:hfsFdAcquire
 -> hfsMetaReadLock + hfsMetaFindById
 -> src/hfs_io.c:hfsIoReadAt
 -> hfsIoSyncReadLock
 -> hfsPosixTicketInit
 -> hfsPosixTicketAdmit
 -> hfsPosixTicketWaitReady
 -> hfsIoReadAtLocked
 -> hfsIoAcquire (4 KiB class HfsIoLease)
 -> hfsIoStartRequest
 -> hfsRuntimeSubmitDataBatch
 -> hfsRuntimeSendMessage / spdk_thread_send_msg
 -> hfsRuntimeDispatchDataBatch
 -> hfsIoStart
 -> spdk_blob_io_read
 -> hfsIoDone
 -> hfsRequestComplete / hfsRuntimeCompletionComplete
 -> futex wake
 -> hfsRuntimeCompletionWait / hfsRequestWaitResult
 -> hfsIoWaitReadPipeline
 -> memcpy(user buffer, lease.buffer, 4096)
 -> hfsIoRelease
 -> hfsPosixTicketComplete
 -> hfsPosixTicketDestroy
 -> hfsIoSyncReadUnlock
 -> hfsFdRelease
 -> return to application
```

依据：`hfs_lib_pread` 位于 `src/hfs_lib.c` 约 1494-1541；`hfsIoReadAt` 位于 `src/hfs_io.c` 约 1763-1872；请求提交、SPDK callback 和等待分别位于 `hfsIoStartRequest`、`hfsIoStart`、`hfsIoDone`、`hfsRuntimeCompletionWait`。

### 2.2 逐步属性

| 步骤 | 跨线程/队列 | 锁或原子 | 分配/复制 | 等待/共享状态 |
|---|---|---|---|---|
| `hfs_pread` hook | 否 | 无 HFS 锁 | 无 | 进入 `hfs_lib_pread` |
| `hfsFdAcquire` | 否 | fd bucket `pthread_mutex`；slot/generation 原子读；refs CAS | 无分配 | 读全局 `g_fdSlots` |
| metadata 查 inode/size | 否 | 全局 `g_metaLock` 读锁 | 无 | 访问 `Hfs::inodes/metaIndex` |
| `hfsIoSyncReadLock` | 否 | inode `pthread_rwlock_rdlock` | 无 | 与 resize/fsync 写屏障协调 |
| ticket init/admit | 否 | `runtime->ticketLock`、ticket `waitLock` | ticket 在调用栈 | 非重叠通常无依赖；仍扫描活动 ticket 链表 |
| DMA lease acquire | 否 | 快速路径是 `freeMask` CAS；耗尽时 shard mutex/cond | lease 在调用栈，buffer 来自预分配 pool | pool 耗尽时 `pthread_cond_wait` |
| batch submit | 否（提交线程） | reactor `batchLock`；全局 `messageSubmitLock` | `HfsRuntimeBatchItem` 嵌入 request | 链接 queue 并发送一条 SPDK message |
| batch dispatch | 是，运行在 data reactor | `batchLock` 取出链表，更新 depth/inflight 原子/计数 | 不分配 request | reactor 调用 `hfsIoStart` |
| `spdk_blob_io_read` | 由 SPDK 执行 | SPDK 内部状态无法从 HFS 源码展开 | 非 HFS 代码 | callback 异步返回 |
| `hfsIoDone` | 在 channel 所属 SPDK 线程 | completion state 原子；归还 runtime credit 时 reactor `batchLock` | 无分配 | futex wake 应用线程 |
| wait/copy-out | 回到应用线程 | `HfsCompletion.state` 原子读；可能 futex | 一次 `memcpy` 4096 | 无 busy-spin；普通路径阻塞在 futex |
| ticket complete/fd release | 应用线程 | ticket mutex/cond；fd ref 原子 | 无分配 | 可能唤醒后继 ticket |

### 2.3 DMA-capable direct 分支

`hfsIoReadAt` 中 `directEligible = aligned && hfsIoBufferDmaCapable(buffer, count)`。`hfsIoBufferDmaCapable` 使用 `spdk_vtophys` 检查物理连续 span 是否覆盖请求长度。满足条件时，`hfsIoReadAtLocked` 创建 stack `HfsIoRequest`，直接把应用 buffer 传入 `spdk_blob_io_read`，completion 后不执行 read copy-out。ticket、reactor queue、SPDK message、futex wait 仍然存在；消失的只是 DMA lease 和一侧 `memcpy`。

## 3. 4 KiB `pwrite` 完整调用链

### 3.1 普通非 DMA-safe buffer

```text
pwrite()
 -> src/hfs_preload.c:hfs_pwrite
 -> src/hfs_lib.c:hfs_lib_pwrite
 -> hfs_acquire_file -> hfsFdAcquire
 -> writable/append 检查
 -> hfsResizeTo(file, offset + 4096)
 -> hfsMetaReadLock + hfsMetaFindById（append 或 size 判断时）
 -> hfsIoWriteAt
 -> hfsIoSyncReadLock
 -> hfsPosixTicketInit / Admit / WaitReady
 -> hfsIoWriteAtLocked
 -> hfsIoAcquire (4 KiB HfsIoLease)
 -> memcpy(lease.buffer, user buffer, 4096)
 -> hfsIoStartRequest
 -> hfsRuntimeSubmitDataBatch
 -> hfsRuntimeSendMessage / spdk_thread_send_msg
 -> hfsRuntimeDispatchDataBatch
 -> hfsIoStart
 -> spdk_blob_io_write
 -> hfsIoDone -> hfsRequestComplete
 -> futex wake -> hfsRuntimeCompletionWait 返回
 -> hfsIoWaitPipeline / hfsIoRelease
 -> hfsPosixTicketComplete / Destroy
 -> hfsIoSyncReadUnlock
 -> hfsPublishWriteEnd
 -> hfsFdRelease
 -> return to application
```

依据：`hfs_lib_pwrite` 在 `src/hfs_lib.c` 约 1545-1617；`hfsResizeTo` 在 `src/hfs_file.c`；非 direct aligned write 的用户到 DMA 复制在 `src/hfs_io.c:hfsIoWriteAtLocked` 约 1598 行以后；`hfsPublishWriteEnd` 在 `src/hfs_lib.c` 约 756-849。

### 3.2 `pwrite` 的特殊分支

* 预分配文件中，`hfsResizeTo` 可以在 `allocatedClusters` 原子值已覆盖 end offset 时快速返回；首次扩展或超出已分配范围时会通过 owner reactor 做 SPDK blob resize，增加一次控制面请求和等待。
* 非 `O_APPEND` 的 `pwrite` 不获取 `HfsOpenFile::ioLock`；`O_APPEND` 需要 `appendLock`，再通过 metadata 锁读取当前大小。
* 已分配范围内的写结束通常命中 `hfsPublishWriteEndFast`：使用 inode runtime 的 atomic logical size/timestamp 状态，必要时再进入全局 metadata 写锁。是否每个 FIO 写都触发重型 metadata commit，取决于是否真正扩展文件以及后续 `fsync`，不能只从一次 `pwrite` 调用推出。

## 4. Reactor/thread 模型

### 4.1 线程角色

`src/hfs_runtime.c:hfsRuntimeOwnerMain` 创建并持续 `spdk_thread_poll` 的 `hfs-owner`。owner 负责 blobstore 控制面、metadata、resize、兼容路径以及没有可用 data reactor 时的 I/O。

`hfsRuntimeDataReactorCountEnv` 的代码语义是“总 reactor 数包含 owner”。未设置 `HFS_DATA_REACTORS` 时，自动策略在 1-vCPU 主机仅保留 owner，其余主机使用 `1 + clamp(floor(nproc/4), 1, 4)` 个 total reactor；因此 8-vCPU VM 为 owner 0 加 data reactor 1～2，16-vCPU 及以上封顶为 owner 加 4 个 data reactor。设置环境变量时，值也表示总数，实际 data reactor 数是 `count - 1`。每个 data reactor 在 `hfsRuntimeDataMain` 中拥有独立 SPDK thread 和 polling loop。

### 4.2 请求流向

```text
Application thread
      |
      | hfsRuntimeSubmitDataBatch
      | lock dataReactor.batchLock
      | append HfsRuntimeBatchItem
      | release lock
      | lock messageSubmitLock
      | spdk_thread_send_msg(dispatch)
      v
Data reactor N
      |
      | dispatch drains batchHead/batchTail
      | hfsIoStart(request)
      | spdk_blob_io_read/write(channel_N)
      v
NVMe / SPDK callback on data reactor N
      |
      | hfsIoDone
      | hfsRequestComplete
      | futex(FUTEX_WAKE_PRIVATE, INT_MAX)
      v
Application thread wakes and copies/releases/returns
```

* reactor 选择由 `hfsRuntimeDataReactorFor` 根据 `blobId + logical stripe` 哈希；同一 inode 的不同逻辑区域可按 stripe 映射到不同 data reactor。
* `fs->channel` 在 owner 线程创建；`fs->dataChannels[]` 由 `hfsRuntimeDataInit` 在相应 data reactor 上创建，因此 SPDK `io_channel` 的线程归属与目标 reactor 一致。
* owner 不是所有数据 I/O 的必经路径。默认多 reactor 且 data path 可用时，标量数据请求走 data reactor；reactor 0、queue 关闭、无 data reactor 或某些控制操作才走 owner。可是所有 `spdk_thread_send_msg` 提交调用共享全局 `messageSubmitLock`，这是一处提交侧的全局串行化。
* data queue 不是无锁 ring，而是 `HfsRuntimeDataReactor::batchLock` 保护的链表。queue depth/inflight 达到上限时，代码不在持有 POSIX range/ticket 状态时阻塞等待，而是走 direct message fallback；因此“队列满时实际是否排队”取决于运行时状态。

### 4.3 是否存在单点串行化

代码中可以确定的共享串行点包括：全局 `g_metaLock`、全局 `messageSubmitLock`、每 inode 的 `ticketLock`、每 data reactor 的 `batchLock`、每个 fd bucket mutex，以及 SPDK owner 控制面。默认 data I/O 本身不是全部经过 owner，但每个请求在提交时仍需要经过全局 message submission mutex。实际哪一个点在当前负载下成为首要瓶颈，不能仅由静态代码确定。

## 5. Range ticket / ordering

### 5.1 ticket 粒度和数据结构

ticket 不是独立的 fd ticket，也不是全局 range ticket。`src/hfs_ticket.h` 的 `HfsPosixIoTicket` 绑定一个 `HfsFile *file`/inode runtime，保存 offset、length、operation、submitSeq、pendingDependencies、ready/done 状态和每 ticket 的 `pthread_mutex_t waitLock`/`pthread_cond_t waitCondition`。活动 ticket 链表、计数、深度限制和序列号属于 `HfsInodeRuntime` 的 `ticketLock` 保护范围。

冲突规则由 `hfsPosixTicketConflicts` 给出：区间不重叠时不冲突；区间重叠且两者都是 READ 时不冲突；READ/WRITE、WRITE/READ、WRITE/WRITE 重叠时冲突。默认 `HFS_POSIX_TICKET=1`，深度默认 64（代码把环境值限制在 8～256）。

### 5.2 admission、等待和完成

`hfsPosixTicketAdmit`：

1. 锁 `runtime->ticketLock`。
2. 若 `ticketCount >= ticketLimit`，在 `ticketAvailable` 上 `pthread_cond_wait`。
3. 分配 ticket id/submit sequence，扫描当前 ticket 链表，计算与更早 ticket 的冲突数。
4. 把 ticket 链入 inode runtime；无依赖则立即 `ready=true`。
5. 解锁 runtime ticket mutex。

`hfsPosixTicketWaitReady` 在 ticket 的 `waitLock`/`waitCondition` 上睡眠，直到前序冲突 ticket 完成或本 ticket 失败。`hfsPosixTicketComplete` 在 `ticketLock` 下摘除当前 ticket，扫描剩余 ticket，递减冲突依赖；对于变为 ready 的 ticket，锁其 waitLock 并 broadcast，然后广播 `ticketAvailable`。

### 5.3 不重叠访问的实际成本

对于：

```c
pread(fd, ..., A);
pread(fd, ..., B);
```

若 A/B 完全不重叠，两张 ticket 没有 dependency wait，可并行提交；但是两次都要获取同一 inode runtime 的 `ticketLock`，插入/扫描活动链表，并在完成时再次获取同一 mutex、更新计数和扫描后继 ticket。

对于：

```c
pwrite(fd, ..., A);
pwrite(fd, ..., B);
```

完全不重叠时也没有 ordering dependency，但仍共享 `ticketLock`、`ticketCount`、`ticketHead`、`nextTicketId`/`nextSubmitSeq`。此外两次 I/O 都会经过 inode `syncLock` 的读侧、fd table、全局 metadata size lookup 和 runtime message submission。也就是说，“没有重叠”只消除了 ticket 的等待依赖，不等于没有协调成本。

读/读不重叠、读/写不重叠、写/写不重叠都遵循上述无依赖但共享 admission/completion 状态的路径。不同 inode 使用不同 runtime ticket mutex，但仍可能共享全局 metadata lock、fd bucket 和 `messageSubmitLock`。

当 ticket 被禁用时，代码回退到 `src/hfs_io.c` 的 range lock 路径：逻辑 unit 按 `HFS_INODE_RANGE_STRIPE_COUNT=64`、每 stripe 64 units 映射到 `rangeStripes[]`，使用 `pthread_mutex_t + pthread_cond_t`，并保留 writer-priority/read burst 规则。默认路径的 aligned I/O 使用 ticket，非默认分支和 legacy compatibility 仍可触及 range lock。

## 6. Buffer / DMA / staging copy

### 6.1 普通 scalar buffer

```text
write:
user malloc buffer
  -> hfsIoWriteAt 判断 aligned/directEligible
  -> HfsIoLease.buffer（固定 DMA pool）
  -> memcpy(lease.buffer, user, bytes)
  -> spdk_blob_io_write
  -> NVMe

read:
NVMe -> spdk_blob_io_read
  -> HfsIoLease.buffer（固定 DMA pool）
  -> memcpy(user, lease.buffer, bytes)
  -> application
```

`src/hfs_io.c` 的 pool class 是 4 KiB、16 KiB、64 KiB、1 MiB 四类，默认数量分别为 32、16、32、8，按 data reactor 分 shard。`hfsIoAcquire` 首先对 `freeMask` 做 CAS；无空闲 slot 时才进入 shard mutex/cond。lease 本身在调用栈，buffer 在初始化时分配的 DMA storage 中，不是每个 scalar request `malloc`。

`hfsIoWriteAtLocked` 的 aligned non-direct 分支对每个 chunk 做一次 user → lease `memcpy`；`hfsIoWaitReadPipeline` 对每个 child 做一次 lease → user `memcpy`。因此在 4 KiB、16 KiB、64 KiB 对齐且非 DMA-safe 的普通 scalar 请求中，写各 1 次 copy-in，读各 1 次 copy-out；不会因为大小变化自动增加为两次，除非被拆成多个 pipeline child。

### 6.2 DMA-safe 判断和 zero-copy 条件

`hfsIoBufferDmaCapable` 使用 `spdk_vtophys(buffer, &span)`，并要求返回物理 span 覆盖整个请求。`hfsIoReadAt`/`hfsIoWriteAt` 还要求 offset 和 count 满足 unit 对齐，才把 `directEligible` 设为 true。direct scalar 路径直接把用户 buffer 传给 SPDK，省略 DMA lease 和一侧 memcpy，但 ticket、queue、SPDK message、completion 和 futex wait 仍保留。

vector 路径的 `hfsIoVectorDmaCapable` 要求总长度、每个 iovec 段的长度/地址满足 unit 和 `spdk_vtophys` 条件；否则 `hfs_lib.c` 的 vector bounce 使用一次最多 1 MiB 的 DMA buffer，`hfsIovCopyIn/CopyOut` 进行复制，并按 chunk 调用 scalar I/O。`hfsIoSubmitVector` 还会把最多 32 个 iovec 描述符复制到栈上的 `struct iovec vector[32]`。

### 6.3 分配、iovec 和无法确定之处

* scalar 普通路径：request、ticket、lease 均为调用栈对象；DMA buffer 来自预分配 pool，不是每请求 heap allocation。
* vector bounce：`hfsIoDmaAlloc` 调用 `spdk_malloc(..., SPDK_MALLOC_DMA)`，在 vector 操作结束后 `spdk_free`；这不是上述 pool。
* metadata 写入也在 `src/hfs_meta.c` 中使用 `spdk_malloc` DMA buffer 并 copy 稳定镜像/intent。
* 当前代码只能判断“某个地址在本次调用时是否被 `spdk_vtophys` 认定为可 DMA”，不能判断 FIO、MySQL 或 RocksDB 在实际运行中有多少比例的 buffer 满足条件；因此实际测试中的 direct/bounce 比例无法从当前代码确定。
* 未对齐 I/O 会执行 unit 级 read-modify-write，边缘读、用户数据复制和边缘写的次数取决于 offset/length。本文的 4 KiB 时间线按对齐 steady-state，不能代表未对齐请求。

## 7. fd/open-file fast path

`src/hfs_fd.c` 使用静态 `g_fdSlots[65536]`，每个 `HfsFdSlot` 是原子 `openFile` 指针和 generation；另有 256 个 `g_fdBuckets` mutex。`hfsFdAcquire` 按 fd 计算 bucket，锁 bucket，原子读取 slot/generation，使用 `hfsFdTryAcquireRef` CAS 增加 `HfsOpenFile::refs`，重新验证 slot/generation 后释放 bucket。

因此每次 `read/write/pread/pwrite` 都会重新 fd lookup 和 ref acquire/release，没有 TLS fd cache。普通 acquire 不持有全局 fd 锁，但可能竞争同一 bucket；refs 是原子计数。`close` 先在 bucket 内 exchange slot 为 NULL 并递增 generation，再释放 open-file；这样 fd reuse 不会把旧请求错误地绑定到新描述。`dup/dup2/dup3/F_DUPFD` 通过有序 bucket 锁和 ref 增加共享同一 `HfsOpenFile`，所以共享 offset/`ioLock`。

`HfsOpenFile::ioLock` 只由隐式 offset 的 read/write 和部分目录/兼容操作使用；非 append `pread/pwrite` 不获取它。`close`、unlink/rename 覆盖和 orphan 的延迟 blob 回收由 `orphanLock`、refs 和 deferred/orphan 状态保护。

## 8. Request 生命周期、所有权和 cacheline 风险

### 8.1 scalar 数据请求

`hfsIoSubmit`、`hfsIoSubmitDirect` 和 pipeline 代码在调用栈创建 `HfsIoRequest`；pipeline 在 `hfsIoReadAtLocked`/`hfsIoWriteAtLocked` 中用最多 16 个 stack request 数组（默认 pipeline depth 8）。request 内嵌 `HfsRequestResult`、`HfsRuntimeBatchItem` 和 completion；runtime queue 只保存指向该 stack item 的指针，SPDK callback 也持有同一 request 指针。因此调用栈必须保持到 `hfsIoWaitRequest`/pipeline wait 完成后才能返回。

流程是：应用线程创建 → queue 链接 → data reactor 调 `hfsIoStart` → SPDK callback 调 `hfsIoDone` → completion state 变 DONE → 应用线程 wait 返回 → release lease/ticket/fd → 栈对象销毁。正常 batch path 没有 request mempool；DMA buffer pool 与 request 生命周期分离。

### 8.2 可能的共享 cacheline

代码明确存在应用线程与 reactor 之间反复修改的原子字段：`HfsCompletion::state`、`runtimeCreditHeld`、data reactor `inflight/queueDepth`；ticket admission/completion 还集中修改 runtime 的 `ticketCount`、`ticketHead` 和序列号；每次提交还触碰全局 `messageSubmitLock`。这些布局可能产生 cacheline bouncing。是否真的发生 false sharing、对应多少 CPU 周期，仅凭当前源文件和结构布局无法确定；源码没有 CPU counter 或 cacheline profile 证据。

## 9. 同步等待机制

### 9.1 数据/控制 I/O

`src/hfs_runtime.c:hfsRuntimeCompletionWait` 使用 Linux `syscall(SYS_futex, FUTEX_WAIT_PRIVATE, ...)`，等待 `HfsCompletion::state` 从 PENDING 变为 DONE。代码没有 caller-side busy polling，也没有 spin-then-sleep 阈值；它直接进入 futex wait，收到 `EAGAIN/EINTR` 时重试。超时时先把状态 CAS 为 TIMED_OUT，再继续等待 DONE，以保证调用栈 request 不被过早回收。

SPDK owner/data reactor 线程则在 `spdk_thread_poll` 中持续轮询，这是后台 SPDK 线程的 polling，不是应用线程的 spin。

### 9.2 ticket、DMA pool 和 durability

* ticket 依赖等待：`pthread_cond_wait(&ticket->waitCondition, &ticket->waitLock)`。
* DMA pool 耗尽：`pthread_cond_wait` 在 shard mutex/condition 上睡眠。
* durability coordinator：应用线程在 `HfsDurabilityRequest::mutex/condition` 上 `pthread_cond_wait`；worker 完成时 `pthread_cond_signal`。

### 9.3 一次普通 4 KiB I/O 的 wakeup

在无 ticket 依赖、pool 不耗尽、SPDK callback 正常一次完成的 steady-state 请求中，应用线程至少有一次 completion futex wait 和一次 futex wake。ticket 已 ready 时不会睡 ticket cond；若存在前序冲突，则会多一次或多次 ticket cond wait/wake。应用线程是否经历多次 futex syscall（例如瞬时 EAGAIN、EINTR、超时）取决于调度和运行时，无法从代码给出固定次数。

## 10. fsync/fdatasync 与 durability epoch

### 10.1 当前入口和 coordinator

```text
src/hfs_preload.c:fsync/fdatasync
 -> src/hfs_lib.c:hfs_lib_fsync / hfs_lib_fdatasync
 -> hfs_lib_durability_call
 -> HfsDurabilityRequest（调用栈）
 -> hfsDurabilityRequestSubmit
 -> coordinator queue + worker cond
 -> hfsDurabilityWorker
 -> hfs_lib_process_durability_batch
 -> data barrier / metadata commit / device flush
 -> hfsDurabilityComplete (pthread_cond_signal)
 -> application cond wait 返回
```

`HfsDurabilityCoordinator` 在 `src/hfs_durability.c` 中保存 `windowUs`（默认 200 µs）、`maxBatch`（默认 64）、请求链表、`nextEpoch` 和 worker pthread。worker 收到第一个请求后，在 deadline 内等待更多请求，达到 maxBatch 或窗口到期后取出整个 queue，递增 epoch，并把同一 epochId 写入该 batch 的所有 request。

代码允许多个 fsync 共享 epoch/flush：它们必须在同一个 worker batch 被取出。两个 MySQL 并发事务的 fsync 是否实际共享一次 flush，取决于两次 submit 的到达时间、worker 的 200 µs 窗口、maxBatch 和请求处理时序；当前源码不能证明某个特定 workload 一定共享或一定不共享。

### 10.2 `hfs_lib_process_durability_batch` 的顺序

1. 为 batch `calloc` `targets` 数组。
2. 对每个 request 锁 `openFile->ioLock`，获取 inode runtime 的 sync write lock，按 runtime/open-file 去重 target，然后释放这些锁。
3. Stage A：若是纯 data-only 且没有 metadata 需求，直接准备一次 `hfsDeviceFlush`；否则对每个唯一 target 调 `hfsSyncData`，即 blob metadata/data barrier（`spdk_blob_sync_md` 路径）。
4. Stage B：持有全局 metadata write lock，合并 pending metadata；必要时通过 `hfsMetaCommitDirty`/`hfsMetaCommitInternal` 写 v4 intent、stable inode after-image、header、clear intent，并执行 metadata blob sync。
5. Stage C：调用一次 `hfsDeviceFlush`，最终走 `spdk_bdev_flush`（或者 backend flush fallback）。
6. 把结果写回每个 `HfsDurabilityRequest`，由 `hfsDurabilityComplete` 唤醒各自 condvar。

`fsync` 和 `fdatasync` 的差别由 `dataOnly` 和 metadata dirty/size 状态决定。不能简单说 fdatasync 永远不写 metadata：如果 size/timestamp 等需要持久化，processor 仍可能进入 metadata stage。当前代码的 data-before-metadata-before-device-flush 顺序是可确定的。

### 10.3 NVMe volatile write cache

`src/hfs_file.c:hfsDeviceFlush`/`hfsSubmitFsyncFlush` 负责发出 SPDK bdev flush 并等待 callback。当前 HFS 源码没有检查 NVMe volatile write cache enable 状态，也没有在此处读取 NVMe identify 后自行决定是否省略 flush。因此设备/驱动是否真正保证 volatile cache 落盘，不能从当前 HFS 代码确定；只能确定 HFS 在 durability path 中发出了 flush 并传播了返回码。

## 11. 性能路径分类（代码结构判断，不是时间剖析）

下表的 HIGH/MEDIUM/LOW 表示相对于 raw SPDK 的结构性软件路径负担，不代表已经测得的 CPU 时间百分比。

| 类别 | 级别 | 代码依据 |
|---|---|---|
| A. POSIX compatibility | HIGH | `hfs_preload.c` 符号分派、完整 fd/errno/offset/目录语义和同步 POSIX 返回；raw SPDK 不经过这些层 |
| B. fd/open-file | MEDIUM | 每次 I/O 都有 fd bucket mutex、slot/generation 原子验证、refs CAS；没有 TLS cache，但通常无 heap allocation |
| C. metadata | MEDIUM | `hfs_lib_pread/pwrite` 每次做 metadata read lock + inode lookup；写扩展/持久化还触发 metadata dirty/commit。steady-state 已分配范围可能只更新 atomic overlay，具体比例无法从代码确定 |
| D. range/ticket | MEDIUM | 默认每个 scalar I/O admission/completion 都访问 inode `ticketLock` 并扫描活动链表；不重叠时无 dependency，但共享状态仍存在 |
| E. buffer copy | HIGH（普通 buffer）；LOW（direct） | non-DMA scalar 每个 4/16/64 KiB 请求一侧 memcpy；DMA-capable aligned buffer 可为零 copy。实际 buffer 分类比例无法从 HFS 源码确定 |
| F. request allocation | LOW（普通 batch）/MEDIUM（vector、metadata、durability） | scalar request、ticket、lease 为栈对象，DMA pool 预分配；vector bounce、metadata 和 durability batch 使用 calloc/spdk_malloc |
| G. reactor messaging | HIGH | 每个 backend request 至少需要一次 reactor dispatch message；所有 `spdk_thread_send_msg` 调用经过全局 `messageSubmitLock` |
| H. queue | MEDIUM | data reactor 使用 `batchLock` 链表 enqueue/dequeue、depth/inflight credit；队列满时 direct fallback。每请求一次排队并不等于每请求一次等待 |
| I. SPDK blob/bdev | MEDIUM（占比无法拆分） | `spdk_blob_io_*`、blob channel、bdev/NVMe 本身是实际设备路径；raw SPDK 也有该成本，HFS 源码无法把设备 latency 与软件 latency 精确分离 |
| J. completion | HIGH | 每个 child 有 `HfsCompletion` 状态、callback、runtime credit 归还和 trace/stats 分支，再转回调用方 |
| K. application wakeup | HIGH | 每个同步 POSIX I/O 通过 futex wait/wake；ticket/pool/durability 竞争时另有 pthread cond wait/wake；没有异步 POSIX API 把等待留在调用方之外 |

## 12. 一次 4 KiB `pwrite` 的实际执行时间线

以下时间线假设：文件已预分配、offset/length 对齐、普通 malloc buffer 被 `spdk_vtophys` 判为不可直接 DMA、ticket ready、DMA pool 有空闲 slot、data reactor queue 未满。若任一条件不成立，会进入文末列出的分支。

```text
Application thread
|
| pwrite(fd, user_buf, 4096, offset)
| hfs_preload.c:hfs_pwrite
| hfs_lib.c:hfs_lib_pwrite
| hfsFdAcquire: bucket mutex + slot/generation + refs CAS
| hfsResizeTo: allocatedClusters atomic fast check
| hfsIoWriteAt: inode syncLock(rd)
| HfsPosixIoTicket init/admit/wait-ready
| HfsIoLease acquire: 4 KiB pool freeMask CAS
| memcpy(lease.buffer, user_buf, 4096)
| HfsRuntimeDataReactor queue enqueue: batchLock
| messageSubmitLock -> spdk_thread_send_msg(dispatch)
v
Data reactor N (SPDK thread owning dataChannels[N])
|
| dispatch removes HfsRuntimeBatchItem from batch queue
| hfsIoStart
| spdk_blob_io_write(blob, channel_N, lease.buffer, ...)
v
SPDK blobstore / bdev / NVMe
|
| media completion
v
Data reactor N
|
| hfsIoDone
| hfsRequestComplete -> HfsCompletion.state = DONE
| release runtime credit / data batch accounting
| futex(FUTEX_WAKE_PRIVATE)
v
Application thread
|
| futex wait returns
| hfsRuntimeCompletionWait / hfsRequestWaitResult
| hfsIoRelease: freeMask fetch_or
| HfsPosixIoTicketComplete/Destroy
| inode syncLock(rd) unlock
| hfsPublishWriteEnd (fast atomic overlay or metadata write-lock fallback)
| hfsFdRelease: refs CAS
v
return 4096 or error to application
```

### 12.1 本次时间线的大致操作数量

源码可以确定“至少存在”的操作类型，但不能给出包含 libc、SPDK 和 CPU 调度内部操作的固定总数：

* thread hop：应用线程 → 一个 data reactor 的 dispatch；completion 在同一 data reactor callback，不额外创建 completion 线程；然后 data reactor 通过 futex 唤醒应用线程。
* HFS queue：正常 batch 路径一次 enqueue、一次 dispatch dequeue；另有一次 `spdk_thread_send_msg` 通知。队列满时可能直接 fallback，具体分支由运行时 depth 决定。
* memcpy：普通非 DMA-safe 4 KiB aligned write 一次 copy-in；direct DMA write 为零次 HFS copy；未对齐和 vector 会增加 copy。
* allocation/free：普通 scalar steady-state 使用 stack request、stack ticket 和预分配 lease，通常没有本请求的 malloc/free；vector bounce、metadata、durability batch 会另行分配。是否开启 stats/trace 也会改变辅助路径分配，无法从一个固定计数概括。
* lock：fd bucket、inode sync rwlock、ticket mutex/可能的 ticket waitLock、pool/queue batchLock、全局 messageSubmitLock、completion credit 的 batchLock、ticket completion mutex 等。是否真的阻塞取决于竞争；精确 acquisition 次数随 queue/direct/fallback 和 ticket 状态分支变化。
* atomic：fd slot/generation、open-file refs、pool freeMask/active、completion state/runtime credit、request id 和 inode size/timestamp overlay 都会访问原子；SPDK/编译器内部原子不在 HFS 源码计数内，因此精确总数无法从当前代码确定。
* wakeup：正常数据路径至少一次 futex wait/wake；ticket 有前序依赖、pool 耗尽或 runtime timeout 时会额外出现 pthread cond wait/wake。`FUTEX_WAIT` 因 EAGAIN/EINTR 重试的次数由调度决定。

## 13. 当前代码无法确定的关键事实

为避免把函数名或 README 推断成测量结论，以下事项必须标记为未知：

1. FIO/MySQL/RocksDB 实际用户 buffer 的 DMA-safe 比例，以及每个 workload 中 direct 与 bounce 的比例。
2. 各个 mutex/rwlock/ticket list 在真实 32-job FIO 中的等待时间和持有时间占比；源码只提供 trace/stats 观测点，不能替代运行数据。
3. SPDK blobstore、DPDK ring、NVMe driver 内部的 queue、atomic、copy 和 scheduler 操作数量；HFS 代码只看到其 API 边界。
4. 某次运行中 data queue 是否达到 depth limit、是否发生 direct fallback，以及应用线程实际经历多少次 futex syscall。
5. CPU cacheline bouncing/false sharing 的实际代价；结构中存在可能共享的字段，但没有 perf/PMU 数据。
6. 某个数据库的两个并发 fsync 是否落入同一 200 µs durability batch；源码只定义了合并规则，不能确定一次运行的到达时序。
7. NVMe volatile write cache 是否开启，以及设备对 flush 的具体持久化时延；HFS 只提交 SPDK bdev flush，没有在源码中检查该设备状态。
8. raw SPDK 与 HFS 的 63%～71% 差距中，设备 latency、SPDK blob 层、HFS copy、ticket、message、completion、futex 各占多少百分比；这需要 trace/perf/FIO 运行数据，不能从静态代码反推出精确分解。

## 14. 审计结论边界

本文件给出的是“当前源码实际执行的路径”和“由源码可证明的协调/复制/等待点”。它没有把任何一个点直接转换成修改方案，也没有把历史文档中的设计目标当作当前实现。若要回答具体瓶颈的时间占比，必须把本文件中的调用点与 HFS trace 字段（`io_*`、`buffer_classify`、`copy_*`、`ticket_*`、`runtime_*`、`spdk_*`、`completion_*`、`thread_wait_*`）以及系统 perf/FIO 结果逐一对应；仅凭函数名、README 或静态结构不能完成这一层归因。
