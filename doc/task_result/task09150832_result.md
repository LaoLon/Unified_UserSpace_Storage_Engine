# task09150832 性能归因与并发扩展结果

执行人：Huizareth 协作任务  
日期：2026-09-15  
基线：`main` 当前 HEAD `a780b16`，本任务的测量修正和归因代码在本次结果生成时已同步到 VM 并通过强制构建。  
设备：VM 内真实 NVMe，`HFS_BDEV_NAME=Nvme0n1`，PCI BDF `0000:0b:00.0`，SPDK commit `a414d49`。

## 1. 执行结论

任务要求的 A3 审计、scaling、绑核和 ordering 矩阵均已完成：

- A3：4K randread/randwrite，workers=`1/4/8/16/32`，10 个点，全部 `errors=0`。
- P0-2 scaling：读写各 `numjobs=1/2/4/8/16/32`，每点 10s ramp + 30s runtime + 3 次重复，共 36 个样本。
- 绑核：owner CPU0、data CPU1-4 或 CPU1-2、应用 CPU5-7，共 4 个样本。
- ordering：`ticket/range/none` × 读写 × `1/8/32 job` × 3 次，共 54 个样本。
- 94 个 FIO 样本全部 `fio_error=0`、`spdk_reject=0`，并满足 `spdk_submit=spdk_complete`。

正式原始结果保留在 VM：`/mnt/hfs-data/task09150832`。本地汇总证据为 [task09150832_full_summary.json](E:/Code/HFS_BASE_SPDK/doc/task_result/task09150832_full_summary.json)。

## 2. 测量审计与修正

### A3

原 A3 在创建 worker 前启动计时，且只输出请求数，无法证明真实 worker 数、SPDK submit 数和 outstanding peak。本次做了以下修正：

- 所有 worker 创建完成后使用 condition variable 起跑门同时启动计时。
- 每个 worker 使用独立 DMA buffer；共享的是同一个 `HfsFile`，不共享请求 buffer 或 scratch 状态。
- 记录 `actual_workers`、`actual_requests`、`actual_spdk_submits`、`outstanding_peak`、实际 runtime 和 errors。
- 增加 `HFS_OWNER_REACTOR_CPU`，用于 owner/data/application CPU 隔离实验。

| workload | workers | actual requests | actual SPDK submits | outstanding peak | runtime(s) | IOPS |
|---|---:|---:|---:|---:|---:|---:|
| randread | 1 | 112,121 | 112,135 | 1 | 5.0001 | 22,423.9 |
| randread | 4 | 245,764 | 245,778 | 4 | 5.0001 | 49,151.9 |
| randread | 8 | 316,935 | 316,949 | 8 | 5.0002 | 63,384.3 |
| randread | 16 | 429,688 | 429,702 | 16 | 5.0003 | 85,931.9 |
| randread | 32 | 420,522 | 420,536 | 32 | 5.0008 | 84,090.4 |
| randwrite | 1 | 102,501 | 102,515 | 1 | 5.0001 | 20,499.9 |
| randwrite | 4 | 217,369 | 217,383 | 4 | 5.0001 | 43,473.0 |
| randwrite | 8 | 265,785 | 265,799 | 8 | 5.0003 | 53,153.4 |
| randwrite | 16 | 337,400 | 337,414 | 16 | 5.0004 | 67,474.6 |
| randwrite | 32 | 325,907 | 325,921 | 32 | 5.0008 | 65,170.6 |

`actual_spdk_submits` 比 A3 logical requests 多 14，来自进程内 mount/metadata 初始化提交；不能把全局 submit 计数直接当作 A3 数据请求一一映射。A3 的 worker 数、同步起跑、请求数和 outstanding peak 均符合预期。

### 测量错误处理

第一次短 smoke 发现脚本把 FIO 文件写成 `/fio/...`，绕过了当前 `/hfs/` preload 前缀；该批结果已全部废弃。修正为 `/hfs/fio/...` 后重新 smoke，并以正式轮次为唯一结论来源。正式 trace 使用 `HFS_TRACE_SAMPLE=1000`，避免 sample=100 在高 IOPS 下产生不必要的大文件。

## 3. Scaling 结果

下表为每个 workload/job 的 3 次中位数；avg/p99 为对应读或写方向的 FIO completion latency，CPU 为 `/usr/bin/time` 的进程 CPU 百分比，runq/context switch 为 vmstat 中位汇总。

