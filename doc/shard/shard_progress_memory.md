# HFS Shard 进度记忆

本文从 `AGENTS.md` 的“Codex && opencode 共用项目记忆”中摘录 shard 相关内容，集中保存分片开发历史、验证结果、性能口径和当前边界。

> 记录时间：2026-09-16
>
> 历史条目中的“待完成”或“仍待验收”只代表对应 shard 完成时的阶段状态，不代表当前状态。当前源码、Git 历史、测试脚本和最新阶段文档优先。

## 文档与总体口径

- `doc/shard/mysql_rocksdb_stage_development.md` 是 shard-26 起的执行计划，优先于旧分片文档中尚未实施的 shard-26 及以后内容。
- `doc/项目总结.md` 是早期项目总结，`doc/shard/libhfs_shard_development.md` 是原始分片计划。使用这些旧文档前，必须与 Git 历史和当前源码交叉核对，不能直接把旧目录结构、旧容量或“待实现”描述当作现状。
- 当前进展应先结合 `git log --stat` 和 `git status`，再以当前源码、测试脚本和最新文档为准。

## Shard-28：fd 生命周期与 open-file description

- 新增 `src/hfs_fd.[ch]`：fd slot 使用短 rwlock，`HfsOpenFile` 通过引用计数、共享 offset 和 I/O 锁承载 open-file description；dup/dup2/dup3/F_DUPFD 共享描述，close 先摘 slot 后释放 placeholder；unlink/rename 覆盖通过 orphan 延迟回收 blob，旧 fd 仍可执行 read/pread/lseek/fstat/ftruncate。
- VM 上 `make -B app lib`、CMake Release、preload smoke、persistent smoke、fd lifecycle smoke 和 16 线程 10 秒 runtime stress 全部通过。专项测试覆盖 fd 复用、共享 offset、`F_DUPFD_CLOEXEC`、dup2、无参 `F_GETFL`、unlink-open、rename 覆盖/同路径 rename 和并发 positioned I/O。独立 TSAN 单测尚未建立；当时 MySQL/RocksDB 尚未完成验收。
- 排查经验：构建共享库必须强制重链；版本脚本只能给 `fcntl64` 保留 `GLIBC_2.28` 版本，不能把所有 wrapper 放到自定义版本；rename 压缩 inode 数组后必须按 id 重新解析指针；同路径 rename 不得进入目标 blob 回收。

## Shard-29：路径并发与目录 POSIX 语义

- `hfs_lib.c` 使用元数据读写锁和 append 串行锁，读路径返回 inode 快照，目录句柄使用独立游标锁；元数据头保存 magic/version/`nextInodeId`，兼容旧 `[count][inodes]` 布局并禁止 inode ID 复用。
- preload 增加受锁保护的 HFS cwd、`fchdir`、`fdopendir`、目录注册表锁、相对 `openat`、`O_EXCL`/`O_APPEND`/`O_TRUNC`/`O_DIRECTORY` 语义；新增 `scripts/db/metadata_path_concurrency_smoke.sh`。
- VM 上 Make/CMake、元数据路径并发、fd 生命周期、持久化 smoke 和 16 线程 10 秒 stress 全部通过；当时真实数据库验收留在 shard-33～35。

## Shard-30：可恢复元数据短事务

- 元数据升级为 v3 header，增加 generation/checksum；固定大小 intent record 保存最多 4 个 inode after-image。
- create/unlink/rename/size 按 `intent → stable image → clear intent` 顺序提交；mount 校验并幂等重放，兼容 v2/legacy 布局并在首次修改时迁移。
- `fsync` 先同步 blob 数据和 metadata，再通过独立可写 bdev descriptor 执行 `spdk_bdev_flush`，最后提交 dirty inode size/mtime；source-last rename 覆盖恢复加入 `scripts/db/metadata_transaction_smoke.sh`。
- VM 上 Make、CMake Release、三个崩溃边界、路径并发和持久化 smoke 全部通过。stable image 当时仍整 blob 写入，脏页/写放大优化留给 shard-31；当时 MySQL/RocksDB 真实数据库验收仍未完成。

## Shard-31：请求级 I/O 与 DMA fast path

