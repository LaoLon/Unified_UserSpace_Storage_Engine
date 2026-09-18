# HFS 2.0 Shard-81 正式 raw/HFS 五次性能 gate

日期：2026-09-18  
设备：真实 `Nvme0n1`，PCI BDF `0000:0b:00.0`  
证据目录：`/mnt/hfs-data/hfs2-shard81-formal-20260918`

## 结论

本轮完成了 raw 与 HFS 各五次采集，但 gate **未通过，且不能作为有效的性能通过证据**。HFS external FIO 引擎每次只完成 8 KiB（读 4 KiB、写 4 KiB），总请求量远未达到 §16.2 要求的 32 jobs/有效并发 32；因此 HFS 吞吐中位数只有 `0.098 IOPS`，与 raw 的 `5151.390 IOPS` 比值为 `0.000019`。这属于上游 FIO 请求生命周期/并发提交边界未闭合，不是可接受的性能差距。注意：`group_reporting=1` 会把多个 job 汇总到 JSON 的 `jobs[0]`，本报告不以该字段推断实际 job 数。

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
| submit/complete 闭合 | FAIL | 总请求量极低，无法证明 32 jobs 的有效 workload 已闭合 |

因此 Shard-81 保持未完成。下一步必须先完成 core 内前端提交/完成生命周期、`io_handle` 保活和停止顺序，恢复真实 `queue/getevents/event` 并发后，再以同一拓扑重跑五次 raw/HFS gate。

## 2026-09-18 async lifecycle gate（当前最新实现）

上面的结果保留为历史记录。本节记录完成 async completion/lifecycle 修复后的同口径
真实 NVMe 重跑，证据目录为 `/mnt/hfs-data/hfs2-shard81-async-gate-final20260918`。
raw 基线仍使用既有官方 SPDK FIO 插件五次结果，证据位于
`/mnt/hfs-data/hfs2-shard81-spdk-fio-baseline-20260918`；两侧均固定为 CPU 2-3、
4 KiB `randrw` 50/50、256 MiB、10 秒，HFS 为 32 jobs、iodepth 1、O_DIRECT。

| layer | run 1 | run 2 | run 3 | run 4 | run 5 | median |
|---|---:|---:|---:|---:|---:|---:|
| raw total IOPS | 108476.052 | 108018.698 | 108272.573 | 108396.060 | 108844.516 | **108396.060** |
| HFS total IOPS | 68331.767 | 66538.784 | 60185.526 | 61880.948 | 70006.698 | **66538.784** |

HFS/raw = **61.38%**，低于 85% 性能门槛，性能 gate **FAIL**。HFS 中位数
read/write p99 为 `3227.648 us`，raw 对应中位数 read/write p99 为 `659.456 us`，
约为 raw 的 4.9 倍，因此 latency gate 也 **FAIL**。HFS CPU 中位数为 `3.095%`，
五次错误数均为 0。

本轮已经证明功能生命周期闭合：每次 `queued == reaped`、`outstanding == 0`、
`peak == 32`、`token_mismatch == 0`，preallocation 为 owner 1/reuse 31；engine
closure 中 `accepted == terminal == published`，child submit/complete 逐次相等，
`plan_fail/dma_fail/preparing` 均为 0。设备为 `uio_pci_generic`，hugepages 为 1024。
因此当前失败不是早期的请求数量塌陷或 close 泄漏，而是 async completion 路径在
32 个浅队列 job 下仍有明显额外开销。

诊断结果：scheduler wait 几乎为零，主要额外成本来自独立 data reactor 与 FIO
前端之间的 eventfd 通知/等待和跨 reactor completion；`strace -f -c` 可见约一对
completion 对应的 `poll/read/write` 系统调用。completion queue 已加入有限的主动
poll（1024 次）以减少浅队列下的通知往返，但不足以达到性能门槛。下一步应继续优化
completion batching/通知策略和 reactor 拓扑，再以本节口径重跑，不能把本轮结果标记为
Shard-81 性能通过。

另外，32 jobs×iodepth32 共享同一随机写文件的 `fio --verify` 实验返回 FIO error 84；
该 workload 存在多个 job 随机重叠写同一 verify 区域，不能据此判定 HFS 数据损坏。
单 job qd32、有效 read/write 以及 bounce 路径的 verify/功能测试通过；共享文件的
重叠随机 verify 保留为独立测试设计问题，不作为本性能 gate 的通过证据。

## 2026-09-18 修复后重跑

修复 external worker pool 后，先完成真实 NVMe 4-job smoke；随后按本报告固定口径
重跑。raw 五次 IOPS 为 `5241.33 / 5004.26 / 5334.68 / 5108.72 / 4175.39`，
中位数 `5108.72 IOPS`。HFS 五次总 IOPS（read+iops 与 write+iops）为
`0.89991 / 1.098791 / 0.199601 / 0.89982 / 10.496851`，中位数
`0.89991 IOPS`，HFS/raw=`0.00017615`，相当于 raw 的 `0.0176%`，性能差距
约 `99.9824%`。

五次 HFS 均返回 `error=0`，但每次仅完成 20 KiB 至 430 KiB 左右，远未达到
32 jobs/10 秒应有的请求量；设备最终恢复为内核 `nvme` 驱动。因此这次重跑仍是
结构性失败，说明同步提交路径能通过低并发 smoke，但不能满足正式 32-job gate。
后续需定位 32 个 FIO job 与 HFS data reactor/Client 的并发阻塞，再重跑正式 gate。
