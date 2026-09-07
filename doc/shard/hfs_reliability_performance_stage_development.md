# HFS 可靠性与性能优化阶段计划（shard-36 起）

> 状态：shard-36～42 核心实现已完成，正确性闸门 A 已通过；shard-43 已先落地可移植 v5 页/WAL 格式基础和离线恢复校验，shard-44 已加入 v4 脏范围 checkpoint 与受限 group commit 基础，shard-45 已完成 async libhfs executor 及公开 aligned positioned direct child path，shard-46 已完成可选多 data reactor/channel 基础与跨 reactor 完成顺序修复，shard-47 第一阶段已完成分级 DMA pool、DMA-capable direct path 和 native vector 尝试；运行时仍使用 v4，v5 迁移、设备 WAL 提交和 SPDK 原生 libaio/io_uring ABI 仍待后续阶段<br>
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

> 状态：已完成（2026-09-06）<br>
> 验收：VM 上 Make/CMake Release、默认 stats-on/off 开销对照、详细 timing 统计、scaling
> smoke、I/O/fd/POSIX/runtime 回归和短时 RocksDB 数据库 benchmark 均通过；VM 内 SPDK
> 保持在 `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`，未从 Windows 同步。

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

详细锁 wait/hold 计时和 runtime 消息包装由 `HFS_STATS_TIMING=1` 显式开启；普通
`HFS_STATS_PATH` 只执行 per-thread 低成本计数，避免观测代码改变被测热路径。

### 验收

- stats 关闭时，4 KiB IOPS 下降不超过 1%；开启完整统计时下降不超过 3%。
- 所有报告同时包含吞吐、延迟、CPU、RSS、错误、submit/complete 和有效 QD。
- 每个全局锁都能给出 wait/hold 数据；每次 metadata mutation 都能给出实际写入字节。
- 同一 bdev 上补齐 raw SPDK QD=1 对照，形成可复现的公平基线。

本轮 VM 烟测使用 16 MiB、runtime 5 s、HFS 1 线程和 raw QD=1，覆盖 4 KiB 随机读/写/混合、
128 KiB 顺序读/写、CPU/RSS、错误数、SPDK submit/complete、有效 QD 和 metadata 字节；
脚本默认参数仍提供线程 `1/2/4/8/16`、raw QD `1/4/16/32` 及 inode `1K/10K/60K` 的完整矩阵。
stats-off/on 配对为 63,446.91/62,540.80 IOPS，测得开销 1.43%；详细模式观察到
`stats_shards=6`、runtime queue peak=2、lock/runtime wait/hold 直方图及 metadata
log/image/page bytes 均已落盘。

### 提交建议

`shard-39: establish contention and queue-depth baselines`

## 9. shard-40：fd 注册表与 runtime 热路径去全局锁

> 状态：核心实现已完成（2026-09-06）；同口径吞吐未宣称提升，后续瓶颈继续由 shard-41 处理<br>
> 验收：VM 上 Make/CMake Release、I/O/fd/POSIX/runtime 回归、详细 timing、64 线程
> fd lookup smoke 均通过；SPDK 仍由 VM 内仓库提供，commit 为
> `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`。

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

本轮实现采用 256 分桶 mutex（仅同一 fd bucket 互斥），slot 指针使用原子 exchange，generation
在每次替换/摘除时递增；acquire 在 bucket 保护下执行 increment-if-not-zero CAS 并复查
指针/generation，close 先摘槽，最后引用负责析构。runtime submit 使用原子 submitter admission
计数与 accepting 状态，stop 等待已进入 submit 的请求后再把 stop 消息排到 owner 队列；owner
completion 通过 TLS 身份判断，不再获取 runtime state mutex。

