# HFS 2.0 原生 SPDK NVMe 基线

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

## 目的

本文固定一批可复用的原生 SPDK 结果，供后续 HFS FIO 测试做吞吐对照。除非
设备、SPDK 版本、CPU 拓扑或测试口径变化，后续不重复采集这批 raw 结果。

## 固定口径

| 项目 | 值 |
| --- | --- |
| 日期 | 2026-09-17 |
| 设备 | `Nvme0n1`，PCI BDF `0000:0b:00.0`，30 GiB |
| SPDK | v26.09-pre，git `f0b63ed86` |
| 工具 | SPDK `bdevperf` |
| workload | `randrw`，读写比例 50/50 |
| queue depth | 32 |
| block size | 4 KiB、8 KiB、16 KiB、32 KiB、64 KiB |
| duration | 每次 10 秒 |
| repetitions | 每种 block size 5 次，取中位数 |
| CPU | `--cpumask 0x4`；SPDK 日志报告可用 reactor core 为 1 |
| trace/stats | 关闭 |
| hugepages | `vm.nr_hugepages=1024` |

测试直接写 raw bdev，不经过文件系统；该过程会覆盖目标盘上的已有内容。每次
运行均确认没有 kernel holder/mount，结束后将 BDF 恢复到 kernel `nvme` 驱动。

## 中位数结果

| block size | IOPS | MiB/s | 平均延迟 (us) | Fail | Timeout |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4 KiB | 77,273.59 | 301.85 | 414.01 | 0 | 0 |
| 8 KiB | 73,112.71 | 571.19 | 437.58 | 0 | 0 |
| 16 KiB | 65,030.90 | 1,016.11 | 491.98 | 0 | 0 |
| 32 KiB | 53,036.50 | 1,657.39 | 603.26 | 0 | 0 |
| 64 KiB | 14,972.07 | 935.75 | 2,133.58 | 0 | 0 |

IOPS 五次原始值依次为：

- 4 KiB：`77273.59, 69416.45, 73990.66, 79054.46, 78512.16`
- 8 KiB：`72826.49, 73112.71, 71540.01, 73386.97, 74903.55`
- 16 KiB：`64824.81, 65030.90, 65485.86, 65554.39, 63814.08`
- 32 KiB：`53036.50, 52377.53, 53299.65, 52120.09, 53200.30`
- 64 KiB：`8354.94, 8407.52, 15012.15, 14972.07, 15128.17`

## 原始证据与使用规则

完整 `bdevperf` 输出保留在 VM：
`/mnt/hfs-data/hfs2-spdk-baseline-20260917/bs-<bytes>-run-<n>.log`；汇总为：
`/mnt/hfs-data/hfs2-spdk-baseline-20260917.tsv`。

本批 bdevperf 输出提供 average/min/max，不提供正式 p99，因此本文件的吞吐基线
不等同于 §16.2 的完整性能 gate。后续 HFS 对比应复用上述设备、QD、block size、
运行时长和 CPU 口径；尾延迟 gate 仍需使用同一口径的 histogram/p99 证据，不能
把本文件的 average 延迟替代 p99。