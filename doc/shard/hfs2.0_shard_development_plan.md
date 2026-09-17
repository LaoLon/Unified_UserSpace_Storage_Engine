# HFS 2.0 Shard 开发计划（Phase 0-6）

> 状态：HFS 2.0 当前执行计划，2026-09-16 起生效
>
> 事实源：`doc/design/hfs2.0.md`；评审依据：`doc/review/hfs2.0_design_review_20260916.md`
>
> 基线：clean-break 工作区（`src/` 为空；旧代码冻结在 `src_legacy_frozen/`；1.x 文档已清理）
>
> 目标：把 `hfs2.0.md` §18 的 Phase 0-6 拆成 shard-68 起的可独立验收单元
>
> 不包含：1.x 兼容（POSIX/preload/fd/Blobstore/旧 ABI）、MySQL/InnoDB、多进程/daemon、
> 通用 mmap、数据面 RAID/复制
>
> 编号：延续 Git 历史（1.x 最后为 shard-67）；HFS 2.0 从 shard-68 开始，无兼容义务
>
> 进度：Shard-68 已完成（2026-09-16；Make/CMake、parser/生命周期/task 单测、
> ASAN+UBSAN、hfsctl validate，VM 验证通过）；Shard-69 已完成（2026-09-16；SPDK env/
> subsystem/reactor 所有权、R=2 自动拓扑、fork/双进程保护、doorbell、clean shutdown）；
> Shard-70 已完成（2026-09-16；CRC32C、superblock A/B + geometry、format/mount、
> hfsctl format/info、损坏 fail closed）；Shard-71 已完成（2026-09-16；journal ring
> BEGIN/RECORD/COMMIT + CRC、checkpoint A/B、通用 replay、阈值 checkpoint、`hfsctl check`）；
> Shard-72 已完成（2026-09-16；逐盘 superblock/checkpoint/journal 校验、显式 UUID 与
> geometry 不匹配 fail closed、缺失设备拒绝、`hfsctl dump`、`import` 骨架）；
> Shard-73 已完成（2026-09-16；FramePool、Operation 池、MPSC ring、actor、completion
> queue，ASAN/TSAN 结构门禁通过）；Shard-74 已完成（2026-09-16；inode dependency
> scheduler、barrier 顺序、32 客户端窗口模拟、shardForFile 映射，CTest 11/11）

---

## 0. 全局规则

### 0.1 每个 shard 的完成定义

1. `make` 与 CMake 均可构建；C++20 target；不引用 `src_legacy_frozen/`；
2. 新增路径有单测或结构测试，生命周期和资源守恒测试通过；
3. VM 真实 NVMe（`Nvme0n1`、PCI BDF `0000:0b:00.0`）上按 `.conf` 短回归通过；
4. `spdk_submit == spdk_complete`、errors=0、无 frame/lease/ring/parent 泄漏；
5. trace/stats off 不引入 per-request 分配；本 shard 新路径按 §16.5 补齐 tracepoint；
6. 结构或恢复门禁失败时不得用性能数字掩盖，按 §17.3 停止并回到修复。

### 0.2 固定口径

- 正式功能/性能使用真实 NVMe + `.conf`（单盘 baseline `config/hfs_nvme.conf`），
  设备 set 不允许用临时环境变量追加；
- 性能门禁固定 §16.2：4K shared-file randrw 50/50、32 jobs、10 s、5-run median、
  trace/stats off、HFS/raw ≥85%、CPU ≤500%（8 vCPU）、p99 ≤ raw×110%；
- 故障注入和对照实验可用 Malloc/Aio，但不得据此得出正式结论；
- 阶段顺序门禁：Phase 2/3 的结构与恢复门禁未过，不进入 Phase 4/5 的性能工作。

VM 环境口径（2026-09-16 起）：

- SPDK 构建树固定在 `/root/code/HFS_BASE_SPDK/spdk`（复用 HFS 1.x 的构建产物）；
  HFS 2.0 源码同步到 `/root/code/hfs2`，VM 无 Git 凭据，使用 SFTP 增量同步；
- NVMe `0000:0b:00.0` 绑定：`modprobe uio_pci_generic` →
  `echo "<vendor> <device>" > /sys/bus/pci/drivers/uio_pci_generic/new_id` →
  `echo <BDF> > /sys/bus/pci/drivers/uio_pci_generic/bind`；测试后必须 unbind 并
  `echo <BDF> > /sys/bus/pci/drivers/nvme/bind` 恢复内核驱动；
- 每轮 SPDK 测试前清理 `/var/run/dpdk` 与 `/run/dpdk`；
- 构建：`make -j4 SPDK_ROOT_DIR=/root/code/HFS_BASE_SPDK/spdk`、
  `cmake -S . -B build-cmake -DHFS_SPDK_ROOT=/root/code/HFS_BASE_SPDK/spdk`。

### 0.3 文档与变更

- 设计冲突先更新 `hfs2.0.md`，再开始实现；评审文件只作历史依据；
- shard 进度只写本文件，不写回 `AGENTS.md`；
- 每个 shard/sub-shard 只推进一个主要机制，必须可回滚（`.conf` key 或提交回退）；
  打包多个机制时必须拆成 sub-shard 并分别验收；
- 每个 shard 可派生 `doc/task/` 任务文档，结果写 `doc/task_result/`（按 AGENTS.md 规则）；
- 新 Bug 立即记入 `doc/bug.md`。

---

## 1. Shard 总览