新增 `scripts/db/fd_lookup_benchmark.sh`，覆盖 1/4/16/64 线程；本轮 VM 1 秒烟测吞吐为
15,971 / 31,687 / 33,928 / 27,049 IOPS，错误数均为 0。扩展 `fd_lifecycle_smoke.sh`
增加 64 线程、每线程 250 次 open/pread/close 的 fd 复用与 lookup churn；回归中
`IO_RC=0 FD_RC=0 POSIX_RC=0 RUNTIME_RC=0`。详细 timing 仍能观察 `stats_shards=6`、
runtime submit/complete 一致以及 fd/meta/runtime lock wait/hold 数据。

为避免把设备或 owner-thread 噪声误判为收益，使用 shard-39 提交前的源码在 VM 复用同一
SPDK、同一 4 KiB randread/4 threads/iodepth=1/5 s 口径重跑：baseline 31,131.97 IOPS，
当前 31,062.79 IOPS（-0.22%，两者 error=0）。因此本 shard 不宣称达到“提升 20%”门槛；
详细 timing 显示 fd contention 33/196,381、metadata contention 30/97,173，runtime
queue wait 3.13 s，剩余主要瓶颈已从进程级 fd/runtime 锁转移到 owner 队列与 metadata/I/O
路径，后续按 shard-41 的 per-inode/区间并发方案继续消除串行化。

### 提交建议

`shard-40: remove fd and runtime global hot-path locks`

## 10. shard-41：per-inode 状态与区间 I/O 并发

> 状态：核心实现已完成（2026-09-06）；正确性专项通过，性能扩展数据已记录；4 线程未达到
> 3.0 倍目标，瓶颈证据指向单 SPDK owner 队列和 metadata 提交路径，不能提前宣称达标。

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

本轮实现细节：

- 新增 `src/hfs_inode_runtime.[ch]`。runtime registry 按 inode id 惰性创建，保存 resize/append
  mutex、fsync/resize 的 `syncLock`、已分配 cluster cache；兼容独立 `HfsFile` 自测路径保留
  单独 fallback runtime，不改变 v4 磁盘格式。
- `Hfs` 不再保存 mount 级 `ioLock`/`resizeLock`；数据面使用 256 个 blob/unit 哈希的 rwlock
  bucket，按升序获取并逆序释放，避免跨范围死锁。不同 inode 的 bucket 哈希通常分离，同一
  unit 的非对齐 RMW 必须共享同一写锁。
- aligned write 在已分配 cluster 范围内只读原子 cache，不再为每个请求进入 resize mutex；跨
  cluster 扩展才进入该 inode 的 resize mutex。`O_APPEND` 改为 inode runtime append mutex。
- `ftruncate`、punch、KEEP_SIZE 和 fsync 使用 inode `syncLock` 写侧屏障；普通 read/write
  使用读侧并允许同一 inode 的不重叠区间并行，且不在持有 metadata lock 时等待 SPDK completion。

### 测试

- 新增 `scripts/db/inode_io_concurrency_test.sh`。
- 新增 `scripts/db/inode_io_benchmark.sh`，覆盖 1/2/4/8/16 线程、4 KiB randrw、CPU/RSS、
  runtime queue peak 和 IO/meta/append contention。
- 覆盖不同文件并发、同文件不重叠 aligned write、同 unit 非对齐 RMW、append+truncate、punch+write、fsync+write。
- 每轮用确定性 pattern 和 checksum 验证不存在 torn RMW、交叉覆盖或错误 size。
- 运行 1/2/4/8/16 线程扩展矩阵并导出锁等待直方图。

VM 专项回归均通过：`INODE_RC=0 IO_RC=0 FD_RC=0 POSIX_RC=0 RUNTIME_RC=0`。同一 VM、持久化
Aio bdev、4 KiB randrw、iodepth=1、runtime=1 s 的短矩阵如下；统计显式开启
`HFS_STATS_TIMING=1`，错误数均为 0：

| 线程 | IOPS | CPU | RSS (KiB) | runtime queue peak | IO contention | metadata contention |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 14,085.91 | 110% | 20,776 | 1 | 226 | 4 |
| 2 | 24,451.55 | 114% | 20,648 | 2 | 299 | 14 |
| 4 | 26,999.00 | 121% | 20,916 | 4 | 476 | 35 |
| 8 | 29,893.11 | 122% | 21,028 | 7 | 866 | 50 |
| 16 | 30,085.91 | 123% | 22,960 | 15 | 1,605 | 110 |

