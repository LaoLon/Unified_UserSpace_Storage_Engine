# HFS 面向 MySQL/RocksDB 的异步持久化与 Group Commit Shard 开发计划

> Shard：64  
> 状态：64A/64B/64D/64E 已实现并验证；64C 的 v5 runtime 切换保持关闭  
> 基线：main@923a2b8（Shard-63 POSIX range-ticket scheduler）  
> 范围：完整 POSIX/preload 路径、MariaDB/MySQL-compatible server、RocksDB  
> 唯一推荐方案：mount 级异步 durability coordinator + metadata WAL group commit + epoch-aware POSIX wait  
> 约束：不重新引入 public async ABI，不要求数据库修改 I/O API，不削弱 fsync/fdatasync 和崩溃恢复语义

本 Shard 解决当前最重要的剩余瓶颈：HFS 已经能够通过 SPDK 直接提交 NVMe
I/O，但 MySQL/RocksDB 的完整 POSIX 路径仍把每个同步调用变成一次全局串行的
metadata transaction、blob sync 和 device flush。

目标不是继续增加 queue depth 或 reactor 数量，而是把多个 POSIX 调用在 HFS 内部
合并成有序、可恢复、可等待的 durability epoch。

---

## 1. 决策结论

后续数据库性能优化只采用一条主线：

> 把 fsync/fdatasync 从“调用线程立即执行完整持久化链”改成“提交到 mount 级
> durability coordinator，按数据完成 epoch 合并 dirty inode 和 metadata WAL，
> 批量提交后唤醒各自的 POSIX waiter”。

该方案同时消除：

1. 不同 inode 的 durability 请求被全局 g_durabilityLock 串行；
2. metadata write lock 覆盖 SPDK I/O 等待；
3. 单次 fsync 重复执行多个 blob sync 和 device flush；
4. SPDK 的异步能力无法跨 POSIX syscall 形成持续 in-flight 窗口。

数据库仍然看到完整同步 POSIX 语义：

    write/pwrite
        -> data ticket 完成并记录 data epoch
        -> dirty inode 保留在内存 dirty set

    fsync/fdatasync
        -> 提交 durability request(target epoch)
        -> 等待目标 data epoch
        -> coordinator 合并 metadata WAL
        -> 批次完成后唤醒对应 waiter

异步只存在于 HFS 内部，不暴露新的应用 API，因此 MySQL、MariaDB、RocksDB 和
其他 POSIX 应用不需要修改。

---

## 2. 当前证据和瓶颈定位

### 2.1 NVMe MySQL 对照结果

同一块 NVMe 上顺序运行原生 ext4 和 HFS direct-SPDK，MariaDB 10.11，
每档 2,000 queries × 3 iterations：

| 场景 | 并发 | 原生 QPS | HFS QPS | HFS/原生 |
|---|---:|---:|---:|---:|
| 范围读 | 1 | 1,103 | 1,086 | 98.5% |
| 范围读 | 4 | 4,292 | 3,284 | 76.5% |
| 范围读 | 16 | 6,557 | 4,843 | 73.8% |
| 单行 autocommit 写 | 1 | 8,299 | 9,709 | 117.0% |
| 单行 autocommit 写 | 4 | 18,349 | 15,267 | 83.2% |
| 单行 autocommit 写 | 16 | 30,769 | 16,949 | 55.1% |

单线程写入的反超不能作为稳定收益。并发增加后，HFS 的同步和元数据串行化
迅速成为主导因素。

### 2.2 与原生 SPDK FIO 的关系

当前 NVMe FIO 矩阵中，HFS 完整 POSIX 路径约为原生 SPDK 的 78%～83%。
MySQL 16 并发写入只有原生 ext4 的 55%，说明数据库额外损失发生在 durability、
metadata 和 caller wait 层，而不只是 NVMe 数据面。

### 2.3 代码级根因

