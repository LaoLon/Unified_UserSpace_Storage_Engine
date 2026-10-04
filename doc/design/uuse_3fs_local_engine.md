# UUSE 作为 3FS 节点本地存储引擎（适配设计）

> 状态：2026-10-04 侦察与方案冻结。本文是 3FS 集成方向的设计依据；
> UUSE core 的产品边界（`doc/design/uuse.md` §3）不变——本集成不是新的 core
> frontend，而是把 UUSE 作为 3FS storage 节点的本地引擎。

## 1. 目标与范围

把 UUSE 放到分布式存储的"节点本地引擎"位置：3FS `storage_main` 的每个
storage target 用 UUSE 管理本地 NVMe，替代当前的 Rust `chunk_engine`
（POSIX 文件 + RocksDB）。分布式语义（CRAQ 链复制、RDMA 数据面、mgmtd/meta、
FDB）全部保留，只替换 target 内部的本地持久化。

首个里程碑不追求性能超过 `chunk_engine`，而是打通：
`storage_main → UUSE 后端 → 真实 NVMe` 的读写、校验、崩溃恢复和 E2E 回归。

## 2. 3FS storage 接缝（代码事实）

参考源码：`src/storage/`（VM `/build/3fs` 与 `E:\Code\3Fs\3FS`）。

| 位置 | 事实 | 对 UUSE 适配的含义 |
|---|---|---|
| `store/StorageTarget.{h,cc}` | `useChunkEngine()` 在旧 `ChunkStore` 与 Rust `chunk_engine::Engine` 间分发 | 新增第三后端 `UuseChunkStore`（推荐），或替换 Rust 引擎 |
| `store/ChunkEngine.h` `aioPrepareRead` | 返回 `fd_and_offset()`，读路径用 **POSIX fd** | UUSE 无 fd：必须新增 UUSE 异步读分支，不能用 AIO worker 的 pread |
| `aio/AioReadWorker` | io_uring `register_files` / `io_prep_pread`、libaio | 同上；UUSE 路径按 `Client::submitRead/poll` 收割 |
| `store/ChunkEngine.cc` `update/commit` | 版本校验、CRC32C 复用/合并、COW、writing list、commit 原子性 | 新后端必须实现等价 chunk 语义（建议移植旧 `ChunkReplica`） |
| `store/ChunkStore.{h,cc}` + `ChunkReplica` | 旧 C++ 后端：版本状态机 DIRTY→CLEAN→COMMIT、KV 元数据、punch-hole 回收 | **语义可直接复用**，把文件/KV 层换成 UUSE |
| Rust `chunk_engine` | 每 size 档 256 个 cluster 文件（O_DIRECT/O_SYNC）+ RocksDB 元数据；COW/组冻结/compact | 不建议替换 Rust 文件层（Rust→C++ UUSE ABI + Rust RocksDB Env 代价过高） |
| `StorageTarget::usedSize/sync/punchHole/uncommitted` | 按后端分发；`chunk_engine` 侧为只读/空实现 | UUSE 后端需实现 usedSize、sync、uncommitted、GC |
| 配置 | `[server.targets] target_paths`、`use_new_chunk_engine`（create-target 的 `--use-new-chunk-engine`） | 新增开关（如 `engine_type = chunk_engine | uuse`），默认不变 |

## 3. 方案选项与决策

- **A. 替换 Rust `chunk_engine` 的文件层（否决）**：Rust 引擎直接 syscall
  （O_DIRECT/fallocate/punch-hole/statfs），且其 RocksDB 用 Rust crate；
  把这两层桥到 UUSE 需要反向 cxx ABI + Rust 自定义 Env，维护成本高、
  收益不确定。
- **B. `StorageTarget` 第三后端 `UuseChunkStore`（推荐）**：C++ 实现，
  移植旧 `ChunkStore/ChunkReplica` 的状态机与元数据语义，数据与元数据都落
  UUSE。改动集中在 `src/storage/`，可用配置开关与 `chunk_engine` 同机 A/B。
- **C. 仅把 chunk 元数据 RocksDB 放到 UUSE Env（不选）**：改动最小，但数据面
  仍是 XFS，不能证明"UUSE 做本地引擎"。

