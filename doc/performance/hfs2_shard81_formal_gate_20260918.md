# HFS 2.0 Shard-81 正式 raw/HFS 五次性能 gate

日期：2026-09-18  
设备：真实 `Nvme0n1`，PCI BDF `0000:0b:00.0`  
证据目录：`/mnt/hfs-data/hfs2-shard81-formal-20260918`

## 结论

本轮完成了 raw 与 HFS 各五次采集，但 gate **未通过，且不能作为有效的性能通过证据**。HFS external FIO 引擎每次只完成 8 KiB（读 4 KiB、写 4 KiB），没有兑现 §16.2 要求的 32 jobs/有效并发 32；因此 HFS 吞吐中位数只有 `0.098 IOPS`，与 raw 的 `5151.390 IOPS` 比值为 `0.000019`。这属于上游 FIO 请求生命周期/并发提交边界未闭合，不是可接受的性能差距。

## 固定口径

- 8 vCPU 基准，raw 和 HFS 均限制在 CPU 2-3；
- 4 KiB、shared-file、`randrw`、读写 50/50、256 MiB、O_DIRECT、10 秒、5 次；
- raw：SPDK `bdevperf`，`-q 32 -o 4096 -w randrw -M 50 -t 10 -m 0xc -C`；
- HFS：FIO 3.42 external engine，`--numjobs=32 --iodepth=1`，有效并发目标为 32；
- trace/stats 未启用；测试前使用同一设备并重新 format HFS；
- raw bdevperf 只提供 randrw aggregate latency histogram，未提供读/写分离 p99；本轮不将 aggregate p99 冒充为读/写 p99。

## 五次原始结果

| layer | run | IOPS | p50 read/write (us) | p99 read/write (us) | CPU % | errors |
|---|---:|---:|---:|---:|---:|---:|
| raw | 1 | 6088.770 | N/A | 35951.177 aggregate | N/A | 0 |
| raw | 2 | 4685.520 | N/A | 62914.560 aggregate | N/A | 0 |
| raw | 3 | 5759.530 | N/A | 44040.192 aggregate | N/A | 0 |
| raw | 4 | 4856.360 | N/A | 56922.697 aggregate | N/A | 0 |
| raw | 5 | 5151.390 | N/A | 44339.785 aggregate | N/A | 0 |
| HFS | 1 | 0.094976 | read N/A / write N/A | read 0 / write 35913.728 | 0.438 | 0 |
| HFS | 2 | 0.091483 | read N/A / write N/A | read 0 / write 65798.144 | 0.435 | 0 |
| HFS | 3 | 0.199482 | read 30801.920 / write 32636.928 | read 30801.920 / write 32636.928 | 0.452 | 0 |
| HFS | 4 | 0.198787 | read N/A / write N/A | read 14876.672 / write 72876.032 | 0.463 | 0 |
| HFS | 5 | 0.097666 | read 33816.576 / write N/A | read 33816.576 / write 0 | 0.456 | 0 |

中位数：raw `5151.390 IOPS`；HFS `0.098 IOPS`；HFS/raw `0.000019`；错误数为 0。原始逐次数据保存在证据目录的 `runs.tsv`，原始日志为 `raw-1.log` 至 `raw-5.log`，FIO JSON 为 `hfs-1.json` 至 `hfs-5.json`。

## Gate 判定与后续边界

| 条件 | 判定 | 说明 |
|---|---|---|
| HFS/raw ≥ 85% | FAIL | HFS external engine 未形成目标并发，仅完成 8 KiB/次 |
| CPU ≤ 500% | 不能据此收口 | HFS JSON 仅反映退化的单请求路径，raw CPU/RSS 未由 bdevperf 导出 |
| errors = 0 | PASS（表面） | 不足以抵消请求数量未闭合 |
| read/write p99 ≤ raw×110% | INCOMPLETE | raw 只有 aggregate p99，HFS 读写样本也不完整 |
| submit/complete 闭合 | FAIL | 32 jobs 目标未兑现；FIO JSON 每次只反映极少量 I/O |

因此 Shard-81 保持未完成。下一步必须先完成 core 内前端提交/完成生命周期、`io_handle` 保活和停止顺序，恢复真实 `queue/getevents/event` 并发后，再以同一拓扑重跑五次 raw/HFS gate。
