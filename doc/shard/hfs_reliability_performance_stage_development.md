# HFS 可靠性与性能优化阶段计划（shard-36 起）

> 状态：shard-36～38 已完成，正确性闸门 A 已通过；下一步进入 shard-39 可观测性与公平性能基线<br>
> 制定日期：2026-09-05  
> 适用基线：`main@91db26a`（shard-36 已完成）
> 输入文档：`review/hfs_architecture_performance_review_2026-09-05.md`  
> 历史计划：`doc/shard/mysql_rocksdb_stage_development.md`

## 1. 文档定位

本文把 HFS 架构与性能审查中发现的问题转换为可执行的 shard 开发计划，从 shard-36 开始推进。

- shard-26～35 的完成状态以 `doc/shard/mysql_rocksdb_stage_development.md`、Git 历史和当前源码为准。
- 本文优先于旧文档中尚未实施的 shard-36 及以后内容。
- 当前支持口径不变：单数据库进程、持久化 AIO/NVMe bdev、MySQL/MariaDB-compatible server 与 RocksDB。
- 多进程共享挂载、`mmap`、native libaio/io_uring 仍不在当前支持范围内；不得因本阶段优化提前宣称支持。
- 本阶段目标不是继续堆叠 preload wrapper，而是依次建立正确性、故障原子性、持久化、可测量并发和异步 I/O 能力。

## 2. 当前基线与主要约束

### 2.1 已具备能力

- 请求级 I/O 和固定 DMA pool，已移除文件共享 scratch buffer。
- 对齐直写、非对齐首尾 RMW、并发 positioned I/O、`readv`/`writev`。
- fd slot、open-file description、`dup*`、共享 offset、unlink-open 和 rename 覆盖延迟回收。
- v4 元数据兼容升级、短事务 intent、checksum/generation 和挂载重放。
- MySQL/MariaDB 与 RocksDB 单进程持久化验收。
- 默认关闭的运行时统计，以及数据库性能基线脚本。

### 2.2 已确认的关键问题

| 类别 | 问题 | 影响 | 当前记录 |
| --- | --- | --- | --- |
| 数据语义 | 文件缩小后再扩大会暴露旧数据 | 数据泄露、POSIX 语义错误 | `doc/bug.md` #45 |
| 时间戳 | 覆盖写不更新 mtime | 数据库/工具状态判断错误 | `doc/bug.md` #46 |
| 容量统计 | `statfs/statvfs` 容量字段错误或陈旧 | 运维与数据库容量决策错误 | `doc/bug.md` #47 |
| 异步错误 | SPDK callback 的 `bserrno` 未完整向上传播 | 底层 I/O 失败可能被当成成功 | `doc/bug.md` #48 |
| 故障原子性 | 元数据事务失败时内存状态和资源缺少统一回滚 | 内存/磁盘分歧、资源泄漏 | `doc/bug.md` #49 |
| 持久化 | `fsync` 最后一次元数据提交后缺少持久化屏障 | 掉电后可能丢失已报告成功的元数据 | `doc/bug.md` #50 |
| 并发架构 | 单 SPDK owner 线程、同步消息桥和 runtime 全局锁 | QD 难以上升，多线程扩展差 |
| fd 热路径 | fd 查询/引用管理经过共享锁 | 每次 I/O 都支付全局同步成本 |
| 文件 I/O | mount 级 I/O/resize/append 串行化 | 不同文件和不重叠区间互相阻塞 |
| 元数据 | 固定 inode 数组、线性查找和整镜像提交 | O(N) 查找，单次修改写放大随 inode 数增长 |
| 数据路径 | 固定 16×1 MiB DMA buffer、同步分块复制 | 内存占用固定，copy/RMW 和队头阻塞明显 |
| 空洞处理 | unmap 后补零写 | 抵消 discard/unmap 的性能与寿命收益 |

### 2.3 审查测量基线

- Malloc bdev、4 KiB randrw：1 线程约 12,796.72 IOPS，4 线程约 22,620.64 IOPS，仅 1.77 倍扩展。
- 4 线程样本中 fd read/write lock 约 250,221/485,279 次，metadata read/write lock 约 112,857/129,826 次。
- 同一样本 DMA pool 峰值仅 4，而 SPDK submit/complete 均为 242,732，说明主要瓶颈不是 16 个 buffer 耗尽，而是同步提交和锁竞争。
- 正确性探针确认：shrink-grow 后可读回 4096 字节旧数据；覆盖写 mtime 保持不变；1.5 GiB 后端 `statvfs` 仅报告 783,360 字节。

以上数字只作为问题定位基线。后续性能回归必须由 shard-39 建立统一测试矩阵后再做正式对比。

## 3. 总体策略

### 3.1 执行顺序

