# HFS 2.0 MySQL 前端 zero-copy（Shard-110）实施与验收（2026-09-29）

> 设计依据：`doc/design/hfs2.0.md` §20.8、`doc/review/hfs2_zero_copy_frontend_review_20260929.md`。
> 实现：`mysql/patches/hfs_seam_shard110_zero_copy.patch`（seam `allocDmaBuffer/
> freeDmaBuffer` + `buf_pool_t::allocate_chunk/deallocate_chunk` 钩子 + `buf_chunk_t`
> HFS 标记 + `madvise/NUMA` 路径跳过 + `mem_size()` 覆盖）。
> 构建入口：`mysql/build_mysql_vm.sh` 新增 shard-110 marker。

## 1. 机制

InnoDB buffer pool chunk（默认 128 MiB，1 instance 时整块）优先通过
`hfs_vfs::allocDmaBuffer` 从 HFS DMA arena 分配：

- arena 内存是 `spdk_dma_zmalloc` hugepage 内存，`DmaArena::ownsRange` 与
  `spdk_vtophys` 直接成立；
- 数据路径因此把 InnoDB 页 buffer 判定为 `directCapable`，设备读直接落到
  buffer pool 页，写直接取自页，不再经过 arena lease + copy-in/copy-out；
- arena 预算不足 / bridge 未激活时回落 `ut::malloc_large_page_withkey`，行为不变；
- HFS chunk 跳过 `madvise_dont_dump/dump` 与 `srv_numa_interleave` 的
  `ut::large_page_low_level_info`（该路径只对 ut 自身分配有效）。

`config/hfs_mysql.conf` 的 `dma_arena_mb = 512` 覆盖 128 MiB buffer pool + 在途
bounce；hugepage preflight 打印 `required 368, available 1024`。

## 2. 功能证据（stats=true，fresh HFS datadir）

`--initialize-insecure` 建库后运行 8 线程 sysbench OLTP read_only 15 s +
read_write 10 s，`mysqladmin shutdown` 后 seam 打印：

```
hfs_vfs: device_io read_ops=13798 read_bytes=30895616 write_ops=175645 write_bytes=615721984
hfs_vfs: zero_copy read_direct_user_children=917 read_bounce_children=50
         read_copy_out_ops=50 read_copy_out_bytes=9299058
```

- `read_direct_user_children=917` 证明 buffer pool 页读走了 zero-copy 直通；
- bounce 仅 50 次、copy-out 9.3 MiB（redo/undo/doublewrite 等非 buffer pool
  buffer 仍走原路径）；
- 无 `dma pool allocation failed` 输出，即 pool 由 arena 服务；
- `CHECK TABLE hfs_perf.sbtest1` → `OK`；`Innodb_buffer_pool_pages_data=1824`。

## 3. 同会话 A/B：native vs HFS（stats=false）

同一二进制、HFS/native 交替 4 轮，HFS datadir 在独占 NVMe，native datadir 在
VM 根盘 ext4（与 2026-09-25 基线同口径）：

| 顺序 | 读 TPS | 读 p99 ms | 读写 TPS | 读写 p99 ms | mysqlslap 读/写 s |
|---|---:|---:|---:|---:|---:|
| HFS 1 | 9384.13 | 2.43 | 2357.61 | 6.91 | 0.073 / 10.464 |
| native 1 | 10837.72 | 1.79 | 2050.54 | 8.28 | 0.061 / 10.772 |
| HFS 2 | 10315.92 | 1.93 | 2422.45 | 6.43 | 0.060 / 10.331 |
| native 2 | 10762.36 | 1.86 | 2140.07 | 8.58 | 0.067 / 10.765 |

比值（HFS/native）：读 86.6% / 95.8%（均值约 91.2%），读写 115.0% / 113.2%。
2026-09-25 基线同口径为读约 94.2%、读写约 77.2%。**跨会话绝对值不可比**，此处仅
记录本会话比值；读写比值的上升在本会话无法归因为 shard-110（见 §4）。

## 4. 同会话 pre/post A/B（关键对照）

为把 shard-110 的效果与环境和会话漂移分离，用同一天构建的
`/home/hfs/mysqld-pre110`（无 shard-110）与 `/home/hfs/mysqld-post110`（含
shard-110）在**同一 HFS datadir** 上交替 4 轮（stats=true，两轮各 15 s/10 s）：

| 顺序 | 读 TPS | 读 p99 ms | 读写 TPS | 读写 p99 ms | mysqlslap 读/写 s |
|---|---:|---:|---:|---:|---:|
| pre 1 | 11343.77 | 1.73 | 2408.84 | 7.04 | 0.072 / 10.487 |
| post 1 | 11400.50 | 1.73 | 2447.39 | 6.43 | 0.065 / 10.453 |
| pre 2 | 10992.57 | 1.79 | 2453.89 | 6.32 | 0.070 / 10.318 |
| post 2 | 10653.90 | 1.86 | 2470.60 | 6.43 | 0.075 / 10.189 |

- 读 TPS 变化 +0.5% / −3.1%（均值约 −1.3%），读写 +1.6% / +0.7%（均值约 +1.1%），
  mysqlslap 写 −0.3% / −1.2%：全部落在会话噪声内，**未观察到吞吐回退**；
- 该 workload 的读绝大多数命中 InnoDB buffer pool（2.8M 查询中设备读仅千级），
  因此设备路径拷贝消除在该形状下不可见；
- post 轮计数器：`read_direct_user_children=1308/1223`、
  `read_bounce_children=5853/5945`、`read_copy_out_bytes≈57 MiB`——本 workload
  的 bounce 主要来自 redo/doublewrite/undo 等非 buffer pool buffer；
- 同会话的 HFS 读写 TPS（约 2.4k）与 §3 的 HFS 读写（约 2.39k）一致，说明 §3 中
  相对 09-25 基线的读写比值变化来自会话/环境漂移，而非 shard-110。

## 5. 崩溃恢复矩阵

用 post-110 二进制重跑 Shard-104 崩溃矩阵（同一 HFS datadir，
`run_shard104_crash_vm.sh`；为新建 datadir 增加
`CREATE DATABASE IF NOT EXISTS hfs_matrix`）：

- 在线 DDL（`ADD INDEX ... ALGORITHM=INPLACE, LOCK=NONE`）中途 SIGKILL：
  恢复后 `COUNT(*)=131072`、`CHECK TABLE ... OK`、可继续写入；
- 页更新中途 SIGKILL（doublewrite 恢复）：同样行数与 `CHECK TABLE OK`；
- 两次 `hfsctl check` 均 `corruption=none`（checkpointA/B valid，
  journal head=tail，usage=0.0%）；
- 结论：buffer pool 直通设备不改变崩溃恢复语义。

## 6. 结论与边界

- Shard-110 在功能上生效：buffer pool 页读直通设备，copy-out 显著下降；
- 在 8 线程、buffer pool 高命中的 sysbench 形状下无吞吐收益/回退（噪声内）；
  收益上限取决于设备读比例，需在冷缓存或大表扫描形状下单独度量；
- 边界：HFS chunk 的 `madvise`/NUMA interleave 被跳过（当前单 NUMA 验证口径）；
  arena 预算必须覆盖 buffer pool（配置注释已标注）；InnoDB shutdown 必须在
  engine stop 前归还 chunk（`srv_shutdown` 先于 `hfs_vfs::shutdown`，已验证）；
- RocksDB 侧未实现（direct 读不经过 `memory_allocator`，需 kFSBuffer 读路径切换，
  见设计 §11.2 与评审 §3.3，属 Shard-111 设计项）。
