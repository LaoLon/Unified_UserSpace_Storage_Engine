# HFS 2.0 前端 zero-copy DMA 设计评审（P2-1，2026-09-29）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

> 评审对象：`doc/review/hfs2_architecture_optimization_review_20260928.md` §5 P2-1。
> 结论：**采纳"前端池内存由 HFS arena 提供"方案**（RocksDB opt-in + MySQL 默认开启），
> 拒绝"每请求注册用户内存"和"RocksDB kFSBuffer"；核心不新增 public ABI。
> 本评审通过后按 §4 更新 `doc/design/hfs2.0.md` §11.2/§20 并开始实现。

## 1. 问题与证据

当前前端 buffer 不是 arena 内存，`acquireLeases` 中
`directCapable = arena_.ownsRange(...) && backend->isBufferDmaCapable(...)` 恒为 false，
所有 RocksDB/MySQL 设备读都走 arena bounce 再 copy-out：

- `rocksdb_readrandom_rootcause_20260922.md` §16.3：`read_direct_user_children=0`、
  全部 `read_bounce_children`、copy-out ≈4.1 µs/read（含 trace 开销）。
- 2026-09-29 8j4 trace：bdev 段占 p50 84%，软件段约 10%；同形状 1j32 相对 raw
  的剩余差距 9.3%。
- bounce 成本 = 每 child 的 arena acquire/release（全局 mutex + 有序索引）+
  两次 memcpy（读：设备→arena→user；写：user→arena→设备）+ arena 预算占用。

## 2. 方案评估

| 方案 | 结论 | 理由 |
|---|---|---|
| A. 每请求 `spdk_mem_register` 用户 buffer | 拒绝 | 注册/注销成本高于拷贝（页表/IOMMU 映射更新 + 锁）；buffer 生命周期与取消/迟到完成竞态不可控 |
| B. RocksDB `kFSBuffer` + buffer allocator | 拒绝 | 该契约只适用于 non-direct MultiRead；当前读路径是 `use_direct_io()==true`，不覆盖 |
| C. 前端长期池由 HFS arena 提供 | **采纳** | `ownsRange`/`spdk_vtophys` 自然成立；无每请求注册；预算可计入 `dma_arena_mb` |
| D. 放宽 `ownsRange` 为仅 `vtophys` | 拒绝 | malloc 的 InnoDB/RocksDB 内存本就不可 vtophys，不解决根因；且失去 arena 生命周期保证 |

## 3. 契约（拟加入设计文档）

### 3.1 核心侧（不新增 public API）

- 复用 `Engine::acquireDmaBuffer`/`releaseDmaBuffer`；前端池按需从同一 arena 分配，
  计入 `dma_arena_mb` 上限与 hugepage preflight；
- 释放按指针查找记录大小，调用方给出的 size 仅用于诊断；
- arena 析构会释放所有仍 live 的分配；前端必须在 engine stop 前归还（否则按
  bounded 泄漏计入诊断，不 double free）。

### 3.2 MySQL（默认开启）

- seam 增加 `hfs_vfs::allocDmaBuffer(size)` / `freeDmaBuffer(ptr)`，包装 engine arena；
- `buf_pool_t::allocate_chunk` 优先向 arena 申请整块 chunk，失败（bridge 未激活、
  预算不足、非 hugepage 环境）回落到 `ut::malloc_large_page_withkey`；
- `deallocate_chunk` 对 HFS-backed chunk 走 `freeDmaBuffer`，并跳过
  `madvise_dont_dump`/`madvise_dump` 与 `srv_numa_interleave` 的
  `ut::large_page_low_level_info` 路径（该路径只对 ut 自己的分配有效）；
- `config/hfs_mysql.conf` 的 `dma_arena_mb` 必须覆盖 buffer pool + 在途 bounce；
  当前默认 buffer pool 128 MiB、arena 512 MiB，满足；preflight 需按总和校验；
- 验收：`read_direct_user_children > 0`、copy-out 字节数归零或显著下降、
  `hfsctl check` `corruption=none`、MySQL 功能/恢复矩阵不回退。

