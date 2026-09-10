# 当前 HFS 与原生 SPDK NVMe FIO 对比

**测试日期**：2026-09-09  
**测试主机**：VM `ubuntu`，8 vCPU，Linux 6.8.0-139-generic  
**FIO**：`fio-3.42-139-gae35`  
**HFS 源码**：本地 `main` `f2379c4`（VM 同步构建副本记录为 `a414d499`）  
**SPDK**：`f0b63ed866f0e4d3c56e73bd4a98d0042956147a`

## 测试口径

- HFS 和 raw SPDK 均使用同一块 SPDK NVMe bdev：`Nvme0n1`。
- PCI 控制器：`0000:0b:00.0`，驱动为 `uio_pci_generic`。SPDK 接管后没有内核 `/dev/nvme*` 节点，这是预期状态，不代表设备不可用。
- workload：`randrw`、读写比例 50/50、`direct=1`、文件大小 256 MiB、每项运行 15 秒、块大小 4/8/16/32/64 KiB。
- HFS：`LD_PRELOAD=build/lib/libhfs.so`，`ioengine=psync`；raw SPDK：`LD_PRELOAD=/root/code/spdk/build/fio/spdk_bdev`，`ioengine=spdk_bdev`、`iodepth=32`。
- 两种 HFS 并发口径均执行：单线程（`numjobs=1`）和此前约定的 `numjobs=32`。后者与 raw SPDK 的 `iodepth=32` 做聚合并发对照。
- 所有项目 `errors=0`；以下只把 HFS 与 raw SPDK 作为主对比，脚本生成的 ext4 结果不参与结论。

## 结果：HFS 单线程 vs raw SPDK iodepth=32

| block size | HFS total IOPS | SPDK total IOPS | HFS/SPDK | HFS p99 R/W (µs) | SPDK p99 R/W (µs) |
|---|---:|---:|---:|---:|---:|
| 4 KiB | 22,203 | 113,975 | 19.5% | 132 / 142 | 569 / 569 |
| 8 KiB | 21,485 | 106,683 | 20.1% | 136 / 142 | 594 / 594 |
| 16 KiB | 20,583 | 87,647 | 23.5% | 153 / 159 | 709 / 717 |
| 32 KiB | 18,879 | 68,765 | 27.5% | 142 / 155 | 872 / 872 |
| 64 KiB | 17,231 | 49,405 | 34.9% | 155 / 167 | 1,139 / 1,139 |

单线程 HFS 只有一个同步请求在飞，因此延迟较低，但吞吐受到调用线程和同步等待限制；该轮不能作为 QD32 吞吐上限的公平比较。

## 结果：HFS numjobs=32 vs raw SPDK iodepth=32

| block size | HFS total IOPS | SPDK total IOPS | HFS/SPDK | HFS p99 R/W (µs) | SPDK p99 R/W (µs) |
|---|---:|---:|---:|---:|---:|
| 4 KiB | 43,897 | 114,471 | 38.3% | 5,931 / 4,358 | 561 / 569 |
| 8 KiB | 27,098 | 104,848 | 25.8% | 14,352 / 8,159 | 610 / 618 |
| 16 KiB | 18,178 | 88,218 | 20.6% | 23,462 / 12,255 | 692 / 692 |
| 32 KiB | 15,528 | 68,870 | 22.5% | 21,627 / 11,469 | 872 / 872 |
| 64 KiB | 4,958 | 49,399 | 10.0% | 30,015 / 29,753 | 1,122 / 1,122 |

## 结论与定位线索

1. 在同一 NVMe 上，HFS `numjobs=32` 的吞吐约为 raw SPDK 的 10%～38%；4 KiB 达到 43.9k IOPS，但仍比 raw SPDK 低 61.7%。
2. HFS 并发提升没有线性扩展：块大小从 4 KiB 增至 64 KiB 时，吞吐降至 4.96k IOPS，raw SPDK 仍为 49.4k IOPS。
3. HFS QD32 的 p99 在 5.9～30 ms，明显高于 raw SPDK 的 0.56～1.12 ms；这表明瓶颈不在 NVMe 控制器本身，而在 HFS 用户态请求路径的排队/锁竞争/回退处理。
4. 优先检查方向：同一 inode 的 range lock 持有范围、bounce/DMA pool 在大块请求下的 backpressure、每请求同步 waiter 与 data-reactor 消息开销，以及 32 个 job 共享单文件时的元数据发布和请求串行化。
5. 本次测试还修正了两个测试基础设施问题：raw SPDK bdev 不再硬编码为 `Malloc0`，并允许 SPDK 接管 PCI 后以 BDF 判断 NVMe 可用性。否则会得到错误的 Malloc 对照或错误的 SKIP。

## 原始结果位置

- VM 单线程：`/var/tmp/hfs-fio-nvme-current-20260909-r1/`
- VM `numjobs=32`：`/var/tmp/hfs-fio-nvme-current-20260909-r2/`
- 每个目录包含 `environment.txt`、`summary.tsv` 及每个块大小的 HFS/raw SPDK JSON 日志。
