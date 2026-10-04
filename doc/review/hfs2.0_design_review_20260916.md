# HFS 2.0 设计文档评审

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

> 评审对象：第一轮为旧版 `doc/design/hfs2.0.md`（2900 行）；第二轮为 2026-09-16
> clean-break 重写版（C++20 coroutine-first、`.conf`、multi-NVMe，见 §9）。
>
> 评审日期：2026-09-16
>
> 评审基线：`main` @ `74af418`（shard-67 架构收敛）、VM 内 RocksDB 8.9.1、fio 3.42、当前源码与既有设计/性能文档
>
> 结论：架构方向（去 Blobstore、直接 bdev、metadata/data 分离、async-first、per-core channel）成立；但文档存在 4 类问题：与既有决策/现状冲突、对 RocksDB 8.9.1 的事实性错误、关键设计细节空白、迁移与验收口径缺失。按 P0/P1/P2 分级如下。
>
> 处置状态：第一轮问题已由 clean-break 重写版吸收；第二轮问题（见 §9）已直接修复到
> `doc/design/hfs2.0.md` 和项目记忆中。本文件保留为评审历史。

---

## 1. P0：与现状及既有决策的冲突（需先决策）

### 1.1 未声明"取代"关系，与现行架构决策直接冲突

- `doc/design/hfs_product_architecture_decision.md`（2026-09-10 生效）与 `doc/design/hfs_posix_only_architecture.md`（已实施）明确规定：
  - POSIX preload 是唯一支持入口；
  - `hfs_lib_async_*`、`hfsAsync*`、registered-DMA/DMA allocation ABI、`HFS_POSIX_DIRECT_ASYNC` bridge 已删除且不再提供。
- hfs2.0 要反转该决策（删除 preload、恢复 native async API），文档开头必须显式声明 supersede 上述两个文档，并给出反转理由（引用当前性能数据与 CPU 效率分析）。否则两套决策并存，执行者无法判断以哪个为准。

### 1.2 Phase 0 删除 LD_PRELOAD 会造成能力真空

- MySQL/MariaDB 是当前唯一完成端到端验收（初始化、CRUD/DDL、事务、重启、SIGKILL 恢复）的数据库（shard-33/34）；删除 preload 后该能力直接回归，而 RocksDB 需等 Phase 6 才能跑通。
- 文档缺少"新旧共存与切换"策略。建议：
  - 旧 `libhfs.so` 冻结为 baseline 继续可跑（含 MariaDB/RocksDB smoke），`libhfs_core` 并行开发；
  - fio/RocksDB/InnoDB 三 frontend 对等后再删除 preload；
  - 否则 85% 性能目标也失去同口径 A/B 对照（当前所有 fio 数据均来自 preload 路径）。

### 1.3 §52/§53"保留/删除"清单已落后于当前源码

- shard-67 刚完成的内容在文档中完全未出现：
  - `src/hfs_op.[ch]` 统一 operation lifetime（67A）；
  - per-reactor actor ring（67B，`HFS_IO_EXECUTOR=legacy` 为回退）；
  - inode dependency scheduler（67C，**已取代 ticket**）；
  - shared on-demand DMA arena（67D）；
  - parent completion（67E）；
  - metadata dirty-set/epoch（67F）；
  - 旧 range compatibility 清理（67G，`hfs_range_lock.[ch]` 已删除）。
- §53 仍写"删除 HFS ticket compatibility layer"，但 `hfs_ticket.c` 在 67C 已被 scheduler 替换，清单与源码不一致。
- 需逐模块明确：**复用 / 参考重写 / 删除**，不能只有一句"actor/ring 部分保留"。

### 1.4 "目前约 raw SPDK 60%"口径不明确