| Shard | Phase | 内容 | 规模 | 依赖 | 出口门禁 |
|---|---|---|---|---|---|
| 68 | 0 | 工程骨架、`.conf` parser、空壳 ABI、hfsctl/stats/trace 骨架 | M | - | 双构建 + parser 单测 + 空 engine 生命周期 |
| 69 | 0 | SPDK env、owner/metadata/data reactor、channel 所有权、进程锁 | M | 68 | 8 vCPU 拓扑、clean shutdown、双进程 Busy |
| 70 | 1 | bdev set、superblock A/B（含 replica 格式字段）、geometry、format | M | 69 | 单/双盘 format→mount→轮换、旧 magic 拒绝、副本字段落盘 |
| 71 | 1 | journal ring、checkpoint A/B、通用 replay 机制、多副本协议冻结 | L | 70 | torn record、journal full、模拟状态机守恒 |
| 72 | 1 | hfsctl format/check/dump、多盘校验 | S | 71 | 工具输出、CRC/geometry 不一致 fail closed |
| 73 | 2 | Task/frame pool、Operation、MPSC ring、doorbell | M | 69 | ASAN/TSAN、Busy 背压、drain 闭合 |
| 74 | 2 | 唯一 inode dependency scheduler | M | 73 | 依赖场景单测 + 重叠写回归 |
| 75 | 2 | 75A planner/parent；75B DMA arena + read/write（synthetic extent map） | L | 73/74 | 合成 fixture 数据正确、direct/bounce 计数、不构成完整文件验收 |
| 76 | 3 | namespace、inode/目录索引、文件 API（不含 allocate） | L | 73/74 | 命名语义回归、handle generation、大目录 |
| 77 | 3 | allocator + allocate/truncate + 真实 extent map 集成 | M | 75/76 | 分配守恒、ENOSPC、真实文件端到端读写、placement |
| 78 | 3 | epoch、datasync/sync、group commit | L | 77 | write→sync→kill 已提交不丢、结构守恒 |
| 79 | 3 | crash replay、fault injection、ext4 对照、多副本执行 | L | 78 | §16.4 五个断点 + 守恒 + fail closed |
| 80 | 4 | 80A FIO engine 骨架；80B 功能矩阵 + verify | M | 77 | 矩阵正确、iomem all-or-nothing |
| 81 | 4 | FIO 性能门禁 | M | 80 | §16.2 主 gate ≥85% |
| 82 | 5 | RocksDB FileSystem 基础接口（buffered 过渡） | M | 78 | buffered 基础负载可跑、不宣告 direct |
| 83 | 5 | direct 契约 + PrepareWrite/Allocate + ReadAsync/AbortIO | L | 82 | direct 下重跑基础负载、SST/WAL/flush/compact |
| 84 | 5 | hfs_db_bench/hfs_ldb、RocksDB 恢复验收 | M | 83 | §16.3 + SIGKILL + checkconsistency |
| 85 | 6 | 多盘完整矩阵、性能基线、支持边界与归档 | M | 81/84 | 多盘门禁 + 文档收口 |

---

## 2. Phase 0：新工程与 ABI

### Shard-68：工程骨架、`.conf` 与 ABI

> 状态：已完成（2026-09-16）。交付 `libhfs_core.so` 骨架、`.conf` parser
> （schema_version 1 + 未知 key/重复 device/角色/阈值校验）、`Engine/Client/Task/Result`
> 生命周期、`hfsctl validate`、`config/hfs_nvme.conf`、Make/CMake C++20 构建和 3 个
> 单元测试；VM 上 Make、CMake/CTest、ASAN+UBSAN 全部通过。

**目标**：建立 C++20 编译和生命周期骨架，冻结配置与 ABI 入口。

**交付物**

- `src/` 新目录：`core/ runtime/ config/ io/ metadata/ storage/ common/`，
  `include/hfs/{core,task,types,config}.hpp`；
- Make/CMake：删除旧 `.c` source list，C++20 target，`libhfs_core.so`
  （visibility hidden + `HFS_API`，`HFS_CORE_ABI_MAJOR=2`）；
- `.conf` parser：`[hfs]/[spdk]/[storage]/[device.*]/[runtime]/[limits]/[metadata]/[observability]`；
  未知 key、重复 device、缺 required device、schema 不符拒绝启动；
- 空壳 `Engine/Client/Task/Result`：process lock、hugepage preflight、版本检查；
- `hfsctl --config` 骨架；`common/stats`、`common/trace` 骨架（off 零开销）。

**验收**：Make/CMake 均通过；parser 单测覆盖合法/未知 key/重复 device/schema 不符；
空 engine `create → mount → stop`；trace off 无 per-request 分配；不链接旧源。

**不做**：bdev 访问、任何持久化格式、coroutine 数据面。

### Shard-69：SPDK runtime 与 reactor 所有权

> 状态：已完成（2026-09-16）。交付 `SpdkRuntime` 单例（env/subsystem 进程级初始化、
> metadata + 2 个 data reactor、每 reactor×每 device channel、doorbell）、Engine fork
> 保护与 mount/stop 集成；VM 上 Make/CMake/CTest 4/4 通过，`R=2`、channel 2→0、
> fork child 返回 ForkedProcess、重启复用 env。

**目标**：固定 engine 启动/停止模型和多 reactor 所有权。

**交付物**

- `spdk_env_init`（用 `[spdk]` + `[device.*] pci_bdf` 构造 allowlist）；
- owner jthread 打开全部 bdev descriptor；1 个 metadata reactor + R 个 data reactor
  （`std::jthread` + `spdk_thread_poll`），每 reactor 对每 device 持有自己的
  `spdk_io_channel`；
- doorbell/`spdk_thread_send_msg` 规则；shutdown 顺序（关 ingress → drain callback →
  flush metadata → 销毁 channel → join）；
- fork child 全部 API 返回 `ForkedProcess`；第二进程/相同 bdev 返回 Busy。

**验收**：8 vCPU 自动 R=2；channel 在绑定线程创建/销毁；clean shutdown；hugepages=1024
下 preflight 通过；双进程测试 Busy。

---

## 3. Phase 1：raw bdev 与磁盘格式

### Shard-70：bdev set、superblock 与 format

> 状态：已完成（2026-09-16）。交付 `Hfs2Superblock` A/B（magic/version/replica 字段/
> geometry/generation/clean flag/CRC32C）、`computeGeometry`、`formatVolume`/`mountVolume`
> 控制路径、`hfsctl format --yes`/`info`；VM 单盘 format→mount→块写读回→槽位损坏选择→
> 双槽损坏 fail closed 全部通过。双盘真实验收受 VM 硬件限制（仅一块 NVMe），逻辑由
> geometry 单测覆盖，待第二块盘可用时补测。

**交付物**：逐盘 geometry/UUID/最小容量校验；`Hfs2Superblock` A/B（magic、版本、
device_id、volume uuid、replica id、`metadata_replica_count` feature flag、generation、
ranges、clean flag、CRC）；`hfsctl format --yes`；拒绝旧 Blobstore/1.x magic。

**验收**：单盘和双盘 device set `format → mount → 正常读写块`；A/B 轮换写序
（写非当前副本 → flush → 发布 generation）；容量不足/设备缺失拒绝 mount；replica
字段在单盘 format 中已经落盘且可校验（多副本执行在 Shard-79，格式不再变更）。