4 线程相对 1 线程约 1.92 倍，仍低于闸门 B 的 3.0 倍扩展线；queue peak 随线程增长而增长，
说明 fd 全局锁已不是主导，但同步 owner queue、单请求 completion 和 metadata write-back
仍限制扩展。该结果用于指导 shard-42/45，不把未达标数字包装成性能收益。

`inode_io_concurrency_test.sh` 还验证了 16 路不同 inode、同 inode 不重叠 aligned write、
同 unit RMW、跨 fd append、punch+write 以及 fsync+write；`make -B app lib`、CMake Release
和既有 I/O/fd/POSIX/runtime 回归均通过。SPDK 仍使用 VM 内仓库，commit 为
`f0b63ed866f0e4d3c56e73bd4a98d0042956147a`。

### shard-41 性能修复迭代（2026-09-06）

在不改变同步 POSIX 语义和 SPDK channel 线程归属的前提下，数据面新增 owner-side batch
dispatcher：`hfsIoSubmit` 的 request 通过 `HfsRuntimeBatchItem` 链接到一个批次，多个并发
调用共享一次 owner queue 通知，再由 owner 逐项提交 SPDK I/O。计时统计按 item 保留 queue
wait/owner time；设置 `HFS_RUNTIME_BATCH=0` 可回退到原始逐消息路径，便于基准 A/B。

VM 上 5 秒持久化 Aio bdev A/B（统计开启、4 KiB randrw、iodepth=1）结果：4 线程 batch
开启 24.35k IOPS、关闭 24.73k IOPS；在当前单 owner 架构下未把微小差异宣称为绝对吞吐收益，
但 fast-I/O 和 runtime stress 均保持通过。该修复消除了数据面“每请求一次 owner 通知”的
额外放大项；4 线程 3 倍目标仍由 owner 串行化和 metadata write-back 限制，后续进入
shard-45 异步 API、shard-46 多 reactor/channel。

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

### 实现状态（2026-09-06）

- `src/hfs_meta_index.[ch]` 已加入 Make/CMake：id open-addressing table、`(parent,name)`
  dentry hash 和按名称排序的 directory child vector 均由 mount/reopen 重建；结构性 mutation
  会标记索引 dirty，数据型 inode commit 不再无条件重建整张索引。
- `hfsMetaFindByName`、`hfsMetaFindById`、`readdir` 已走索引并保留 dirty/failure 时的线性回退；
  create/unlink/rename 的索引失效窗口会在成功提交后重建，失败仍交给现有 v4 undo/recovery 路径。
- metadata blob 在 mount 生命周期内保持 resident handle，关闭顺序在 `hfsMetaFini` 中统一处理；
  当前 v4 镜像布局未改变，旧 v4 镜像仍按原路径加载并重建索引。

### 测试

- 新增 `scripts/db/metadata_scaling_benchmark.sh`。
- 基准默认使用目录对象（`HFS_METADATA_OBJECT_TYPE=dir`）和 128 GiB sparse image，避免把每个
  普通文件的 data blob 容量消耗混入 namespace 测量；可显式切换到 `file`。
- VM 上 1K/10K 目录样本 errors=0，SPDK submit/complete 均闭合；代表性 p99（微秒）如下：

  | inode | create | open | stat | readdir | rename | unlink |
  |---:|---:|---:|---:|---:|---:|---:|
  | 1K | 1548.06 | 7.03 | 3.55 | 195.37 | 1741.72 | 1651.82 |
  | 10K | 8128.73 | 7.91 | 3.49 | 1930.04 | 8976.14 | 8780.27 |