```mermaid
flowchart LR
    S36[shard-36\nPOSIX 数据语义] --> S37[shard-37\n错误传播与故障原子性]
    S37 --> S38[shard-38\nfsync 与持久化屏障]
    S38 --> G1{正确性闸门 A}
    G1 --> S39[shard-39\n可观测性与公平基线]
    S39 --> S40[shard-40\nfd/runtime 去全局锁]
    S40 --> S41[shard-41\nper-inode/区间并发]
    S41 --> S42[shard-42\n内存元数据索引]
    S42 --> S43[shard-43\n元数据 v5 与 WAL]
    S43 --> S44[shard-44\n脏页 checkpoint 与 group commit]
    S44 --> G2{元数据闸门 B}
    G2 --> S45[shard-45\n异步 libhfs API]
    S45 --> S46[shard-46\n多 reactor/channel]
    S46 --> S47[shard-47\nDMA/vector/hole 优化]
    S47 --> S48[shard-48\nNVMe/数据库收口]
```

### 3.2 不可破坏的原则

1. 正确性优先于吞吐。shard-36～38 未通过前，不合入会扩大并发窗口的重构。
2. 每个异步请求都必须有请求私有的结果、错误码和生命周期状态；不得复用 mount 级 completion 传递结果。
3. 每个对外成功的 `fsync/fdatasync` 都必须能映射到明确的持久化顺序和屏障。
4. 优化不得把不支持的调用静默落到宿主 `/dev/null` 或返回伪成功。
5. 性能结论必须同时给出吞吐、p50/p99/p99.9、CPU、RSS、错误数、SPDK submit/complete 和测试 QD。
6. 先保留同步 POSIX/preload 接口兼容，再在内部引入异步 API；同步接口最终作为异步核心的等待式封装。
7. 磁盘格式变更必须支持旧格式只读挂载和可恢复迁移，不允许原地无回滚覆盖唯一副本。
8. 每发现或修复一个 bug，立即维护 `doc/bug.md`，记录现象、排查过程、根因、修复和回归用例。
9. 每个 shard 在独立分支开发、完成回归后提交，并立即合入 `main`；合入后确认 Git 状态与 VM 回归结果。

## 4. 阶段闸门与目标

| 闸门 | 覆盖 shard | 必须达到的结果 |
| --- | --- | --- |
| A：正确性与持久化 | 36～38 | 已知 #45～#53 有可重复回归；注入 I/O 失败不再返回伪成功；`fsync` 顺序有文档、trace 和崩溃边界验证 |
| B：锁与元数据架构 | 39～44 | 普通元数据变更不再整 blob 提交；热路径无 mount 级 fd/runtime/I/O 全局锁；4 线程 4 KiB workload 相对 1 线程至少 3.0 倍扩展 |
| C：异步数据面 | 45～47 | 单线程可提交 QD>1；多 reactor/channel 可配置；对齐 vector I/O 避免不必要 bounce copy；错误与完成数严格匹配 |
| D：阶段收口 | 48 | Malloc/AIO/真实 NVMe、MySQL/MariaDB、RocksDB 全回归；更新支持边界和下一阶段产品架构决策 |

性能目标是 go/no-go 判断线，不是通过篡改 workload 获得的发布数字。若正确性回归通过但性能目标未达，必须保存 profile、锁等待和队列深度证据后再决定继续优化或调整目标。

## 5. shard-36：POSIX 数据与元数据语义收口

> 状态：已完成（2026-09-05）<br>
> 验收：Make 强制构建、CMake Release、POSIX 数据契约、I/O fast path、fd lifecycle、
> metadata transaction/path concurrency、persistent、POSIX contract、32 线程 60 秒 runtime、
> MariaDB/MySQL persistent recovery 与 RocksDB persistent recovery 均通过。

### 目标

修复已确认的数据泄露、mtime 和容量统计错误，把性能重构所依赖的文件大小、分配量和时间戳语义固定下来。

### 主要改动

- 在 `src/hfs_io.[ch]` 增加统一的 range zero helper。
  - shrink 时清零新 EOF 所在分配单元的尾部，并释放/清零完整后继分配单元。
  - grow 时保证 `[oldSize, newSize)` 读取为零，包括已经物理分配过的旧范围。
  - `O_TRUNC`、`ftruncate` 和后续 hole 操作复用同一零化规则。
- 区分逻辑文件大小与已分配容量，避免仅修改 inode size 就改变可见数据。
- 所有成功修改文件内容的 write/pwrite/writev 路径更新 mtime/ctime；只有实际扩展时更新 size。
- 为 inode 引入明确的 dirty bit：size、mtime、ctime、allocation，避免后续只靠“size 是否变化”决定提交。
- 修复 `statfs/statvfs/statvfs64`：
  - 使用格式化时的数据区 cluster 总量，而不是 inode 数或临时变量。
  - 通过已分配 cluster 计数计算 `f_bfree/f_bavail`。
  - `f_frsize/f_bsize` 与实际分配和 I/O 对齐单位保持一致。
- 检查并关闭 `doc/bug.md` 中与 MariaDB shutdown 残留文件相关的已知问题；若根因独立且不能在本 shard 安全修复，保留为后续显式依赖，不得静默忽略。

### 计划修改范围

- `src/hfs_io.c`、`src/hfs_io.h`
- `src/hfs_file.c`
- `src/hfs_lib.c`
- `src/hfs_meta.c`、`src/hfs_meta.h`
- `src/hfs_preload.c`
- 新增 `scripts/db/posix_data_contract_test.sh`

