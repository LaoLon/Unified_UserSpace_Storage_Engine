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

VM 设备盘点只有一块 NVMe；其余 `sda/sdb` 是虚拟 SATA/块设备，不能代替正式 NVMe
device set。因此以下项目没有伪造通过：

- 双盘 device set 和配置中全部 data device 矩阵；
- metadata replica=2 的掉盘、CRC 不一致、generation 落后组合；
- HFS 与 raw SPDK 的性能 gate 已执行但失败，且当前输出未提供正式 p99；不能作为
  Shard-81 完成证据。

补充第二块真实 NVMe 后，应以同一 `.conf` 声明完整 device set，重新执行上述缺失
矩阵，并记录每盘 placement、submit/complete 和故障恢复结果。