- `doc/performance/hfs_final_performance_baseline_20260913.md`：4K 63.4%、8K 63.5%、16K 65.5%、64K 70.9%（HFS POSIX vs raw SPDK，32-job 共享文件，trace-off）。
- shard-63 记录：8K/32-job 达 88.4~88.6K IOPS，vs raw 112.5K ≈ 79%（A 布局、5 秒矩阵）。
- 引用性能起点与 85% 目标时，必须固定 block size、jobs、文件布局（shared/per-job）、trace 开关、raw 侧工具（spdk fio plugin / bdevperf），否则无法验收。

---

## 2. P0：事实性错误（已对照 VM 内 RocksDB 8.9.1 头文件核对）

核对文件：`/usr/include/rocksdb/file_system.h`、`/usr/include/rocksdb/env.h`（librocksdb-dev 8.9.1-2）。

| # | 文档位置 | 文档写法 | 8.9.1 实际情况 | 影响 |
| --- | --- | --- | --- | --- |
| 2.1 | §23 | 应声明 `kAsyncIO`、`kFSBuffer`、`kFSPrefetch` | `enum FSSupportedOps { kAsyncIO, kFSBuffer };`（file_system.h:84），**无 kFSPrefetch** | 需升级 RocksDB 或删除该声明 |
| 2.2 | §22 | 实现 `SubmitReadAsync()`，返回 io_handle | 真实接口为 `FSRandomAccessFile::ReadAsync(FSReadRequest&, opts, std::function<void(const FSReadRequest&, void*)> cb, void* cb_arg, void** io_handle, IOHandleDeleter* del_fn, IODebugContext*)`（file_system.h:965） | API 名称与签名需修正 |
| 2.3 | §22 | 只提 `Poll()` | 同时**必须实现 `FileSystem::AbortIO()`**（file_system.h:681） | 缺失会导致 abort/超时路径不完整 |
| 2.4 | §23 | 零拷贝读与 async 并列为第一阶段能力 | kFSBuffer 注释明确 "only enabled for MultiReads (sync and async both) with **non direct io**"（file_system.h:831-836）；RocksDB 官方文档明确 async_io（FilePrefetchBuffer 走 ReadAsync）**要求 direct IO** | 两条路径不能同时吃到，需明确选型 |
| 2.5 | §10/§25/§27 | 依赖 RocksDB 的 preallocation/`PrepareWrite()`/`Allocate()` hint | 三个接口确实存在（file_system.h:1186/1199/1221），但 **只在 adapter 声明 `use_direct_io()` 时才被调用** | 文档未决策是否声明 direct IO，这决定预分配、对齐、`PositionedAppend` 是否必须实现 |
| 2.6 | §21/§27 | 接口清单 | 缺 `FSDirectory::Fsync/FsyncWithDirOptions`（file_system.h:1342/1347）、`LockFile/UnlockFile`、`FSSequentialFile::Skip/InvalidateCache` | rename/create 后目录持久化与 RocksDB 建库锁依赖这些接口 |
| 2.7 | §8 | `HfsFile` 无时间戳字段 | `GetFileModificationTime` 为 FileSystem 必需接口（file_system.h:524） | `HfsFile` 需补 mtime/ctime（及 atime 视需要） |
| 2.8 | §21/§57 | 用 `NewCompositeEnv` 接入，Phase 6 "db_bench 可运行" | `NewCompositeEnv(const std::shared_ptr<FileSystem>&)` 存在（env.h:1910），但 **Debian 版 `db_bench` 无 CLI 参数注入 `DBOptions::env`** | 需自编 driver 或自编译 RocksDB 工具，验收方式需写明 |
| 2.9 | §23 | `fs_scratch` 零拷贝移交 | 类型为 `std::unique_ptr<void, std::function<void(void*)>>`，支持自定义 deleter（可行） | 但需明确 `FSReadRequest` 生命周期保持到回调、`io_handle` + `IOHandleDeleter` 语义 |

---

## 3. P1：关键设计缺口（建议新增章节）

### 3.1 SPDK 线程模型没有落地