- `src/hfs_io.[ch]` 使用 16 个固定 DMA buffer 承载请求级读写，移除 `HfsFile` 共享 scratch 状态；对齐 I/O 走直接 chunk，非对齐首尾只做 unit RMW，pread/pwrite 与 readv/writev 保持调用方 buffer 语义。
- `PUNCH_HOLE` 在 unmap 后补 zeroes/池化写零，修复后端 unmap 成功但不保证读零的问题；运行时记录 owner PID，fork child 不再进入 HFS 路由或 SPDK queue。
- 新增 `scripts/db/io_fast_path_smoke.sh`，覆盖向量 I/O、稀疏读、KEEP_SIZE、对齐/非对齐 punch、8 路 positioned write 和 fork child 防护。
- VM 上 Make、CMake Release、元数据事务/路径并发、fd 生命周期、持久化 bdev 和 fio 全部通过；fio 结果记录在阶段文档和 `doc/design/fio_baseline_notes.md`。实现提交为 `9a5de90`，已在 `main`。

## Shard-32：核心 POSIX 契约收口

- 元数据升级为 v4，兼容 v3/v2/legacy；inode 保存 mode/uid/gid 与 atime/mtime/ctime。
- preload 补齐目录 fd 的 `*at` 路由、statx/statfs/statvfs（含 64 位 ABI）、chmod/chown/utimensat/futimens、fadvise，并对 HFS fd 的 mmap 返回 ENODEV。
- 修复 open mode 传递、AT_FDCWD 路径物化和 Python statvfs64 符号；新增 `scripts/db/posix_contract_test.sh`。
- VM 上契约连续 10 次、Make/CMake、既有 smoke、持久化、16 线程 10 秒压力和 fio 全部通过；strace 审计确认没有 `/hfs` 数据调用落入内核 `/dev/null` 路径。旧 MySQL/RocksDB smoke 当时仍受进程私有 Malloc0/跨进程生命周期限制，真实数据库端到端验收留在 shard-33～35。

## Shard-33：MariaDB/MySQL 端到端验收

- `scripts/db/mysql_smoke.sh` 使用持久化 Aio bdev、bootstrap wrapper 和隔离 process group，覆盖系统表初始化、CRUD/DDL、事务提交回滚、LONGTEXT、临时表、正常重启、SIGKILL 恢复和 4/16 并发客户端负载；VM 输出 `MYSQL PERSISTENT RECOVERY OK`。
- preload 补齐 MariaDB 使用的 64 位/fortify open、readlink/realpath、绝对路径和目录 fd 路由；HFS owner/log 线程屏蔽宿主终止信号，避免数据库信号线程被 SPDK 后台线程抢收。
- sysbench 未安装，使用 mysqlslap fallback；当时阶段记录中 RocksDB 仍待 shard-34，不能据此把阶段状态提前称为 MySQL/RocksDB 全部支持。

## Shard-34：RocksDB 端到端验收

- `scripts/db/rocksdb_smoke.sh` 使用持久化 Aio bdev 和独立运行目录，首个进程格式化、后续进程跨进程重开，覆盖普通/direct I/O、fillseq/readrandom/overwrite/readwhilewriting、flush/compact、WAL、SIGKILL recovery 和 `ldb checkconsistency`。
- VM 上 Make/CMake 强制构建及 20,000 条、8 线程 60 秒流程连续 3 轮通过。
- RocksDB 实际使用的 GLIBC_2.2.5 `read`/`__read`、`fread`/`fread_unlocked` 已版本化导出并路由到 HFS；stdio EOF/error 状态由轻量表维护，`fclose` 先摘 HFS fd slot 再关闭 libc 占位 fd。
- 性能汇总和两种数据库最终支持口径留给 shard-35；这是 shard-34 完成时的阶段状态。

## Shard-35：性能回归与阶段收口

- 新增 `scripts/db/database_benchmark.sh`，记录持久化 Aio 的环境、RocksDB 吞吐、p50/p99、CPU/RSS、错误数和可选 fd/meta/DMA/SPDK 计数；`src/hfs_stats.[ch]` 默认关闭统计，仅 `HFS_STATS_PATH`/`HFS_STATS=1` 开启。
- VM 正式 benchmark（20,000 条、60 秒、WAL、mmap off）：1 线程 63,854 ops/s，4 线程 235,831 ops/s（3.69x）；p50/p99 为 345/972 与 380/1,165 µs；errors/reject=0，SPDK submit/complete 一致。
- Malloc FIO 4K/128K 相对 shard-31 同口径结果下降 1.7%～5.4%；最终 Make/CMake、preload/fd/元数据/I/O/POSIX/runtime、MariaDB smoke 和 RocksDB smoke 均通过。
- 阶段支持口径：单数据库进程、持久化 AIO/NVMe 上的 MySQL/MariaDB-compatible server 与 RocksDB；多个 HFS 进程同时挂载、mmap、native libaio/io_uring 仍不支持。

