# task09151930 Reactor / Backend 分解与拓扑矩阵结果

Huizareth，任务已完成。测试日期为 2026-09-15，后端为 VM 内真实 NVMe `Nvme0n1`，配置为 `config/hfs_nvme.json`。本轮未修改 ticket scheduler、allocator、completion-wait 策略、open-file 或 metadata 路径；仅增加最小观测能力和 raw Blobstore 多 channel 对照。

## 1. 执行口径与完成性

- HFS 矩阵：fixed-affinity 与 deployment-optimization 两组；data reactors=1/2/3/4；4K randread/randwrite；jobs=8/16/32；每项 10 秒 ramp、30 秒 runtime、3 次取中位数，共 144 个样本。
- 环境语义：`HFS_DATA_REACTORS` 包含 owner reactor，因此 data reactors=1/2/3/4 分别对应 `HFS_DATA_REACTORS=2/3/4/5`；owner 为 reactor 0，数据 reactor 实际编号为 1..N。
- raw Blobstore：channels=1/2/4、总 QD=32，即 QD/channel=32/16/8，读写各 10 秒；另补采同口径 `/usr/bin/time` CPU。raw 与 HFS 使用隔离的 Blobstore 状态，避免 raw 初始化覆盖 HFS metadata。
- 完整性：FIO JSON=144，stats=144，trace=144，raw=6；所有样本 `errors=0`、`spdk_reject=0` 且 `spdk_submit=spdk_complete`。

## 2. Table 1：HFS reactor scaling

表中 T6-T7 为均值，p99 为 FIO latency p99，CPU 为 `/usr/bin/time` 的进程 CPU 百分比；每行是 3 次运行中位数。

### 2.1 Fixed affinity

固定 owner CPU0、应用 CPU5-7，数据 reactor 依次使用 CPU1..N；idle CPU 保持空闲。

| data reactors | jobs | read IOPS | read T6-T7 (us) | read p99 (us) | read CPU | write IOPS | write T6-T7 (us) | write p99 (us) | write CPU |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 8 | 55514.3 | 153.2 | 407.6 | 251% | 44353.5 | 193.0 | 954.4 | 245% |
| 1 | 16 | 66678.2 | 258.3 | 692.2 | 266% | 49473.4 | 373.7 | 2310.1 | 255% |
| 1 | 32 | 76943.5 | 458.2 | 1450.0 | 285% | 37919.1 | 906.7 | 5406.7 | 259% |
| 2 | 8 | 14859.1 | 662.2 | 4358.1 | 318% | 44908.5 | 209.8 | 585.7 | 351% |
| 2 | 16 | 16392.0 | 1251.1 | 6586.4 | 322% | 53029.6 | 342.4 | 2277.4 | 369% |
| 2 | 32 | 70181.3 | 489.7 | 3391.5 | 403% | 57274.7 | 621.1 | 3784.7 | 392% |
| 3 | 8 | 48945.4 | 156.6 | 1286.1 | 460% | 39231.8 | 212.3 | 1613.8 | 457% |
| 3 | 16 | 11436.6 | 1852.9 | 10289.2 | 417% | 49654.3 | 356.3 | 2637.8 | 475% |
| 3 | 32 | 54084.1 | 663.0 | 4948.0 | 465% | 51067.4 | 673.7 | 4112.4 | 496% |
| 4 | 8 | 47757.1 | 170.6 | 1171.5 | 561% | 38245.6 | 218.4 | 1761.3 | 555% |
| 4 | 16 | 41618.7 | 366.0 | 4358.1 | 569% | 42383.7 | 427.8 | 3063.8 | 570% |
| 4 | 32 | 10469.6 | 3951.2 | 16187.4 | 517% | 43723.6 | 821.5 | 4423.7 | 587% |

### 2.2 Deployment optimization

固定 owner CPU0；数据 reactor 使用 CPU1..N；应用使用剩余 CPU（d=1: 2-7，d=2: 3-7，d=3: 4-7，d=4: 5-7）。