### Shard-71：journal ring、checkpoint 与通用 replay 机制

> 状态：已完成（2026-09-16）。交付 `Journal`（slot 化 ring、BEGIN/RECORD/COMMIT +
> CRC32C、volume UUID 世代校验、阈值 checkpoint、A/B 槽轮换、superblock 发布、tail
> 推进）、`JournalTarget`/`JournalDevice` 接口、内存设备单测（追加/回放/checkpoint/
> 回绕/撕裂/损坏）与真实 NVMe 端到端测试、`hfsctl check`；CTest 9/9 通过。真实
> CREATE/ALLOC 记录语义与 namespace/size/bitmap 守恒验收仍按计划留在 Shard-76/77/79。

**交付物**：`BEGIN/RECORD/COMMIT + CRC` 事务格式；record 类型枚举定义（§9.3，语义实现
随后续 shard 落地）；50% 触发、80% 强制同步 checkpoint；checkpoint 写 inactive slot →
flush → superblock publish → 推进 journal tail；replay 只接受完整事务；torn/未知/超长
record 丢弃并记录 corruption evidence；**多副本协议格式冻结**：journal header 带
replica set/generation，commit marker 语义为全部 required replica，superblock 记录
replica id 与 `metadata_replica_count`（单盘也必须写这些字段，后续不再改格式）。

**验收**：注入 torn record/journal full/checkpoint 中断；用 **test-only 模拟状态机**
验证 replay 幂等、generation/CRC 守恒；`hfsctl check` 可定位损坏。真实
CREATE/ALLOC/FREE/RESERVE 记录语义与 namespace/size/bitmap 守恒验收推迟到
Shard-76/77/79。

### Shard-72：hfsctl 与 device set 工具

> 状态：已完成（2026-09-16）。交付逐盘 `readSuperblockSlots` 与 `hfsctl check`（SB A/B、
> checkpoint、journal 汇总，corruption 非空 rc=1）、`hfsctl dump`（SB A/B 全字段 +
> checkpoint + journal）、`hfsctl import` 骨架（Phase 2 数据面实现前 rc=2）；mount 增加
> 显式 `volume_uuid` 校验、stored geometry 不匹配返回 Corrupt、缺失 bdev 有界等待后拒绝；
> `format_mount_test` 覆盖 uuid/geometry/缺失设备三种 fail-closed。双盘真实验收仍受 VM
> 单盘限制，逻辑由 parser 与 geometry 单测覆盖。

**交付物**：`format/check/dump`（`import` 骨架）；逐盘 geometry/UUID/CRC 检查；
设备缺失、重复 bdev、volume UUID 不一致时拒绝 mount；设备故障返回可定位 `IoError`。

**验收**：单盘/双盘工具输出正确；人为 CRC/geometry 不一致时 check 报错且 mount
fail closed；不静默迁移到未声明设备。

---

## 4. Phase 2：coroutine 数据面

### Shard-73：Task、Operation、ring 与 doorbell

> 状态：已完成（2026-09-16）。交付 `FramePool`（size-class、有界、overflow 计数，
> Task promise 分配钩子）、`Operation` 对象池（intrusive refcount、单次终态、
> onComplete）、`ShardRing`（MPSC reserve/publish/drain、满则 Busy、doorbell 去重与
> lost-wake 处理）、`ShardActor` 批量消费、`CompletionQueue`（SPSC）；tracepoints
> `RING_*`/`ACTOR_BATCH`/`CQ_*` 已接入（`FE_SUBMIT/FE_REAP` 随 coroutine client
> / File API 接入，Shard-76/77）；
> `data_plane_test` 覆盖 ASAN/TSAN、ring Busy、并发闭合、pool 无溢出。CTest 10/10。

**交付物**：`Task/Result`（无异常）、coroutine frame pool、`OperationRef` 侵入式生命
周期、per-shard MPSC ring、doorbell 去重、actor 批量消费、ring 满返回 Busy；
tracepoints：`FE_SUBMIT/FE_REAP/CQ_PUSH/CQ_REAP/RING_*/ACTOR_BATCH`。

**验收**：ASAN/TSAN 结构测试（无 UAF/double resume/orphan frame）；ring 满 Busy 且不
丢请求；shutdown 后 ring/frame/operation 全部闭合；trace off 无 per-request 分配。

### Shard-74：唯一 inode dependency scheduler

> 状态：机制完成，真实 NVMe 集成回归延期（2026-09-16）。交付 `InodeDependencyScheduler`：intrusive interval tree
> （按 start 排序、maxEnd 剪枝）、read/read 并行、重叠写/RMW 冲突、inode barrier
> （truncate/size mutation/gap-zero/sync）、FIFO 公平唤醒（合法可并行节点可越过等待者）、
> admit callback 批量延迟派发（避免回调重入破坏状态）、`SCHED_WAIT/SCHED_ADMIT/SCHED_DONE`
> tracepoints、`shardForFile` 文件到 shard 映射；`scheduler_test` 覆盖依赖场景、barrier
> 顺序、32 客户端 4096 op 窗口模拟（逐次准入做冲突不变量检查）与 ASAN/TSAN。真实 NVMe
> 32 线程重叠写与短读回滚回归依赖 coroutine client / File API 接入（Shard-76/77），随其落地。

**交付物**：intrusive interval tree + `max_end`；read/read 并行；重叠写、RMW、append、
truncate、gap-zero、size barrier、durability epoch 统一依赖；`hash(file_id) % R` 文件
到 shard 固定映射；tracepoints：`SCHED_WAIT/SCHED_ADMIT/SCHED_DONE`。

**验收**：确定的依赖场景单测（重叠/相邻/跨越）；真实 NVMe 32 线程重叠写与短读回滚；
dependency waits/pending peak 统计可与操作计数对上。

### Shard-75：数据面（75A planner/parent，75B DMA/read-write）