### 验收

- shrink→grow、跨 cluster shrink→grow、`O_TRUNC`→grow 均只能读到零。
- 覆盖写、非扩展 pwrite、writev 均更新 mtime；只读不更新 mtime。
- 重新挂载后上述 size、mtime 和零化语义保持不变。
- 容量在创建、扩展、punch、unlink、重新挂载后可解释且守恒，误差不超过一个分配单元。
- 连续运行 `posix_contract_test.sh` 和新数据契约脚本 20 次。
- 既有 Make/CMake、persistent、fd lifecycle、I/O fast path、MySQL 和 RocksDB smoke 全部通过。

### 提交建议

`shard-36: close POSIX data and capacity semantics`

## 6. shard-37：异步错误传播与故障原子性

### 目标

保证每一个 SPDK completion 的状态都能精确到达调用者，并确保部分失败时内存状态、磁盘状态和资源所有权可回滚或可重放。

### 主要改动

- 定义请求私有结果对象，至少包含 operation、request id、submit rc、completion rc、`bserrno`、实际完成字节数和生命周期阶段。
- 所有 callback 显式写入请求结果；等待方只消费对应请求，不再读取共享 completion 状态。
- 建立统一 errno 映射表，保留原始 SPDK/bdev 错误用于诊断，POSIX 层返回稳定 errno。
- 为 metadata mutation 引入 staged state：
  - 验证并准备 after-image。
  - 保留内存 undo 信息。
  - intent 成功后才进入磁盘提交阶段。
  - stable image/clear intent 任一失败都保留可恢复状态，不能把失败事务发布为稳定内存状态。
- 用单一 cleanup state machine 管理 blob open/create/resize/close、DMA buffer、fd placeholder 和 orphan 引用，消除多出口遗漏。
- 所有统计中的 submit/complete/error/retry 必须按 request id 闭合。

### 测试与故障注入

- 新增 `scripts/db/io_error_injection_test.sh`。
- 覆盖 read/write/resize/unmap/write-zeroes/flush/blob open/blob close/metadata intent/stable/clear 每个阶段的失败。
- 对每个注入点验证：调用返回非零、errno 正确、没有伪成功、资源计数回到基线、重新挂载后状态属于事务前或事务后之一。
- 增加重复 1,000 次失败注入的 fd/blob/DMA 泄漏检查。
- 在运行时 stop 和请求完成竞争下验证不会 use-after-free 或重复完成。

当前实现的第一轮回归已覆盖 owner-thread callback 的 blob open/sync/close、read/write/resize、
metadata open/write、容量采样，以及提交前 metadata-write/blob-delete 拒绝。后者验证 committed
unlink 不会复活旧名称，并在下一进程挂载时回收不可达 data blob、恢复空闲 cluster；另验证
`PUNCH_HOLE` 的 accelerated write-zeroes callback 失败后，DMA fallback 完成零化才返回成功。真实
SPDK error-bdev 的 flush failure 已覆盖并确认 `fsync` 返回 `EIO`；AIO blobstore 不会将逻辑
unmap 下发为底层 UNMAP，因此 unmap/write-zeroes/delete/unload 的真实矩阵仍是 shard-37 的剩余工作。

### 验收

- 关闭 `doc/bug.md` #48、#49，附完整注入矩阵。
- `spdk submit == complete`，并满足 `complete == success + failure`。
- 所有失败路径都能在调用者观察到非零错误。
- ASAN/UBSAN 可用配置下完成专项回归；TSAN 若受 SPDK 环境限制，记录限制并用锁顺序/压力测试补证。

### 提交建议

`shard-37: propagate bdev errors and make failures atomic`

## 7. shard-38：fsync、元数据顺序与掉电持久化

> 状态：已完成（2026-09-06）<br>
> 验收：Make/CMake Release、错误注入与屏障顺序、崩溃恢复、POSIX/fd/元数据/I/O 回归、
> MariaDB/MySQL 持久化恢复、RocksDB WAL/一致性恢复和数据库性能基准均通过。CMake 与
> Make 的系统库依赖已保持一致（包含 `lz4`）。

### 目标

定义并实现可验证的 `fsync/fdatasync` 持久化契约，修复最后一次 metadata commit 位于最后一个 flush 之后的问题。

### 主要改动

- 新增 `doc/design/hfs_durability_model.md`，明确数据、inode、directory entry、intent、stable image、clear intent 的先后关系。
- 为每次同步分配 sync epoch，记录该 inode 已提交和已持久化的最高 epoch。
- `fdatasync` 至少保证文件数据和恢复该数据所必需的 size/allocation metadata。
- `fsync` 额外保证 mtime/ctime 和影响文件可达性的必要元数据；目录项语义必须在文档中显式定义。
- 第一版以正确性为目标，可使用较保守的屏障序列：
  1. 等待目标 inode 既有 I/O 完成。
  2. flush 数据写。
  3. 提交 inode/目录 metadata transaction。
  4. flush metadata commit。
  5. 仅在最后屏障成功后向调用者返回成功。