| data reactors | jobs | read IOPS | read T6-T7 (us) | read p99 (us) | read CPU | write IOPS | write T6-T7 (us) | write p99 (us) | write CPU |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 8 | 43979.8 | 193.5 | 1073.2 | 248% | 44918.4 | 192.9 | 477.2 | 248% |
| 1 | 16 | 59552.5 | 276.4 | 733.2 | 263% | 16352.2 | 1251.8 | 4620.3 | 233% |
| 1 | 32 | 62059.8 | 536.6 | 1663.0 | 292% | 46961.0 | 811.1 | 2867.2 | 281% |
| 2 | 8 | 51941.4 | 154.6 | 419.8 | 372% | 38748.0 | 212.0 | 684.0 | 366% |
| 2 | 16 | 65193.9 | 254.7 | 700.4 | 391% | 47017.8 | 365.5 | 1155.1 | 378% |
| 2 | 32 | 64829.8 | 542.1 | 1613.8 | 426% | 51657.5 | 691.7 | 1810.4 | 418% |
| 3 | 8 | 46030.5 | 167.1 | 553.0 | 473% | 38316.8 | 227.9 | 823.3 | 467% |
| 3 | 16 | 56443.3 | 294.7 | 1003.5 | 489% | 43639.6 | 412.5 | 1318.9 | 477% |
| 3 | 32 | 56382.9 | 612.3 | 1892.4 | 510% | 45367.0 | 807.5 | 2146.3 | 499% |
| 4 | 8 | 44654.3 | 174.4 | 610.3 | 567% | 36194.1 | 227.3 | 806.9 | 557% |
| 4 | 16 | 52699.7 | 324.1 | 1155.1 | 581% | 39488.4 | 470.4 | 1515.5 | 568% |
| 4 | 32 | 53507.9 | 627.4 | 2039.8 | 595% | 36201.0 | 963.4 | 3981.3 | 573% |

fixed d=2 读侧有明显 run-to-run 波动：jobs=16 中位数仅 16392.0 IOPS、p99 6586.4 us。因此不能只按 reactor 数量做单调外推；deployment 组 d=2、jobs=16 是更稳定的综合点。

## 3. Table 2：raw Blobstore multi-channel 对照

总 QD 固定为 32；CPU 为补采的 `/usr/bin/time` 结果。

| channels | QD/channel | read IOPS | read avg (us) | read p99 (us) | read CPU | write IOPS | write avg (us) | write p99 (us) | write CPU |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 32 | 79083.4 | 404.57 | 1198.37 | 99% | 74693.1 | 428.36 | 599.19 | 99% |
| 2 | 16 | 99201.7 | 322.52 | 599.19 | 99% | 70381.3 | 454.60 | 1198.37 | 99% |
| 4 | 8 | 97110.6 | 329.46 | 599.19 | 99% | 68606.9 | 466.37 | 1198.37 | 99% |

raw 读侧 channels=2 最好，channels=4 没有继续提升；写侧 channels=1 反而最好。增加 backend channel/qpair context 不会自动带来线性扩展。

## 4. Table 3：per-reactor 分布（deployment d=2, jobs=16）

该拓扑为 deployment 组 jobs=16 的综合最佳点：读 65193.9 IOPS、写 47017.8 IOPS。submit 占比跨 3 次样本汇总；其他列为每个 reactor 的三次样本中位数。两侧约 53.5/46.5%，分布均衡，没有单一 reactor 热点。

| data reactor | read submit share | write submit share | read T6-T7 avg (us) | write T6-T7 avg (us) | read inflight avg/p95 | write inflight avg/p95 | read queue avg/p95 | write queue avg/p95 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 53.50% | 53.52% | 182.40 | 271.85 | 8/16 | 9/16 | 0/2 | 0/2 |
| 2 | 46.50% | 46.48% | 180.69 | 270.34 | 8/16 | 9/16 | 0/2 | 0/2 |

owner reactor 0 不承载数据 I/O。对应 3 次样本的 poll calls 中位数约为：读 reactor1/2 = 304.6M/334.1M，写 reactor1/2 = 285.4M/306.0M；inflight 与 queue 数据说明该点的主要差异不来自 queue credit 长时间堆积。

## 5. T6-T7 分解与 backend/channel 结论

在 deployment d=2、jobs=16 的最佳拓扑，TB0-TB1 约为读 0.79 us、写 0.88 us；TB1-TB2 约为读 1.21 us、写 1.38 us。同期 T4-T5 约为读 16.17 us、写 19.41 us，而完整 T6-T7 约为读 254.68 us、写 365.54 us。

