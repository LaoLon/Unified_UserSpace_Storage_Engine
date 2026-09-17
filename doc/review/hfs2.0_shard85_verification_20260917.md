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

在加入 worker-backed pending/completion 队列后，补跑了 QD=1/8/32、numjobs=1/2/4/8、
4K/16K/64K/1M、write/read/randwrite/randread/randrw 子矩阵，共 240/240 组合通过，
每项 `err=0`。这仍是单盘证据；QD=64/128 及双盘矩阵尚未据此宣称完成。

此前已完成同一正式设备上的 RocksDB direct/async、正常 reopen、SIGKILL recovery、
`hfs_ldb checkconsistency` 和 `hfsctl check`；FIO 4K loader smoke 加 `--verify=crc32c`
也通过且 `err=0`。

## 当前硬门禁

VM 设备盘点只有一块 NVMe；其余 `sda/sdb` 是虚拟 SATA/块设备，不能代替正式 NVMe
device set。因此以下项目没有伪造通过：

- 双盘 device set 和配置中全部 data device 矩阵；
- metadata replica=2 的掉盘、CRC 不一致、generation 落后组合；
- HFS 与 raw SPDK 的正式 5-run median 性能 gate；当前记录不是性能结论。

补充第二块真实 NVMe 后，应以同一 `.conf` 声明完整 device set，重新执行上述缺失
矩阵，并记录每盘 placement、submit/complete 和故障恢复结果。