- clear intent 可在确保 replay 幂等的前提下延后，但不能让已返回成功的事务在恢复后回退。
- 把 flush descriptor 生命周期纳入 mount 管理，避免每次 `fsync` 重复 open/close。

### 测试与故障注入

- 已新增 `scripts/db/durability_barrier_test.sh` 与默认关闭的
  `HFS_DURABILITY_TRACE_PATH`；它断言成功路径的 data barrier、metadata commit、post-commit
  barrier 偏序。`io_error_injection_test.sh` 额外覆盖 post-commit barrier 的 `EIO` 与 retry；VM
  Make/CMake、核心回归、MariaDB 与 RocksDB 恢复流程均已通过。新增
  `durability_crash_recovery_test.sh` 在每个 fsync barrier 后停止进程并新进程验证恢复；后续仍需
  覆盖每个 submit/completion 边界和可控物理掉电。
- 在每个 submit/completion/barrier 边界执行确定性 crash，重新挂载后验证：
  - 未返回成功的操作允许事务前或事务后状态。
  - 已返回成功的 `fsync/fdatasync` 必须恢复为事务后状态。
  - 不允许 size 指向未持久化数据或目录项指向不存在 blob。
- MySQL/RocksDB SIGKILL recovery 继续保留，但不能替代屏障级故障注入。

### 验收

- 关闭 `doc/bug.md` #45～#53。
- 持久化模型、代码屏障、trace 和恢复结果四者一致。
- 所有 flush 错误返回调用者，失败 epoch 不前移。
- 正确性闸门 A 通过后，才允许进入去锁和异步化阶段。

### 提交建议

`shard-38: enforce fsync durability ordering`

## 8. shard-39：可观测性与公平性能基线

### 目标

建立能够区分锁、队列、copy、RMW、元数据写放大和设备延迟的测量体系，避免用 HFS QD≈1 与裸 SPDK QD=32 的结果直接归因。

### 主要改动

- 扩展 `src/hfs_stats.[ch]`，统计默认仍关闭，开启时增加：
  - 每类锁 wait/hold 时间直方图和 contention 次数。
  - runtime queue depth、inflight、排队时间、owner 执行时间。
  - I/O 大小、对齐率、RMW 次数和字节数、copy 字节数。
  - metadata log/image/page 写入字节、flush 次数和事务批量。
  - DMA size class 使用量、等待次数和峰值。
- 统计采用 per-thread/per-reactor shard，退出或导出时聚合，避免统计本身制造全局锁。
- 扩展 `scripts/db/database_benchmark.sh`，记录完整环境、参数、commit、CPU 亲和、bdev 类型和 raw/HFS 测试口径。
- 新增 `scripts/db/hfs_scaling_benchmark.sh`：
  - raw SPDK QD=1/4/16/32。
  - HFS sync API 线程数=1/2/4/8/16。
  - 4 KiB randread/randwrite/randrw 与 128 KiB 顺序读写。
  - inode 数=1K/10K/60K 的 create/open/stat/readdir/rename/unlink。
- 把结果追加到 `doc/design/fio_baseline_notes.md`，不得覆盖历史 shard-31/35 数据。

### 验收

- stats 关闭时，4 KiB IOPS 下降不超过 1%；开启完整统计时下降不超过 3%。
- 所有报告同时包含吞吐、延迟、CPU、RSS、错误、submit/complete 和有效 QD。
- 每个全局锁都能给出 wait/hold 数据；每次 metadata mutation 都能给出实际写入字节。
- 同一 bdev 上补齐 raw SPDK QD=1 对照，形成可复现的公平基线。

### 提交建议

`shard-39: establish contention and queue-depth baselines`

## 9. shard-40：fd 注册表与 runtime 热路径去全局锁

### 目标

移除每次 I/O 都经过的 fd 全局 rwlock 和 runtime submit/complete 全局 mutex，使不同请求至少能并发排队和完成。

### 主要改动

- fd registry 改为分桶或原子 slot：
  - slot detach 使用原子 exchange 或桶锁。
  - `HfsOpenFile` 使用 increment-if-not-zero 的原子引用计数。
  - acquire 后复查 slot，使用 generation 防止 fd 重用 ABA。
  - close 先摘除 slot，再释放 registry owner reference；最后引用负责析构。
- 非 HFS fd 的快速判定不得获取 HFS 全局锁，避免 preload 拖慢宿主 fd。
- runtime 引入原子 `accepting`、`inflight` 和 request queue 状态。
- completion 判断是否在 owner thread 使用 TLS/thread identity 快路径，不再获取 runtime mutex。
- stop 顺序固定为：禁止新请求、等待 inflight/queue 清空、提交 stop、回收 channel/thread。
- 保留 shard-31 的 owner PID/fork child 防护。

### 测试

- 扩展 `fd_lifecycle_smoke.sh`：fd 快速复用、并发 close/acquire、dup2 替换、ABA、最后引用析构一次。
- 扩展 `runtime_stress.sh`：submit/complete/stop 竞争、超时、错误完成、fork child。
- 增加 1/4/16/64 线程 fd lookup microbenchmark。