- open/stat p99 已基本脱离 inode 数量；create/readdir/rename/unlink 分别约为 5.3x/9.8x/5.2x/
  5.3x，证实主耗时来自 v4 每次 mutation 的 pending/stable/clear 整镜像写入，而不是查找。
- 60K 完整矩阵暂不宣称通过：在 v4 整镜像提交和单 owner metadata 队列下，继续扩大只会放大
  已观测的写放大；该闸门转由 shard-43 页式布局/WAL 解决后重新测量。
- fuzz rename/unlink/create 顺序，每轮对比 inode table、id index、dentry index 和目录 index 的一致性。
- crash/reopen 后重新构建索引并与磁盘 inode table 校验。

### 验收

- path lookup、inode id lookup 和单目录 readdir 不再依赖全 inode 数量的线性扫描。
- 1K/10K 样本的 open/stat p99 分别为 7.03/7.91 和 3.55/3.49 微秒，满足当前可观测的
  “查找不随 inode 数线性增长”目标；60K 仍待 shard-43 完成后复验。
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

### 当前完成（shard-43 foundation）

- 新增 `src/hfs_meta_v5.[ch]`，实现显式 little-endian superblock A/B、4 KiB
  inode page、固定字段 WAL record、checksum 和 newest-generation 选择。
- WAL recovery 已按 transaction id 暂存 SET_INODE，只在 commit marker 的记录数
  完全匹配时应用；缺少 commit、页损坏和单个 superblock 损坏均有离线校验覆盖。
- 新增 `scripts/db/metadata_v5_format_test.sh` 与
  `scripts/db/metadata_wal_recovery_test.sh`，VM 上 v5 round-trip/WAL recovery、
  Make 强制构建和 CMake Release 均通过。
- 该提交尚未切换当前挂载格式；v4 仍是运行时格式，v4→v5 copy-on-write 迁移、
  SPDK metadata blob 提交和 dirty-page checkpoint 留在后续实现，不能宣称 shard-43
  的迁移验收已完成。

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

> 状态：第一阶段基础已完成（2026-09-06），本轮补齐 v4 受限 group commit 与同步 epoch
> 约束；v4 事务协议保持不变，物理写入已收窄到 intent/header/受影响 inode 的对齐范围；
> 完整 v5 WAL checkpoint、后台 group commit 和 WAL 回收仍待后续迭代，不能提前宣称 shard-44
> 闸门 B 已通过。

### 目标

用 WAL + dirty page checkpoint 替代整 metadata blob 的 pending/stable/clear 三次镜像提交，并在保持 shard-38 持久化语义的前提下降低 flush 和写放大。

### 主要改动

- 把 inode/dentry/free-space 状态映射为独立 metadata page，仅修改受影响页面。
- transaction 提交流程：append WAL records → append commit → flush WAL epoch → 发布内存状态。
- 后台 checkpoint 批量写 dirty pages，写完并 flush 后推进 checkpoint LSN，再回收旧 WAL。
- group commit 按时间窗、字节阈值或显式 `fsync` 触发；同步调用等待包含自身事务的 durable epoch。
- metadata 空间不足时优先 checkpoint/reclaim；失败必须回到可重试状态。
- checkpoint 与 foreground lookup/update 不持有长时间全局 metadata write lock。

本轮先落地兼容 v4 的脏范围写基础：`HfsMetaIoRequest` 支持 byte offset，范围写对首尾
blobstore I/O unit 做 read-modify-write，中间对齐区直接写；单次元数据事务仍按现有
`intent -> stable image -> clear intent` 顺序提交，但 stable image 不再为每个 mutation
构造并写回整张 metadata blob。intent 发布后先写 header 和变更 inode，再进行一次 blob sync，
最后只清除 intent 范围。旧 v4 镜像、回滚和崩溃重放路径保持兼容；成功路径只读取 header，
只有事务尚未发布 intent 且需要恢复内存状态时才重新装载稳定镜像，已达到“成功路径不读整
blob”的这一项基础口径。

