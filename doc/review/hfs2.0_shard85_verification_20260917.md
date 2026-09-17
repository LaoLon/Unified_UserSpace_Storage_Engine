# HFS 2.0 Shard-85 verification record

时间：2026-09-17  
分支：`main`  
正式设备：`Nvme0n1`, PCI BDF `0000:0b:00.0`  
配置：`config/hfs_nvme.conf`（`metadata_devices=nvme0`, `data_devices=nvme0`,
`metadata_replication=1`）

## 已完成

在真实 NVMe 上执行了 FIO external engine 单盘矩阵：

| 维度 | 覆盖 |
| --- | --- |
| block size | 4K、16K、64K、1M |
| workload | write、read、randwrite、randread、randrw |
| iodepth | 1 |
| numjobs | 2 |
| 结果 | 20/20 运行 `err=0`，submit/complete 闭合 |

在加入 worker-backed pending/completion 队列后，补跑了 QD=1/8/32/64/128、
numjobs=1/2/4/8、4K/16K/64K/1M、write/read/randwrite/randread/randrw 子矩阵，
共 400/400 组合通过，每项 `err=0`。这仍是单盘证据；双盘矩阵尚未据此宣称完成。

此前已完成同一正式设备上的 RocksDB direct/async、正常 reopen、SIGKILL recovery、
`hfs_ldb checkconsistency` 和 `hfsctl check`；FIO 4K loader smoke 加 `--verify=crc32c`
也通过且 `err=0`。

在 FIO arena admission 修正后再次执行单盘组合回归：fillseq/readrandom/overwrite/
readwhilewriting、`checkconsistency`、SIGKILL 后 reopen/check、`hfsctl check` 与
`file_api_test` 均通过；SIGKILL 后 superblock 显示 `clean=0`，随后 mount 与一致性
检查成功完成。该复测仍不替代双盘和完整错误/取消矩阵。

RocksDB FileSystem smoke 现已额外覆盖 DB identity 锁竞争：首个 `LockFile` 成功，
第二个同名锁请求被拒绝，释放后可再次获取；该检查在同一真实 NVMe 回归中通过。

RocksDB FileSystem smoke 还验证了 allocator-backed `GetFreeSpace`：真实 NVMe 单盘
上初始可分配空间为 `31,398,559,744` 字节，写入一个 extent 后为 `31,397,511,168`
字节，释放最后一个硬链接并关闭读句柄后恢复为 `31,398,559,744` 字节。该结果只
证明单盘 data extent 统计和 open-handle 回收语义，不替代多盘容量矩阵。

Shard-80 的 FIO loader 仍以共享 DMA arena 提供整块 `orig_buffer`；arena 结构测试覆盖
了 2 MiB 连续分配、3 MiB cap 下第二次分配返回 `Busy`，以及释放后 bytes 归零。真实
NVMe `--verify=crc32c` loader smoke 继续通过。另执行了 QD=128、numjobs=8、1MiB 的
真实 NVMe 超限探针：FIO 返回非零并记录 `requested fio DMA arena exceeds mount cap`；
由于 FIO 外层仍打印已创建 job 的统计，该结果只证明 cap 拒绝路径，不证明完整高负载
容量矩阵，也不宣称零 worker 启动。

随后在同一真实 NVMe 上完成 QD=128 的 80 项高深度功能矩阵：numjobs=1/2/4/8、
4K/16K/64K/1M、write/read/randwrite/randread/randrw；脚本逐项检查 `err=0`，
80/80 通过。该结果补强了高深度功能正确性，但不替代完整 QD×jobs 矩阵和性能 gate。

带持久化结果的完整矩阵重跑在前 315 项通过后，于 QD=64、numjobs=8、1MiB、write
首次触及正式 512 MiB 共享 arena cap；FIO 返回 `rc=1` 并报告 DMA arena allocation
failure。该项是可定位的资源边界，不是数据错误；矩阵因此停止，不能记录为 400/400
通过，后续需在 arena 预算或 admission 设计修复后重跑。

DmaArena 单元回归还覆盖了共享 cap 满载时的背压：释放在 200ms 等待窗口内唤醒
成功，无释放时 10ms 后返回 `Busy`；该路径未引入无界等待。

带持久化结果的完整 FIO 矩阵最终收集到 385 项 `PASS`、15 项 `RESOURCE_BOUNDARY`、
0 项功能性 `FAIL`。资源边界来自正式 512 MiB arena cap 下的大 buffer/高并发组合，
其中最大组合为 QD=128、numjobs=8、1MiB；结果文件为 VM 上的
`/mnt/hfs-data/hfs-fio-matrix-20260917.log`。

