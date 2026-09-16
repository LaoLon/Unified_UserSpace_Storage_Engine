# task09160854 验证结果

## 结论

**PASS**。在不提供显式 reactor 配置、未使用 `taskset` 的情况下，VM 真实 NVMe 数据面完成自动拓扑和短回归验证。

## 环境与前置

- 在线 CPU：8
- NVMe：PCI `0000:0b:00.0`，驱动 `uio_pci_generic`
- hugepages：1024
- 残留进程检查：通过
- 编译：`make -B app lib` 通过

## 自动拓扑

- 实际日志：`online_cpus=8 total=3 data=2 source=auto`
- 期望公式：`data=min(4, max(1, floor(online_cpus/4)))`（`online_cpus >= 2`），`total=1+data`
- 判定：通过，当前 8 vCPU 自动使用 owner reactor + 2 个 data reactor。

## 正确性回归

- `POSIX CONTRACT OK`
- `REQUEST-LOCAL FAST IO OK`
- 判定：通过

## 短性能 sanity

单次 4K randrw、16 jobs、5 秒运行，仅用于确认可运行和错误为零，不作为正式基线：

| 方向 | IOPS | 带宽 (KiB/s) | clat avg | clat p99 |
|---|---:|---:|---:|---:|
| read | 43,337.33 | 173,349 | 182.7 us | 378.9 us |
| write | 43,289.94 | 173,159 | 185.3 us | 383.0 us |

- fio `error=0`
- CPU：user 4.0875%，sys 2.1025%
- 运行后残留进程：无
- trace：本次未启用且未生成，因此不适用 trace closure 检查

## 证据

远端证据目录：`/mnt/hfs-data/task09160854`

包含 `precheck.txt`、`reactor_topology.log`、`auto-reactor-fio.json`、探针日志及两个 smoke 的锁文件。

本轮所有进程均清除了 `HFS_DATA_REACTORS`、`HFS_DATA_REACTOR_CPUS`、`HFS_OWNER_REACTOR_CPU`、`HFS_REACTOR_PIN` 和 `HFS_BASELINE_*`，没有显式 reactor 设置。