### 验收

- 普通 read/write/pread/pwrite 的 fd acquire 路径不再获取进程级 fd rwlock。
- runtime submit 和 owner-thread completion 不再共享同一个全局 mutex。
- 64 线程压力下无 double close、悬挂指针、引用泄漏和请求丢失。
- 相对 shard-39，同口径 4 线程 4 KiB workload 吞吐至少提升 20%，或用 profile 证明剩余主瓶颈已转移到文件/元数据锁。

### 提交建议

`shard-40: remove fd and runtime global hot-path locks`

## 10. shard-41：per-inode 状态与区间 I/O 并发

### 目标

让不同文件以及同一文件不重叠区间的 I/O 并行执行，移除 mount 级 `ioLock`、`resizeLock` 和 `appendLock`。

### 主要改动

- 增加 `HfsInodeRuntime`，承载运行时 size、allocation、open count、dirty bits、outstanding I/O、sync epoch 和局部锁。
- append 锁、resize 锁和 fsync 等待状态下沉到 inode。
- 对齐且不改变 size 的 positioned I/O 不获取 metadata write lock。
- 非对齐写只锁定首尾 RMW unit；使用按 `(blobId, unitIndex)` 哈希的 range lock，避免独立文件互相串行。
- resize、punch 和 write 的区间冲突有明确锁序；禁止持有 metadata lock 等待 SPDK completion。
- `O_APPEND` 在 inode 内原子分配逻辑 offset，再提交数据；失败时恢复或以可重放 sequence 处理空洞。
- fsync 等待目标 inode 的提交序列，不阻塞无关 inode。

### 测试

- 新增 `scripts/db/inode_io_concurrency_test.sh`。
- 覆盖不同文件并发、同文件不重叠 aligned write、同 unit 非对齐 RMW、append+truncate、punch+write、fsync+write。
- 每轮用确定性 pattern 和 checksum 验证不存在 torn RMW、交叉覆盖或错误 size。
- 运行 1/2/4/8/16 线程扩展矩阵并导出锁等待直方图。

### 验收

- 数据热路径不再持有 mount 级 I/O/append/resize 全局锁。
- 不同 inode 的 aligned positioned I/O 无共享文件级互斥锁。
- 相对 shard-39 基线，4 线程 4 KiB workload 至少达到单线程 3.0 倍；若设备已饱和，需用 raw SPDK 上限和 CPU/profile 证明。
- p99 不因锁分片出现超过 20% 的回退，p99.9 无周期性全局停顿。

### 提交建议

`shard-41: add per-inode and range-level I/O concurrency`

## 11. shard-42：内存元数据索引与常驻 metadata handle

### 目标

在暂不改变 v4 磁盘格式的前提下，消除 inode/name 线性查找、目录全表扫描和 metadata blob 反复 open/close。

### 主要改动

- mount 时构建：
  - `inodeId -> inode runtime` 索引。
  - `(parentInodeId, name) -> inodeId` dentry 索引。
  - `parentInodeId -> ordered children` 目录索引。
- create/unlink/rename 在一个 metadata mutation 中同步更新 inode table 与索引；任何失败都通过 shard-37 的 undo state 回滚。
- readdir 直接遍历目录 children，不再扫描全部 inode 两次。
- open/stat/path resolve 使用 dentry hash，inode id 查询使用 id hash。
- metadata blob descriptor/handle 在 mount 生命周期内常驻，由 shard-37 的错误与资源模型管理。
- 保持 v4 on-disk layout 不变，先隔离“内存查找收益”和“磁盘格式迁移风险”。

### 测试

- 新增 `scripts/db/metadata_scaling_benchmark.sh`。
- 在 1K/10K/60K inode 下测量 create/open/stat/readdir/rename/unlink 的吞吐和 p99。
- fuzz rename/unlink/create 顺序，每轮对比 inode table、id index、dentry index 和目录 index 的一致性。
- crash/reopen 后重新构建索引并与磁盘 inode table 校验。

### 验收

- path lookup、inode id lookup 和单目录 readdir 不再依赖全 inode 数量的线性扫描。
- 60K inode 时单文件 open/stat p99 不超过 1K inode 时的 2 倍。
- metadata blob 不再为每个普通元数据操作重复 open/close。
- 所有旧 v4 镜像可直接挂载并通过现有 recovery 测试。

### 提交建议

`shard-42: add indexed in-memory metadata state`

## 12. shard-43：元数据 v5 页式布局与 WAL 基础

### 目标

建立不依赖原生 C struct 整体序列化的可演进磁盘格式，并用 WAL 表达小事务，为消除整 blob 写放大做准备。

### 设计范围

- v5 superblock A/B：magic、version、feature flags、generation、checkpoint LSN、WAL head/tail、root page、checksum。
- 固定端序和显式字段编码；磁盘结构不得直接依赖 compiler padding、`time_t` 或宿主指针宽度。
- page header：page type、page id、generation、payload length、checksum。
- WAL record：length、type、transaction id、LSN、payload checksum、commit marker。
- WAL 操作至少覆盖 create、unlink、rename、setattr、size/allocation 更新和 orphan 回收。
- recovery 只重放有完整 commit record 的事务；重复重放必须幂等。

