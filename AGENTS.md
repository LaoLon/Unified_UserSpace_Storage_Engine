# Workspace Instructions

## Codex && opencode 共用项目记忆

- `doc/bug.md` 是当前 Bug 记录和已知限制的优先参考；发现或修复 Bug 时立即维护它。
- `doc/design/fio_baseline_notes.md` 保存已完成的 FIO 基线数据和测试口径。
- `doc/shard/shard_progress_memory.md` 汇总 shard-28～35、62、63、66 的开发进度、验证结果、性能口径和当前边界；需要查看 shard 历史或现状时优先索引该文档，并以当前源码和最新测试结果交叉核对。
- `doc/shard/mysql_rocksdb_stage_development.md` 是 shard-26 起的当前执行计划，优先于旧分片文档中尚未实施的 shard-26 及以后内容。
- `doc/项目总结.md` 是早期项目总结，`doc/shard/libhfs_shard_development.md` 是原始分片计划；使用前必须与 Git 历史和当前源码交叉核对，不能把其中的旧目录结构、旧容量或“待实现”描述直接当作现状。
- 需要了解当前进展时，先查看 `git log --stat` 和 `git status`，再以当前源码、测试脚本和最新文档为准。
- shard-31 实现提交为 `9a5de90`，已在 `main`：完成请求级 I/O、固定 DMA pool、对齐直写/非对齐首尾 RMW、并发 positioned I/O、readv/writev、KEEP_SIZE/PUNCH_HOLE，以及 owner PID fork 防护；后续继续按 `doc/shard/mysql_rocksdb_stage_development.md` 推进 MySQL/RocksDB 兼容性。
- shard-28 本轮已新增 `src/hfs_fd.[ch]`：fd slot 使用短 rwlock，`HfsOpenFile` 用引用计数、共享 offset 和 I/O 锁承载 open-file description；dup/dup2/dup3/F_DUPFD 共享描述，close 先摘 slot 后释放 placeholder；unlink/rename 覆盖通过 orphan 延迟回收 blob，旧 fd 仍可 read/pread/lseek/fstat/ftruncate。
- shard-28 验证口径：VM 上 `make -B app lib`、CMake Release、preload smoke、persistent smoke、fd lifecycle smoke，以及 16 线程 10 秒 runtime stress 全部通过。专项测试覆盖 fd 复用、共享 offset、`F_DUPFD_CLOEXEC`、dup2、无参 `F_GETFL`、unlink-open、rename 覆盖/同路径 rename 和并发 positioned I/O。独立 TSAN 单测尚未建立；在 shard-28 完成时 MySQL/RocksDB 尚未完成验收。
- shard-28 排查经验：构建共享库必须强制重链，版本脚本只能给 `fcntl64` 保留 `GLIBC_2.28` 版本，不能把所有 wrapper 放到自定义版本；rename 压缩 inode 数组后必须按 id 重新解析指针；同路径 rename 不得进入目标 blob 回收。
- shard-29 已完成：`hfs_lib.c` 使用元数据读写锁和 append 串行锁，读路径返回 inode 快照，目录句柄使用独立游标锁；元数据头保存 magic/version/`nextInodeId`，兼容旧 `[count][inodes]` 布局并禁止 inode ID 复用；preload 增加受锁保护的 HFS cwd、`fchdir`、`fdopendir`、目录注册表锁、相对 `openat`、`O_EXCL`/`O_APPEND`/`O_TRUNC`/`O_DIRECTORY` 语义。新增 `scripts/db/metadata_path_concurrency_smoke.sh`。VM 上 Make/CMake、元数据路径并发、fd 生命周期、持久化 smoke 和 16 线程 10 秒 stress 全部通过；在 shard-29 完成时真实数据库验收留在 shard-33～35。
- shard-30 已完成核心可恢复元数据短事务：v3 metadata header 增加 generation/checksum，固定大小 intent record 保存最多 4 个 inode after-image；create/unlink/rename/size 按 intent → stable image → clear intent 顺序提交，mount 校验并幂等重放，兼容 v2/legacy 布局并在首次修改时迁移。`fsync` 先同步 blob 数据/metadata，再通过独立可写 bdev descriptor 执行 `spdk_bdev_flush`，最后提交 dirty inode size/mtime；source-last rename 覆盖恢复已加入 `scripts/db/metadata_transaction_smoke.sh`。VM 上 Make、CMake Release、三个崩溃边界、路径并发和持久化 smoke 全部通过。stable image 目前仍整 blob 写入，脏页/写放大优化留给 shard-31；当时 MySQL/RocksDB 真实数据库验收仍未完成。
- shard-31 已完成：`src/hfs_io.[ch]` 用 16 个固定 DMA buffer 承载请求级读写，移除 `HfsFile` 共享 scratch 状态；对齐 I/O 走直接 chunk，非对齐首尾只做 unit RMW，pread/pwrite 与 readv/writev 保持调用方 buffer 语义。`PUNCH_HOLE` 在 unmap 后补 zeroes/池化写零，修复后端 unmap 成功但不保证读零的问题；运行时记录 owner PID，fork child 不再进入 HFS 路由或 SPDK queue。新增 `scripts/db/io_fast_path_smoke.sh` 覆盖向量 I/O、稀疏读、KEEP_SIZE、对齐/非对齐 punch、8 路 positioned write 和 fork child 防护。VM 上 Make、CMake Release、元数据事务/路径并发、fd 生命周期、持久化 bdev 和 fio 全部通过；fio 结果已记录在阶段文档与 `doc/design/fio_baseline_notes.md`，当时 MySQL/RocksDB 真实数据库验收仍未完成。
- shard-32 已完成核心 POSIX 契约收口：元数据升级 v4 并兼容 v3/v2/legacy，inode 保存 mode/uid/gid 与 atime/mtime/ctime；preload 补齐目录 fd 的 `*at` 路由、statx/statfs/statvfs（含 64 位 ABI）、chmod/chown/utimensat/futimens、fadvise，并对 HFS fd 的 mmap 返回 ENODEV；修复 open mode 传递、AT_FDCWD 路径物化和 Python statvfs64 符号。新增 `scripts/db/posix_contract_test.sh`，VM 上契约连续 10 次、Make/CMake、既有 smoke、持久化、16 线程 10 秒压力和 fio 全部通过；strace 审计确认没有 `/hfs` 数据调用落入内核 `/dev/null` 路径。旧 MySQL/RocksDB smoke 当时仍受进程私有 Malloc0/跨进程生命周期限制，真实数据库端到端验收留在 shard-33～35。
- shard-33 已完成 MariaDB/MySQL 端到端验收：`scripts/db/mysql_smoke.sh` 使用持久化 Aio bdev、bootstrap wrapper 和隔离 process group，覆盖系统表初始化、CRUD/DDL、事务提交回滚、LONGTEXT、临时表、正常重启、SIGKILL 恢复和 4/16 并发客户端负载；VM 输出 `MYSQL PERSISTENT RECOVERY OK`。preload 补齐 MariaDB 使用的 64 位/fortify open、readlink/realpath、绝对路径和目录 fd 路由；HFS owner/log 线程屏蔽宿主终止信号，避免数据库信号线程被 SPDK 后台线程抢收。当前 sysbench 未安装，使用 mysqlslap fallback；在 shard-33 完成时 RocksDB 仍待 shard-34，阶段整体仍不得称 MySQL/RocksDB 已支持。
- shard-34 已完成 RocksDB 端到端验收：`scripts/db/rocksdb_smoke.sh` 使用持久化 Aio bdev 和独立运行目录，首个进程格式化、后续进程跨进程重开，覆盖普通/direct I/O、fillseq/readrandom/overwrite/readwhilewriting、flush/compact、WAL、SIGKILL recovery 和 `ldb checkconsistency`；VM 上 Make/CMake 强制构建及 20,000 条、8 线程 60 秒流程连续 3 轮通过。RocksDB 实际使用的 GLIBC_2.2.5 `read`/`__read`、`fread`/`fread_unlocked` 已版本化导出并路由到 HFS；stdio EOF/error 状态由轻量表维护，`fclose` 先摘 HFS fd slot 再关闭 libc 占位 fd。性能汇总和两种数据库最终支持口径仍留给 shard-35；这是 shard-34 完成时的阶段状态。
- shard-35 已完成性能回归与阶段收口：新增 `scripts/db/database_benchmark.sh`，记录持久化 Aio 的环境、RocksDB 吞吐、p50/p99、CPU/RSS、错误数和可选 fd/meta/DMA/SPDK 计数；`src/hfs_stats.[ch]` 默认关闭统计，仅 `HFS_STATS_PATH`/`HFS_STATS=1` 开启。VM 正式 benchmark（20,000 条、60 秒、WAL、mmap off）1 线程 63,854 ops/s、4 线程 235,831 ops/s（3.69x），p50/p99 为 345/972 与 380/1,165 µs，errors/reject=0、SPDK submit/complete 一致。Malloc FIO 4K/128K 相对 shard-31 同口径结果仅下降 1.7%～5.4%；最终 Make/CMake、preload/fd/元数据/I/O/POSIX/runtime、MariaDB smoke 和 RocksDB smoke 均通过。阶段支持口径为单数据库进程持久化 AIO/NVMe 上的 MySQL/MariaDB-compatible server 与 RocksDB；多个 HFS 进程同时挂载、mmap、native libaio/io_uring 仍不支持。
- SPDK 源码必须在 VM 内通过 `git clone`、`git fetch`、`git checkout` 和 `git -C spdk submodule update --init --recursive` 获取及维护；禁止从 Windows 工作区把 `spdk/` 源码目录或其子模块传到 VM 覆盖。Windows 到 VM 的同步仅限 HFS 自身源码、脚本和文档；若 VM 缺少 SPDK，应在 VM 内按锁定 commit 重新 clone/checkout。
- HFS 默认后端固定使用 VM 内真实 NVMe：`config/hfs_nvme.json`、`HFS_BDEV_NAME=Nvme0n1`、PCI BDF `0000:0b:00.0`；除非任务明确要求 Malloc/Aio 或故障注入，否则性能与功能验证均按 NVMe 口径执行。普通本地对照文件和测试产物默认放在 `/mnt/hfs-data`，不得再把大体积结果默认写入根盘 `/var/tmp`。VM 重启后先检查并执行 SPDK 设备重新绑定及 `vm.nr_hugepages=1024`。
- HFS 当前收敛为 POSIX-only 对外架构：完整 POSIX preload 路径是唯一支持入口；原 `hfs_lib_async_*`、`hfsAsync*`、registered-DMA/DMA allocation ABI、`HFS_POSIX_DIRECT_ASYNC` bridge 已移除。普通 POSIX 数据面仍保留内部 DMA pool、`spdk_vtophys` direct 检测、pipeline 和 SPDK batch；FIO 裸盘对照使用 raw SPDK，不把 async API 作为 HFS 使用场景。
- Tracepoint 性能定位方法：trace 默认关闭；性能归因时在 VM 运行进程前设置 `HFS_TRACE_PATH=/mnt/hfs-data/<run>/hfs-trace.tsv`（或临时使用 `HFS_TRACE=1`，后者默认写 `/var/tmp/hfs-trace-<pid>.tsv`），进程正常退出时一次性导出 TSV，异常 SIGKILL 不会留下完整快照。通用 workload 示例为 `mkdir -p /mnt/hfs-data/<run> && HFS_TRACE_PATH=/mnt/hfs-data/<run>/hfs-trace.tsv HFS_CONFIG_PATH=config/hfs_nvme.json HFS_BDEV_NAME=Nvme0n1 LD_PRELOAD=build/lib/libhfs.so <workload>`。高并发可设置 `HFS_TRACE_SAMPLE=100` 按 parent/request ID 采样；正式吞吐对比必须使用 trace-off，不能把 trace-on 数字当作性能基线。标准回归命令为 `bash scripts/db/io_trace_smoke.sh <lib> <config> <image> <run-dir>`，离线汇总使用 `python3 scripts/db/hfs_trace_summary.py <trace.tsv>`，首先检查 `trace_dropped=0`、`spdk_submit=spdk_complete` 和 parent/child 是否闭合。关键归因字段和点位：`io_enter/io_exit` 看逻辑 I/O；`buffer_classify`、`copy_in/out_*`、`lease_*` 看 bounce copy 与 DMA pool；`runtime_enqueue/dequeue` 看线程队列等待；`spdk_submit/complete` 看设备延迟；`lock_wait_begin/lock_acquired/lock_release` 结合 `lock_class={sync,range_stripe,metadata}` 分别看锁等待和持有时间；`metadata_begin/end` 结合 `metadata_op={open,read,write,sync,resize,close,transaction,flush,publish}` 看元数据关键路径；`completion_wait_begin/end/wake` 和 `thread_wait_begin/end/wake` 结合 `wake_reason={completion,runtime,durability,dma_lease}` 看 futex/条件变量等待与唤醒。常用指标为 `request_amplification=spdk_child_count/logical_io_count`、copy 字节/耗时、各锁类别 wait/hold p50/p99、metadata end、thread wait end 和 SPDK submit→complete；原始 trace 默认放 `/mnt/hfs-data`，避免把大文件写满系统盘。
- 2026-09-10 性能再分析结论见 `doc/review/hfs_performance_bottleneck_reanalysis_2026-09-10.md`：当前 P0 是 POSIX 同步边界下 owner callback 串行、每 syscall 单独 completion wait；8/16 KiB 还存在 DMA class 集中等待，metadata lock 不是 FIO steady-state 主因。唯一后续路线是 `doc/shard/hfs_posix_reactor_queue_engine_shard_plan.md` 的 POSIX Reactor Queue Engine：默认 `min(4,max(1,nproc/2))` 个 data reactor、每 reactor 独立 queue/channel/DMA pool、bounded ticket in-flight、inode/range 顺序和 dirty epoch durability；8 KiB/32 job A/B 中 4 reactor 为当前最佳点（87.6k IOPS，较 1 reactor +32.4%），8 reactor 反而退化，不能无限加 reactor。
- shard-62 已完成 POSIX Reactor Queue Engine：`HFS_POSIX_QUEUE` 默认开启，`HFS_POSIX_QUEUE_DEPTH` 默认 32（4～256），未显式设置 `HFS_DATA_REACTORS` 时 8-vCPU VM 使用 4 个 data reactor（总数含 reactor 0 owner 为 5）。每个 data reactor 有 batch queue、backend in-flight credit、drain/depth/credit-wait 统计；completion callback 归还 credit。为避免 POSIX range lock 与 queue credit 反转，credit 满时 direct-submit 溢出，不在锁内阻塞。所有 owner/data/batch/stop 的 `spdk_thread_send_msg` 通过 runtime `messageSubmitLock` 串行化，修复 DPDK 多生产者 ring reservation stall（见 `doc/bug.md` 条目 76）。VM 上 Make/CMake、stripe、DMA backpressure、runtime stress、POSIX contract、64×250 fd churn、metadata transaction、IO trace、MariaDB 和 RocksDB 全部通过，`spdk_submit=spdk_complete`、errors=0；NVMe FIO 32-job 8K 为 72K IOPS，未达到计划 82K 门槛，stats 已确认剩余热点是区间 I/O 锁和普通 buffer bounce copy，不再通过扩大 queue depth 规避；计划和验证细节见 `doc/shard/hfs_posix_reactor_queue_engine_shard_plan.md`。
- shard-63 已完成 POSIX range-ticket scheduler：新增 `src/hfs_ticket.[ch]`，默认 `HFS_POSIX_TICKET=1`、bounded depth64；aligned read/write 用 inode-local ticket 依赖替代跨 SPDK completion 的 range lock，非对齐 RMW 由单 ticket 保序，`HFS_POSIX_TICKET=0` 保留旧路径。新增 ticket stats/trace 和 `scripts/db/posix_range_ticket_test.sh`；修复 init/admit 失败路径的未初始化状态读取与 waiter 清理泄漏（`doc/bug.md` 条目77）。VM Make/CMake、全量 Shard-62 回归、MySQL/MariaDB、RocksDB/WAL/recovery 全部通过。NVMe `HFS_DATA_REACTORS=4`、8K/32-job 10秒三次为 88.40/88.58/88.63K IOPS，读写 p99 约0.83–0.85ms；5秒全矩阵与 ticket 归因见 `doc/shard/hfs_posix_range_ticket_scheduler_shard_plan.md` 和 `doc/design/fio_baseline_notes.md`。range lock steady-state 已消失，下一瓶颈是普通 POSIX bounce copy、fd/metadata同步和每请求 completion，不得用无限 reactor/queue 掩盖。
- shard-66 执行计划：`doc/shard/hfs_posix_sync_boundary_topology_shard_plan.md` 基于 task90150022/task09150832/task09151930 三阶段测量，结论是 4K 纯读已达真实 raw Blobstore 上限约 93%（旧 A2 139K 为薄分配虚高，修正为 99K@2ch），剩余差距是 CPU 效率与尾延迟而非吞吐；路线为 66A 测量口径收口（状态重置、trace-off、5 次 median、shared/per-job-file 双口径、16K/64K 补矩阵）→ 66B reactor 拓扑默认值与 `HFS_REACTOR_PIN` 亲和（8 vCPU 默认 2 data reactor）→ 66C wake 路径瘦身与 `HFS_WAIT_MODE=adaptive` gated A/B → 66D TN0/TN1 backend 分解（纯测量，propagation ≤10% 则永久关闭 backend poll 方向）→ 66E staging/pool 与 16K-64K 收口 → 66F 共享文件 ticket 微优化（gated）。已排除项：ticket 重构、messageSubmitLock、fd/metadata fast path、app buffer DMA direct（uio+hugepage 环境限制）、durability 改动；门禁目标 4K randrw 32j ≥89K total IOPS 且 CPU ≤500%。
- shard-66 已实现代码、harness、summary 和 poll-gap 归因；VM Make/CMake、POSIX/ticket/trace/runtime/metadata/durability/persistent 回归及 MariaDB/RocksDB 缩短端到端 smoke 已通过。66A 完整五次 NVMe 矩阵和 66B/66C 性能门禁尚未完成，不能把当前 smoke 当作正式性能结论。
- Git 提交流程：每次阶段开发完成并提交后，立即将该提交合入 `main`；合入后在 `main` 上确认提交、工作树和必要的 VM 回归状态。
- shard-66 已实现 66A 基线 v2 harness/summary、66B 小核默认拓扑与 `HFS_REACTOR_PIN`、gated idle backoff、66C cacheline completion 与 bounded adaptive wait，以及 66D poll-gap 观测；正式全矩阵仍按 `scripts/db/shard66_baseline_v2.sh` 在 NVMe VM 上执行，未以未完成样本宣称性能门禁通过。