1. src/hfs_lib.c 的 hfs_lib_durability_call 使用 g_durabilityLock 管理一个全局
   durability epoch。不同 inode 也要等待前一个 epoch。
2. hfs_lib_fsync_open_file 在 metadata write lock 生命周期内执行 metadata commit；
   commit 内部等待 SPDK completion，导致全局 metadata 操作被后端延迟放大。
3. dirty metadata 的一次 fsync 可能包含 data blob sync、一次 bdev flush、
   intent 写入并 sync、header/inode 写入、intent 清除并再次 sync、post-commit
   blob sync 和第二次 bdev flush。
4. src/hfs_meta.c 的 hfsMetaBlobWriteRange 为每个 metadata range 分配 DMA staging
   buffer，并执行对齐、RMW 和 copy；超过固定 intent slot 时回退到完整 image 保存。
5. src/hfs.c 的 hfsRequestWait 让每个 POSIX syscall 等待自己的 SPDK completion。
   当前 MySQL 测试使用 innodb-use-native-aio=0 和 innodb-flush-method=fsync，
   无法形成跨 syscall 的稳定 in-flight 窗口。
6. reactor 0 仍然是 metadata owner；增加 data reactor 不能消除 metadata owner、
   全局 lock 和 flush barrier 的串行点。

---

## 3. 目标架构

### 3.1 Mount 级 durability coordinator

新增 HfsDurabilityCoordinator，生命周期与一个 HFS mount 绑定：

    POSIX fsync/fdatasync
        -> 获取 fd/open-file 引用
        -> 读取目标 dataEpoch 和 dirty inode snapshot
        -> 创建 HfsDurabilityWaiter
        -> 入队 coordinator
        -> bounded batch window
        -> drain data tickets 和 build metadata WAL
        -> ordered metadata commit
        -> batch barrier/flush
        -> complete epoch 并唤醒 waiters

coordinator 只有一个 owner 状态机，不允许多个 POSIX 调用线程直接执行完整
metadata/device flush。所有等待均为 bounded condition/futex wait，不 busy-spin。

推荐默认容量：

| 参数 | 默认值 | 说明 |
|---|---:|---|
| durability batch window | 200 us | 有等待请求时收集同一时间窗内的请求 |
| maximum waiters per batch | 64 | 防止单批次尾延迟失控 |
| maximum dirty inode records | 64 | 超出后切分为连续 batch |
| metadata WAL page | 4 KiB | 与现有 v5 page 对齐 |
| queue capacity | 1024 | 满时有界等待或返回 EAGAIN |

所有容量都必须有硬上限，禁止无界 malloc。

### 3.2 Data epoch

每个 inode runtime 维护单调递增的 dataEpoch：

1. POSIX write 进入现有 Shard-63 ticket scheduler；
2. SPDK child 完成后，发布该写入完成的 dataEpoch；
3. write 可以返回，但 dirty inode 在 durability epoch 完成前不得清除；
4. fsync 记录目标 epoch，只等待自己需要的数据；
5. 同一 inode 的多个 waiter 可以共享一个完成 epoch；
6. 不同 inode 的 waiter 可以进入同一 metadata transaction。

这样可以保持 fsync 返回即此前写入达到持久化边界，同时允许不同文件合并提交。

### 3.3 Metadata WAL group commit

复用现有 src/hfs_meta_v5.[ch] 的显式 page/WAL 编码框架，接入正常运行时：

    prepare records:
      inode size/mtime/ctime
      directory create/unlink/rename
      blob id/orphan state
      mode/uid/gid and extent state

    commit marker:
      txid, lsn, record count, checksum

一次 group commit 的顺序固定为：

1. coordinator 在短 metadata critical section 中取得 dirty snapshot；
2. 释放 metadata lock，等待本批次 data epoch 全部完成；
3. 在预分配 WAL page 中写入 prepare records；
4. 写入 commit marker，只有 marker 完成才允许发布事务；
5. 以有序 SPDK batch 提交 WAL/page 数据；
6. 执行批次级 metadata sync 和 device barrier；
7. 发布 committed generation，清除对应 dirty set；
8. 唤醒目标 epoch 已满足的 waiters。