## Shard-62：POSIX Reactor Queue Engine

- `HFS_POSIX_QUEUE` 默认开启，`HFS_POSIX_QUEUE_DEPTH` 默认 32（4～256）；未显式设置 `HFS_DATA_REACTORS` 时，8-vCPU VM 使用 4 个 data reactor（总数含 reactor 0 owner 为 5）。
- 每个 data reactor 有 batch queue、backend in-flight credit、drain/depth/credit-wait 统计；completion callback 归还 credit。为避免 POSIX range lock 与 queue credit 反转，credit 满时 direct-submit 溢出，不在锁内阻塞。
- 所有 owner/data/batch/stop 的 `spdk_thread_send_msg` 通过 runtime `messageSubmitLock` 串行化，修复 DPDK 多生产者 ring reservation stall（见 `doc/bug.md` 条目 76）。
- VM 上 stripe、DMA backpressure、runtime stress、POSIX contract、64×250 fd churn、metadata transaction、IO trace、MariaDB 和 RocksDB 全部通过，`spdk_submit=spdk_complete`、errors=0。
- NVMe FIO 32-job 8K 为 72K IOPS，未达到计划 82K 门槛；stats 确认剩余热点是区间 I/O 锁和普通 buffer bounce copy，不再通过扩大 queue depth 规避。

## Shard-63：POSIX range-ticket scheduler

- 新增 `src/hfs_ticket.[ch]`，默认 `HFS_POSIX_TICKET=1`、bounded depth64；aligned read/write 用 inode-local ticket 依赖替代跨 SPDK completion 的 range lock，非对齐 RMW 由单 ticket 保序，`HFS_POSIX_TICKET=0` 保留旧路径。
- 新增 ticket stats/trace 和 `scripts/db/posix_range_ticket_test.sh`；修复 init/admit 失败路径的未初始化状态读取与 waiter 清理泄漏（`doc/bug.md` 条目 77）。
- VM Make/CMake、全量 Shard-62 回归、MySQL/MariaDB、RocksDB/WAL/recovery 全部通过。
- NVMe `HFS_DATA_REACTORS=4`、8K/32-job 10 秒三次为 88.40/88.58/88.63K IOPS，读写 p99 约 0.83～0.85 ms；5 秒全矩阵与 ticket 归因见对应 shard 计划和 `doc/design/fio_baseline_notes.md`。
- range lock steady-state 已消失，下一瓶颈是普通 POSIX bounce copy、fd/metadata 同步和每请求 completion；不得用无限 reactor/queue 掩盖。

## 性能再分析与 Shard-66 之前的路线

2026-09-10 性能再分析结论：当前 P0 是 POSIX 同步边界下 owner callback 串行、每 syscall 单独 completion wait；8/16 KiB 还存在 DMA class 集中等待，metadata lock 不是 FIO steady-state 主因。后续路线确定为 POSIX Reactor Queue Engine：每 reactor 独立 queue/channel/DMA pool、bounded ticket in-flight、inode/range 顺序和 dirty epoch durability；reactor 数量应按 CPU 自动匹配并封顶，不能无限增加 reactor。8 KiB/32 job A/B 中较少 reactor 为当前最佳点，继续增加 reactor 反而退化。

## Shard-66：同步边界与 reactor 拓扑

### 立项时的计划

基于 task90150022、task09150832、task09151930 三阶段测量，结论是 4K 纯读已达真实 raw Blobstore 上限约 93%（旧 A2 的 139K 为薄分配虚高，修正为 99K@2ch），剩余差距是 CPU 效率与尾延迟而非吞吐。

路线：