决策：**B**。Rust `chunk_engine` 保持默认，UUSE 后端以独立 `engine_type`
接入，便于回退和对照。

## 4. UUSE 能力映射与缺口

已有：文件 namespace（create/open/read/write/append/allocate/truncate/sync/
rename/remove/dirs）、C++20 异步 Client（submitRead/submitWrite/poll）、CRC32C、
1 MiB extent + journal/checkpoint 恢复、RocksDB FileSystem adapter（可复用为
元数据 KV）。

缺口与设计：

1. **无 POSIX fd / O_DIRECT**：读路径改为 UUSE 异步提交到目标 buffer；
   `AioReadJob` 增加 UUSE 分支（`UuseReadJob`），io_uring 只服务旧后端。
2. **无 fallocate / punch-hole / statfs**：空间按 UUSE `File::allocate` 预分配；
   GC 不做洞回收，改为"标记 + 文件重写/压缩"；容量统计走 UUSE usedSize。
3. **chunk 元数据规模**：UUSE 目标是 ≤1 KiB/inode、`max_inodes=1M`，而 3FS
   单盘可有百万级 chunk。设计：**固定 slot 大文件**（每 chunk 档少量 UUSE 文件，
   slot = 固定偏移），chunk 元数据不进 UUSE inode，放紧凑 KV（UUSE 上的
   RocksDB 或自管 append-only 元数据文件 + checkpoint）。
4. **COW / 版本 / 校验和**：沿用 `ChunkReplica` 语义（updateVer/chainVer、
   DIRTY→CLEAN→COMMIT、CRC32C combine/reuse、幂等去重由上层 `ReliableUpdate`
   承担）；UUSE `File::sync` 作为 commit/durability 边界。
5. **多线程访问**：3FS storage 有 update/read worker 线程池；UUSE 采用
   per-thread `Client`（与 MySQL seam 相同的 owner-file + per-thread client 模式），
   线程退出/retire 规则按 `doc/design/uuse.md` §5.3。
6. **崩溃恢复**：UUSE journal/checkpoint 负责文件与 extent 状态；chunk 版本
   状态在 KV 中，两层恢复顺序需定义（UUSE mount 完成后再恢复 chunk KV）。

## 5. 里程碑

### M2 实施方案（2026-10-05 冻结）

**后端选择**：`StorageTarget` 增加第三分支 `useUuseEngine()`（由 target 的
`PhysicalConfig.use_uuse_engine` 或 storage 配置的 `engine_type` 决定）。旧
`ChunkStore`/`ChunkReplica` 依赖 `ChunkFileView` 的 POSIX fd 与 `ChunkMetaStore`
的 KV 文件，无法直接复用；UUSE 后端按同一语义**新实现**。

**`UuseChunkStore` 类面（对齐 `StorageTarget` 现有调用）**：

| 方法 | 语义（映射到 UUSE） |
|---|---|
| `create/load(PhysicalConfig)` | 在 UUSE 卷内建/载入目标目录：每 chunk 尺寸档 2 个固定 slot 文件（`f<fileId>.dat`，slot = 档位大小）+ append-only 元数据 journal（`meta.log`） |
| `get(ChunkId)` | journal 重放后的内存映射（含 `commitVer/updateVer/chainVer/size/state/checksum/innerFileId/innerOffset`） |
| `createChunk(id, size, ChunkInfo&, executor, allowToAllocate)` | 分配空 slot，返回 `ChunkInfo`（无 fd，`view` 由 UUSE 位置描述替代） |
| `set(id, ChunkInfo, persist)` | 追加 PUT 记录（写数据后）；COW：旧 slot 回收 |
| `remove(id, ChunkInfo&)` | 追加 DELETE 记录 + slot 回收 |
| `queryChunks(range)` / `getAllMetadata` | 内存映射过滤（逆序，与旧语义一致） |
| `usedSize` / `uncommitted` / `resetUncommitted` | 由元数据聚合 |
| `sync()` | 同步 slot 文件与 journal（`File::sync` 触发 metadata checkpoint） |
| `update(job, executor)` | 移植 `ChunkReplica::update` 的版本/校验和/COW 语义，I/O 走 UUSE `File::write` |
| `aioPrepareRead/aioFinishRead` | UUSE 无 fd：`readFd` 置无效，记录 `innerFileId/innerOffset`；读执行分支见下 |