本轮继续补齐同步边界：运行时按 inode ID 维护 dirty 集合，`fsync` 将显式 inode 与同一
mount 上的其他 dirty inode 合并为一个受 v4 intent 四槽位约束的 durable epoch；成功后才
清空集合，避免单文件 `fsync` 清除其他文件的脏状态。超过四个 after-image 槽位时回退到
已有完整镜像保存路径，保证 header checksum 始终覆盖已写入的 inode 集合。统计新增
`metadata_sync_count`，与设备 `flush_count` 分开，能直接解释 group commit 的 barrier 成本。

### 测试

- 新增 `scripts/db/metadata_checkpoint_test.sh`。
- 新增 `scripts/db/metadata_group_commit_test.sh`，覆盖只 `fsync(A)` 后 B 的 dirty inode
  仍可跨进程恢复，并校验 `metadata_batch_max>=2`。
- 覆盖 checkpoint 前/中/后 crash、WAL wrap、空间耗尽、重复 mount/replay、并发 rename/unlink/create。
- 记录每个逻辑事务的 WAL bytes、page bytes、flush 数和 batch size。
- 在 1K/10K/60K inode 下比较 v4 整镜像和 v5 WAL/page 写放大。

本轮专项 smoke 使用 `HFS_STATS_PATH` 校验 mutation 过程中
`metadata_image_bytes=0`、`metadata_page_bytes>0`，并覆盖 256 次 create/rename/unlink；
`metadata_transaction_smoke.sh`、`metadata_path_concurrency_smoke.sh`、
`io_error_injection_test.sh`、`durability_crash_recovery_test.sh`、fast I/O 和 runtime stress
均作为兼容性回归。错误注入测试中的旧 `metadata-open` 断言已改为实际可达的
`metadata-read` RMW 注入，详见 `doc/bug.md` #58。

### 验收

- 普通 create/unlink/rename/setattr 不再读写完整 metadata blob。
- 常见单 inode 元数据事务的写入量目标小于 64 KiB，且不随 inode 总数线性增长。
- group commit 后所有 `fsync` 等待者只在对应 durable epoch 完成后返回。
- metadata 并发压力下无超过 checkpoint 周期的全局停顿；p99.9 停顿必须可由统计解释。
- 元数据闸门 B 通过。

### 提交建议

`shard-44: replace metadata image commits with WAL checkpoints`

## 14. shard-45：异步 libhfs 核心 API

> 状态：公开 direct path 已完成（2026-09-06）<br>
> 已提供有界 worker executor、request-private context、callback/wait/result/cancel ABI，以及
> positioned read/write、readv/writev 和 fsync；同步 preload 入口保持不变。至少两个完整
> unit 且首尾对齐的 positioned read/write 现在直接绑定一个 public request 和有界 SPDK child
> parent，不再占用 executor worker 等待数据 I/O；非对齐首尾、需要 resize、vector、fsync、
> 小请求和资源不足场景继续使用有界 executor fallback。

### 目标

让单线程调用者能够维持 QD>1，把同步 POSIX/preload 入口变成异步核心之上的兼容封装。

### 主要改动

- 新增 `src/hfs_async.[ch]` 和 `hfs_lib_async_*` C ABI：positioned read、write、readv、writev、
  fsync；每个请求拥有独立的参数、完成状态、错误码和生命周期引用。
- 增加有界 executor：`HFS_ASYNC_WORKERS`（默认 4，最大 32）和
  `HFS_ASYNC_QUEUE_DEPTH`（默认 256，最大 4096）控制 worker 数和排队上限；队列满时明确返回
  `EAGAIN`，不会无限制分配请求上下文。
- `hfsAsyncWait` 只等待请求完成，`hfsAsyncResult` 返回结果/errno；`hfsAsyncCancel` 只抑制回调
  交付，已开始的 I/O 不提前释放 buffer、iovec 或 fd 相关资源。
- 增加 `async_submit/complete/cancel/queue_depth_peak` 统计，专项脚本验证 submit/complete
  闭合、vector I/O、回调、取消和 fsync。