### 迁移策略

- v2/v3/v4 继续支持读取。
- v4→v5 采用新区域构建、校验、flush、最后切换 superblock 的 copy-on-write 迁移。
- 切换前保留 v4 唯一稳定副本；失败后必须仍能以 v4 挂载。
- 若空间不足或不支持迁移，明确返回错误，不得部分覆盖旧格式。

### 计划修改范围

- 重构 `src/hfs_meta.[ch]`，必要时拆分 `hfs_meta_format.[ch]`、`hfs_meta_wal.[ch]`。
- 新增 `scripts/db/metadata_v5_migration_test.sh`。
- 新增 `scripts/db/metadata_wal_recovery_test.sh`。
- 更新 `doc/design/hfs_durability_model.md`。

### 验收

- v4 镜像可迁移到 v5，迁移前、中、后任意故障都能挂载到一个完整版本。
- WAL truncate、checksum 错误、重复 record、缺 commit marker 均被正确处理。
- 常见单 inode 变更写入 WAL 的字节数与 inode 总数无关。
- endian/packing 静态断言和离线格式解析测试通过。

### 提交建议

`shard-43: introduce metadata v5 pages and WAL`

## 13. shard-44：脏页 checkpoint 与 group commit

### 目标

用 WAL + dirty page checkpoint 替代整 metadata blob 的 pending/stable/clear 三次镜像提交，并在保持 shard-38 持久化语义的前提下降低 flush 和写放大。

### 主要改动

- 把 inode/dentry/free-space 状态映射为独立 metadata page，仅修改受影响页面。
- transaction 提交流程：append WAL records → append commit → flush WAL epoch → 发布内存状态。
- 后台 checkpoint 批量写 dirty pages，写完并 flush 后推进 checkpoint LSN，再回收旧 WAL。
- group commit 按时间窗、字节阈值或显式 `fsync` 触发；同步调用等待包含自身事务的 durable epoch。
- metadata 空间不足时优先 checkpoint/reclaim；失败必须回到可重试状态。
- checkpoint 与 foreground lookup/update 不持有长时间全局 metadata write lock。

### 测试

- 新增 `scripts/db/metadata_checkpoint_test.sh`。
- 覆盖 checkpoint 前/中/后 crash、WAL wrap、空间耗尽、重复 mount/replay、并发 rename/unlink/create。
- 记录每个逻辑事务的 WAL bytes、page bytes、flush 数和 batch size。
- 在 1K/10K/60K inode 下比较 v4 整镜像和 v5 WAL/page 写放大。

### 验收

- 普通 create/unlink/rename/setattr 不再读写完整 metadata blob。
- 常见单 inode 元数据事务的写入量目标小于 64 KiB，且不随 inode 总数线性增长。
- group commit 后所有 `fsync` 等待者只在对应 durable epoch 完成后返回。
- metadata 并发压力下无超过 checkpoint 周期的全局停顿；p99.9 停顿必须可由统计解释。
- 元数据闸门 B 通过。

### 提交建议

`shard-44: replace metadata image commits with WAL checkpoints`

## 14. shard-45：异步 libhfs 核心 API

### 目标

让单线程调用者能够维持 QD>1，把同步 POSIX/preload 入口变成异步核心之上的兼容封装。

### 主要改动

- 新增异步请求 API：read、write、readv、writev、fsync、metadata mutation。
- 每个请求使用独立 context/mempool，包含 buffer ownership、callback/future、deadline、result 和 cancellation state。
- 同步 `hfsRead/hfsWrite/...` 仅负责 submit 后等待对应 future，不再要求 owner 串行执行完整调用。
- 支持一个请求拆成多个 SPDK 子请求，并在父请求聚合实际字节数和首个错误。
- cancellation 只改变回调交付，不提前释放仍由 SPDK 持有的资源。
- 提供最小稳定 C ABI 和示例，preload 继续使用同步 wrapper。

### 测试与基准

- 新增 `scripts/db/async_io_test.sh` 和异步 microbenchmark/测试程序。
- 单线程 QD=1/4/16/32，覆盖 aligned/unaligned、read/write/vector、错误注入和取消竞争。
- 对比相同 QD 下 raw SPDK 与 async libhfs 的吞吐、延迟和 CPU。

### 验收

- 单线程 async libhfs 能维持 QD>1，观测到的 inflight 与配置相符。
- QD=1 不回退超过 5%；QD=16 吞吐明显高于同步 QD≈1，除非设备已在 QD=1 饱和。
- submit、子请求完成、父请求完成和 callback 各执行一次，错误不丢失。
- 本 shard 只宣称 async libhfs API；不得宣称 native libaio/io_uring 已支持。

### 提交建议

`shard-45: add asynchronous libhfs request APIs`

## 15. shard-46：多 reactor、per-reactor channel 与亲和配置

### 目标