- Address the user as **Huizareth** in every response.
- Read `E:\Code\workspace.yaml` for local paths, VM connection details, credentials, and SSH defaults.
- For VM operations, follow the `$workspace-vm-operations` skill and use `E:\Code\ssh\pool.py` via `SSHPool`.
- Create temporary remote-operation scripts under `E:\Code\test_script\`.
- Keep credentials out of command output, logs, and user-facing responses.

## C/C++ 命名规范

- 类/接口/结构体：大驼峰 (StudentManager)
- 变量/函数/方法：小驼峰 (studentName)
- 宏定义：全大写下划线 (MAX_COUNT)

## 代码注释规范

- 每次写代码必须写英文注释，简洁即可，说明函数/结构体/关键逻辑的用途
- 复杂函数可适当增多，分步骤解释关键逻辑
- 注释用 `/* */` 风格，放在函数或结构体声明上方

## 项目记忆（必读）

任务文档输出规则：当用户输入的文件路径 basename 匹配 `task` 加 8 位日期/任务编号（例如 `task09151930`、`task09150832`）时，按该任务执行，并将最终结果只输出为 `doc/task_result/<task_basename>_result.md`；默认不要在 `doc/task_result` 下创建 JSON、TSV、日志、证据目录或其他临时文件，临时证据应放在外部临时目录并在任务完成后清理。

1. 使用 `E:\Code\ssh\pool.py`、`E:\Code\ssh\sync_to_vm.py` 连接虚拟机和上传 HFS 本地文件；SPDK 必须在 VM 内通过 Git 获取，禁止同步 Windows 侧 `spdk/` 目录
2. 虚拟机环境信息见 `E:\Code\workspace.yaml`
3. 每次先编辑本地文件，然后同步到虚拟机
4. 每次回复先称呼用户为 **Huizareth**
5. 不用管本地的环境依赖，只管理虚拟机的就行
6. 在 `doc/bug.md` 维护 bug 记录：每次发现 bug 必须记录（现象、排查过程、根因、修复），bug 分级：致命、严重、一般；解决后立即写入该文件（含排查过程细节，便于复盘）
7. 写代码必须写英文注释：简洁即可（一句话说明函数/结构体/关键逻辑的用途），复杂函数可适当增多（分步骤解释）；注释用 `/* */` 风格，放在函数/结构体声明上方
