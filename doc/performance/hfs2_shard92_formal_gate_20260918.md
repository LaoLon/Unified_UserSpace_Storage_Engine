# HFS 2.0 Shard-92 正式 raw/HFS 五次性能 gate

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

日期：2026-09-18
设备：真实 `Nvme0n1`，PCI BDF `0000:0b:00.0`
结果目录：

- raw SPDK：`/mnt/hfs-data/hfs2-shard81-spdk-fio-baseline-20260918-shard92`
- HFS：`/mnt/hfs-data/hfs2-shard81-async-gate-20260918-shard92`

## 结论

本轮 raw 与 HFS 均完成五次同口径采集，所有请求生命周期、错误和设备环境
条件均闭合；但性能阈值未通过，因此 Shard-81 的正式性能 gate 仍保持未完成。
Shard-92 完成了最终 gate 的证据采集和归档，不将失败结果宣称为性能通过。

- HFS 中位总 IOPS：`76893.931820`
- raw SPDK 中位总 IOPS：`109474.152585`
- HFS/raw：`70.239%`，低于 `85%` 门槛
- HFS 读/写中位 p99：`3227.648 us / 3227.648 us`
- raw 读/写中位 p99：`643.072 us / 643.072 us`
- HFS/raw p99：`5.019x`，高于 `1.10x` 门槛

## 固定口径

- 真实 NVMe、CPU 2-3、4 KiB、`randrw`、读写 50/50、256 MiB、O_DIRECT、10 秒、5 次；
- raw 有效 outstanding 为 `iodepth=32`；HFS 使用 `32 jobs × iodepth=1`，总有效并发为 32；
- raw 使用 SPDK FIO plugin 和 `config/hfs_nvme.json`；该 JSON 仅是 raw SPDK plugin
  的测试输入，不是 HFS 2.0 架构配置；
- HFS 使用正式入口 `config/hfs_nvme.conf`、`Nvme0n1` 和 external `fio-hfs.so`；
- 正式 gate 未启用 trace/stats；每次 HFS 运行前重新 format；
- VM 设备驱动为 `uio_pci_generic`，`vm.nr_hugepages=1024`。

实际执行的核心参数为：

```text
raw: taskset -c 2-3 fio --rw=randrw --rwmixread=50 --bs=4k --size=256m
     --iodepth=32 --thread=1 --direct=1 --time_based=1 --runtime=10
     --norandommap=1 --randrepeat=0 --numjobs=1

HFS: taskset -c 2-3 fio --rw=randrw --rwmixread=50 --bs=4k --size=256m
     --iodepth=1 --numjobs=32 --thread=1 --direct=1 --hfs_direct=1
     --time_based=1 --runtime=10 --norandommap=1 --randrepeat=0
     --hfs_async=1 --hfs_client_depth=32
```

## 五次原始结果

| layer | run | total IOPS | read p99 (us) | write p99 (us) | CPU % | errors | read I/O | write I/O |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| raw | 1 | 108472.253 | 651.264 | 659.456 | 99.99 | 0 | 542481 | 542350 |
| raw | 2 | 110416.258 | 643.072 | 643.072 | 99.98 | 0 | 551777 | 552496 |
| raw | 3 | 109474.153 | 643.072 | 651.264 | 99.98 | 0 | 547841 | 547010 |
| raw | 4 | 109884.312 | 634.880 | 634.880 | 99.98 | 0 | 550195 | 548758 |
| raw | 5 | 108900.610 | 643.072 | 643.072 | 99.97 | 0 | 544762 | 544353 |
| HFS | 1 | 79811.457 | 3129.344 | 3129.344 | 2.276 | 0 | 399127 | 399227 |
| HFS | 2 | 64851.445 | 3325.952 | 3325.952 | 1.860 | 0 | 324882 | 323827 |
| HFS | 3 | 60579.368 | 3293.184 | 3293.184 | 1.752 | 0 | 302753 | 303283 |
| HFS | 4 | 82833.983 | 3162.112 | 3162.112 | 2.338 | 0 | 414211 | 414543 |
| HFS | 5 | 76893.932 | 3227.648 | 3227.648 | 2.184 | 0 | 384274 | 384896 |

中位数为 raw `109474.152585 IOPS`、HFS `76893.931820 IOPS`。CPU 数值按工具
输出原样保留；raw 的高 CPU 是 SPDK busy-polling 口径，不能与 HFS CPU 直接作为
同一实现成本指标解读，本轮只将其作为辅助观测。

## 生命周期与环境闭合

HFS 每次运行的 engine closure 和 FIO closure 如下。所有行均满足请求计数相等，
没有错误、未回收 outstanding 或 token mismatch。

| run | accepted=terminal=published | child submit=complete | queued=reaped | outstanding | token mismatch |
|---:|---:|---:|---:|---:|---:|
| 1 | 798354 | 798354 | 798354 | 0 | 0 |
| 2 | 648709 | 648709 | 648709 | 0 | 0 |
| 3 | 606036 | 606036 | 606036 | 0 | 0 |
| 4 | 828754 | 828754 | 828754 | 0 | 0 |
| 5 | 769170 | 769170 | 769170 | 0 | 0 |

环境检查通过：驱动为 `uio_pci_generic`，hugepages 为 `1024`，五次运行错误数
均为 0，正式 gate 未打开 trace/stats。

## Gate 判定

| 条件 | 判定 | 证据 |
|---|---|---|
| HFS/raw >= 85% | FAIL | `70.239%` |
| read/write p99 <= raw p99 × 110% | FAIL | `5.019x`，不是 `<= 1.10x` |
| errors == 0 | PASS | 五次均为 0 |
| queued == reaped | PASS | 五次均相等 |
| accepted == terminal == published | PASS | 五次均相等 |
| child_submit == child_complete | PASS | 五次均相等 |
| outstanding == 0 | PASS | 五次均为 0 |
| token_mismatch == 0 | PASS | 五次均为 0 |
| trace/stats off 的正式结果可复现 | PASS | raw/HFS 各五次已归档 |

因此，本轮失败不是早期的请求数量塌陷、completion 泄漏或设备绑定错误；当前
仍有明显的 HFS async 路径吞吐和尾延迟开销。该 gate 只负责确认最终口径和保留
可复核证据，不足以单独把额外开销归因到某一个子阶段。后续若继续优化，必须以
本报告的固定口径和两个结果目录为对照，并在新的 trace/基准证据支持下再改动。