恢复规则：

- 只有 checksum 正确且存在 commit marker 的 transaction 才能 replay；
- 未提交 transaction 丢弃，不能发布半写 inode 或目录项；
- checkpoint 在后台将 WAL 合并到 inode pages，不能阻塞普通 POSIX read；
- v4/v3/v2/legacy image 继续只读兼容，首次可写 mount 在安全边界迁移到 v5；
- WAL 空间不足时先 checkpoint，再继续提交，不能把完整 image 写入作为常态回退。

### 3.4 Barrier 语义

本 Shard 不简单删除 flush，而是把 flush 从“每个 syscall 的多次 barrier”变为
“每个 durability batch 的有序 barrier”：

| 调用 | 要求 |
|---|---|
| fdatasync | 目标 data epoch 和 size/extent metadata 持久化；时间戳可延后 |
| fsync | 目标 data epoch、完整 inode metadata、namespace 和 WAL commit 持久化 |
| 同一 inode 并发 fsync | 共享同一或后继 epoch，不重复 flush |
| 不同 inode 并发 fsync | 可以进入同一 group commit |
| fsync 后的 write | 必须进入更高 epoch，不能被旧 waiter 错误满足 |
| crash/replay | 只重放 committed WAL，未提交数据不改变可见 namespace |

若底层 bdev 需要两个硬件顺序屏障，必须显式建模为 DATA_READY 到
COMMIT_DURABLE 两阶段；禁止每个 inode 独立执行同样的屏障链。

---

## 4. MySQL/RocksDB 兼容性要求

### 4.1 MySQL/MariaDB

必须保持：

- redo log 顺序写和 fsync 不能被普通 data page 覆盖；
- innodb_flush_log_at_trx_commit=1 时，事务提交前等待对应 durability epoch；
- fdatasync 不能跳过 size/extent 更新；
- O_APPEND、O_DSYNC、O_DIRECT 保持现有 errno 和顺序语义；
- redo、doublewrite、ibdata、独立 ibd 文件可以共享 batch，但不能共享错误 dirty 状态；
- 正常 shutdown、SIGKILL recovery 和重复 mount 结果不变。

验证 workload：

1. 范围读；
2. 单行 autocommit UPDATE；
3. 批量事务 UPDATE；
4. redo-heavy mixed workload；
5. 4/16/32 并发客户端；
6. 正常重启和 SIGKILL recovery。

### 4.2 RocksDB

必须保持：

- WAL Sync/Fsync 返回前，WAL 数据和 commit marker 达到持久化边界；
- RangeSync 不得被错误合并到未覆盖范围；
- Truncate、Close、WAL rollover 和 manifest rename 使用 exclusive metadata fence；
- direct reads、direct flush/compaction 继续遵守 aligned/direct buffer 语义；
- flush、compaction、WAL append 可以共享 queue，但恢复顺序不能被打乱；
- ldb checkconsistency、SIGKILL recovery 和跨进程重开继续通过。

验证 workload：

1. fillseq、overwrite；
2. readwhilewriting；
3. WAL-heavy small write；
4. flush/compact 并发；
5. direct I/O 和 buffered I/O；
6. 20,000～100,000 条数据、1/8/16 线程、60 秒运行。

---

## 5. 代码改动范围

### 5.1 新增 src/hfs_durability.[ch]

建议接口：

    int hfsDurabilitySubmit(Hfs *fs, HfsOpenFile *openFile, bool dataOnly);
    void hfsDurabilityMarkDataComplete(HfsInodeRuntime *runtime,
                                       uint64_t dataEpoch);
    void hfsDurabilityDrain(Hfs *fs);
    void hfsDurabilityStop(Hfs *fs);

