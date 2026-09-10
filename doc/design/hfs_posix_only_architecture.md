# HFS POSIX-only 架构收敛

> 状态：已实施，2026-09-10

## 决策

HFS 最终只保留完整 POSIX 语义路径。对外的 `hfs_lib_async_*`、
`hfsAsync*`、registered-DMA 和显式 DMA 分配 ABI 不再作为 HFS 功能提供。
FIO 的裸盘性能基线继续使用 raw SPDK `spdk_bdev`，HFS 性能基线使用
`LD_PRELOAD` 驱动的 POSIX 路径。

## 保留的数据面

普通 POSIX syscall 仍由 HFS preload 路由到 `hfs_lib_*`，数据面保留：

- request-local DMA pool 和 backpressure；
- `spdk_vtophys()` 自动检测可直接 DMA 的调用方 buffer；
- 对齐 direct I/O、非对齐首尾 RMW、vector I/O；
- reactor/channel 路由和 bounded pipeline；
- inode/range 锁、size/mtime 发布、fsync/fdatasync durability coordinator；
- PUNCH_HOLE、fallocate、truncate 和崩溃恢复所需的顺序。

这些能力在 syscall 返回前完成，因此不改变 POSIX 的可见顺序、错误码和
持久化边界。

## 删除的路径

- 删除 `src/hfs_async.[ch]` 及其 worker/request 生命周期；
- 删除 `hfs_lib_async_*`、`hfsAsync*`、`hfsRegisterBuffer`、`hfsDmaAlloc` 等
  对外符号；
- 删除 `HFS_POSIX_DIRECT_ASYNC` bridge。该 bridge 仍需要 staging copy、
  callback 和 condition wait，NVMe A/B 已显示比同步 POSIX 路径慢；
- 删除 registered-DMA/direct-async 专项测试脚本。

内部 `hfsIoDmaAlloc()` 仍保留，因为 POSIX staging 和 metadata 操作需要
SPDK DMA 内存；它不再是用户可见 ABI。

## POSIX 性能优化方向

1. 对普通 heap buffer，最多保留每个请求必要的一次 copy-in/copy-out；通过
   pool fast-acquire、减少锁竞争和批量提交降低 copy 之外的成本。
2. 对 `spdk_vtophys()` 可验证的 aligned buffer，使用同步 syscall 内部的
   direct batch/window，提交多个 SPDK child 后统一等待，不引入 async worker。
3. 继续缩短 per-inode/range 锁持有时间；元数据更新与数据提交保持明确的
   POSIX 顺序，不能为了吞吐删除 RMW、size publish 或设备 flush。
4. `fdatasync` 只刷新读取所需的数据和 metadata；多个同步者通过 durability
   coordinator 合并，`fsync` 保留完整设备 flush 语义。
5. FIO、MySQL/MariaDB 和 RocksDB 都通过同一普通路径验收；raw SPDK 仅作为
   无文件系统语义的设备上限，不作为 HFS 实现。

## 兼容性边界

这次收敛移除了实验性 async ABI，属于库符号层面的不兼容变更。完整 POSIX
应用接口不变；多个 HFS 进程共享挂载、mmap、native libaio/io_uring 仍不在
支持范围内。