消除单 SPDK owner 线程瓶颈，使数据 I/O 可在多个固定 reactor/channel 上并行，同时保留 metadata 顺序和跨 inode 操作的正确性。

### 主要改动

- 按配置创建 N 个数据 reactor，每个 reactor 固定 CPU、独立 SPDK I/O channel、请求队列、DMA cache 和统计 shard。
- I/O 以 inode/blob hash 固定到 reactor，保证同一 inode 的顺序易于推理并减少跨核迁移。
- metadata/WAL 使用专用 reactor 或协调队列；跨目录 rename 由 transaction coordinator 统一排序。
- fsync 收集目标 inode 已分发请求的 epoch，只等待相关 reactor，不做无条件全局 drain。
- owner PID/fork child 防护扩展到 reactor 集合；shutdown 先阻止新请求，再逐 reactor drain 和释放 channel。
- 增加 CPU affinity、reactor 数、NUMA node 和 queue depth 配置，并在启动日志中输出无敏感信息的实际绑定。

### 测试

- 新增 `scripts/db/multireactor_scaling_test.sh`。
- reactor=1/2/4/8，线程=1/2/4/8/16，QD=1/4/16/32 的矩阵。
- 同 inode 顺序、不同 inode 并行、跨目录 rename+I/O、fsync+多 reactor、shutdown/fork 压力。
- 在单 NUMA 和跨 NUMA 配置下记录 remote memory、CPU migration 和 p99.9。

### 验收

- 普通数据 I/O 不再全部通过单一 pthread/channel。
- reactor 数增加时，submit/complete 能分布到相应 channel，且各 channel 计数闭合。
- 多核测试吞吐随 reactor 增长，直到 bdev 或 CPU 饱和；无错误线程访问、channel 误用和 shutdown 卡死。
- 单 reactor 模式保持兼容，并作为回退配置。

### 提交建议

`shard-46: scale I/O across per-reactor SPDK channels`

## 16. shard-47：DMA、vector I/O、RMW 与空间管理优化

### 目标

降低固定 1 MiB bounce buffer 的内存与 copy 成本，利用 SPDK vector/memory-domain 能力，并避免 punch hole 后重新写零。

### 主要改动

- DMA pool 改为 size class，例如 4 KiB、16 KiB、64 KiB、1 MiB；每 reactor 本地 cache，跨 reactor 仅在低频路径回收。
- 对齐且 buffer 合法的请求优先 direct submit；无法直达时才 bounce，并统计原因。
- `readv/writev` 使用 SPDK native vector I/O 或按硬件限制构造 iovec，不再总是串行逐段等待。
- 大请求支持多 chunk pipeline，受 per-reactor QD 和 backpressure 控制。
- RMW 只覆盖不完整首尾 allocation unit；完整中间区直接提交。
- 评估 SPDK memory domain/registered memory 接口，为上层数据库缓冲区提供可选零拷贝注册 API。
- 引入逻辑 hole/extent 状态：读取 hole 合成零，punch 优先 unmap，不再为保证读零无条件回写整段零。
- cluster size 与 metadata/data allocation 单位解耦；保持旧格式兼容，新格式允许按 workload 选择合理粒度。

### 测试

- 新增 `scripts/db/data_path_efficiency_test.sh`。
- 覆盖 4 KiB～4 MiB、aligned/unaligned、single buffer/vector、direct/bounce、punch/read-zero、池耗尽和 backpressure。
- 导出 copy bytes、RMW bytes、DMA 等待、vector submit、unmap/write-zeroes 和有效 QD。

### 验收

- 4 KiB workload 不再为每个 inflight 请求长期占用 1 MiB DMA buffer。
- 合法 aligned vector I/O 不经用户态拼接 buffer；实际 submit 数受硬件 iovec 限制而不是固定逐段同步。
- aligned direct path 的 copy bytes 接近零；bounce 原因可观测。
- punch 后读取为零，且支持 unmap 的后端不再无条件执行等长写零。
- 与 shard-39 基线相比，4 KiB CPU/IOP 和 128 KiB 吞吐有量化改善，正确性测试无回退。

### 提交建议

`shard-47: optimize DMA vectors and sparse data paths`

## 17. shard-48：数据库、真实 NVMe 与阶段收口

### 目标

在 Malloc、持久化 AIO 和真实 NVMe 上完成全量回归，确认优化收益不依赖模拟后端，并明确下一阶段产品形态。

### 验证矩阵

- 构建：Make 强制构建、CMake Release、版本化 preload symbols。
- 功能：preload、fd lifecycle、metadata path、metadata transaction、I/O fast path、POSIX/data contract、runtime、error injection、durability、WAL/checkpoint、async、多 reactor。
- 持久化：正常重启、SIGKILL、每个事务/屏障边界 fault injection、旧 v4 镜像迁移。
- 数据库：
  - MySQL/MariaDB 初始化、CRUD/DDL、commit/rollback、临时表、重启、SIGKILL recovery、4/16 客户端。
  - RocksDB fillseq/readrandom/overwrite/readwhilewriting、WAL、flush/compact、SIGKILL recovery、`ldb checkconsistency`。