结论是：submit 调用本身只有约 1--2 us，主要时间发生在 submit 后到 completion callback 的区间；当前 P0 仍是 backend completion / reactor poll propagation，而不是 `spdk_blob_io_*` 调用入口。当前 SPDK 构建没有产出可用的逐 I/O NVMe TN0/TN1 tracepoint 记录，故 TN1-TN0（设备/driver backend）与 TN2-TN1（driver 到 HFS callback propagation）不能被严格拆开；本轮采用 HFS TB0/TB1/TB2、per-reactor inflight/queue/poll telemetry 和 raw Blobstore 对照，不把 proxy 当作 NVMe 精确时延。

## 6. Q1--Q5 结论

**Q1：为什么 reactor=2 常优于 reactor=4？**

在 deployment 组，d=2/jobs=16 达到读 65193.9、写 47017.8 IOPS；d=4/jobs=16 降到读 52699.7、写 39488.4 IOPS，CPU 升至约 581%/568%。d=2 已经把请求均衡分到两个 data reactor，继续增加 reactor 只增加 SPDK poller、runtime message 和 channel/qpair 管理开销，而 NVMe backend 的有效并行度没有同步增加；fixed 组还叠加了应用 CPU 与 data CPU 的隔离/争用差异。

**Q2：HFS 与 raw Blobstore 的差距？**

raw 最佳读为 channels=2 的 99201.7 IOPS，最佳写为 channels=1 的 74693.1 IOPS；HFS deployment d=2/jobs=16 为读 65193.9、写 47017.8 IOPS。相对 raw 最佳点，HFS 约低 34%（读）和 37%（写），差距包含 POSIX/preload、每 syscall completion、buffer/descriptor 路径和 HFS runtime 调度开销，不能简单归因给 NVMe qpair。

**Q3：T6-T7 主要是 device latency 还是 completion poll delay？**

现有观测能确定 TB0-TB1/TB1-TB2 只有约 1--2 us，而 T6-T7 为约 255/366 us；主要等待在 submit 后。由于缺少可用的 NVMe CQ TN0/TN1 事件，本轮不能把剩余时间严格切成 device latency 与 poll propagation；下一步应优先让 SPDK trace 或最小 NVMe completion timestamp 打通，再比较 TN1-TN0 与 TN2-TN1。

**Q4：最佳并发应选 jobs=16 还是 32？**

jobs=16 是 throughput/latency 平衡点：deployment d=2 读 p99 700.4 us、写 p99 1155.1 us；jobs=32 的读写 p99 扩大到 1613.8/1810.4 us。jobs=32 虽然写吞吐略有上升，但尾延迟明显变差，因此 jobs=16 用于最佳拓扑，jobs=32 保留为 overload/tail-stress，不作为默认并发目标。

**Q5：A/B/C/D/E 重新排序？**

1. **A — backend completion / reactor poll path（P0）**：T6-T7 远大于 TB0-TB1、TB1-TB2，且 reactor 数量是最强拓扑信号。
2. **B — completion wait / callback propagation（P0/P1）**：当前无法用 SPDK TN0/TN1 完全拆分，应先补齐 timestamp，再决定是 device 还是 wake/poll 主导。
3. **C — POSIX bounce/copy staging（P1）**：保留为后续 copy 归因项，本轮只观察、不改 allocator 或 staging 策略。
4. **D — fd/metadata synchronization（P1）**：不是本轮 steady-state backend 主因，按任务要求暂不处理 open-file/metadata。
5. **E — ticket/range scheduler（降级）**：ticket 已完成并在本轮保持不变；不再把它作为 P0 主线，也不做 scheduler 重构。

## 7. 本轮修改与验证

- HFS trace 增加 TB0/TB1/TB2 和 submit/complete CPU 字段。
- HFS stats 增加 per-reactor TB、T6-T7、inflight、queue、poll busy/gap 统计；保留原有 submit/complete 守恒。
- raw Blobstore benchmark 增加 channels=1/2/4 对照。
- 修复任务汇总脚本 TB 字段名错误，并在 `doc/bug.md` 增加条目 87。
- VM 上已完成 Make/CMake Release、raw benchmark、144 个 FIO 样本与 summary 生成；正式样本均 `errors=0`，`spdk_submit=spdk_complete`。

原始汇总证据保存在 VM `/mnt/hfs-data/task09151930`；本地汇总副本用于生成本文档。