所有权要求：

- caller 只拥有 waiter；
- coordinator 只拥有 batch、epoch 和 dirty snapshot；
- runtime 只拥有 inode-local data epoch；
- metadata WAL buffer 来自固定/有界 DMA pool；
- unmount 前完成或取消所有 waiter。

### 5.2 修改 src/hfs_lib.c

- 删除 g_durabilityRunning 对不同 inode 的全局互斥语义；
- fsync/fdatasync 只创建 waiter 并提交 coordinator；
- metadata lock 不得覆盖 SPDK I/O 等待；
- metadata lock 只保护 snapshot、generation publish 和 dirty set merge；
- 保留 crash point，并为 batch prepare/commit/flush 增加故障注入边界；
- 共享 open-file description 的多个 fd 共享目标 epoch，但不重复计数。

### 5.3 修改 src/hfs_meta.[ch] 和 src/hfs_meta_v5.[ch]

- 将 v5 WAL 编解码器接入正常 mount/write/fsync；
- 新增 namespace、orphan、blob allocation 和 timestamp record；
- 预分配 WAL ring，记录 head/tail/checkpoint generation；
- small transaction 只写 WAL page，不再常态化写完整 stable image；
- checkpoint 设置最大 dirty age，防止长期不 checkpoint；
- 保留旧格式读取和一次性迁移，迁移失败必须阻止写入而不是静默降级。

### 5.4 修改 src/hfs_io.[ch] 和 src/hfs_runtime.[ch]

- Shard-63 ticket scheduler 负责 data ticket 顺序；
- data completion 发布 inode epoch，不直接触发独立 metadata flush；
- reactor queue 允许一个 durability batch 的多个 data/metadata child 连续提交；
- metadata owner 不得在单个 child 提交时阻塞另一个 POSIX caller；
- DMA staging 只保留一次 copy，group commit 不得再次复制用户 data buffer。

### 5.5 统计和 tracepoint

新增计数和时延：

    durability_submit
    durability_batch
    durability_batch_waiters
    durability_batch_inodes
    durability_batch_bytes
    durability_wait_ns
    durability_data_wait_ns
    durability_metadata_wait_ns
    durability_flush_wait_ns
    metadata_wal_bytes
    metadata_checkpoint_bytes
    metadata_checkpoint_count
    metadata_full_image_fallback

继续输出并关联：

    durability_leader
    durability_waiter
    metadata_transactions
    metadata_sync_count
    flush_count
    io_copy_ops
    io_copy_bytes
    spdk_submit
    spdk_complete
    lock_meta_wait_count
    lock_meta_wait_ns
    ticket_dependency_wait_ns

trace 必须用 txid、epoch、inodeId、requestId 关联：

    DATA_EPOCH_READY
    DURABILITY_ENQUEUE
    DURABILITY_BATCH_BEGIN
    METADATA_WAL_PREPARE
    METADATA_WAL_COMMIT
    DURABILITY_FLUSH_BEGIN
    DURABILITY_FLUSH_END
    DURABILITY_WAKE

---

## 6. 分阶段开发

### Shard-64A：统计和基准闭环

- 为 MySQL fsync/fdatasync、RocksDB Sync/Fsync/RangeSync 增加 epoch/batch 统计；
- 增加带 HFS_STATS_PATH、HFS_TRACE_PATH 的数据库 benchmark；
- 记录 native NVMe、HFS direct NVMe、commit、CPU、RSS、p50/p99、错误和恢复结果；
- 统计关闭时 FIO/MySQL/RocksDB 不得有可测回归。

### Shard-64B：durability coordinator

- 新增 bounded waiter queue、epoch 分配和 batch window；
- 将 g_durabilityLock 改成 coordinator admission lock；
- 先只合并同一 inode 的重复 fsync，暂时复用旧 metadata commit；
- 验证正常 fsync、并发 fsync、fdatasync 和 close/unmount。