> 状态：已完成（2026-09-17）。交付 75A：`ExtentMap`/`SyntheticExtentMap`（字节口径
> lookup 裁剪）与 `IoPlanner`（child ≤1 MiB、不跨 extent/设备；head/tail RMW；
> 覆盖校验；空洞返回 NotFound）、`ParentCompletion`（first error、completed bytes、
> 并发单次唤醒）；75B：`DmaArena`（4K/16K/64K/1M size class、cap 背压 Busy、
> 可插拔分配器、RAII `BufferLease`、析构归还缓存块）、`SpdkRuntime::submitDataRead`
> /`submitDataWrite`（data reactor channel 提交，回调在 reactor 线程）与 `isDmaCapable`
> （`spdk_vtophys`）、`DataPath` 同步执行器（direct/bounce 双路径、RMW 合并、parent
> 聚合、元数据块到设备扇区换算）；tracepoints `EXTENT_PLAN/PARENT_COMPLETE/DMA_*`
> `/BDEV_*/COPY_*` 已接入。验收：`io_planner_test`（planner/arena/parent，ASAN+TSAN）
> 与 `data_path_device_test` 在真实 `Nvme0n1` 上完成合成 4 MiB extent 的零填充、
> 对齐/非对齐跨块 RMW 读写、DMA caller 的 direct 无拷贝路径、3 MiB 分片提交
> （submits=completes=21、direct=2/bounce=17、copy≈10.5 MB、arena 全量回收）；
> CTest 13/13、hfsctl 全套通过。**不构成完整文件验收**：真实 extent map 端到端与
> placement 仍按计划在 Shard-77；32 线程重叠写/短读回滚回归随 coroutine client /
> File API 接入（Shard-76/77）落地，当前 `DataPath` 为同步测试执行器。

**75A 交付物**：parent/child 规划（child ≤1 MiB、不跨设备）；head/tail RMW；parent
completion（first error、completed bytes、wake-all）；tracepoints：
`EXTENT_PLAN/PARENT_COMPLETE`。

**75B 交付物**：shared DMA arena（4K/16K/64K/1M、cap + backpressure）；direct
（`spdk_vtophys` 校验）与 bounce 双路径；每 device channel 提交；read/write/append；
tracepoints：`DMA_*/BDEV_*/COPY_*`。

**验收**：使用 **test-only synthetic extent map / 预分配物理 extent fixture** 验证
planner 拆分、RMW、parent 聚合和 direct/bounce 数据正确（含非对齐、跨 extent、稀疏读
零）；direct/bounce 比例与 copy bytes 可见；`spdk_submit == spdk_complete`。**本 shard
不构成完整文件验收**；真实 extent map 上的端到端读写与单/双盘 placement 验收在
Shard-77。

---

## 5. Phase 3：namespace、allocator 与 durability

### Shard-76：namespace 与文件 API 骨架

> 状态：已完成（2026-09-17）。交付 `src/metadata/` 常驻 namespace（`Namespace`：
> inode 表 + free-slot 复用 + generation、目录 hash/ordered 双索引、绝对路径解析、
> create/open/remove/rename/setTimes/getChildren、`max_inodes`/`max_namespace_bytes`
> 限流、unlink-while-open orphan 生命周期、可注入时钟）；公共 `include/hfs/file.hpp`
> （`OpenFlags`/`FileHandle`/`FileStat`/`DirectoryEntry`/`DirectoryPage`、`File` 骨架）
> 与 `Client::open/remove/rename/getChildren`（`ClientOptions::maxOpenFiles`、
> Directory/Exclusive/Truncate 语义、关闭后拒绝路径操作）；设计文档 §13.1 草案同步
> 增补这些类型与 §13.2 路径规则；`Engine::mount` 建立 namespace，`Engine::stop` 在
> 仍有 client/open file 时返回 Busy（`MetadataLease` 计数，§14.3）。验收：
> `namespace_test` 覆盖路径/类型检查、rename 原子替换与目录环检测、分页游标、限流
> NoMemory、orphan + generation 复用、double-close、并发创建者+目录读者、RocksDB
> 风格 1000 文件 create/rename/remove churn（ASAN+TSAN 干净）；`file_api_test` 在
> 真实 `Nvme0n1` 上覆盖公共 API 语义与 handle generation、open-file 上限、Busy 停止
> 契约。CTest 15/15。**尚未持久化**：namespace 是 mount 时重建的内存状态，
> CREATE/LINK/UNLINK/RENAME/SET_TIMES 记录与 checkpoint snapshot 随 metadata
> transaction path（Shard-78/79）接入；`File` 数据操作按计划返回 NotSupported，
> 直到 Shard-77/78。

**交付物**：inode/目录/name hash/ordered cursor；`open/remove/rename`（`allocate`/
`truncate` 的空间语义在 Shard-77 落地）；handle generation；mtime/ctime；`GetChildren`
不扫全表；`max_inodes`/`max_namespace_bytes` 限流；常驻内存元数据模型（§13.3）的
namespace/inode 部分。

**验收**：create/open/rename/delete/overwrite 语义回归；旧 handle 不解析到新 inode；
rename 是单事务且无中间态；超限返回 NoMemory；RocksDB 风格文件创建/rename 压测。
依赖 Shard-73/74，不依赖数据面 Shard-75。

### Shard-77：allocator 与真实 extent map 集成

> 状态：成功路径完成，一致性边界已补强（2026-09-17；持久化部分按计划留待
> Shard-78/79）。交付：
> `Allocator`（两级 bitmap + free-run index、next-fit、相邻 run 合并、
> FREE→RESERVED→INITIALIZED→COMMITTED、单次 ≤64 extent、capacity/ENOSPC、
> CRC 快照 serialize/deserialize、RESERVED replay 回收、`verify()` 不变量）；
> `ExtentDescriptor`/`InodeExtentMap`（per-inode 真实 extent map，实现 `ExtentMap`
> 查询、truncate、快照）；`PlacementPicker`（capacity_weighted/round_robin）；
> Namespace 扩展 `reserve/commit/releaseExtents`、`extents`、`truncateFile/setSize`
> （unlink/free 时回收 extent）；`DataPlane`（Engine 持有 DMA arena + DataPath）与
> `File::read/write/append/allocate/truncate` 端到端数据面；`IoPlanner` 稀疏读
> Zero child；`DataPath` 按 `child.deviceId` 选设备，DMA 直通收紧为仅 arena 拥有的
> 缓冲（`DmaArena::owns` + `spdk_vtophys`），避免未注册堆内存被设备直接 DMA。
> 验收：`allocator_test` 覆盖守恒/容量/ENOSPC 回滚/快照损坏/RESERVED 回收/
> ALLOC+FREE+RESERVE 记录语义联合回放/namespace 分配一致性/双设备 placement
> run 不跨设备（ASAN+TSAN 干净）；`file_data_test` 在真实 `Nvme0n1` 上覆盖 allocate
> 零初始化、非对齐跨块 RMW、覆盖写、append、稀疏读零、truncate 收缩/增长、多 iovec、
> 2 MiB 分片读写；`file_api_test` 增加数据面 smoke。CTest 17/17。
> **边界**：namespace/extent/allocator 的 checkpoint snapshot 与 CREATE/RENAME 等
> name-carrying 记录持久化仍待 metadata transaction path（Shard-78），crash replay
> 在 Shard-79；双盘真实链路受 VM 单盘限制，由多设备 placement 单测与
> planner/DataPath 的 device-local child 约束覆盖。