**读路径分期**：
- M2.1（单机 E2E）：`StorageOperator` 对 UUSE target 走**内联同步读**（`File::read`
  + `.wait()`），不经过 `AioReadWorker`；以文档化 stepping stone 形式验收正确性。
- M2.2：接 `uuse::Client::submitRead/poll` 异步完成，替代 fd/io_uring 分支。

**构建集成**：3FS 侧新增 `UuseChunkStore.{h,cc}` 与 smoke 目标，链接 UUSE
`libuuse_core.so`（UUSE 头文件与 .so 从 .250 构建产物复制到 .251；SPDK/DPDK
静态闭包已在 .so 内）。运行前提：目标 NVMe 绑定 uio、`vm.nr_hugepages=1024`、
UUSE `.conf` 指向该 BDF。

**去风险结果（2026-10-05，.251 实测）**：
- 链接：clang-17（`--gcc-install-dir=.../12`）链接 gcc-15 构建的
  `libuuse_core.so` 需加 `-Wl,--allow-shlib-undefined`（新 libstdc++ 符号由运行时
  系统库解析），与 3FS 现有链接口径一致。
- 运行：.251 真实 NVMe（`0000:13:00.0`）上 format/mount/128 KiB 写读/CRC 通过
  （`UUSE_LINK_SMOKE_PASS`）。
- 约束：.251 运行态下 hugepage 最多分配到 ~357 页，M2 测试配置改用
  `dma_arena_mb = 256`（需求 221 页）。
- 产物位置：.251 `/root/code-uuse`（UUSE 头文件 + `libuuse_core.so` + 冒烟程序）。

**分阶段与验收**：
- **M2.1**：`UuseChunkStore` 核心 + smoke（put/get/COW/remove/query/跨进程恢复）
  在 .251 真实 NVMe 上通过 → 目标：语义正确性。
  - 已完成（2026-10-05）：`SMOKE1_PASS`/`SMOKE2_PASS`（64K/512K/4M 写入+CRC、
    COW 版本递增、删除不复活、journal 重放守恒）；源码与运行说明见
    `integrations/3fs/uuse/` 与 `integrations/3fs/README.md`。
- **M2.2**：`StorageTarget` 第三分支 + `engine_type` 开关 + 异步读；单 storage
  节点创建 UUSE target，直连 storage client 写读通过。
  - 进展（2026-10-05）：`ChunkReplica::update` 的版本/COW/校验和/commit 语义已移植到
    `UuseChunkStore::update/commit` 并通过 smoke（自动递增版本、陈旧版本拒绝、
    commit 持久化、跨进程恢复）；`StorageTarget` 第三后端接线已完成并构建出带 UUSE
    后端的 `storage_main`（create/load/update/commit/query 分支；UUSE target 由
    `target.toml` 的 `use_uuse_engine/uuse_conf` 选择）。读路径已在
    `AioStatus::collect` 内联直读（无 fd）；服务级验证通过——新 `storage_main`
    同时加载 2 个 chunk_engine target + 1 个 UUSE target 并稳定运行。
  - 待做：单链 E2E（客户端写读经 UUSE target）+ 崩溃恢复；`StorageTargets::create`
    路径尚未带 UUSE 选项。
  - M2.3 试验记录（2026-10-05，两轮）：
    1) 新建单 target chain `900100003` + 新链表（ChainTableId 3）后客户端写读
       成功但 UUSE target 用量不变——查明 **新 inode 继承父目录布局
       `ChainTableId(1)` 并取该表最新版本**（`MetaServer` root layout +
       `MetaClient::updateLayout`），新链表不生效。
    2) 上传 **表 1 的新版本（v2，仅含 chain 900100003）** 后路由生效：客户端
       16MB 写入进入 UUSE target，但 **storage 在 `AioReadWorker::run`（collect
       的 UUSE 直读分支）崩溃**，写入中断（文件截断、I/O 错误）。
    根因假设：UUSE `Client`/`File::read` 从 AioRead 多个 worker 线程调用主线程
    创建的 Client，违反 UUSE 的线程模型（M1 smoke 为单线程）。
    待做：读路径改为 per-thread Client 或专用 UUSE I/O 线程（或加锁验证）；
    修复后重跑单链 E2E。试验后环境已恢复（systemd active、表 1 v3 恢复原两链、
    设备归还内核）。
