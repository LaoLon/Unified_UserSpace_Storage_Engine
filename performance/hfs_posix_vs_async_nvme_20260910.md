# HFS 普通 POSIX 路径与高性能路径 NVMe 对比

## 结论先行

本轮确认了两条路径都能在同一块 NVMe `Nvme0n1` 上工作：

- 普通路径：`LD_PRELOAD + psync + O_DIRECT`，完整 POSIX 语义，调用方在 syscall 返回前等待完成。
- 高性能路径：`hfsDmaAlloc()`/`hfsRegisterBuffer()` + `hfs_lib_async_pread/pwrite()`，显式 DMA buffer、callback/request 生命周期和多请求 outstanding。

普通路径的 FIO 结果已经与 raw SPDK 做了同盘对比；高性能路径使用项目现有 registered-DMA async 矩阵验证。现有 FIO 3.42 engine 不能直接调用 HFS async ABI，因此高性能侧不是把 raw SPDK 结果冒充 HFS，也不与普通 FIO 数字做严格一对一吞吐结论。

## 测试口径

| 项目 | 普通 POSIX FIO | HFS 高性能路径 |
|---|---|---|
| HFS 后端 | `config/hfs_nvme.json` → `Nvme0n1` | 同左 |
| 设备 | PCI `0000:0b:00.0`，SPDK `uio_pci_generic` | 同左 |
| Buffer | FIO 普通用户 buffer，`HFS_POSIX_DIRECT_ASYNC=0` | `hfsDmaAlloc` + `hfsRegisterBuffer` |
| 负载 | `randrw` 50/50，4/8/16/32/64 KiB，64 MiB，5 s | 对齐 positioned read/write，4/8/16/32/64 KiB，depth 1/32 |
| 并发 | HFS `numjobs=1/32`；raw SPDK `iodepth=1/32` | HFS async outstanding depth 1/32，4 data reactors |
| 普通结果 | `/mnt/hfs-data/performance-posix-nvme-20260910/{qd1,qd32}` | `/mnt/hfs-data/performance-hfs-highpath-nvme-20260910/{4k,8k,16k,32k,64k}` |

## 普通完整 POSIX 路径：HFS vs raw SPDK

### qd1

| 块大小 | HFS 总 IOPS | raw SPDK 总 IOPS | HFS/SPDK |
|---|---:|---:|---:|
| 4K | 23,882 | 33,347 | 71.6% |
| 8K | 23,493 | 32,566 | 72.1% |
| 16K | 22,714 | 31,305 | 72.6% |
| 32K | 21,016 | 28,390 | 74.0% |
| 64K | 18,494 | 24,568 | 75.3% |

### qd32

| 块大小 | HFS 总 IOPS | raw SPDK 总 IOPS | HFS/SPDK |
|---|---:|---:|---:|
| 4K | 67,627 | 121,718 | 55.6% |
| 8K | 66,736 | 114,874 | 58.1% |
| 16K | 60,493 | 95,155 | 63.6% |
| 32K | 52,958 | 73,982 | 71.6% |
| 64K | 43,158 | 52,557 | 82.1% |

普通 POSIX 路径所有样本 `errors=0`。qd32/4K 的 p99 读/写约为 `1.09/1.52 ms`，raw SPDK 约为 `0.60/0.61 ms`。

## HFS 高性能 registered-DMA async 路径

高性能矩阵所有样本 `write_errors=0/read_errors=0`，`io_copy_ops=0`，且 backend peak 和 queue peak 均达到 depth 32，`async_submit == async_complete`。

| 块大小 | depth | 写 IOPS | 读 IOPS | direct ops | copy ops | backend peak |
|---|---:|---:|---:|---:|---:|---:|
| 4K | 1 | 10,638 | 11,196 | 512 | 0 | 1 |
| 4K | 32 | 22,273 | 44,859 | 16,384 | 0 | 32 |
| 8K | 1 | 9,651 | 11,892 | 512 | 0 | 1 |
| 8K | 32 | 30,617 | 42,850 | 16,384 | 0 | 32 |
| 16K | 1 | 8,863 | 10,396 | 256 | 0 | 1 |
| 16K | 32 | 43,112 | 40,485 | 8,192 | 0 | 32 |
| 32K | 1 | 10,330 | 11,412 | 256 | 0 | 1 |
| 32K | 32 | 39,136 | 40,442 | 8,192 | 0 | 32 |
| 64K | 1 | 9,867 | 10,179 | 128 | 0 | 1 |
| 64K | 32 | 36,665 | 33,344 | 4,096 | 0 | 32 |

## 如何解读差距

普通 FIO 与高性能矩阵的驱动方式不同：普通侧是 FIO `randrw`，高性能侧是 HFS async API 的分阶段 read/write；因此不能直接用两组总 IOPS 相减作为严格 FIO 加速比。

可以确认的事实是：

1. 普通 POSIX 路径具备完整 fd、offset、append、元数据和同步 durability 语义，但普通用户 buffer 通常需要 DMA staging 和 copy。
2. 高性能路径可以形成真实 outstanding window，depth 32 时 backend/queue 均达到 32，且没有 bounce copy。
3. 若要得到“FIO 普通路径 vs FIO 高性能路径”的完全同一负载数字，需要新增 HFS async FIO ioengine；当前 FIO plugin 只有 raw SPDK engine，不能代表 HFS async ABI。