**交付物**：two-level bitmap + free-run index；`FREE → RESERVED → INITIALIZED →
COMMITTED`；单次 ≤64 extent 预分配；`File::allocate/truncate` 空间语义；NoSpace 判定；
RESERVED replay 回收；释放异步回收；allocator bitmap 只随 checkpoint 写入；真实
extent map snapshot 接入数据面（替换 Shard-75 的 synthetic fixture）。

**验收**：分配/回收长期守恒；非 COMMITTED extent 不可见；ENOSPC 前后文件与 bitmap
一致；**真实预分配文件上的 read/write/append 端到端正确**（非对齐、跨 extent、稀疏
读零、覆盖写）；单盘与双盘 placement（capacity_weighted/round_robin）child 不跨设备；
ALLOC/FREE/RESERVE 记录语义与主状态机联合回放通过。

### Shard-78：epoch、sync 与 group commit

> 状态：进行中（78A/78B，2026-09-17）。已加入 namespace 的 logical/durable epoch、
> per-inode logical/durable size、单调 durable publish 校验，以及基于 Journal 的
> 有序 group commit 协调器和批量 flush 验证；data flush、metadata transaction 与
> File sync 接线尚未完成，当前不宣称 sync 崩溃恢复能力。

**交付物**：`logical_size/durable_size` + metadata epoch；datasync 顺序
（data drain → device flush → metadata tx → journal flush → 需要时 checkpoint/superblock
→ publish durable epoch）；sync 额外提交时间戳/namespace/directory；group commit；
checkpoint 不得成为每次 sync 的固定步骤（避免破坏 group commit 和写放大）；
tracepoints：`META_TX_*/JOURNAL_*/CHKPT_*/FSYNC_*/EPOCH_PUBLISH`。

**验收**：write→sync→SIGKILL 后已提交数据可见；未达到 durable epoch 的事务不得被声明
为已提交，崩溃后 namespace/extent/size 结构守恒（原地覆盖不承诺崩溃后是旧值还是新值）；
metadata 不指向未 durable 数据；journal bytes/op 与 group commit 批量可报告；目录
rename 后 Fsync 语义正确。

### Shard-79：crash replay、fault injection 与多盘 fail-closed

> 状态：进行中（2026-09-17）。Journal replay 已对结构损坏返回 `Corrupt` 并保留
> corruption evidence；已加入 transaction after-image 回滚、mount 前 scan-only
> replay，以及 BEGIN/RECORD/COMMIT 三阶段写入失败、COMMIT 后 flush 失败、checkpoint
> slot/superblock publish/flush 失败注入测试；mount/正常 stop 的 clean-flag 生命周期
> 已落盘，真实 NVMe 上正常 reopen 显示 clean=1、SIGKILL recovery 显示 clean=0。
> 其余 data flush crash 断点仍待完成；双盘真实 fail-closed 已覆盖缺失 BDF、落后
> generation 和双槽 CRC，且修复了控制 flush 单位、目标盘 geometry 镜像及 mirrored
> journal geometry 生命周期问题。

> 2026-09-17：修正 `format_mount_test` 的单盘损坏边界：generation 断言不再要求
> 两个 superblock 槽位始终相等；geometry tamper 选择当前最高 generation 槽位；
> fallback mount 后同时损坏 A/B，并验证下一次 mount 返回 `NotFound` fail-closed。
> 该真实 NVMe 测试已通过，仍不替代真正掉电等价验证。

> 2026-09-17：新增显式 `HFS_TEST_FAIL_FLUSH_DEVICE` 测试钩子，并在真实 NVMe
> `file_api_test` 中验证 data child 完成后、device flush 提交前的失败边界：
> `datasync` 返回 `IoError`，清除注入后同一数据可正常完成 datasync/sync。该钩子
> 只在测试环境变量指定设备时生效，属于可重复的顺序边界证据，不等价于掉电模拟。
> 随后补齐 data flush 后、metadata commit 前的 `HFS_TEST_FAIL_METADATA_COMMIT`，
> 以及 clean-flag publish 前的 `HFS_TEST_FAIL_CLEAN_FLAG`；三者均在真实 NVMe
> `file_api_test` 中验证失败后重试。journal commit/flush 与 checkpoint publish
> 断点已有单元证据；真正掉电等价验证和断点后的完整 namespace/extent/size 守恒仍未闭合。
> 2026-09-18：Shard-79 的可重复错误/超时/取消组合矩阵已与 Shard-83/84 联合闭合，
> 覆盖 data/checkpoint/metadata/clean flush 失败、journal/副本 fail-closed、DataPath
> 超时和取消边界；真实掉电等价及每个断点后的全量结构守恒仍是独立未完成项。

**交付物**：§16.4 五个崩溃断点注入；orphan reclaim；内存 after-image 失败回滚；
`hfsctl check` 取证；ext4 同 workload 语义对照；**多副本执行**：按 Shard-70/71 冻结的
格式实现全副本 commit、最小已提交 txid 回滚、CRC/缺失 fail closed。

**验收**：每个断点后 namespace/extent state/size/CRC 守恒并可正常 mount；损坏时
fail closed 且现场保留；多盘掉一块 metadata device 拒绝 mount，不降级、不静默迁移。

---

## 6. Phase 4：FIO frontend

### Shard-80：FIO external engine（80A 骨架，80B 功能矩阵）