- 增加 `HFS_IO_PIPELINE_DEPTH`（默认 4，最大 16），对齐完整 unit 的大块读写在 owner 线程
  上拆成多个 child request；固定 DMA lease 在所有 child completion 后才归还，并记录
  `io_pipeline_peak` 以区分 SPDK 子请求并发和 executor queue depth。
- 公开 async positioned read/write 先尝试 direct path：请求生命周期、open-file 引用、range
  guard 和 DMA lease 均由父状态持有，SPDK owner-thread completion 直接完成 public request；
  direct path 无法安全覆盖的形状返回 `EAGAIN` 并回退到原 executor，保留 resize/RMW/元数据
 语义。
- 提供最小稳定 C ABI 和示例，preload 继续使用同步 wrapper。
- 父请求聚合对齐数据 child completion；direct callback 在 SPDK owner thread 交付，fallback
  callback 仍在 executor worker 交付。deadline/cancellation state 和同步入口改为等待 future
  仍保留为后续工作；当前不扩展 native libaio/io_uring ABI。

### 测试与基准

- 新增 `scripts/db/async_io_test.sh`，覆盖 16 路 positioned write、vector write、callback、
  wait/result、取消和 fsync；额外覆盖 4 MiB 对齐写，验证 async 统计 submit=complete、
  executor queue peak>1 且 `io_pipeline_peak>1`；direct 4 MiB 请求验证 public request 不进入
  executor 且 direct submit/complete 闭合。
- 后续 microbenchmark 必须把“executor QD”和“SPDK owner/channel QD”分开报告；本阶段专项只验证
  API/lifecycle，不把 worker 并发误报成设备层异步收益。
- 单线程 QD=1/4/16/32，覆盖 aligned/unaligned、read/write/vector、错误注入和取消竞争。
- 对比相同 QD 下 raw SPDK 与 async libhfs 的吞吐、延迟和 CPU。

### 验收

- 第一阶段：单线程 async libhfs 能维持 executor QD>1，request callback/result 生命周期和
  submit/complete 计数闭合。
- 第二阶段基础：单个大块对齐请求能维持有界 SPDK child QD>1，`io_pipeline_peak` 与配置相符。
- 完整阶段：公开 async aligned positioned request 直接绑定父/子状态，worker 不再被数据 I/O
  等待占用；需要 resize、RMW、vector/fsync 或资源不足时允许 fallback，并完成 error 聚合验收。
- QD=1 不回退超过 5%；QD=16 吞吐明显高于同步 QD≈1，除非设备已在 QD=1 饱和。
- submit、子请求完成、父请求完成和 callback 各执行一次，错误不丢失。
- 本 shard 只宣称 async libhfs API；不得宣称 native libaio/io_uring 已支持。

### 提交建议

`shard-45: add asynchronous libhfs request APIs`

## 15. shard-46：多 reactor、per-reactor channel 与亲和配置

> 状态：基础实现与专项回归已完成（2026-09-07）。`HFS_DATA_REACTORS=1..8` 现可选启用；reactor 0 保留为 metadata/兼容 owner，额外 reactor 只承载大块、全 unit 对齐的 async direct data path。该 shard 不宣称 metadata/WAL 已多 reactor 化，也不改变多进程、mmap、native libaio/io_uring 的支持边界。

### 目标

消除单 SPDK owner 线程瓶颈，使数据 I/O 可在多个固定 reactor/channel 上并行，同时保留 metadata 顺序和跨 inode 操作的正确性。

### 主要改动