- 当前实现：`spdk_env_init`（hfs_runtime.c:665）+ `spdk_thread_create`（:686/:932）+ 自建 pthread 轮询 `spdk_thread_poll`（:708/:950），非 `spdk_app_start`/event framework。
- 文档只说"busy-poll data reactor"，未说明：谁创建 reactor 线程、如何 CPU pin、与 SPDK thread 库的关系、metadata reactor 的 io_channel 从哪来、§17"不要 spdk_thread_send_msg"如何在实现上成立。
- 这是实现者最容易退回老路（双层队列）的地方，必须写清。

### 3.2 `hfs_client` 生命周期 API 缺失

- §15 的 read/write/sync/get_completions 均以 `struct hfs_client *` 为参数，但没有 `hfs_client_create/destroy/attach` 定义。
- 未定义：同一 client 多线程调用、多 handle 并发、client 与 reactor 的亲和关系。

### 3.3 §7 与 §10/§18 的写路径矛盾未闭环

- §7 原则："普通 read/write 不允许进入 metadata reactor"。
- §10：预分配窗口（64 MiB）耗尽时 writer 必须去 metadata reactor 再分配。
- §18：writable file 固定 writer reactor。
- 缺：跨 reactor 同步点定义（谁阻塞、是否允许 data reactor 阻塞、还是 ring 异步回填 + 继续写 staging）。若不明确，容易演变成隐式全局锁。

### 3.4 Journal 细节全缺

- 环形还是追加式？journal 满时的行为（阻塞写、强制 checkpoint、返回 ENOSPC）？
- `journal_checkpoint_threshold: 0.5` 的确切含义与 checkpoint 触发条件？
- checkpoint 原子更新方式（双缓冲/世代/写序）？
- 事务 commit record 的落盘顺序、CRC 覆盖范围、512B/4K 扇区原子性假设？
- `fdatasync` 语义、group commit（RocksDB WAL 每次 sync，单 metadata reactor 吞吐是硬约束）。

### 3.5 Allocator 细节全缺

- free-space 结构（bitmap？内存开销）、分配策略（顺序/就近）、并发分配模型。
- 碎片与 ENOSPC 判定、TRIM/unmap 支持。
- 预分配窗口与 1 MiB extent 的事务批量关系（64 个 extent 一次事务？）。

### 3.6 Namespace 细节全缺

- 目录如何表示、`GetChildren` 大目录性能、rename 原子性、路径解析与内存 hash 的一致性。
- 每文件内存开销与超大 namespace 的 mount 时间预算（in-memory metadata 全量加载的可行性边界）。

### 3.7 失败语义未吸取现有 bug 教训

`doc/bug.md` 未修条目在新设计中必须有对应解法，否则会重新引入同类问题：

| 条目 | 问题 | 新设计需明确 |
| --- | --- | --- |
| #48 | 多个 SPDK 异步回调只唤醒一个 waiter、不传播 bserrno | completion 唤醒与错误传播模型 |
| #49 | 元数据事务失败不回滚内存 after-image，创建失败遗留孤儿 blob/占位 fd | journal 提交失败的内存回滚与孤儿回收 |
| #50 | fsync 在 inode 元数据事务之前执行设备 flush，事务写入后无持久化屏障 | §14 顺序必须覆盖该场景 |

### 3.8 背压与错误模型不完整

- §36 retry queue"必须有界"——有界后满了怎么办（返回错误/阻塞/丢弃）未定义。
- §41 错误模型缺 ENOMEM/EBUSY/EAGAIN；缺到 RocksDB `IOStatus`（如 ENOSPC→NoSpace）与 fio `io_u->error` 的映射表。

### 3.9 teardown / fork / 多进程边界

- unmount 时 inflight I/O、journal、handle 引用的 drain 规则。
- fork 后子进程行为（当前实现有 owner PID 保护，新设计未提）。
- 多进程共享挂载仍不支持，需显式声明。

### 3.10 资源预算自相矛盾