- **M2.3**：接 3FS 集群（单链单副本或替换一个副本），FUSE 写读、SIGKILL 恢复、
  `hfsctl`/`list-targets` 检查。

**停止条件**：数据校验错、崩溃恢复不守恒、版本语义与 `ChunkReplica` 不一致、
journal 重放丢记录——出现任一即回退修复，不做性能结论。

- **M0 侦察与接口冻结**（本 doc）。
- **M1 UUSE chunk 数据面 PoC（不动 3FS）**：独立 C++ 工具/库，按固定 slot
  布局在 UUSE 上实现 put/get/overwrite/delete + CRC32C + sync，并用
  64 KiB/512 KiB/4 MiB 随机读写微基准与 `chunk_engine` 的负载形状对照；
  先回答"UUSE 能不能支撑 chunk 数据面"。
  - 形状探针已完成（2026-10-04，.250）：见
    `doc/performance/uuse_chunk_shape_probe_20261004.md`——64k 随机读 48.2K IOPS、
    512k/4m 带宽 3.0–4.0 GiB/s，无软件错误，数据面可行性通过。
  - PoC 工具已完成（2026-10-04）：`tools/uuse_chunkstore_poc.cpp`——固定 slot
    布局（64K/512K/4M 三档 × 2 文件 × 32 slot）、append-only 元数据 journal
    （PUT/DELETE 记录 + 记录 CRC，重挂载重放）、COW overwrite + free list、
    数据 CRC32C、`File::sync` 持久化。验证：跨进程 CLI 全流程
    （put/get/overwrite/delete/verify/info）与工具内 `selftest` 全部通过
    （版本递增、空间核算 188/192、删除后 NotFound、journal 重放零错误），
    非设备 CTest 21/21；证据在 VM `/home/uuse/uuse-chunkstore-poc-20261004`。
  - 边界：同进程 stop→mount 后数据路径读挂起（`doc/bug.md` #47），恢复验证
    按跨进程重启口径执行（与 3FS storage 重启语义一致）。
- **M2 3FS fork 单机 E2E**：新增 `UuseChunkStore` 后端 + UUSE 读分支 +
  `engine_type` 开关；在 uuse 3FS 实验室单 storage 节点跑通
  create-target → 写读 → `storage_bench`/FIO → SIGKILL 恢复 → `hfsctl` 等价检查。
  - 前置条件已完成（2026-10-05）：.251 的 3FS 构建环境已恢复——`/build/3fs`
    @ `22fca04`（与部署版本一致），clang-17 + gcc-12，6 个目标二进制构建通过；
    构建流程与兼容补丁见 `integrations/3fs/README.md` 和
    `integrations/3fs/patches/`。
- **M3 双存储节点与对照**：.251/.250 双节点跑 CRAQ 复制、resync、节点故障；
  与 `chunk_engine` 同机 A/B 吞吐/延迟/CPU，记录正式结论。

## 6. 风险与停止条件

- **风险**：元数据性能（chunk commit 频率高）；GC 无 punch-hole 的空间放大；
  UUSE 单 metadata actor 与 3FS update/commit 并发的组合；fork 维护成本；
  Rust/C++ 混合构建。
- **停止条件**：数据不一致/校验和错、崩溃恢复不守恒、`submit != complete`
  或句柄/lease 泄漏、UUSE mount 后 chunk KV 状态不闭合、错误被映射成
  静默重试——出现任一即回退到结构/正确性修复，不做性能结论。

## 7. 环境与交付

- 实验室：现有 uuse 3FS 集群（manager .254、storage .251/.250、client
  .252/.253，`cluster_id=uuse`，rxe0）。M2 在 .251 上以 `engine_type=uuse`
  新建一个独立 target 做 A/B，不破坏现有 `chunk_engine` target。
- 3FS fork 的改动以 patch 集形式保存在 UUSE 仓库（拟 `integrations/3fs/patches/`），
  与 `mysql/patches/` 同一维护方式；3FS 源码本体仍在 VM 内 Git 管理，不跨机同步。