| workload/jobs | IOPS | avg us | p99 us | CPU% | runq r | ctx/s |
|---|---:|---:|---:|---:|---:|---:|
| read/1 | 20,180 | 49.2 | 126.5 | 514 | 5.0 | 39,329 |
| read/2 | 29,233 | 67.6 | 148.5 | 536 | 5.5 | 56,046 |
| read/4 | 45,286 | 87.6 | 181.2 | 557 | 6.3 | 68,835 |
| read/8 | 63,805 | 124.5 | 261.1 | 586 | 7.8 | 89,465 |
| read/16 | 87,833 | 181.3 | 436.2 | 627 | 10.8 | 113,007 |
| read/32 | 87,229 | 366.0 | 856.1 | 653 | 11.2 | 139,580 |
| write/1 | 18,974 | 52.3 | 134.1 | 514 | 5.1 | 37,347 |
| write/2 | 27,599 | 71.6 | 154.6 | 535 | 5.3 | 53,500 |
| write/4 | 40,538 | 97.9 | 201.7 | 552 | 5.8 | 64,722 |
| write/8 | 56,587 | 140.5 | 317.4 | 575 | 7.5 | 76,065 |
| write/16 | 70,038 | 227.4 | 544.8 | 610 | 8.9 | 95,770 |
| write/32 | 67,959 | 469.9 | 1,036.3 | 631 | 9.4 | 116,278 |

profile stage 中位数的主要趋势如下（单位 us）：

| workload/jobs | T1-T2 | T4-T5 | T6-T7 | T8-T9 | T9-T10 |
|---|---:|---:|---:|---:|---:|
| read/1 | 6.0 | 2.8 | 43.1 | 18.3 | 4.6 |
| read/8 | 7.9 | 7.2 | 115.7 | 38.7 | 6.9 |
| read/16 | 9.6 | 12.5 | 169.8 | 57.3 | 10.3 |
| read/32 | 27.1 | 28.6 | 340.0 | 109.5 | 31.4 |
| write/1 | 5.8 | 2.8 | 48.4 | 18.3 | 3.0 |
| write/8 | 7.4 | 7.8 | 134.0 | 44.7 | 4.8 |
| write/16 | 9.3 | 15.7 | 229.7 | 59.9 | 7.6 |
| write/32 | 25.2 | 39.1 | 480.3 | 101.7 | 24.4 |

结论：高并发下最大的阶段是 T6-T7（SPDK submit 到 callback），其次是 T8-T9（completion wake 到 application resume）；T1-T2、T4-T5 和 T9-T10 随 job 增长，但不是首要热点。

## 4. 绑核与 reactor 数

owner 固定 CPU0，应用固定 CPU5-7；4-reactor 为 data CPU1-4，2-reactor 为 data CPU1-2。

| workload | data reactors | IOPS | p99 us | CPU% | runq r | ctx/s |
|---|---:|---:|---:|---:|---:|---:|
| read | 2 | 108,734 | 659.5 | 445 | 11.9 | 154,070 |
| read | 4 | 87,413 | 839.7 | 657 | 13.7 | 139,566 |
| write | 2 | 90,617 | 749.6 | 428 | 8.6 | 135,267 |
| write | 4 | 67,885 | 1,028.1 | 629 | 9.7 | 113,140 |

本轮固定 workload 下，2 data reactors 比 4 data reactors 更好：读约 +24%，写约 +33%，同时 CPU 使用率更低。该结果支持后续保留 2/4 reactor 的小矩阵调优，但不足以推导“reactor 越少越好”的普遍结论。

注意：部分 affinity 原始 stats 的 `runtime_queue_depth_peak` 为无符号下溢值 `UINT64_MAX`，已记录为 `doc/bug.md` 条目 86；本报告不使用该字段，改用 vmstat run queue 与 per-reactor submit/complete。所有 per-reactor submit/complete 仍闭合。

## 5. Ticket 细分归因

以下为 ordering=ticket、32-job、3 次样本中的代表性 r3 stats；时间是 HFS ticket 统计的 avg/p50/p95/p99，上报直方图桶为上界值。