> 状态：进行中（2026-09-17）。80A 外部模块已接入 CMake，FIO 3.42 ABI 编译通过；
> 真实 NVMe bind→format→mount→cleanup 下 direct=1 和显式 bounce 的 direct=0 均通过
> 4K `--verify=crc32c` smoke，numjobs worker 共享进程级 Engine；FIO 的整块 buffer
> 已接入 Engine-owned 共享 DMA arena，并覆盖大块连续分配、容量耗尽和释放统计单测；
> 正式高负载 arena 容量矩阵仍待补齐；在 VM 上显式加入 FIO `iomem=malloc` 会被 FIO
> 报告为与 external engine 冲突并拒绝分配，不能把该路径误记为 arena 通过证据。
> 2026-09-17 已补真实 NVMe 超限探针（QD=128、numjobs=8、1MiB）：返回非零并输出
> `requested fio DMA arena exceeds mount cap`；FIO 外层仍会打印已创建 job 的统计，
> 因而这里只记录为 cap 拒绝证据，不宣称“零 worker 启动”或完整容量矩阵通过。
> 随后补跑 QD=128 的 80 项高深度功能矩阵（numjobs=1/2/4/8、4 种 block size、5 种
> workload），脚本逐项检查 `err=0` 并全部通过；这仍不等价于完整 QD×jobs 矩阵。
> 重新执行带持久化结果的完整矩阵后，前 315 项通过；在 QD=64、numjobs=8、1MiB、
> write 首次触及 512 MiB 共享 arena cap，FIO 返回资源申请失败。该结果证明了
> cap 边界和失败可定位，但完整矩阵仍未通过，不能把资源失败当作数据面通过。
> DmaArena 单测同时覆盖了 cap 满时的有限等待：释放在超时窗口内会唤醒并成功取得
> lease；无释放则按时返回 `Busy`，没有无界等待。
> 带持久化结果的完整矩阵收集到 385 项 `PASS`、15 项 `RESOURCE_BOUNDARY`、0 项
> 功能性 `FAIL`；资源边界集中在大 buffer/高并发组合，最大为 QD=128、numjobs=8、
> 1MiB，结果文件保留在 VM `/mnt/hfs-data`。
> 随后使用双盘 `hfs_nvme_dual.conf` 重跑完整 400 项组合，得到 320 项 `PASS`、80 项
> `RESOURCE_BOUNDARY`、0 项功能性 `FAIL`；边界集中在 64K/1M 与高 QD×jobs，结果为
> `/mnt/hfs-data/hfs-fio-matrix-dual-20260917.log`。
> 复跑时脚本另要求 FIO `issued rwts` 至少包含一个完成的读写，结果仍为 320/80/0；
> 该次结果保存在 `/mnt/hfs-data/hfs-fio-matrix-20260917.log`，结束后两块 BDF
> 均恢复内核 `nvme`，`vm.nr_hugepages=1024`。

**80A 交付物**：`fio-hfs.so`（`extern "C"` 注册）；options（`hfs_config/hfs_direct/
hfs_preallocate/hfs_client_depth`）；`open_file` 完成 create/open/allocate +
`FIO_NOEXTEND`；`queue/getevents/event`；`queue()` 三态返回（`FIO_Q_QUEUED` /
`FIO_Q_BUSY` / `FIO_Q_COMPLETED` + `io_u->error`）；`iomem_alloc/iomem_free` 为
all-or-nothing：整块 arena DMA `orig_buffer` 成功，否则 FIO 初始化失败并明确报错；
启动按 bs×iodepth×jobs 预检 arena 容量，不做半量 fallback。

**80B 交付物**：完整功能矩阵跑法与 `fio --verify`。

**验收**：iodepth 1/8/32/64/128 × numjobs 1/2/4/8 × read/write/randread/randwrite/
randrw × 4K/16K/64K/1M 全部正确；`fio --verify` 通过；iomem 生效时 direct ratio ≈100%；
arena 不足时初始化失败信息可定位；errors=0、submit/complete 闭合。

### Shard-81：FIO 性能门禁

**交付物**：raw comparator（同设备 SPDK bdevperf 或官方 fio SPDK plugin，同 bs/QD/jobs）；
5-run median 报告（HFS/raw、p50/p99、CPU%、RSS、copy bytes、bounce ratio、journal
bytes/op）。

**验收**：§16.2 主 gate：4K shared-file randrw HFS/raw ≥85%、CPU ≤500%、errors=0、
p99 ≤ raw×110%。不达标时停在上游结构修复，不扩大 reactor/arena 掩盖。

> 2026-09-17：发现 `DataPath` 原先将所有请求固定投递到 data reactor 0；现已按请求
> 在已启动的 data reactors 间轮询，真实 NVMe FIO smoke 通过。该修复尚未改变既有
> raw 对照的历史失败结论；随后按正确 external-engine 配置完成新的 5-run 复测，
> HFS/raw 中位 IOPS 比为 86.56%，但 raw bdevperf 结果没有 p99，且本轮 raw 使用
> 单 core、HFS 使用两个 data reactor，故仍不能宣称 §16.2 通过。后续需取得同口径
> raw p99 并统一 reactor/core 拓扑后再收口。已补采 raw histogram：中位 p99 为
> 1825.646 us；现有 HFS write p99 中位约 2056.192 us，高于 raw×110% 的
> 2008.211 us，故性能门禁仍未通过。
> 2026-09-17：新增可复用的原生 SPDK 5-run 基线文档
> `doc/performance/hfs2_spdk_raw_baseline_20260917.md`，覆盖 4K/8K/16K/32K/64K
> `randrw` 吞吐和平均延迟；该批结果不替代尚需统一拓扑采集的 p99 gate。
> 2026-09-18：按统一 CPU 2-3、真实 `Nvme0n1`、4K/randrw/50%/有效并发 32/10 秒
> 口径完成 raw 与 HFS 各五次采集。raw 中位 IOPS 为 5151.390；HFS external
> engine 每次仅完成 8 KiB，未兑现 32 jobs 并发，HFS/raw 为 0.000019，故 gate
> 结构性失败；raw 仅有 aggregate p99，读写分离 p99 和 submit/complete 也不能收口。
> 证据见 `doc/performance/hfs2_shard81_formal_gate_20260918.md`；必须先修复 FIO
> 前端提交/完成生命周期，再重跑正式 gate。
> 2026-09-18 修复已落地：移除 external worker pool，FIO `queue()` 在提交线程同步
> 完成 HFS core I/O 并返回 `FIO_Q_COMPLETED`；真实 NVMe 4-job/4K/randrw/3 秒
> smoke 完成约 19 MiB、errors=0，4 个 client 正常关闭。32-job 正式 gate 尚未
> 重跑，Shard-81 仍不标记完成。

---

## 7. Phase 5：RocksDB frontend

