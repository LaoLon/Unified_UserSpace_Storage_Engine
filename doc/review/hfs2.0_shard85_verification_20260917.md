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

## Shard-81 raw 对照结果

按 §16.2 口径在同一 `0000:0b:00.0` 上完成 raw SPDK `bdevperf` 与 HFS FIO
各 5 次：4K、randrw、读写 50%、QD=32、32 jobs、10 秒、trace/stats 关闭。raw
五次 IOPS 为 `99894.69, 102524.48, 100148.85, 98855.09, 99633.24`，中位数
`99894.69 IOPS`；五次均 `Fail=0/TO=0`。HFS 五次 FIO 均 `err=0`，但从每次
READ/WRITE 吞吐换算的总 IOPS 约为 `7066, 11418, 3550, 6554, 26266`，中位数约
`7066 IOPS`，约为 raw 的 `7.1%`，未达到 85% gate。当前输出未包含正式 p99，
因此不补写 p99 结论；该性能失败触发 §17.3 上游结构修复边界，不能继续扩大性能
测试或宣称 Shard-81 完成。

## 当前硬门禁

VM 设备盘点只有一块 NVMe；其余 `sda/sdb` 是虚拟 SATA/块设备，不能代替正式 NVMe
device set。因此以下项目没有伪造通过：

- 双盘 device set 和配置中全部 data device 矩阵；
- metadata replica=2 的掉盘、CRC 不一致、generation 落后组合；
- HFS 与 raw SPDK 的性能 gate 已执行但失败，且当前输出未提供正式 p99；不能作为
  Shard-81 完成证据。

补充第二块真实 NVMe 后，应以同一 `.conf` 声明完整 device set，重新执行上述缺失
矩阵，并记录每盘 placement、submit/complete 和故障恢复结果。
