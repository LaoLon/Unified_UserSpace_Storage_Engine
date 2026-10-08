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
    根因已定位并修复：`StorageTarget::aioFinishRead` 对 UUSE target 仍调用旧
    `ChunkReplica::aioFinishRead`，访问未加载的 legacy `ChunkStore`（空 KV）→
    SIGSEGV；已加 UUSE 分支（版本校验走 `uuseStore_->get`）。同时把 UUSE
    数据面 I/O 串行化到专用 I/O 线程（`UuseChunkStore::runIo`）。
    修复：`aioFinishRead` 加 UUSE 分支；数据面 I/O 串行化到专用 I/O 线程；元数据
    改记**逻辑 chunk 尺寸**（slot 档位只决定物理位置，解决 1MiB chunk 映射到
    4MiB class 后的 `kChunkSizeMismatch`）。
    **M2.3 单链 E2E 通过（2026-10-05）**：客户端 4MiB/16MiB 写读 md5 全对、尺寸
    精确；UUSE target 用量 25,755,648 = smoke 4,784,128 + 4MiB + 16MiB（精确
    吻合）；SIGKILL → 重启（journal 重放）后两文件 md5 仍精确匹配。
    试验后环境已恢复（systemd active、表 1 v7 恢复原两链、设备归还内核）。
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
  - **正式 A/B 收口（2026-10-05，TCP 稳定口径）**：FUSE 客户端
    `force_use_tcp=true`（排除 Soft-RoCE 抖动），3× 64MiB `dd conv=fsync`
    写+读，md5 与尺寸逐次校验，同机同客户端同配置。
    | 臂 | 写（3 次） | 读（3 次） | 校验 |
    |---|---|---|---|
    | 原生 chunk_engine（原两链，干净态） | 1.11 / 1.15 / 0.92 s（均 1.06s ≈ **60 MiB/s**） | 3.02 / 3.04 / 2.95 s（均 3.00s ≈ **21 MiB/s**） | 3/3 md5 精确 |
    | UUSE（此前 TCP 扫描，单 target 链） | 64MiB 1.02 s ≈ **63 MiB/s** | 3.89 s ≈ **16.5 MiB/s** | 精确 |
    - **结论：写吞吐与原生持平（~60 MiB/s 量级）；读略低（16.5 vs 21 MiB/s），
      均受 FUSE/设备上限约束。**
    - 限制与遗留：UUSE 臂的 RF2 重测被链归属/表格路由问题阻塞（UUSE target
      固定关联在 chain 900100004，新建链被 `TargetId duplicated` 拒绝；表格
      切换脚本引号问题未完成），严格同拓扑 A/B 需下次用**全新 target id +
      脚本化表格切换**重跑；清理后残留链 900100003/900100004 与 offline
      target 为无害记录。
  - RF2 建链尝试（2026-10-05）：新建 .250 target（101000200103）成功，但
    `upload-chains` 拒绝把 UUSE target 放进新链——**target 只能属于一条链**，
    而 101000100103 仍关联在旧链 900100003 上（`TargetId duplicated`）。
    结论：做 RF2 前需先释放 UUSE target 的旧链关联（`remove-target
    --node-id 10001 --target-id 101000100103` 或清理旧链 900100003），再上传
    RF2 链；本次已回滚（移除新建 .250 target、恢复环境）。
  - RF2 建链尝试 v2（2026-10-05）：先 `offline-target` + `remove-target`
    101000100103 释放旧链归属，但两个请求转发到 storage 节点后均失败（节点侧
    目标视图与 mgmtd 记录不一致：systemd 进程不认识该 target），旧记录仍在 →
    `upload-chains` 继续 `TargetId duplicated`。结论：需在**同一 storage 进程
    视图**下清理（先停 systemd、由重建进程处理 remove-target，或直接清理 mgmtd
    侧旧链记录），再建 RF2 链。本次已回滚，环境恢复。
  - RF2 建链尝试 v3（2026-10-05）：在重建进程已注册为 node 10001 的情况下重试
    `offline-target`/`remove-target`，两者仍在 **RPC 层失败**（CLI 只回显
    `'_result' has error`，需完整输出定位）；旧 target 记录与链 900100003 仍在，
    RF2 链上传继续被 `TargetId duplicated` 阻挡。下一步：用完整错误输出定位
    offline/remove 的 RPC 失败原因（或直接清 FDB 中旧链记录后重建）。
  - RF2 阻塞根因（2026-10-05，完整错误输出）：`offline-target`/`remove-target`
    返回 **`RPC::SendFailed(2007)`**——admin 请求发不到 node 10001 的注册端点；
    同时 mgmtd 中 .251 的 chunk_engine target 也显示 OFFLINE，说明重建进程本次的
    服务端点/注册不完整（数据面此前可用，但 admin RPC 路径不通）。下一步：
    排查重建进程的 endpoint 注册（routing info 中的服务地址）或改用直接清 FDB
    旧链记录的方式解除 RF2 阻塞。
  - RF2 阻塞确认（2026-10-05，等待注册 30s 后复测）：node 10001 的 .251
    target 仍全部 OFFLINE、`offline/remove` 仍 `RPC::SendFailed`——重建进程的
    admin 端点在该环境下不可达（数据面此前可用）。结论：RF2 解锁应走**直接清理
    mgmtd/FDB 侧旧链与 target 记录**（或完整集群重置）的路径，而不是依赖
    offline/remove RPC。
  - **RF2 双节点验证通过（2026-10-05）**：用**全新 UUSE target id
    （101000100104）**绕开旧链归属阻塞，建链 `900100004 = [UUSE(.251) HEAD,
    chunk_engine(.250) TAIL]` **SERVING**。
    - 客户端 4MiB/16MiB 写读 md5 精确（CASE_4M_PASS / CASE_16M_PASS）
    - 复制验证：TAIL(.250) 用量 **20,971,520 = 4+16MiB 精确**；HEAD(UUSE)
      用量 **25,755,648 = smoke 4,784,128 + 4+16MiB 精确**
    - 故障测试：kill HEAD 后客户端仍**精确读回**（CRAQ 故障切换到 .250 副本）；
      HEAD 重启后读取正常
    - 环境已恢复：表 1 v27 = 原两链；临时 .250 target 已 offline；残留链
      900100003/900100004 为无害记录。
  - M3 首个数据点（2026-10-05，客户端 FUSE 64MiB dd 写+读，同机同客户端）：
    - UUSE 后端（chain 900100003，单副本）：写 **11.15s（~5.7 MiB/s，61MiB
      截断）**、读 3.29s（~19.4 MiB/s）
    - 原生 chunk_engine（原两链）：写 **1.76s（~36.4 MiB/s，尺寸精确）**、
      读 3.13s（~20.5 MiB/s）
    - 结论：读路径已接近原生；写路径慢 ~6×，原因明确——单 I/O 线程串行 +
      同步 UUSE I/O + 每次 update 全 chunk 校验和回读（1 次额外读）+ journal
      append/commit；且 **4M class 仅 64 slot 的固定布局容量上限**在 64MiB
      文件上被击穿（61MiB 截断）。
    - 优化轮（2026-10-05，第二轮）：
      - 定位并修复**最后一个 chunk 的 `TRUNCATE` update 未实现**（客户端用它
        物化末 chunk）——这是 61/63MiB 截断的真正原因；修复后 64MiB **完整写入**
        （size=67108864）。
      - slot 布局扩大（4 文件 × 128 slot/档 = 1536 slot）；整 chunk 写走
        **增量校验和**（从写缓冲直接计算，免回读）。
      - 结果：写 11.07s（~5.8 MiB/s，完整 64MiB）、读 3.89s（~16.5 MiB/s）。
        写吞吐仍 ~6× 慢于原生，瓶颈在**串行同步 I/O** 与每次 update/commit
        的等待。
    - 下一步优化：I/O 线程改 `Client::submitRead/submitWrite` 批量异步、
      commit 合并、多并发客户端验证。
    - **瓶颈定位（2026-10-05）**：存储层微基准（smoke `bench` 模式，直接
      `update+commit` 1MiB chunk）**64 op / 81ms = 1.27ms/op（~790 MiB/s）**，
      说明 UUSE store 数据面很快；客户端可见的 ~172ms/chunk 开销在**服务/客户端
      流程**（疑似客户端对未创建 chunk 的 RMW 读重试/退避、RPC 往返），下一步
      需抓客户端写流程（read-before-write 行为）与 RPC 时序。
    - 写延迟扫描（2026-10-05，8/16/32/64MiB 新文件）：快路径 **16MiB 0.26s、
      32MiB 0.47s、64MiB 1.12s（~57–68 MiB/s，接近原生）**；异常样本
      （8MiB 11.4s、1MiB 截断）与客户端侧 **RDMA transport 写错误告警**
      （Soft-RoCE）相关，**后端无 update/commit 失败**（truncate 修复后）。
      结论：store 层 1.27ms/op，客户端快路径已接近原生；此前 5.8MiB/s 均值被
      传输重试/超时段拉低。下一步：排查 Soft-RoCE 传输稳定性 + storage 侧细粒度
      RPC 计时，再出正式 A/B 结论。
    - Soft-RoCE 排查（2026-10-05）：rxe0 hw counters 显示高负载下大量重试/延迟
      确认（.251 `completer_retry_err=53826`、`ack_deferred=650121`；.252
      `1173`/`23403`），NIC（ens33）零丢包、dmesg 无 rxe 错误；客户端
      `Transport.cc` 告警均为**连接级**（storage 重启/连接关闭窗口 + 扫描期间的
      一次读写错误对），与 UUSE 后端错误无关。结论：异常样本源于 **Soft-RoCE
      重试/超时 + 客户端重试退避**，属实验室传输环境特性；正式 A/B 建议同机
      对照（含 TCP 口径）并记录 rxe 计数器。
    - TCP 口径对照（2026-10-05，客户端 `force_use_tcp=true`）：**8MiB 异常
      消失**（0.17s，精确）、16MiB 0.25s、32MiB 0.59s、64MiB 1.02s（47–64 MiB/s）
      ——确认此前 8MiB 11.4s 是 Soft-RoCE 抖动；但**尾部截断仍在**（32MiB→31、
      64MiB→60），且**两端均无写错误日志**（静默尾丢失）。结论：截断与传输
      无关，是客户端写流程/尾 chunk 处理问题（下一步：抓 FUSE 写返回字节数与
      末 chunk 的 update/commit/truncate 时序）。
    - 尾截断归因（2026-10-05）：
      - UUSE 链 + `dd conv=fsync` 32MiB：**完全正确**（0.53s / 64MB/s、尺寸精确、
        md5 一致）
      - UUSE 链 + `cp` 32MiB：`cp_rc=0` 但**静默截断到 28MiB**、md5 不一致
      - 原生链 + `cp` 32MiB：**完全正确**（尺寸精确、md5 一致）
      结论：截断是 **UUSE 后端与 FUSE `copy_file_range`/cp 写路径的交互问题**
      （dd 路径正常），最可疑为 copy 路径的短读/`lengthInfo` 语义；下一步抓 FUSE
      `copy_file_range` 的读写序列与返回字节数（或让 cp 走 `--sparse=never`/普通
      读写路径验证）。
    - writer 矩阵复测（2026-10-05，`isExtend` 映射补齐后）：结果**非确定性**——
      dd 1M/128K 本轮 FAIL（14MiB）、cat OK、cp FAIL（15MiB）；上一轮则相反。
      **后端始终零 update/commit 失败**。结论：静默尾丢失是**客户端侧时序/重试
      交互**（close/尺寸更新时在途写丢失，或客户端重试语义与我们的版本响应），
      不是后端错误。下一步：客户端 storage op 级调试日志 + storage 侧每 op 成功
      计时（定位丢失的具体 op）；或调客户端并发/超时参数复测。
    - op 级计数与 `lengthInfo` 修正（2026-10-05）：一次矩阵中服务端记录
      **66 update + 66 commit**（4 文件 × 16 chunk 全部到达），文件仍短 1MiB
      → **数据未丢，问题在客户端尺寸记账/响应语义**。同时发现 `lengthInfo`
      语义偏差：Rust `ChunkEngine` 普通写返回 `updateIO.length`（本次写长度），
      仅 truncate/extend 返回 chunk 长度，而 UUSE 后端曾一律返回 chunk 长度
      （客户端 split-IO 会累加该值）——已修正为同语义。修正后复测仍非确定性
      （dd_1m FAIL / cp OK 等），剩余因素需**客户端 op 级追踪**（FUSE 写返回
      字节数与 split-IO 合并路径）继续定位。
    - 聚焦复现（2026-10-05）：连续 3 次 16MiB dd（新文件）**全部精确通过**
      （26/51/62 MB/s）；说明尾丢失是**低频、多文件序列相关**的间歇事件，
      单文件隔离复现不出——需客户端写返回字节数/元数据尺寸更新的 op 级追踪
      （或提高客户端日志级别抓偶发样本）。当前已知：数据均到达服务端、
      `lengthInfo` 语义已修正、单文件与 E2E 场景（含崩溃恢复）均通过。
    - 客户端 op 级追踪尝试（2026-10-05）：矩阵挂起 30min+——根因是 **UUSE 链的
      .250 副本在 A/B 清理时被 offline**（`offlineUponUserRequest`），chain
      900100004 变 UNAVAILABLE；表 1 v28 路由到该链后新文件写入在客户端重试中
      挂起。已恢复（终止客户端进程、表 1 v29 回原两链、.251 恢复 systemd/设备）。
      **下次追踪前提**：先把 RF2 UUSE 链两个副本都恢复到 SERVING/UPTODATE
      （online 或新建一对 target），再路由新文件做 writer 矩阵 + 双端 op 计数。
    - 客户端 op 级追踪尝试 v2（2026-10-05）：用全新 target 对
      （101000100105/101000200105，chain 900100006）恢复双副本在线，但 `sed`
      引号失败导致 .251 仍加载旧 target（101000100104），新链只有 .250 一侧在线；
      表路由后客户端出现瞬时 meta 错误（dd `open: Remote I/O error`），矩阵未
      产出有效数据。已恢复（FUSE 硬重启、表 1 v31 回原两链、.251 systemd/设备）。
      结论：op 级追踪需**脚本化、无 shell 引号**的建链/切换流程（target id、
      chain id、表格上传全部写入脚本文件），并先验证双副本 SERVING/UPTODATE
      后再路由新文件。
    - op 级追踪 v3（2026-10-05）：脚本化建链成功（.251 的 UUSE target
      101000100105 加载到 chain 900100006），但 .254 在路由表格时 SSH 反复
      `ECONNRESET`，随后**失联（ping 不通）**——根因是 **manager VM 宕机/不可达**
      （FDB+mgmtd+meta），导致 FUSE 客户端挂载失败、admin 操作全部失败。其余 VM
      正常（.250/.251 storage active）。
      **恢复步骤**：启动 .254 并确认 FDB/mgmtd/meta active → 确认表 1 仍为原两链
      （v31）→ .252/.253 `systemctl reset-failed hf3fs_fuse_main &&
      systemctl start hf3fs_fuse_main` → 继续 op 级追踪（脚本化流程已就绪：
      `uuse251_target105.sh` + `uuse254_route_t.sh` + `uuse252_trace_matrix.sh`
      + 服务端 op 计数）。
    - op 级追踪 v4（2026-10-05，.254 重启后）：manager 恢复（FDB/mgmtd/meta
      active），客户端挂载自动恢复；但 .254 重启清空了 /tmp，路由脚本缺少
      `mkdir -p /tmp/gen` 而失败（表 1 未切换）；且 manager 宕机后集群处于降级态
      （各链 UNAVAILABLE/LASTSRV），矩阵对不可用链写入报 I/O 错误，未产出有效
      数据。已恢复：.250/.251 storage 重启重新注册、.251 清理回 systemd、原链
      回到 SYNCING→SERVING。
      **下一次一次性执行清单**：① 确认原链 SERVING；② `uuse251_target105.sh`
      （UUSE target 101000100105 → chain 900100006）；③ 修正后的
      `uuse254_route_t.sh`（已补 mkdir；路由表 1 → 900100006 并验证双副本
      SERVING/UPTODATE）；④ `uuse252_trace_matrix.sh` + 服务端 op 计数对照。
    - **op 级追踪执行（2026-10-06，干净环境重建后）**：环境（克隆机重建 + 集群
      重置 + 3FS/UUSE 重建构建）完成后按清单执行：① 原链 900100001/2 SERVING；
      ② .251 UUSE target 101000100105（链 900100003）；③ .250 侧 101000200105 +
      链 900100003 + 表 1 路由（v2）；④ writer 矩阵 + 服务端 op 计数。
      - **发现并修复：UUSE target 的 sync/resync 路径缺失导致 storage 崩溃**
        （SIGSEGV）：`StorageOperator::syncStart` → `StorageTarget::getAllMetadata`
        → 落到 `chunkStore_.getAllMetadata`（UUSE target 的 `chunkStore_` 从未
        加载）→ 空指针。同类缺口：`getAllMetadataMap`（ResyncWorker/DumpWorker）、
        `reportUnrecycledSize`（CheckWorker）、`addChunkSize`（chunk size 变更）、
        `migrate` 守卫。修复：为 UUSE 增加分支（`allChunks()` → `ChunkMeta`
        转换；size 报告/`addChunkSize` 直接返回；migrate 条件加
        `!use_uuse_engine`）。补丁
        `integrations/3fs/patches/apply-uuse-syncpaths.sh`（已并入一键安装器）。
        修复后链同步完成（`sync receive done ONLINE -> UPTODATE`）且不再崩溃。
        **意义**：旧环境出现的"storage 重启/连接关闭窗口"（客户端 Transport
        告警）与此崩溃吻合——UUSE 副本在 sync/resync 触发时崩溃会丢在途写，
        很可能就是"静默尾丢失"的重要成因。
      - **op 成功日志**（`apply-uuse-oplogs.sh`）：`uuse update ok` / `uuse commit
        ok` / `uuse read ok`（含 chunk/ver/len），用于客户端矩阵与服务端 op
        逐项对账。
      - **矩阵结果**：干净环境下 7 轮（1+6）writer 矩阵 **28/28 文件精确**
        （size 16MiB + md5 一致），服务端 op 守恒（update == commit，全部到达，
        含同步转发）；尾丢失**未复现**。对比旧环境（间歇性 15/14MiB 截断 +
        服务端 op 全达）——支持"尾丢失由环境降级/崩溃窗口引入"的判断。
        待续：更长批次复现（后台 20 轮）+ 若复现则用 op 日志逐 chunk 对账。
      - **slot 池耗尽复现与修复（2026-10-06）**：12 轮批次（768 chunk）在第 8 轮
        复现**经典尾丢失**：`cp` 16MiB → 15MiB（恰好少 1 chunk）+ I/O error；
        随后 dd 报错（同批次 12 轮为对照：修复后 48/48 全过）。根因：UUSE
        store 的固定 slot 池（4 文件 × 128 槽/类 = 512 槽）被打满——
        `update(isRemove)` 只把 chunk 标记 `REMOVAL_IN_PROGRESS` 并保留 slot，
        真正回收依赖 `punchHole()`，而 UUSE 的 `punchHole()` 是 no-op；同时
        meta 的 chunk 回收流滞后数分钟，持续写入下必然打满。
        修复（`UuseChunkStore.{h,cc}` + `apply-uuse-syncpaths.sh`）：
        ① 实现 `punchHole()`/`recycleRemovedLocked()`（删除 REMOVAL_IN_PROGRESS
        的 chunk、journal kOpDelete、释放 slot；`recycleState` 持久化到 journal
        `reserved2`，重启后可继续回收）；② 分配失败时**内联回收重试**；
        ③ slot 池扩容并改为按类布局：`kClassFiles = {8, 8, 24, 8}`
        （64K/512K/1M/4M 四类），1MiB 类（3FS 默认 chunk 尺寸）**3072 槽**，
        总 6144 槽；总预留比原 4×16 布局更小。
        修复后：12 轮 48/48 全过；24 轮在累计 2048 chunk 边界（第 21 轮）出现
        一次 dd 失败后**自动恢复**（22–24 轮全过）；60 轮 238/240（单窗口 2
        失败后自动恢复）；按类布局后 **80 轮 320/320 全过（5.1GB 持续写入、
        0 失败）**——残余 `no free slot` 重试仅 56 行且被客户端重试吸收。
        边界行为可预期：单波写入 < ~3GB 时稳定。
      - **客户端静默行为确认**：store 打满时，`echo` 小写入返回 rc=0 但文件为
        0 字节（**完全静默**）；`cp` 截断时有时报 I/O error（rc=1）、有时静默
        （旧环境 cp rc=0 截断 28MiB）。即后端 update 失败时，客户端错误上抛
        不一致——"静默尾丢失" = 后端失败 + 客户端静默路径的组合。修复 slot
        耗尽即消除该触发源；客户端错误上抛问题属于 3FS 客户端行为，另行记录。
      - **UUSE vs 原生 双副本双客户端 A/B（2026-10-06，TCP 口径）**：
        配置：原生链 900100001 [HEAD .250 stock + TAIL .251 stock] vs UUSE 链
        900100003 [HEAD .250 stock + TAIL .251 UUSE]——同 HEAD 节点、同副本分布，
        唯一差异为 .251 后端（XFS chunk_engine vs UUSE raw-NVMe）；客户端
        .252/.253 并发同跑（每客户端 512MiB 顺序写 conv=fsync + drop-cache
        顺序读），原生/UUSE 交叉 2 轮。
        结果（每客户端 MiB/s）：
        - 原生：写 21.1/20.7/22.1/21.9（均 21.4），读 22.0/21.8/22.2/22.2（均 22.1）
        - UUSE：写 21.7/21.8/21.6/22.0（均 21.8），读 21.8/21.9/21.4/21.4（均 21.6）
        结论：双客户端并发 + RF2 下**写/读均持平（差异 <3%，噪声内）**，聚合
        写 ~43 MiB/s、读 ~43 MiB/s；服务端 op 守恒（update == commit），无
        slot 耗尽。对比单客户端旧 A/B（写持平 ~60MiB/s、读 16.5 vs 21 MiB/s）：
        本轮读路径持平；写随并发上升而单客户端下降属容量/竞争所致。
      - **性能矩阵：顺序/随机 × 读/写 × 32/64 并发（2026-10-06，双客户端并发，
        TCP 口径，交叉读）**：原生链 900100004 [.251 stock HEAD + .250 stock
        TAIL] vs UUSE 链 900100003 [.251 UUSE + .250 stock]；客户端 .252/.253
        同时运行相同负载（每客户端 64 文件 × 16MiB；顺序 1MiB / 随机 4KiB；
        20s/格；读为交叉读对方文件 + drop_caches）。聚合吞吐（双客户端）：

        | 单元格 | 原生 | UUSE |
        |---|---|---|
        | 顺序写 32 | 47.7 MiB/s | 40.9 MiB/s |
        | 顺序写 64 | 42.9 | 47.3 |
        | 顺序读 64 | 23.6 | 22.3 |
        | 随机读 4K 32 | 753 IOPS | 737 IOPS |
        | 随机读 4K 64 | 718 | 682 |
        | 随机写 4K 32 | 198 | 205 |
        | 随机写 4K 64 | 193 | 197 |

        （顺序读 32 并发受客户端页缓存影响，不计入；单格客户端间波动 ±40%，
        聚合值更稳定。）
        **结论：UUSE 与原生在全部维度持平（差异 <10%，噪声内）**。
      - **聚焦 A/B 复核（2026-10-06，64 并发 × 2 轮交替，双客户端）**：矩阵中
        读/写的方向性小差异经复核为**噪声**——读（随机 4K / 顺序 1M 64 并发）：
        原生 691/688 IOPS、21.4/21.4 MiB/s vs UUSE 691/683 IOPS、21.5/21.1 MiB/s；
        写（顺序 1M / 随机 4K 64 并发）：原生 45.3/43.3 MiB/s、196/196 IOPS vs
        UUSE 44.7/45.9 MiB/s、197/194 IOPS。**两配置在全部维度等价（≤2%）**；
        矩阵单格差异主要来自客户端间分配波动（±40%）。注：UUSE 数据面在
        storage 内串行于单一 I/O 线程（同步 API 非线程安全），当前瓶颈在传输
        层（~45 MiB/s）未显现该上限；若换更快介质/网络需关注串行化上限。
      - **异步优化：worker-direct 快路径（2026-10-06）**：按 uuse.md §20.4 /
        Shard-106 落地——数据面从"单 I/O 线程 + 全局锁内 I/O"改为
        **per-thread client 池**（owner client 持文件；调用线程懒建自己的
        client，`submitRead/submitWrite` 后本线程 `poll`）；`update` 重构为
        三阶段（锁内规划 → **锁外设备 I/O** → 锁内 journal+发布）；submit
        失败/前端受限时回落同步门面。新增 `bench_mt`（多线程读扩展探针）。
        验证：① 正确性 SMOKE1/2_PASS（含 journal 重放）；② 本地并发探针：
        单线程 1MiB 读 4.5 GB/s（~219µs/op，设备上限），16 线程交错 2.1 GB/s；
        ③ 3FS 级修正 A/B（新链、每配置先写新文件再交叉读，64 并发双客户端）：
        写 44.2 vs 41.2 MiB/s、顺序读 20.7 vs 20.5、随机读 683 vs 480 IOPS
        （注意：两链 HEAD 不同节点且客户端读路由只落到 stock 侧——UUSE 侧
        `read_ok=0`，读差异属节点/环境；UUSE 读路径的 3FS 级验证需先将 UUSE
        副本设为 preferred/HEAD）。结论：异步化后写与本地并发能力达标、
        无回归；UUSE 副本作为读服务方的 3FS 级验证留待后续（受读路由控制）。
      - **sync_when_write 对照（2026-10-06，假设否证）**：把两台存储的
        `sync_when_write` 从 true 改为 false（关掉每次 chunk 元数据的 KV 同步
        写/fsync）后复测写路径（64 并发双客户端聚合）：原生 seqW 103.2 vs
        102.5、randW 302 vs 310；UUSE seqW 96.9 vs 102.1、randW 199 vs 310
        （后两者在噪声/漂移内）。**结论：per-op fsync 不是写吞吐上限的来源**。
        写上限（~50 MiB/s/客户端）更符合**更新流水线的 in-flight 深度 × 单 op
        延迟**模型（客户端 channel 深度/引擎提交批处理），与"32≈64 并发持平、
        单流即饱和"一致。配置已恢复 true。
        （该假设已于 2026-10-07 定位实验否证：真实上限是 FUSE 客户端被钳制为
        **单 worker 线程**，见下条。）
      - **高并发环境修复（2026-10-06，矩阵跑通前提）**：32/64 并发下
        Soft-RoCE 转发饱和 → 通道锁死/超时 → 写路径整体损坏（EIO/挂起）。
        修复：① 存储侧 `force_use_tcp = true`（storage_main.toml +
        storage_main_launcher.toml，转发走 TCP）；② 客户端 `max_connections`
        1→8（hf3fs_fuse_main.toml + launcher）；③ 卡死的会话/通道状态必须
        **全集群重置（清 FDB + 重建 targets/chains）+ 两台存储重启**才能清除
        （单独重启存储或 mgmtd 均无效）。修复后 32/64 全矩阵零错误跑通。
        - **写吞吐上限定位（2026-10-07，§5 待办 1 完成）**：现象——单客户端
          顺序写 1MiB 恒定 **~35–38 MiB/s（≈35–38 ops/s）**，1/8/32/64 并发
          完全持平；单 op 墙钟 ~26–29ms；双客户端聚合 ~54 MiB/s（~1.5×，之后
          受共享更新流水线约束）。定位与证据：
          - **根因：FUSE 客户端 worker 线程数被钳制为 1**。
            `src/fuse/FuseClients.cc`：`maxThreads = std::min(max_threads,
            (logicalCores + 1) / 2)`；客户端 VM 为 2 vCPU → `(2+1)/2 = 1`。
            实测（`ps -L`）：客户端进程仅有 **1 个 `fuse_worker` 线程**（32 流
            1MiB 负载下仍为 1）。因此写回调串行：吞吐 = 1/单 op 延迟；应用层
            并发只增加排队延迟（fio n=32 clat ~0.85–0.98s ≈ 32×单 op），不提升
            吞吐；每多一个客户端多 1 个 worker（双客户端 ~1.5–2×）。
          - **客户端指标（log reporter 实测，32 流 1MiB）**：
            `storage_client.concurrent_user_calls` 与 `inflight_requests`
            max=1；`fuse.write.latency` ~29ms = `fuse.piov.prepare` ~7ms +
            `submit` ~27ms；`request_latency` ~26ms 中 `server_latency` ~24ms、
            `network_latency` ~1.2ms。对照：4KiB 顺序写（写缓冲合并）可达
            ~8.5k IOPS，但每 256 次写触发一次 1MiB flush，flush 速率仍是
            ~33/s——串行点是 1MiB flush 的写 RPC，不是 FUSE 回调数量。
          - **客户端 update channel 池不是上限**：`chanAllocator_ =
            traffic_control.max_concurrent_updates() = (32×128)×3 = 12288`
            （部署配置），且每 op 完成后释放；在途 channel 远低于该值。
          - **chunk engine 无定时器提交批处理**：存储侧写路径是
            `UpdateWorker`（`num_threads=4`）逐 job 执行 `update`/`commit`；
            Rust 引擎 `commit_chunk(sync=config)` 无周期批处理；UUSE 头
            update→commit 跨度 p50 ~11.5–17ms（含向 TAIL 转发的往返）。
            native 与 UUSE 两条链的客户端吞吐完全一致（~37 ops/s），证明引擎与
            store 不是上限。
          - **传输口径修正**：客户端与存储的**生效配置来自 mgmtd**
            （`app_detail::initConfig` → `launcher_->loadConfigTemplate()`），
            对 `/opt/3fs/etc/hf3fs_fuse_main.toml` / `storage_main.toml` 的
            `force_use_tcp=true`、`max_connections=8` 编辑**从未生效**；实测
            生效值一直是 `force_use_tcp=false, max_connections=1`
            （RDMA/Soft-RoCE）。本次用 `admin_cli get-config/set-config`
            真正切到 TCP 后，非 inline 写全部报 `update no RDMA socket`
            （1MiB 数据面需要 RDMA READ/WRITE）；再把
            `max_inline_write_bytes/max_inline_read_bytes='2MB'` 走 inline-TCP
            后吞吐仍 ~31–32 ops/s——证明传输也不是上限。Soft-RoCE 实测
            `ib_send_bw` 平均 ~0.51 Gb/s（64KiB，峰值 0.99），与写路径同量级
            但非主因。**此前记录的"TCP 口径"应读作 RDMA/Soft-RoCE 口径**。
          - **结论**：写吞吐上限 = 单 worker FUSE 客户端串行处理 × 单 op
            延迟（~26–29ms）。下一步验证/修复：提高 FUSE worker 数（改
            `FuseClients.cc` 的钳制公式，或把客户端 VM 提到 ≥4 vCPU）；
            预期 2 worker ≈ 2×（受 2 vCPU 与共享更新流水线 ~55–65 ops/s 约束）。
            环境已恢复（FUSE 配置 = 原始模板、路由表 1 → 900100005、测试文件
            清理、残留 ib 测试进程清理）。
        - **4 vCPU 复测与存储侧阻塞（2026-10-07）**：两台客户端 VM 升到 4 vCPU
          后 FUSE worker 由 1 → **2**（`(4+1)/2`，`ps -L` 实测）。单客户端
          UUSE 链复测（1MiB，两次独立运行）：io_driver n=8/32 **42.0 MiB/s**、
          fio n=32 **43.0 MiB/s**（2 vCPU/1 worker 时为 37.4），单流 ~29–30；
          即 2 worker 的高并发收益为 **+12~15%**。当日环境整体偏慢 ~15–20%
          （头侧 update→commit p50 11.5→18.2ms、dd 单 op 55→65–80ms），
          归一化后收益应更大，但本次无法厘清。
          双客户端 × 2 worker（每链至多 4 并发 1MiB）在 UUSE 链和原生链上
          **4 次尝试全部失败**（客户端 EIO + `Channel is locked` 风暴）：
          ① **UUSE 链——删除非幂等 bug**：`uuse commit failed: chunk ...:
             uuse chunk not found` 当日 291 次，单 chunk 被 meta GC 重试 300+
             次（持续数分钟）。路径：`UuseChunkStore::update` 对
             `isRemove` 且 chunk 不存在时返回成功，但随后的 `commit` 对已被
             `recycleRemovedLocked` 回收的 chunk 返回 `kChunkMetadataNotFound`
             → GC 无限重试 → 通道锁风暴 → 客户端 EIO。修法：commit 在未找到
             时对 remove 语义按成功返回，或保留 tombstone。
          ② **原生链**：4 并发下 HEAD→TAIL 出现 `forward failed`（1s 级重试，
             与同进程 UUSE 侧 GC 重试风暴/Soft-RoCE 转发抖动叠加）。
          结论：写并发上限当前由**存储侧稳定性**决定（实测稳定 ≤2–3 并发/
          链），FUSE worker 数不再是唯一瓶颈；先修 ①/② 再复测双客户端
          2-worker 聚合。测试后已恢复（两存储 + 两客户端重启、链 SERVING、
          路由表 1 → 900100005、测试文件清理）。
        - **修复与复测（2026-10-07 晚，§5 待办 2 完成）**：
          ① **已修复 UUSE 删除幂等性**：`UuseChunkStore::commit` 对不存在的
          chunk 改为告警并按成功返回（与 stock 引擎语义一致；源码
          `integrations/3fs/uuse/UuseChunkStore.{h,cc}`，增量重建
          `storage_main` 部署到 .251）。实测：GC 删除触发
          `uuse commit: chunk ... already gone, treat as success` WARN，
          `uuse commit failed` / `Channel is locked` 归零，store 用量正常
          回收（2.25 → 1.24 → 0.18 GiB）。
          ② 剩余的通道锁问题（RDMA 传输错误后 HEAD 转发重试撞 TAIL 通道锁，
          持锁协程不释放；gdb 显示 update worker/UUSE 反应堆/IO 线程均空闲）
          以**客户端 inline-TCP 数据面**绕开：FUSE 生效配置
          `force_use_tcp=true` + `max_inline_write_bytes/max_inline_read_bytes
          ='2MB'`（mgmtd ConfigVersion 7）。1MiB 写全部 inline，链上转发随
          inline 标志传递，链路上不再有 1MiB 的 RDMA READ 数据搬运。
          **复测结果**（1MiB 顺序写，双客户端 32 作业同刻启动）：

          | 配置 | 单客户端 | 双客户端聚合 |
          |---|---|---|
          | 2 vCPU / 1 worker，RDMA | 37.4 MiB/s | 53.6（原生链） |
          | 4 vCPU / 2 workers，RDMA | 42–43 | 未跑通（通道锁/转发失败） |
          | 4 vCPU / 2 workers，RDMA（幂等修复后复测） | **33.9–35.2** | 未跑通（.253 卡死 + 通道锁风暴；prep 触容量边界 EIO） |
          | 4 vCPU / 2 workers，inline-TCP | **47.8–48.0**（UUSE） | **63.9（UUSE）/ 60.0（原生）** |

          **结论**：4 vCPU + inline-TCP 下双客户端首次稳定跑通，聚合
          ~60–64 MiB/s（≈ 链的 ~64 ops/s 流水线上限，**客户端不再是瓶颈**）；
          UUSE 与原生持平。Soft-RoCE 复测（幂等修复后）：单客户端
          33.9–35.2 MiB/s（低于 inline-TCP 的 47.8–48.0），双客户端仍在
          .253 卡死 + 通道锁——**RDMA 数据面在双客户端 2-worker 下仍不稳定**，
          inline-TCP 是本实验环境目前唯一稳定且更快的写配置。遗留：UUSE 链
          prep 在 store 接近 3 GiB 槽池容量时仍会出现 `no free slot`-类 EIO
          （容量边界，非传输）；单流仍受单 op 延迟（~37ms）限制。当前 FUSE
          生效配置 = inline-TCP；原始 RDMA 配置备份在 .254
          `/tmp/fuse_cfg_orig.toml`，恢复方式：`admin_cli set-config -t FUSE -f`
          `<file>` + 重启客户端。
        - **双客户端 RF2 正式 A/B（2026-10-07，4 vCPU + inline-TCP）**：链路
          900100004（.251 stock + .250 stock）vs 900100005（.251 UUSE + .250
          stock），双客户端同刻 32 作业、两轮交错（N/U/N/U）；1MiB 顺序写 20s、
          4K 随机写 15s、`drop_caches` 后**单遍 1MiB 交叉冷读**：

          | 维度（双客户端聚合） | 原生 | UUSE |
          |---|---|---|
          | 顺序写 1M 第 1/2 轮 | 65.1 / 66.9 MiB/s | **67.6 / 67.9 MiB/s** |
          | 4K 随机写 第 1/2 轮 | 286 / 246 IOPS | **317 / 316 IOPS** |
          | 冷读 1M 单遍 | 151.0 MiB/s | **153.6 / 152.3 MiB/s** |

          结论：UUSE 全维度 ≥ 原生（差异在噪声内；随机写 UUSE 略高，需更长
          采样确认）；聚合顺序写 ~65–68 MiB/s ≈ 链 ~64 ops/s 流水线上限。
          **读路径首次由 UUSE 实际服务**：`uuse read ok` 每轮 2020 次（此前为
          0）——UUSE 作为 HEAD + inline-TCP 时读落到 UUSE store，冷读
          ~77 MiB/s/客户端。注意读口径必须用 drop_caches + **单遍**冷读：
          io_driver 的循环读第二遍起被客户端页缓存放大（曾出现 6.5 GB/s 假象）。
        - **环境重建 + UUSE 侧收尾项验证（2026-10-08）**：VM 重启后 .250/.251
          数据盘（NVME_0000/0001）被清空 → 按恢复手册重建：重格式化 data1/build、
          .251 重新克隆 3FS@22fca04 + 补丁 + 构建（含本日改动）、FDB 清库重建、
          cluster reset + 重建 targets/chains、客户端同步 token。借此验证
          "不改 3FS 源码"的收尾项：
          ① **槽池动态扩容**（`allocateSlotWithRecycle` + `growClass`）：写
          3.5GiB（3584×1MiB）触发自动新建 5 个 class-2 槽文件（file 48–52，各
          128MiB），无 `no free slot`/EIO（61 MiB/s）；重启 .251 storage 后
          目录扫描恢复动态文件（0 个 unknown slot file），UsedSize
          3,758,096,387 前后一致。
          ② **UUSE 读路径复验**：preferred order 指向 UUSE 后 112MiB 文件
          md5 精确（1521 次 `uuse read ok`）。
          ③ `no free slot` → `StorageClientCode::kNoSpace`（客户端可见
          no-space 语义而非 EIO）。
          ④ update/recycle 竞态加固：phase-1 对 pending-removal chunk 立即
          标记 NORMAL，避免 phase-2 期间 punch-hole 回收其 slot。
          剩余仅 3FS 核心改动项（RDMA 通道锁根因、FUSE worker 公式、纯 TCP
          数据面等，见 handover §5.7）。

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
- **克隆机恢复手册（2026-10-06）**：manager 克隆 → storage 节点标准流程与防复发
  措施：① 确认 MAC 唯一、IP 正确；② `systemd-machine-id-setup` 重置 machine-id；
  ③ 停用/禁用 FDB+mgmtd+meta；④ NVME_0000 格式化 XFS `data1`，fstab 条目必须带
  `nofail,x-systemd.device-timeout=15s`（数据盘缺失/延迟时不再阻塞启动——这是
  克隆机出现 1min30s 启动等待的根因）；⑤ 配 node_id（.251=10001/.250=10002）+
  mgmtd 地址 + target_paths + systemd；⑥ 集群重置 + 建 target/chain；
  ⑦ 客户端同步 token + 重挂载；⑧ **chain 创建后重启 storage 服务**刷新 routing
  （否则客户端写读报 Remote I/O error）。
  脚本：`uuse_clone_to_storage.sh`、`uuse_cluster_reset.sh`、
  `uuse_targets_chains_all.sh`、`uuse_fuse_restart.sh`（均在 `temp_scripts/`）。
- **重建构建备忘（2026-10-06，CMake 4.2）**：Arrow 的 bundled 依赖（xsimd/lz4/
  zlib 等）`cmake_minimum_required` 过旧会被 CMake 4.2 拒绝——整行规范化为
  `VERSION 3.5`，且构建时导出 `CMAKE_POLICY_VERSION_MINIMUM=3.5`（环境变量对
  嵌套 configure 生效，已实测）；`uuse_chunkstore_smoke` 目标构建需
  `CC=gcc-12 CXX=g++-12`（否则 cargo 指纹失配触发 Rust 重编并失败）。