### Shard-82：RocksDB FileSystem 基础接口（buffered 过渡契约）

> 状态：基础接口已实现并随 Shard-83/84 在单盘真实 NVMe 回归；当前实现已切换到
> direct IO 契约，故本 shard 的历史 buffered-only 验收不再单独作为最终支持边界。
> `GetFreeSpace` 已改为返回 allocator-backed data capacity（排除元数据区和已分配
> extent），并由 namespace/真实 smoke 回归覆盖；完整 stock RocksDB 兼容性仍不宣称。

**交付物**：`libhfs_rocksdb.so`；`HfsFileSystem/HfsRandomAccessFile/HfsWritableFile/
HfsSequentialFile/HfsDirectory`；New/Get/Delete/Rename/Link/Lock/Directory/
`GetFileModificationTime`/`GetFreeSpace`；`NewCompositeEnv` 注入；本 shard 明确
`use_direct_io()==false`（buffered 过渡契约，不宣告 direct IO 支持）；`hfs_db_bench`
骨架。

**验收**：hfs_db_bench 注入 HFS Env 后 buffered 模式 fillseq/readrandom/overwrite/
readwhilewriting 基础运行；WAL、MANIFEST 能创建/rename/同步；stock `db_bench` 结果
不作为证据；不得宣称 direct IO 或性能结论。

### Shard-83：async read、direct IO 与 preallocation

> 状态：接口实现完成，单盘真实 NVMe 已验证 direct read/write、ReadAsync/Poll/AbortIO
> 生命周期、MultiRead、Allocate、PositionedAppend、RangeSync、Fsync 和错误映射；
> RocksDB smoke 已覆盖 async、MultiRead、8 个句柄立即 AbortIO 的并发竞态，以及
> 8 个线程共享一个 `FSRandomAccessFile` 的 positioned-read 回归。
> 当前仍缺完整 SST 并发读、性能放大和更广泛 cancel/timeout 矩阵证据，不能据此宣称
> 全部 direct IO 性能目标。

> 2026-09-17：`File::read/write` 已实现 pre-cancelled boundary；请求在进入数据面
> 前返回 `Cancelled`，写请求不会预留 extent。同步 I/O 的 in-flight 取消不伪装成可中断，
> 仍由 RocksDB `ReadAsync/AbortIO` 契约承担；完整 cancel/timeout 矩阵仍待补齐。
> 随后为 `ReadAsync/AbortIO` 增加显式 callback-started 边界：callback 开始后取消不再
> 改写 callback-visible status；真实 NVMe smoke 覆盖该竞态，完整矩阵仍待补齐。
> 另加入测试专用 async worker 延迟，使 8 个请求稳定落在 callback 前取消边界；真实
> NVMe smoke 验证所有请求最终为 `Aborted` 且 callback 次数为 0，避免立即取消测试
> 因调度竞态产生假阳性。
> 2026-09-18：async 读现在尊重 RocksDB `IOOptions::timeout`；前端等待超时返回
> `TimedOut`，后端 I/O 继续排空并由句柄析构回收。真实 NVMe smoke 已验证超时
> callback/status，DataPath 也将 `-ETIMEDOUT` 映射为 `TimedOut` 并验证 submit/complete
> 闭合；超时后的 request result 保持不变，避免后台完成越过 callback 边界写回；完整
> 组合矩阵已在 2026-09-18 的联合真实 NVMe 回归中闭合，详见
> `doc/review/hfs2_error_timeout_cancel_matrix_20260918.md`。
> 随后统一 RocksDB FileSystem 各输出参数的空指针保护，所有对应 API 返回
> `InvalidArgument` 而不解引用空指针；真实 NVMe smoke 已覆盖这些永久错误路径，
> 并保持原有取消/超时回归通过。
> 2026-09-18：已补齐并在真实 NVMe 执行 Shard-79/83/84 联合错误、超时、取消组合矩阵，
> 包括超时后 AbortIO、取消抢先于超时、callback-started 边界、8 句柄批量取消以及
> RocksDB flush 错误映射；性能放大和 SST 专项规模矩阵仍不在本次闭合范围。

**交付物**：切换到 `use_direct_io()==true` 并**同时**落地 `PrepareWrite/Allocate/
PositionedAppend`（三者必须一起启用）；`ReadAsync/Poll/AbortIO`（FSReadRequest/
io_handle 生命周期、`IOHandleDeleter`）；`MultiRead`；`RangeSync`；
`Fsync/FsyncWithDirOptions`；错误映射（`Busy` 而非不存在的 `TryAgain`，Phase 5 验证
RocksDB 8.9.1 对 Busy 的处置）。

**验收**：direct 契约下重跑 Shard-82 的 buffered 基础负载；SST 并发读、WAL sync、
flush/compact 正确；async 路径不回退成每请求同步；preallocation hint 生效且不产生
RMW 放大；`AbortIO` 遵守 cancel boundary。

### Shard-84：RocksDB 验收与恢复