- §44 `dma_pool_mb: 2048`，而 VM 为 `vm.nr_hugepages=1024 × 2 MiB = 2 GiB`，未计 SPDK 自身内存；文档无 hugepage 校验/预算项。
- §31 fio `iomem_alloc` 需要一次性分配整块 `orig_buffer`（bs × iodepth，可达几十~上百 MB），§19 的固定 4K/16K/64K/1M class pool 无法服务，需要大块连续 DMA 分配路径。

### 3.11 Reactor 数量规则缺失且与既有结论不一致

- §5 示例 4 个 data reactor、§45 示例 CPU9-12；但 shard-66/67 结论是"按 CPU 自动匹配且封顶，8 vCPU 默认 2 个 data reactor，继续增加会退化"。
- 需给出拓扑选择规则（配置项、默认值、上限）并与 §44/§45 统一。

### 3.12 on-disk 兼容与版本策略缺失

- 当前镜像是 Blobstore 布局，与 superblock/extent 新格式不兼容：需明确"旧镜像不可用、`hfsctl format` 破坏数据"。
- §6 superblock 缺 clean-shutdown 标志、设备几何（sector size/total blocks）；`meta_start/meta_blocks` 与 `checkpoint_lba` 的关系（Metadata Region vs Checkpoint Region）未讲清；A/B 写入顺序规则未定义。

### 3.13 非功能目标缺失

- metadata 操作吞吐目标（RocksDB compaction 建文件频率）、mount 时间、内存上限、延迟 p50/p99 目标。

### 3.14 构建/ABI 章节缺失

- `libhfs_core.so` 符号可见性/版本、C core 与 C++ adapter 边界、Make/CMake 双构建、fio engine 编译安装目标。

---

## 4. P1：fio 侧缺口

1. **加载方式**：fio 3.42 支持 `--ioengine=external:/path/fio-hfs.so`（不必安装）；若安装到 engine 目录需 root。文档只写"安装到 fio engine 目录"。
2. **自定义 option**：`hfs_config` 需在 `ioengine_ops.options` 注册（option_struct_size + options 数组），文档未提。
3. **§33 预分配机制**：fio `setup_files()` 会按 `size=` 自行 extend（ftruncate/写零）。自定义引擎需在 `open_file` 中 create+allocate 并设置 `FIO_NOEXTEND`（或自行处理 extend），否则 fio 绕过 HFS 直接扩展文件。
4. **§30 异步语义**：需写清 `getevents` 的 min/max 语义、`event()` 中 `io_u->error` 设置、`FIO_Q_BUSY` 背压返回。

---

## 5. P1：测试与验收缺口

1. **crash 注入矩阵**：data flush / metadata commit / journal flush 各阶段断电模拟，当前只有 §51 的概念描述。
2. **正确性参考模型**：与 ext4 上同一 workload 的语义对照。
3. **RocksDB 验收命令化**：fio `--verify`、db_bench 具体命令与 Env 注入方式、`ldb checkconsistency`。
4. **85% 目标口径固定**：CPU 预算、QD、jobs、trace-off、prealloc、文件布局、raw 侧工具（spdk fio plugin 还是 bdevperf）。

---

## 6. P2：一致性与小问题

1. §3 措辞"HFS 已经自己负责 allocator/journal/recovery"与现状不符（现在是 Blobstore 在做），应写"HFS 2.0 将负责"。
2. §5 与 §45 的 core 编号/数量不一致；§5 缺 metadata reactor 的 channel 说明。
3. §15 定义了 `hfs_iovec` 但所有 read/write API 都用 `void*/len`，未使用；`hfs_completion` 缺 opcode/offset 回带说明（仅靠 user_ctx 可行但应写明）。
4. §11 journal 类型缺 `HFS_J_FREE`、`HFS_J_SET_TIMES`。
5. "async-first"与元数据 API 全同步的矛盾未解释（第一阶段 metadata 串行 + 同步等待是合理选择，但需明说）。
6. §52"保留"清单未包含 `hfs_meta_v5`（事务/恢复逻辑）、`hfs_durability.c`（group commit）、`hfs_op.c` 的设计经验。
7. 建议补两节：**"取代的旧文档/相关文档"**（列出被 supersede 的 design/shard 文档）与 **"风险与缓解"**（in-memory metadata 规模、单 metadata reactor 瓶颈、journal 写放大、RocksDB 版本升级风险）。