| metric | avg ns | p50 ns | p95 ns | p99 ns |
|---|---:|---:|---:|---:|
| admission lock wait（read） | 10,248 | 64 | 32,768 | 32,768 |
| admission scan（read） | 908 | 512 | 1,024 | 2,048 |
| admission insert（read） | 1,336 | 64 | 2,048 | 16,384 |
| dependency wait（read） | 0 | 0 | 0 | 0 |
| completion lock wait（read） | 13,427 | 64 | 32,768 | 32,768 |
| completion scan（read） | 1,261 | 1,024 | 1,024 | 4,096 |
| wake processing（read） | 121 | 64 | 128 | 128 |
| admission lock wait（write） | 9,998 | 64 | 32,768 | 32,768 |
| admission scan（write） | 947 | 512 | 1,024 | 2,048 |
| admission insert（write） | 1,285 | 64 | 2,048 | 16,384 |
| dependency wait（write） | 279,555 | 32,768 | 32,768 | 32,768 |
| completion lock wait（write） | 11,761 | 64 | 32,768 | 32,768 |
| completion scan（write） | 1,291 | 1,024 | 1,024 | 4,096 |
| wake processing（write） | 135 | 64 | 128 | 128 |

扫描长度直方图（同一代表样本）：

| bucket | admission read | completion read | admission write | completion write |
|---|---:|---:|---:|---:|
| 0 | 65,539 | 65,539 | 1 | 1 |
| 1-4 | 35 | 8 | 110 | 28 |
| 5-8 | 11 | 29 | 19 | 85 |
| 9-16 | 65 | 18 | 24 | 19 |
| 17-32 | 3,588,273 | 19,024 | 2,789,429 | 8,814 |
| 33-64 | 0 | 3,569,305 | 0 | 2,780,636 |
| >64 | 0 | 0 | 0 | 0 |

结论：wake processing 很小；主要代价是 admission/completion lock wait，且扫描大多落入 active list 的 17-32 或 33-64 桶。读路径 dependency wait 为 0，写路径有 dependency wait，但不是当前最大阶段。

## 6. Ordering A/B

`none` 只使用独立文件和非重叠访问，是诊断对照，不代表共享文件可以取消顺序保证。

| mode | read 1/8/32 IOPS | read p99 1/8/32 us | write 1/8/32 IOPS | write p99 1/8/32 us |
|---|---:|---:|---:|---:|
| ticket | 20,665 / 63,635 / 89,987 | 123 / 259 / 807 | 18,979 / 56,666 / 68,373 | 132 / 317 / 1,020 |
| range | 20,247 / 64,447 / 92,444 | 127 / 257 / 766 | 19,216 / 53,942 / 68,879 | 130 / 387 / 1,630 |
| none | 20,924 / 67,333 / 91,069 | 124 / 259 / 782 | 19,678 / 56,603 / 69,133 | 131 / 317 / 987 |

ordering 对 steady-state IOPS 的影响小于 completion/同步阶段的影响：range/none 在部分读点略高，但写 32-job 的 range p99 反而更差。当前不能因为 none 的独立文件结果更高就取消 ticket/range 顺序保证。

## 7. 后续优先级

1. **A：先处理 POSIX completion 边界。** T6-T7 在 32-job 达到读约 340us、写约 480us，T8-T9 也达到约 100us；下一轮应围绕 completion batching、wake 合并和 owner callback 串行边界做 A/B，但本任务按要求没有实现 reactor ring 或 completion 优化。
2. **B：ticket scheduler lock/scan。** admission/completion lock wait p95/p99 为约 32.8us，扫描长度集中在长 active list；应优先做锁等待与扫描成本的独立实验。
3. **C：验证 2/4 reactor 的自适应选择。** 本次绑核结果偏向 2 data reactors，但需要更多 CPU/queue depth 点确认，不能无限扩大 reactor 数。
4. **D：保留 ordering 语义。** ordering A/B 没有显示 ticket 是主导瓶颈，`none` 仅继续作为独立文件诊断对照。
5. **E：修复 runtime counter 统计并补单测。** 先让 queued/inflight 计数在所有 dispatch/batch 路径严格配对，再将 runtime queue peak 纳入下一轮归因。

## 8. 本次落地文件

- `src/hfs_main.c`：A3 起跑门、实际 worker/request/submit/outstanding 统计。
- `src/hfs_ticket.[ch]`：ordering 模式和 ticket admission/completion 细分计时。
- `src/hfs_io.c`：ticket/range/none ordering A/B 路由。
- `src/hfs_runtime.c`：owner reactor CPU 绑核参数。
- `src/hfs_stats.[ch]`：ticket avg/p50/p95/p99 与扫描长度直方图。
- [task09150832_benchmark.sh](E:/Code/HFS_BASE_SPDK/scripts/db/task09150832_benchmark.sh)：正式矩阵脚本。
- [task09150832_summary.py](E:/Code/HFS_BASE_SPDK/scripts/db/task09150832_summary.py)：FIO/stats/trace/vmstat 汇总脚本。
- `doc/bug.md`：条目 86，runtime queue peak 统计下溢隔离记录。
