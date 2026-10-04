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