---

## 7. 建议的修改优先级

| 优先级 | 内容 | 对应章节 |
| --- | --- | --- |
| P0 | 声明 supersede、新旧共存策略、shard-67 现状对齐、性能口径 | §1 |
| P0 | RocksDB 8.9.1 API 修正（kFSPrefetch/ReadAsync/AbortIO/kFSBuffer 边界/direct IO 决策/db_bench 注入） | §2 |
| P1 | SPDK 线程模型、client 生命周期、写路径跨 reactor 闭环 | §3.1~3.3 |
| P1 | journal/allocator/namespace 设计细节、失败语义、错误模型 | §3.4~3.8 |
| P1 | teardown/fork、资源预算、reactor 规则、on-disk 版本、非功能目标、构建 ABI | §3.9~3.14 |
| P1 | fio 机制、测试与验收口径 | §4、§5 |
| P2 | 措辞与一致性清理 | §6 |

---

## 8. 附：核对证据

- RocksDB 版本：`db_bench --version` → `8.9.1`；头文件 `/usr/include/rocksdb/file_system.h`、`env.h`（librocksdb-dev 8.9.1-2）。
- fio 版本：`fio-3.42-139-gae35`；引擎目录为空，需 `external:` 加载或安装。
- 当前源码核对：
  - `src/hfs_runtime.c:665/686/708/932/950`（SPDK env + 自建 thread poll）；
  - `src/hfs_mount.c`、`src/hfs_file.c`、`src/hfs_meta.c`（Blobstore 依赖）；
  - shard-67 模块见 `doc/shard/shard_progress_memory.md`。
- 性能基线：`doc/performance/hfs_final_performance_baseline_20260913.md`（4K 63.4%~64K 70.9%）；shard-63 8K/32-job 约 79%。
- 未修 bug：`doc/bug.md` 条目 #48、#49、#50。

---

## 9. 第二轮评审（2026-09-16，clean-break 版）与处置

第一轮 P0/P1 问题已被 clean-break 重写版（C++20 coroutine-first、raw bdev、`.conf`、
multi-NVMe）吸收。第二轮针对新版设计文档和项目记忆的问题已直接修复，记录如下。

### 9.1 设计文档问题与处置