- 按 `HFS_DATA_REACTORS` 创建 1～8 个运行时 reactor；reactor 0 复用现有 owner/channel，额外 reactor 拥有独立 SPDK I/O channel、提交队列和统计 shard。
- 数据 I/O 以 blob id 的稳定混合 hash 选择额外 reactor，避免 SPDK blob id 固定低位步长造成分布偏斜；同一 blob 始终落到同一 data reactor。
- `HFS_DATA_REACTOR_CPUS` 可提供额外 reactor 的 CPU 列表；未配置时保持 SPDK 默认调度，不伪造 NUMA 绑定。
- direct write 完成先回 metadata owner 更新 inode；metadata owner 忙于同步元数据 I/O 时使用 owner poller 重试，随后把 public request 完成消息送回原 data reactor，避免跨线程直接持有全局 metadata 锁。
- 大范围 RMW/zero/unmap 不再因哈希到全部全局 range bucket 而阻塞其他文件；每个 inode 的 `rangeGate` 在覆盖大多数 bucket 时提供局部排他保护。
- owner PID/fork child 防护扩展到 reactor 集合；shutdown 先停止新提交，逐 reactor drain，再释放 channel。
- 每个 data reactor 导出 `data_reactor_N_submit/complete` 计数，便于确认 channel 闭合和分布。

### 测试

- 新增 `scripts/db/multireactor_scaling_test.sh`，覆盖 reactor=2/4 的多文件 direct aligned write、open/create 并发、完成等待和 per-reactor submit/complete 计数。
- VM 上 reactor=2（16 文件）与 reactor=4（32 文件）均通过；reactor=2 的 64/64 请求落在额外 reactor，reactor=4 的 128/128 请求分布到 3 个额外 reactor。
- 与 async pipeline、metadata transaction/path、I/O error、durability crash、fast path、runtime stress、persistent smoke 和 RocksDB WAL/recovery 回归一起验证；Make/CMake Release 均通过。
- CPU affinity、跨 NUMA、跨目录 rename+I/O 和 fsync epoch 仍留给后续真实硬件矩阵，不在本 shard 宣称完成。

### 验收

- 单 reactor 模式保持兼容并作为回退配置；额外 reactor 的 direct data I/O 不再全部通过单一 SPDK channel。
- reactor 数增加时，submit/complete 能分布到相应 data channel，且每个 channel 计数闭合。
- metadata owner 与 data reactor 的完成顺序无全局锁死锁；大范围 range 操作不再锁住其他 inode 的全部哈希 bucket。
- 多 reactor shutdown、fork child rejection、错误传播和既有数据库回归无回退。
- 吞吐随 reactor 增长的正式目标、NUMA/CPU 亲和收益和真实 NVMe 证据留给 shard-48，避免把 Malloc smoke 误报为设备扩展性结论。

### 提交建议

`shard-46: scale I/O across per-reactor SPDK channels`

## 16. shard-47：DMA、vector I/O、RMW 与空间管理优化

> 状态：第一阶段已完成（2026-09-07）。DMA pool 已按请求大小选择 4 KiB/16 KiB/64 KiB/1 MiB class；对齐且可由 SPDK 地址转换的连续 buffer 走 direct submit，满足同样条件的 vector 尝试一次 native `spdk_blob_io_*v`，否则保留原有 bounce/fallback 路径。hole extent 和 registered-memory API 仍未宣称完成。

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
- 当前实现已覆盖分级 DMA class、连续 DMA buffer direct path 和 native vector 尝试；非 DMA 用户 buffer 会验证兼容 fallback，并导出 `io_direct_*`/`io_vector_*` 计数。
- 覆盖 4 KiB～4 MiB、aligned/unaligned、single buffer/vector、direct/bounce、punch/read-zero、池耗尽和 backpressure。
- 导出 copy bytes、RMW bytes、DMA 等待、vector submit、unmap/write-zeroes 和有效 QD。

### 验收

- 4 KiB workload 不再为每个 inflight 请求长期占用 1 MiB DMA buffer；当前 smoke 已验证四个 class 均有命中。
- 合法 aligned vector I/O 尝试 native submit；非 DMA 用户 buffer 保持兼容 fallback，实际 NVMe vector 收益留待真实硬件矩阵。
- aligned direct path 的 copy bytes 接近零；`io_direct_*`/`io_vector_*` 和 DMA class 计数可观测。
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