> 状态：核心验收工具和单盘真实 NVMe 最小闭环已完成；已验证 fillseq/readrandom/
> overwrite/readwhilewriting、direct/async、显式 Flush/CompactRange、正常 reopen、
> SIGKILL recovery、clean-flag 取证、`hfs_ldb checkconsistency`、`hfsctl check` 和 MultiRead。完整
> §16.3 组合（含跨进程矩阵及所有错误/取消路径）仍保留为未完成证据项。双盘
> `hfs_nvme_dual.conf` 上已补跑四类 db_bench workload、SIGKILL 后 checkconsistency
> 和 `hfsctl check`，均通过且两副本状态一致。随后新增不启动 SPDK 的
> `hfs_lock_probe`，真实 VM 上第一进程持锁、第二进程返回 `Busy` 并报告锁路径，
> 跨进程独占边界已得到直接证据；完整跨进程 workload 顺序矩阵及所有错误/取消
> 路径仍保留为未完成证据项。
> 2026-09-17：修正 RocksDB `LockFile` 将竞争映射为 `Busy`（而非通用 `IOError`），并
> 在真实 NVMe `file_api_test` 中验证；同时修正 durable fault-injection 测试先扩展
> logical range，确保 checkpoint/metadata commit 断点不会被幂等 sync 短路。
> 随后统一 `GetFileSize`/`GetFileModificationTime` 的 stat 错误映射，并在真实 NVMe
> RocksDB smoke 中验证缺失路径不会退化为通用 I/O 错误。
> 另补齐 `LockFile` 空输出参数保护、锁竞争与其他 host 错误的区分，以及 `NoSpace`
> 到 RocksDB 子码的映射；真实 NVMe smoke 覆盖空输出、锁竞争和缺失路径断言。
> FileSystem smoke 还覆盖了 atomic rename 的新旧路径语义，并在删除重命名后的文件
> 后验证 free-space reclaim；该路径已随单盘真实 NVMe 全 smoke 通过。
> 随后按 RocksDB 8.9.1 的 exact `IOStatus` 接口收口：`TryAgain` 统一映射为设置
> `retryable` 的 `Busy`（该版本没有 `IOStatus::TryAgain`），`Cancelled` 映射为
> `Aborted`，`TimedOut` 映射为 `TimedOut`，`Corrupt` 映射为 `Corruption`；构建、
> 内部测试及真实 NVMe smoke 均通过。
> 另补充真实 FileSystem smoke 的 atomic rename：新路径可见、旧路径返回 `NotFound`，
> 并在后续删除中验证 extent reclaim 不受 rename 影响。
> `file_api_test` 还明确检查 checkpoint flush 与 metadata commit 注入期间
> `logicalSize/durableSize` 不变量，清除注入后重试才推进 durable size。
> 另以两个独立 `hfs_db_bench` 进程执行 `fillseq`→`readrandom`，再由独立
> `hfs_ldb` 进程执行 `checkconsistency`；真实 NVMe 结果为 64/64 读写与
> `entries=64`，补齐顺序跨进程 reopen 的直接证据。
> 随后将 `fillseq`、`readrandom`、`overwrite`、`readwhilewriting` 分别放入四个独立
> 进程顺序执行（各 64 项），独立 `hfs_ldb checkconsistency` 仍报告 `entries=64`；
> 正式 `.conf`/真实 NVMe 通过；错误、超时、取消组合矩阵已由联合回归闭合。
> 另补充 FileSystem smoke 的直接 `CreateDir`/`DeleteDir` 生命周期，单盘真实 NVMe
> 全 smoke 已通过；Shard-84 的错误、超时、取消组合矩阵已由联合回归闭合。
> 2026-09-18：联合矩阵已在单盘真实 NVMe 通过；跨进程 workload、SIGKILL/replay
> 和 checkconsistency 既有证据保持有效，stock RocksDB 全兼容性仍不宣称。
> 同时固定 `IsDirectory` 的 RocksDB 谓词语义：对缺失路径返回 `OK + false`，并由
> 真实 NVMe smoke 回归验证，避免将其误判为 `NotFound`。

**交付物**：`hfs_db_bench`/`hfs_ldb` 完整参数；SIGKILL recovery、跨进程顺序 reopen、
`checkconsistency`。

**验收**：§16.3 全项覆盖（create/write/close/reopen、SST read、WAL sync、
flush/compact、rename/delete、normal restart、SIGKILL、跨进程 reopen、
checkconsistency）；错误/超时/取消映射完整。

---

## 8. Phase 6：收口

### Shard-85：多盘完整矩阵、性能基线与归档

> 状态：单盘真实 NVMe 功能/FIO workload 矩阵和 `--verify=crc32c` loader smoke 已
> 通过；VM 现有两块独立 NVMe：`Nvme0n1`（`0000:0b:00.0`）和 `Nvme1n1`
> （`0000:13:00.0`）。新增 `config/hfs_nvme_dual.conf` 已通过双盘 format、mount、
> metadata replica check，以及双盘 FIO `randrw`/CRC verify；metadata replica 掉盘/
> CRC/generation 故障组合以及 raw 对照/5-run 性能门禁仍未完成，不能用
> SATA 或 Malloc/Aio 替代。

双盘 FIO 矩阵已完成 400/400 组合的分类执行（320 PASS、80 RESOURCE_BOUNDARY、0
FAIL）；RESOURCE_BOUNDARY 是当前 512 MiB 共享 arena 对大 buffer/高并发的明确容量
边界，不计作功能失败，也不计作完整矩阵全部通过。

2026-09-17 双盘验收还发现 clean/dirty 生命周期逐盘递增会使副本 generation 漂移，
已改为先读取全部副本、计算一次共享版本后逐盘发布；修复后双盘 `hfsctl check` 连续
通过。此前 free-space 立即减少断言与 extent 粒度不一致的问题已修正，当前双盘
RocksDB smoke 的 free-space reclaim 与 `hfsctl check` 均通过。随后对真实双盘完成缺失第二 BDF、合法落后
generation、双槽 CRC 损坏三类 fail-closed 探针，三项均返回非零并保留明确错误。
另以两块真实 NVMe 完成 required metadata device 掉盘探针：双盘 format 后仅将
`0000:0b:00.0` 绑定给 SPDK、让 `0000:13:00.0` 保持内核 `nvme` 驱动，dual
`hfsctl info` 返回非零并报告缺失控制器；测试结束后两块设备均恢复到内核驱动。
随后重新执行双盘 generation 回退与双槽 CRC 损坏探针：两项均返回非零，分别报告
metadata replica 不一致和设备未格式化；测试结束后两块设备仍恢复到内核驱动。

**交付物**：

- 单盘/双盘/配置全 data device 三种 device set 的功能与性能矩阵；
- metadata replica 故障组合（掉盘、CRC 不一致、generation 落后）fail-closed 验证；
- 正式性能基线（与 FIO/raw 同口径）写入支持边界文档；
- `hfs2.0.md` 状态更新、支持边界与迁移说明；`src_legacy_frozen/` 归档策略确认。

**验收**：多盘矩阵全部通过且每次记录 device set/placement/每盘 submit/complete；
支持边界明确；无 1.x 代码、镜像、API 回流；HFS 2.0 才可作为正式产品线。

---

## 9. 风险与停止条件

- 结构/恢复门禁优先于性能：出现 §17.3 任一情况立即停止性能工作（数据错误、
  metadata corruption、资源泄漏/double resume、submit≠complete、errno 丢失、错误映射
  不完整）。
- 设计冲突先改 `hfs2.0.md`；实现中的临时决定必须回写设计文档或显式记录为待办。
- 主要风险见 `hfs2.0.md` §17.2：metadata actor 瓶颈、frame 膨胀、direct IO 对齐、
  hugepage 预算、背压、恢复错误、旧功能回流。