| # | 问题 | 处置（已修改） |
| --- | --- | --- |
| 1 | §14.1 使用 RocksDB 8.9.1 不存在的 `IOStatus::TryAgain` | 改为 `IOStatus::Busy()`（可 `SetRetryable(true)`），并注明 ReadAsync 对 Busy 的处置需 Phase 5 exact-version 验证 |
| 2 | §5.2 示例先 scheduler 后 DMA，与 §7.1 固定顺序矛盾 | 示例改为 DMA lease 先于 scheduler node，§7.1 顺序补入 ring slot |
| 3 | `co_await ring.enqueue` 与 §6.3 "ring 满返回 Busy 不挂起"矛盾 | 改为 `ring.reserve`，明确 caller 线程非挂起、满则 Busy |
| 4 | §15.2 hidden visibility + 仅导出工厂符号与 adapter 独立链接 core C++ 符号矛盾 | 改为 public header `HFS_API` 导出 + version script 导出 `hfs::*` 公共符号，并恢复 `HFS_CORE_ABI_MAJOR=2` 记录 |
| 5 | SPDK env 配置缺失 | `.conf` `[spdk]` + `[device.*] pci_bdf` 生成 DPDK allowlist 写入 `spdk_env_opts` |
| 6 | extent map "cache miss 回填" 与常驻内存元数据不一致 | §7.3 改为 immutable snapshot + epoch 发布；新增 §13.3 常驻内存元数据模型 |
| 7 | allocator 持久化语义不清 | §9.1/§9.4 明确 bitmap 只随 checkpoint 写入 inactive slot，free-run index mount 重建，journal replay 增量 |
| 8 | 文件→shard 映射缺失 | §7.2 增加 `hash(file_id) % R` 固定 shard，metadata actor 只做 namespace/extent/epoch |
| 9 | Client 生命周期/drain API 缺失 | §13.1 增加 `Client::create/close`、`Engine::drain` |
| 10 | FIO `iomem_alloc/iomem_free` 缺失 | §12.2 增加整块 DMA `orig_buffer`，无法容纳时回退 ≤1 MiB child bounce |
| 11 | MySQL/MariaDB 范围未声明 | §2.1/§3.2 明确 v1 不做 InnoDB，MariaDB 仅在 1.x 冻结线 |
| 12 | §11.1 env 所有权与 `DB::Open` 示例 | 示例改为 `Options.env = env.get()`，注明 RocksDB 不接管所有权 |
| 13 | §16.2 p99 口径歧义、raw 对照工具未定义 | 改为 "p99 不高于 raw 基线 110%"，raw 工具固定为 SPDK bdevperf 或官方 fio SPDK plugin |
| 14 | 配置缺 `max_inodes`/`max_namespace_bytes`/`max_extents_per_file`/checkpoint 阈值/trace/stats | `.conf` 增加 `[metadata]`、`[observability]` 和 `[storage]` 相关 key |
| 15 | §12.1 `hfs_volume` 无定义、配置扩展名错误 | 删除 `hfs_volume`，`hfs_config` 改为 `.conf` |
| 16 | 多盘 metadata 镜像的崩溃恢复规则缺失 | §9.5 增加：journal 逐盘镜像、commit point 为全部副本、recovery 取最小已提交 txid 回滚、CRC/缺失 fail closed |
| 17 | §18 Phase 0 构建清理描述不足 | 明确删除旧 C source list、建立 C++20 target 和编译标志 |

### 9.2 项目记忆问题与处置

| # | 问题 | 处置 |
| --- | --- | --- |
| 1 | `.gitignore` 含 `/AGENTS.md` 但文件仍被 Git 跟踪 | 已删除该 ignore 行 |
| 2 | `doc/review/hfs2.0_design_review_20260916.md` 未跟踪 | 文件保留在工作区，等待随文档提交入库 |
| 3 | 1.x 文档与新记忆混放 | 工作区已清理：1.x 设计/性能/review/shard/task 文档从 Git 和磁盘移除（仅存 Git 历史）；`doc/bug.md` 重写为 HFS 2.0 Bug 记录，`AGENTS.md` 同步改为清理后的文档边界 |
| 4 | AGENTS.md 测试口径与新 `.conf` 不一致 | 已统一为 `.conf`/device set 口径（`52301b4` + 本次编辑） |
| 5 | README 仍描述 Blobstore 1.x 架构 | 顶部加 HFS 2.0 重构状态横幅，1.x 构建说明保留用于旧线复现 |

清理后工作区文档只有 `doc/design/hfs2.0.md`、`doc/review/hfs2.0_design_review_20260916.md`
和 `doc/bug.md`（HFS 2.0）。

### 9.3 第二轮核对证据

- RocksDB 8.9.1 `io_status.h`：只有 `Busy()`/`IOError(SubCode)` 等工厂，无 `TryAgain()`；`Status::SubCode::kTryAgain = 13`（`status.h:89`）。
- RocksDB 8.9.1 `options.h:571`：`Env* env = Env::Default();`，`DBOptions::env` 为裸指针，RocksDB 不接管所有权。
- fio 3.42 `ioengines.h:80/110`：`FIO_NOEXTEND` 存在；`iomem_alloc/iomem_free` 为 ioengine_ops 回调。
- 工作区：`src/` 已移除、`src_legacy_frozen/` 本地冻结并被 `.gitignore` 覆盖；`Makefile`/`CMakeLists.txt` 仍引用旧 `.c`（Phase 0 处理）。