验收：不同 inode 的 fsync 不再全局互相阻塞；crash injection 全部恢复。

### Shard-64C：v5 WAL group commit

- 接入 v5 page/WAL runtime；
- 支持 inode size/extent、timestamp、namespace、orphan 和 blob state；
- 一个 batch 只提交一次 WAL transaction；
- 增加 WAL replay、checkpoint、空间回收和旧格式迁移。

验收：prepare、data complete、commit marker、flush、checkpoint、replay 每个边界
均有故障测试，MariaDB/RocksDB 一致性检查全部通过。

### Shard-64D：数据库专用语义验证

- MariaDB：redo、doublewrite、autocommit、批量事务、正常重启、SIGKILL；
- RocksDB：WAL、manifest、flush、compaction、direct I/O、SIGKILL；
- 对比 native ext4 与 HFS direct NVMe，固定 vCPU、NUMA、数据量和参数；
- 确认没有 /hfs 数据请求落入内核 /dev/null 路径。

### Shard-64E：性能收口

- 调整 batch window、WAL page、checkpoint threshold 和 reactor credits；
- 只有统计证明 copy 是主因时，才继续优化普通 buffer copy；
- 保留 HFS_DURABILITY_GROUP_COMMIT=0 作为短期回退开关；
- 更新 doc/bug.md、doc/design/fio_baseline_notes.md 和阶段总结。

---

## 7. 性能和正确性验收门槛

### 7.1 性能目标

同一块 NVMe、同一数据库版本和相同 workload：

| 指标 | Shard-63 当前 | Shard-64 目标 |
|---|---:|---:|
| MySQL 16 并发读 HFS/native | 73.8% | ≥90% |
| MySQL 16 并发写 HFS/native | 55.1% | ≥85% |
| MySQL 4 并发写 HFS/native | 83.2% | ≥90% |
| RocksDB WAL/写入吞吐 | 以当前基线为准 | ≥90% native，且不低于现有 HFS |
| HFS P99/native P99 | 未统一采集 | ≤1.5x |
| metadata full-image fallback | 未约束 | 正常 workload 为 0 |
| flush_count / durability_batch | 每 syscall 多次 | ≤2 |
| metadata_sync_count / durability_batch | 每 syscall 多次 | ≤2 |

不能通过降低 fsync 语义、关闭 WAL 或跳过 recovery 达标。

### 7.2 正确性目标

- Make 和 CMake Release 强制构建通过；
- POSIX contract、fd lifecycle、metadata transaction、ticket、queue、DMA 和 runtime
  stress 全部通过；
- MariaDB 初始化、CRUD/DDL、事务、LONGTEXT、临时表、正常重启、SIGKILL recovery、
  4/16/32 并发全部通过；
- RocksDB fillseq/readwhilewriting/overwrite、WAL、flush/compact、direct I/O、
  SIGKILL recovery、ldb checkconsistency 全部通过；
- 每个 WAL/barrier/checkpoint 故障边界至少运行 3 次；
- spdk_submit == spdk_complete，errors/reject=0；
- 不允许新增未记录的严重或致命 bug。

---

## 8. 风险、回退和禁止事项

### 风险

- batch window 可能提高低负载单个 fsync 的尾延迟；
- WAL ring 空间管理错误会导致 mount 只读或恢复失败；
- namespace transaction 与 data epoch 顺序错误会暴露旧 inode size 或孤儿 blob；
- 过大的 batch 会让一个慢请求拖累不相关数据库文件。

### 回退

1. HFS_DURABILITY_GROUP_COMMIT=0：回退到 Shard-63 durability 路径；
2. HFS_META_V5_RUNTIME=0：只允许已验证的旧格式兼容路径；
3. 任一 WAL/replay 校验失败时 mount fail-closed，不得静默当作空文件系统；
4. 回退开关只用于验证和故障隔离，不能作为性能验收配置。

### 禁止事项

