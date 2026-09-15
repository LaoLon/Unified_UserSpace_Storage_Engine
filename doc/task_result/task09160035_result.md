# task09160035 执行结果

## 结论

本次按修改后的短测版任务执行。正确性回归通过；性能门禁在 A 组首个样本即触发停止条件，因此未运行 B 拓扑和 adaptive wait，也没有修改默认拓扑、亲和或等待模式。

## 完成内容

- 在真实 NVMe 路径下修复稀疏扩展写的 POSIX 零填充问题：对所有 `end > logical EOF` 的 POSIX 写取得 inode 级 `writeGapLock`，重新确认 EOF，清零 `[oldEOF, offset)`，完成数据写入和 inode size 发布后释放锁；覆盖 write/pwrite/writev/pwritev。
- 将 poll-gap 的 `lastPollExitNs` 记录限制在 timing profile 开启时，避免 trace-off/stats-off 热路径的无条件 release 原子写。
- 使两个快速 smoke 脚本支持 `HFS_BDEV_NAME=Nvme0n1` 的真实设备口径，不创建 Aio 文件镜像。
- 已更新 `doc/bug.md`（条目 89、90）、shard-66 计划、shard 进度记忆和 baseline v2 记录。

## 验证

VM 前置状态：真实 NVMe `Nvme0n1`，PCI BDF `0000:0b:00.0` 使用 `uio_pci_generic`，`vm.nr_hugepages=1024`，运行前无 HFS/FIO 进程。

- `make -B app lib`：通过。
- CMake Release：通过。
- `scripts/db/posix_contract_test.sh`：通过。
- `scripts/db/io_fast_path_smoke.sh`：通过，包含 sparse-read、并发 positioned write、向量 I/O、KEEP_SIZE、PUNCH_HOLE 和边缘校验。

## 性能门禁结果

A 组布局为 5 个 total reactor、data CPU 1-4、应用 CPU 5-7；共享文件、4K randread、32 jobs、trace/stats off。首个样本结果：

- IOPS：`58,277.97`
- FIO errors：`0`
- 进程 CPU：`595%`
- 新鲜共享文件 prefill：`272.261 s`

该 IOPS 低于任务规定的 `70K` 停止线，且下一次 prefill 超出短测墙钟预算，故按任务要求停止。该结果是门禁诊断样本，不是完成的 median 或新的性能基线。

原始证据保留在 VM：`/mnt/hfs-data/task09160035-short/A`；快速正确性证据在 `/mnt/hfs-data/shard66-correctness`。

## 后续状态

shard-66 当前实现保持不变，默认配置不变。完整 5-run NVMe 矩阵、B 拓扑比较和 adaptive wait 门禁仍待后续单独处理。