随后修正了 FIO 对 arena `Busy` 的诊断：共享 cap 耗尽现在输出
`requested fio DMA arena exceeds mount cap`，而非与真实 allocator `NoMemory` 混淆；
低深度真实 NVMe `--verify=crc32c` smoke 在该修正后继续通过。

## Shard-81 raw 对照结果

按 §16.2 口径在同一 `0000:0b:00.0` 上完成 raw SPDK `bdevperf` 与 HFS FIO
各 5 次：4K、randrw、读写 50%、QD=32、32 jobs、10 秒、trace/stats 关闭。raw
五次 IOPS 为 `99894.69, 102524.48, 100148.85, 98855.09, 99633.24`，中位数
`99894.69 IOPS`；五次均 `Fail=0/TO=0`。HFS 五次 FIO 均 `err=0`，但从每次
READ/WRITE 吞吐换算的总 IOPS 约为 `7066, 11418, 3550, 6554, 26266`，中位数约
`7066 IOPS`，约为 raw 的 `7.1%`，未达到 85% gate。当前输出未包含正式 p99，
因此不补写 p99 结论；该性能失败触发 §17.3 上游结构修复边界，不能继续扩大性能
测试或宣称 Shard-81 完成。

随后将 FIO engine 从每个 I/O 创建线程改为每个 FIO worker 固定 4 个持久线程，
消除了 qd32×32 jobs 下的千级线程创建。对应 64 MiB/10 秒探索性复测的总 IOPS
约为 `40800, 35800, 30400, 43700, 43200`，明显高于原实现但仍低于 raw；正式
256 MiB gate 下尝试 16 个线程/job 后总 IOPS 约为 `7685, 2424, 5405, 9778,
7020`，并出现 60--80 ms 级 p99，说明盲目放大并发会造成排队和尾延迟放大。
因此保留 4 worker/job 作为当前有界实现，性能 gate 仍未通过。

## 当前硬门禁

VM 现有两块独立 NVMe 控制器：`Nvme0n1`（`0000:0b:00.0`，30 GiB）和 `Nvme1n1`
（`0000:13:00.0`，25 GiB）；其余 `sda/sdb` 是虚拟 SATA/块设备，不能代替正式 NVMe
device set。双盘配置 `config/hfs_nvme_dual.conf` 已完成 format、mount、metadata
replica check，以及 2 jobs/4 KiB/16 MiB 的 `randrw` CRC verify，返回 `err=0`。
因此以下项目仍没有伪造通过：

- 双盘 device set 和配置中全部 data device 矩阵；
- metadata replica=2 的掉盘、CRC 不一致、generation 落后组合；
- HFS 与 raw SPDK 的性能 gate 已执行但失败，且当前输出未提供正式 p99；不能作为
  Shard-81 完成证据。

此前缺盘环境下的重复 BDF 探针仍保留为边界证据；当前双盘测试使用两个独立 BDF，且
已确认 `vm.nr_hugepages=1024`、`HugePages_Free=1024`。生命周期验收中发现逐盘更新
clean/dirty 标志会造成 superblock generation 漂移，已修复为共享版本发布；修复后
双盘 `hfsctl check` 连续通过。RocksDB smoke 仍因“12 字节写入后 free space 必须
立即减少”这一与 extent 粒度统计不一致的断言返回 7，需另行修订测试口径。

真实双盘 fail-closed 探针结果：

| 故障 | 结果 |
| --- | --- |
| 只接管 `0000:0b:00.0`，缺失 required `0000:13:00.0` | `hfsctl info` 非零，SPDK 报 `No such device` |
| `Nvme1n1` 两个合法 superblock 都回退一个 generation | mount 非零，报告 metadata replica 不一致 |
| `Nvme1n1` 两个 superblock CRC 损坏 | mount 非零，报告设备未格式化 |

每项探针后均重新 format 双盘并恢复两个 BDF 到 kernel `nvme`，未留下损坏测试状态。

随后修复了三项双盘实现缺陷：控制 flush 的块/字节单位错误、镜像 superblock 未应用
目标盘 geometry、以及 mirrored journal 保存临时 geometry 引用造成的悬空引用。修复
后的真实双盘 `hfs_rocksdb_smoke` 和 `hfsctl check` 均返回 0；两盘分别报告有效
superblock、checkpoint 和相同 journal 状态。