1. 66A：收口测量口径，包括状态重置、trace-off、5 次 median、shared/per-job-file 双口径和 16K/64K 补矩阵。
2. 66B：reactor 拓扑默认值与 `HFS_REACTOR_PIN` 亲和，8-vCPU 默认 2 个 data reactor。
3. 66C：wake 路径瘦身与 `HFS_WAIT_MODE=adaptive` gated A/B。
4. 66D：TN0/TN1 backend 分解，仅做测量；若 propagation ≤10%，则永久关闭 backend poll 方向。
5. 66E：staging/pool 与 16K～64K 收口。
6. 66F：共享文件 ticket 微优化，按证据 gated。

已排除项：ticket 重构、messageSubmitLock、fd/metadata fast path、app buffer DMA direct（受 uio+hugepage 环境限制）和 durability 改动。门禁目标为 4K randrw 32-job ≥89K total IOPS 且 CPU ≤500%。

### 当前实现状态

- 已实现 66A 基线 v2 harness/summary、66B 按 CPU 自动选择小核拓扑与 `HFS_REACTOR_PIN`、gated idle backoff、66C cacheline completion 与 bounded adaptive wait，以及 66D poll-gap 观测。
- 实现提交为 `818c872`（`feat: implement shard-66 sync boundary optimizations`），已在 `main`。
- VM Make/CMake、POSIX/ticket/trace/runtime/metadata/durability/persistent 回归及 MariaDB/RocksDB 缩短端到端 smoke 已通过。
- 2026-09-16 按修改后的 `task09160035` 短测版在真实 NVMe 上复跑：`posix_contract_test.sh` 和 `io_fast_path_smoke.sh` 均通过；A 布局首个共享文件 4K randread/32-job 样本为 58,277.97 IOPS、595% CPU，低于 70K 停止线，随后按门禁停止，未运行 B 或 adaptive。
- 因 A 门禁触发，没有采纳新的 topology/pinning/wait-mode 默认值；完整五次 NVMe 矩阵和正式 66B/66C 性能门禁仍未完成。
- 正式全矩阵入口为 `scripts/db/shard66_baseline_v2.sh`，汇总工具为 `scripts/db/shard66_summary.py`；详细实施记录见 `doc/shard/hfs_posix_sync_boundary_topology_shard_plan.md` 和 `doc/performance/hfs_shard66_baseline_v2.md`。

## 共享验证边界与性能观测口径

- SPDK 源码必须在 VM 内通过 `git clone`、`git fetch`、`git checkout` 和 `git -C spdk submodule update --init --recursive` 获取及维护；禁止从 Windows 工作区把 `spdk/` 源码目录或其子模块传到 VM 覆盖。Windows 到 VM 的同步仅限 HFS 自身源码、脚本和文档。
- HFS 默认后端固定使用 VM 内真实 NVMe：`config/hfs_nvme.json`、`HFS_BDEV_NAME=Nvme0n1`、PCI BDF `0000:0b:00.0`。除非任务明确要求 Malloc/Aio 或故障注入，否则性能与功能验证按 NVMe 口径执行；普通本地对照文件和测试产物默认放在 `/mnt/hfs-data`。
- HFS 当前为 POSIX-only 对外架构：完整 POSIX preload 路径是唯一支持入口；原 `hfs_lib_async_*`、`hfsAsync*`、registered-DMA/DMA allocation ABI、`HFS_POSIX_DIRECT_ASYNC` bridge 已移除。普通 POSIX 数据面仍保留内部 DMA pool、`spdk_vtophys` direct 检测、pipeline 和 SPDK batch；FIO 裸盘对照使用 raw SPDK，不把 async API 作为 HFS 使用场景。
- Trace 默认关闭。性能归因时设置 `HFS_TRACE_PATH=/mnt/hfs-data/<run>/hfs-trace.tsv`，高并发可设置 `HFS_TRACE_SAMPLE=100`；正式吞吐对比必须使用 trace-off。标准回归命令为 `bash scripts/db/io_trace_smoke.sh <lib> <config> <image> <run-dir>`，离线汇总使用 `python3 scripts/db/hfs_trace_summary.py <trace.tsv>`。
- Trace 分析首先检查 `trace_dropped=0`、`spdk_submit=spdk_complete` 和 parent/child 是否闭合；重点关注逻辑 I/O、bounce copy/DMA pool、runtime queue、SPDK submit/complete、sync/range_stripe/metadata 锁、metadata 操作、completion/thread wait 及 wake reason。