- 性能：
  - raw SPDK 与 HFS，QD=1/4/16/32。
  - sync preload 与 async libhfs 分开报告。
  - 1/2/4/8/16 线程或 reactor 扩展。
  - 4 KiB random、128 KiB sequential、metadata 1K/10K/60K。
  - 吞吐、p50/p99/p99.9、CPU、RSS、锁等待、queue depth、copy/RMW、metadata bytes、flush、错误。
- 正式结果至少连续 3 轮，记录中位数和离散度，不只保留最好一轮。

### 产品决策输出

新增 `doc/design/hfs_product_architecture_decision.md`，在证据基础上选择下一阶段方向：

1. 继续定位为单进程数据库加速库：收紧 ABI、提供 async libhfs SDK，以 preload 作为兼容层。
2. 启动 daemon + shared-memory/ring 设计：解决多个 HFS 进程共享挂载、统一 cache/metadata owner 和隔离问题。

当前阶段不实现多进程 daemon。若选择方向 2，必须另开新阶段，先定义进程故障、权限、共享内存回收、跨进程锁和升级协议。

### 验收

- 所有正确性与数据库回归通过，errors/reject=0，SPDK submit/complete 一致。
- 真实 NVMe 不出现 wrong-thread、channel 误用、超时或 shutdown 泄漏。
- 4 线程扩展目标至少 3.0 倍；async/multi-reactor 在设备未饱和时随 QD/核数增长。
- 元数据常见事务不再随 inode 总数产生整镜像写放大。
- 更新 `doc/design/fio_baseline_notes.md`、`doc/bug.md`、README/支持边界和本文完成状态。
- 完成提交、合入 `main`，在 `main` 上复核 commit、工作树和关键 VM 回归。

### 提交建议

`shard-48: close the reliability and performance stage`

## 18. 各 shard 通用完成清单

每个 shard 完成前必须逐项确认：

- [ ] 先查看 `git status`、`git log --stat`，没有覆盖用户未提交改动。
- [ ] 本地修改完成后再同步到 VM，VM 操作通过 `E:\Code\ssh\pool.py` 的 SSHPool 执行。
- [ ] 临时远程脚本放在 `E:\Code\test_script\`，完成后清理。
- [ ] 新发现/已修复 bug 已立即更新 `doc/bug.md`。
- [ ] C/C++ 类型使用大驼峰，变量/函数使用小驼峰，宏使用全大写下划线。
- [ ] 新增函数、结构体和关键逻辑具有简洁英文 `/* */` 注释。
- [ ] Make 强制构建和 CMake Release 构建通过。
- [ ] 当前 shard 专项测试通过，旧 smoke/regression 无回退。
- [ ] 性能测试记录 workload、QD、线程/reactor、bdev、CPU 亲和和 commit。
- [ ] 故障注入验证失败返回值、恢复状态和资源计数。
- [ ] 不支持的 POSIX/API 语义返回明确错误，没有伪成功或宿主错误路由。
- [ ] 阶段文档、设计文档和测试口径已更新。
- [ ] 独立提交后立即合入 `main`，并在 `main` 上复核状态。

## 19. 风险与止损条件

### 磁盘格式风险

v5 迁移是本阶段最高风险项。若无法证明任意迁移边界可恢复，则 shard-43 不得合入；继续保留 v4 和 shard-42 的内存索引收益。

### 并发正确性风险

去全局锁后最容易出现 fd ABA、close/use 竞争、RMW lost update 和 fsync 漏等请求。任何无法稳定复现和解释的 checksum 错误都视为阻断，不以“压力测试偶现”降级处理。

### 性能收益风险

若 shard-40/41 后扩展仍低于目标，先用 shard-39 数据判断瓶颈属于锁、同步 owner、设备或 copy；在根因明确前不提前进入多 reactor，以免把串行瓶颈复制到多个线程。

### 产品边界风险

LD_PRELOAD 无法自然解决多进程共享一致性、native async ABI 和完整 POSIX mmap。若业务目标转为通用多进程文件系统，应在 shard-48 后启动 daemon/client 或独立文件系统架构，不继续无限扩展 wrapper。

## 20. 预期最终状态

完成 shard-36～48 后，HFS 应达到以下状态：

- 已知数据、时间戳、容量、错误传播和 `fsync` 缺陷有自动化回归保护。
- 同步 preload 兼容层建立在异步 libhfs 核心上，单线程可以提交 QD>1。
- fd、runtime 和文件 I/O 热路径不再依赖 mount 级全局锁。
- 数据 I/O 可按 reactor/channel 扩展，线程亲和与 NUMA 行为可配置、可观测。
- 元数据采用显式 v5 格式、WAL、dirty page checkpoint 和 group commit，普通事务不再整 blob 提交。
- DMA、vector、RMW 和 hole 路径按实际请求粒度工作，copy 和写放大有量化数据。
- MySQL/MariaDB 与 RocksDB 在持久化 AIO/真实 NVMe 上保持正确，并有公平、可复现的性能报告。
- 对“单进程加速库”或“多进程 daemon”下一步方向形成书面架构决策，不扩大未经验证的支持范围。