- 不得把 fsync 改成只返回成功而不等待持久化；
- 不得用 O_DIRECT、mmap 或应用改造绕过普通 POSIX 兼容性要求；
- 不得重新引入独立的高性能 public async API；
- 不得通过关闭 WAL、降低 innodb_flush_log_at_trx_commit 或关闭 RocksDB WAL
  制造性能提升；
- 不得在 metadata lock、ticket lock 或 DMA lease 内等待 SPDK completion。

---

## 9. 完成定义

当且仅当以下条件同时满足，Shard-64 才算完成：

1. durability coordinator、epoch waiter 和 v5 WAL group commit 已集成到默认 POSIX 路径；
2. fsync/fdatasync、namespace、orphan、checkpoint/replay 的语义和故障边界有测试；
3. MySQL 16 并发写入达到至少 85% native NVMe，RocksDB WAL/写入达到至少 90% native
   或相对现有 HFS 不回归；
4. 所有 MariaDB/RocksDB recovery、consistency、Make/CMake 和既有回归通过；
5. 统计/trace 能证明 batch size、metadata sync、flush、wait 和 copy 的变化；
6. 文档、doc/bug.md、支持边界和回退配置已更新，并在 main 上完成提交。

成功标准不是“SPDK submit 更快”，而是让一次数据库事务提交只承担一次有序、可恢复、
可批量合并的 durability 成本，同时保留完整 POSIX 语义。

---

## 10. 本轮实现与验证状态

本轮已经把安全、默认开启的部分落到完整 POSIX 路径：

- `HfsDurabilityCoordinator` 与 bounded waiter queue 已加入 mount 生命周期；
- `fsync`/`fdatasync` 通过 200 us/64 请求窗口做批处理，并按
  `HfsInodeRuntime *` 去重，避免同 inode 多个 open-file description 重复获取
  `syncLock`；
- 一个 batch 只执行一次设备 barrier，blob metadata sync 只对每个 runtime 执行一次；
- worker 屏蔽宿主终止信号，避免 MariaDB bootstrap 的 signal thread 停滞；
- 新增 `durability_group_batches`、`durability_group_requests`、
  `durability_group_batch_max` 统计，并保留 `HFS_DURABILITY_GROUP_COMMIT=0` 回退；
- `HFS_DURABILITY_GROUP_COMMIT=1` 与 `=0` 的 barrier、metadata group、crash/recovery、
  POSIX、fd lifecycle、16 线程 runtime stress 均通过；
- MariaDB/MySQL smoke（初始化、CRUD/DDL、事务、正常重启、SIGKILL recovery、
  mysqlslap 4/16 并发）和 RocksDB smoke（fillseq/readrandom/overwrite、flush/compact、
  readwhilewriting、WAL SIGKILL recovery、`ldb checkconsistency`）均通过；
- NVMe FIO 32 job/32 depth 对比原生 SPDK，所有档位 errors=0，HFS 为原生 SPDK
  总 IOPS 的约 77.4%～83.0%（4 KiB～64 KiB）。

`src/hfs_meta_v5.[ch]` 目前仍是经过独立校验的离线 v5 编解码器，运行时默认继续使用
已验证的 v4 intent/stable-image 事务。原因是直接切换在线 metadata blob 会改变已有
磁盘布局和旧格式迁移边界，未经完整 mixed-version/replay 验证不能安全开启。因而本轮
完成的是 coordinator/group-commit 和数据库 POSIX 验收；v5 WAL runtime 接入应作为独立
迁移 shard，不能把当前 v4 路径误标成 v5 WAL 已上线。

数据库短 benchmark 的统计也显示了这一边界：RocksDB 10 秒 Aio 运行中
`spdk_submit == spdk_complete`、`errors=0`，但单线程顺序 workload 的
`durability_group_batch_max=1`，说明只有并发 fsync 才能体现 group commit 的合并收益，
后续性能结论必须使用真实并发 WAL/事务 workload。