### 3.3 RocksDB（opt-in）

RocksDB 9.11.2 `BlockFetcher` 源码核对结论（2026-09-29）：

- direct 读（`file_->use_direct_io()==true`）由 `file_->Read(..., scratch=nullptr,
  aligned_buf=&direct_io_buf_)` 读入 RocksDB 自分配的 `AlignedBuf`；`memory_allocator`
  只用于读后的 `heap_buf_`/`compressed_buf_`，**不能改变设备路径的 bounce**，因此
  "block cache allocator 即 zero-copy"不成立；
- 唯一可行路径是声明 `FSSupportedOps::kFSBuffer` 且读文件 `use_direct_io()==false`：
  `BlockFetcher` 走 `MultiRead`，由 adapter 用 arena 分配 `fs_scratch`（`FSBufferAllocator`
  内部提供），设备直接读入 arena；`GetBlockContents()` 仍会做一次
  `fs_scratch→heap_buf_` 的内部 memcpy（`BlockContents` 所有权），收益为
  去掉 HFS bounce + copy-out；
- 因此 RocksDB 侧不按本节原方案实现，改为 **Shard-111 设计项**：先修订
  `hfs2.0.md` §11.2 的读路径契约（kFSBuffer + 非 direct 读 + arena `fs_scratch`），
  评审通过后再实现；在此之前保持现状（direct 读 + bounce），不得声称 RocksDB
  zero-copy。

## 4. 实施计划

1. **Shard-110（MySQL，已实施 2026-09-29）**：seam `allocDmaBuffer/freeDmaBuffer` +
   `buf_pool` chunk hook + `madvise/NUMA` 跳过 + 诊断计数；
   patch = `mysql/patches/hfs_seam_shard110_zero_copy.patch`，构建入口 marker 已加入
   `build_mysql_vm.sh`，`config/hfs_mysql.conf` 标注 `dma_arena_mb` 预算含义。
   验收证据（功能计数、native/HFS 与 pre/post 同会话 A/B、崩溃矩阵）见
   `doc/performance/hfs2_mysql_zero_copy_20260929.md`：`read_direct_user_children`
   917–1308、copy-out 显著下降、`hfsctl check corruption=none`、
   pre/post 吞吐在噪声内（读约 −1.3%、读写约 +1.1%）。
2. **Shard-111（RocksDB）**：按 §3.3 先修订设计（kFSBuffer + 非 direct 读 +
   arena `fs_scratch`），评审通过后实现 `MultiRead` 的 arena scratch 与
   `readrandom` A/B；本轮不实现。
3. 两者都必须通过 RocksDB L4 无回退门禁与 MySQL 功能/恢复矩阵（§16.2/§16.3/§20.7）。

## 5. 风险与缓解

| 风险 | 缓解 |
|---|---|
| arena 预算被前端池占满导致数据路径 Busy | preflight 按 pool+in-flight 校验；busy 计数与日志；`dma_arena_mb` 显式配置 |
| MySQL shutdown 顺序（pool 释放晚于 engine stop） | seam 检查 bridge 状态；arena 析构兜底释放；fail-closed 诊断 |
| HFS chunk 上的 madvise/NUMA 路径 UB | patch 对 HFS-backed chunk 跳过这些路径 |
| NUMA：pool 与 reactor/device 不同 node | 当前单 node VM；多 node 留待 NUMA gate（设计 §15.1/§20 边界） |
| RocksDB allocator 未覆盖 direct 读 | §3.3 边界声明，不作为性能结论 |

## 6. 参考

- `doc/review/hfs2_architecture_optimization_review_20260928.md` §5 P2-1
- `doc/performance/hfs2_mysql_zero_copy_20260929.md`（Shard-110 验收证据）
- `doc/performance/rocksdb_readrandom_rootcause_20260922.md` §16.3
- `doc/performance/hfs2_8j4_shape_attribution_20260929.md`
- `doc/design/hfs2.0.md` §8.1/§11.2/§20