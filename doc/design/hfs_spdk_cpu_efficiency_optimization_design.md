# HFS 对比 SPDK 的 CPU 效率优化设计

> 日期：2026-09-16
>
> 适用架构：POSIX-only `LD_PRELOAD` HFS，真实 NVMe `Nvme0n1`
>
> 目标：在不弱化 POSIX、ordering 和 durability 语义的前提下，降低 HFS 的进程级 CPU 占用并保持当前吞吐。

## 1. 结论与推荐版本

当前最推荐的优化版本是：

1. 保持 8 vCPU 下 `owner + 2 data reactor` 的自动拓扑，不回退到 4 data reactor。
2. 保持应用线程默认使用 futex，不把 adaptive spin 设为默认值。
3. 第一优先实现 **可唤醒的 reactor adaptive idle**：有 backend inflight 时继续 busy poll；队列和 inflight 均为零时，owner/data reactor 进入有界 futex/condition wait，生产者提交新 batch 时立即唤醒。
4. 第二优先优化 **per-reactor batch credit accounting**：从逐请求获取 `batchLock` 改为批量更新 queue/inflight，并只在 credit 从满变为可用时唤醒生产者。
5. completion、ticket、staging 和 fd/metadata 只按新证据继续，不先做大范围重构。

该版本优先解决最新测量暴露的核心问题：HFS 已达到 raw SPDK 约 80%～91% 的吞吐，但消耗约 3.8～4.3 个 CPU 核；raw SPDK 约消耗 1 个核。现阶段的第一目标应从“继续堆吞吐”调整为“提高 IOPS/CPU”。

## 2. 文档与结果汇总

### 2.1 task90150022：建立可信的分层基线

任务与结果：[`task90150022.md`](../task/task90150022.md)、[`task90150022_result.md`](../task_result/task90150022_result.md)。

已完成：

- 建立 T0～T11 单调 profiling、parent/child profile、trace/stats 闭合检查。
- 建立 raw bdev、raw Blobstore、HFS internal 和完整 POSIX 五层对照。
- 4K/32-job 正式结果为 randread 92.2K IOPS、randwrite 70.9K IOPS。
- 发现 100% staging；但 copy 不是第一热点。

关键结论：

- T6～T7（SPDK submit 到 callback）最大。
- T8～T9（wake 到应用恢复）是首要软件同步边界。
- T4～T5 reactor ingress 有明显尾延迟，但 `messageSubmitLock` 单次成本较小。
- ticket 冲突很少，不应作为第一优化对象。

### 2.2 task09150832：并发、CPU 调度与 ticket A/B

任务与结果：[`task09150832.md`](../task/task09150832.md)、[`task09150832_result.md`](../task_result/task09150832_result.md)。

已完成：

- 修正 A3 worker 起跑和请求计数。
- 完成 jobs=1/2/4/8/16/32 scaling、CPU affinity、ticket/range/none A/B。
- 32-job 时 T6～T7 达到读约 340 us、写约 480 us；T8～T9 约 100 us。
- 两个 data reactor 相比四个 data reactor：读吞吐约 +24%、写吞吐约 +33%，CPU 同时从约 630%～657% 降到 428%～445%。

关键结论：

- 8 vCPU 上增加 reactor 会加重 CPU oversubscription、run queue 和 completion 调度延迟。
- ordering A/B 差异远小于 completion/reactor 差异，不能为性能删除 ordering 语义。
- jobs=16 是吞吐和尾延迟的平衡点；jobs=32 更适合作为压力点。

### 2.3 task09151930：reactor/backend 分解

任务与结果：[`task09151930.md`](../task/task09151930.md)、[`task09151930_result.md`](../task_result/task09151930_result.md)。

已完成：

- data reactor=1/2/3/4、jobs=8/16/32 的固定 CPU 与 deployment 两套矩阵。
- raw Blobstore channel=1/2/4 对照。
- per-reactor submit、inflight、queue、poll 和 T6～T7 统计。

关键结论：

- deployment 模式下 2 data reactor/jobs=16 是更稳定的综合点。
- raw Blobstore 读在 2 channel 最好，写在 1 channel 最好；4 channel 不产生线性收益。
- 两个 data reactor 的请求分布约 53.5%/46.5%，不存在单 reactor 严重倾斜。
- TB0～TB1 和 TB1～TB2 各约 1 us，长时间主要发生在 submit 后到 callback；但现有数据还不能严格区分设备时间与 completion propagation。

### 2.4 task09160035：Shard-66 短门禁

任务与结果：[`task09160035.md`](../task/task09160035.md)、[`task09160035_result.md`](../task_result/task09160035_result.md)。

已完成：

- 真实 NVMe 正确性回归通过。
- 修复 trace-off 轮询热路径无条件原子写。
- 旧五 reactor 布局首个 4K randread/32-job 样本仅 58,277.97 IOPS、595% CPU，触发 70K 停止线。

关键结论：

- 五 reactor 布局不应再作为部署默认值。
- 该轮没有完成两 reactor 与 adaptive wait 的正式 median，不能把门禁样本当成新基线。

### 2.5 task09160854：按 CPU 自动选择 reactor

任务与结果：[`task09160854.md`](../task/task09160854.md)、[`task09160854_result.md`](../task_result/task09160854_result.md)。

已完成：

- 自动拓扑公式已经落地并通过真实 NVMe 短回归。
- 8 vCPU 实际为 `total=3, data=2`，即一个 owner 加两个 data reactor。
- POSIX contract、fast-I/O smoke 和短 FIO 均通过。

注意：该结果中的 FIO `user/sys CPU` 来自 FIO 线程统计，不包含独立 HFS reactor 线程，不能用于 HFS 与 raw SPDK 的总 CPU 对比。

## 3. 当前 HFS 与 raw SPDK CPU 对比

### 3.1 测试口径

- VM：8 vCPU，真实 NVMe `0000:0b:00.0` / `Nvme0n1`。
- workload：`randrw=50/50`、256 MiB、1 秒 ramp、5 秒 runtime。
- HFS：完整 POSIX preload，`psync`，32 jobs，自动拓扑 `owner + 2 data reactor`。
- raw SPDK：官方 `spdk_bdev` FIO engine，单 job，QD32。
- CPU：`/usr/bin/time` 的进程级 user+sys CPU，包含 HFS 后台 pthread；不是 FIO JSON 的 `RUSAGE_THREAD` 字段。
- 证据目录：VM `/mnt/hfs-data/hfs-cpu-process-20260916-015757`。

### 3.2 结果

| 块大小 | HFS IOPS | raw SPDK IOPS | HFS/raw | HFS CPU | raw CPU | HFS 等效核数 | HFS IOPS/核 | raw IOPS/核 | HFS/raw 每核效率 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4K | 94,527 | 118,589 | 79.7% | 394% | 98% | 3.94 | 23,992 | 121,009 | 19.8% |
| 8K | 85,998 | 107,673 | 79.9% | 432% | 98% | 4.32 | 19,907 | 109,870 | 18.1% |
| 16K | 76,422 | 91,907 | 83.2% | 433% | 98% | 4.33 | 17,649 | 93,782 | 18.8% |
| 32K | 63,113 | 73,072 | 86.4% | 387% | 98% | 3.87 | 16,308 | 74,564 | 21.9% |
| 64K | 46,863 | 51,462 | 91.1% | 376% | 98% | 3.76 | 12,464 | 52,512 | 23.7% |

在 8 vCPU VM 上，HFS 进程约占整机 47%～54% 的 CPU 容量，raw SPDK 约占 12%。HFS 吞吐已经接近 raw SPDK，但每核效率只有 raw SPDK 的约 18%～24%。

### 3.3 结果边界

- 这是单次 5 秒诊断样本，不替换 [`hfs_final_performance_baseline_20260913.md`](../performance/hfs_final_performance_baseline_20260913.md)。正式结论仍需要同提交、干净工作树、5 次 median。
- 当前短测的远端 Git revision metadata 与本地 `main` HEAD 未完全对齐；正式性能门禁前必须同步当前提交并确认 VM `git status --porcelain` 为空。
- raw SPDK 64K workload 已产生 `error=0` JSON 和 time 数据，但退出清理阶段出现 FIO 残留并被终止；64K CPU 行只作诊断，正式矩阵必须干净重跑。
- 测试结束后 NVMe 已恢复到内核 `nvme` 驱动，`/dev/nvme0n1` 存在，无残留 FIO/SPDK 进程。

## 4. CPU 消耗来源判断

当前源码与实测共同支持以下判断：

1. `hfs-owner` 在 [`hfs_runtime.c`](../../src/hfs_runtime.c) 中无条件循环调用 `spdk_thread_poll()`；即使 steady-state 数据请求全部走 data reactor，owner 仍可持续占用一个核。
2. 两个 data reactor 在有 inflight 时必须持续 poll NVMe completion，这是当前吞吐的一部分，不能直接用长 sleep 替代。
3. data reactor 已有 `HFS_REACTOR_IDLE_BACKOFF_US`，但只在 `queueDepth=0 && inflight=0` 时执行 `nanosleep`，默认值为 0，而且 owner 没有等价的空闲机制。
4. 应用线程使用 futex 等待，不做默认 busy-spin；FIO JSON 只看到这些低 CPU worker，因此此前出现“HFS CPU 很低”的假象。
5. data batch 已经把多个请求合并为一次 SPDK message，但 dispatch 和 completion credit 仍按请求获取 `batchLock`，产生锁、cacheline 和条件变量成本。
6. 32 个同步应用线程会造成大量 futex wait/wake、run queue 和上下文切换；历史数据说明 16 jobs 的延迟更稳定。

因此，当前约 4 核 CPU 可以粗略理解为：一个持续轮询的 owner、两个 data poller，加上约一个核的应用线程、复制、锁和 futex 系统开销。这个分解需要 per-thread CPU 采样确认，但已经足以确定 owner 空转是第一项低风险优化。

## 5. 目标与约束

### 5.1 必须保持

- POSIX preload 是唯一支持入口，不恢复公开 async/DMA ABI。
- 不弱化 ticket/range ordering、fsync/fdatasync 或 crash recovery。
- 不用 Malloc/Aio 替代真实 NVMe 性能验证。
- 正式吞吐测试保持 trace-off/stats-off；归因样本单独运行。
- `spdk_submit=spdk_complete`、FIO `error=0`、所有 runtime credit 和 DMA lease 必须闭合。

### 5.2 第一阶段目标

以 4K randrw/32-job 为主要门禁：

- HFS IOPS 不低于当前样本的 97%，即至少约 91.7K。
- 进程 CPU 从 394% 降到不高于 320%，目标降幅至少 20%。
- p99 回退不超过 10%。
- IOPS/核至少提升 20%。

### 5.3 最终目标

- 4K randrw/32-job median 不低于 90K IOPS。
- 4K～64K 的 HFS/raw 吞吐比不低于当前同日对照的 95%。
- HFS 进程 CPU 目标不高于 300%，stretch 目标为 250%。
- 4K IOPS/核不低于 30K，较当前约提升 25%。
- 16-job 作为推荐并发点，32-job 保留为压力和尾延迟门禁。

## 6. 优化设计

### 6.1 阶段 0：先修正 CPU 测量合同

新增统一 CPU benchmark/summary：

- 使用 `/usr/bin/time` 记录整个 FIO/HFS 进程的 elapsed、user、sys 和 CPU%。
- 使用 `/proc/<pid>/task/*/stat` 或 `pidstat -t` 每 200 ms 记录 owner、data reactor 和 FIO worker 的线程级 CPU。
- 同时记录 VM run queue、context switch/s、在线 CPU 和频率。
- FIO JSON 的 `usr_cpu/sys_cpu` 只保留为 worker 指标，不用于总 CPU 门禁。
- 每个样本计算 `IOPS / (CPU% / 100)`，作为主要 CPU 效率指标。

输出文件建议为：

- `scripts/db/hfs_cpu_efficiency_benchmark.sh`
- `scripts/db/hfs_cpu_efficiency_summary.py`
- `/mnt/hfs-data/<run>/process-time.tsv`
- `/mnt/hfs-data/<run>/thread-cpu.tsv`
- `/mnt/hfs-data/<run>/summary.tsv`

### 6.2 阶段 1：可唤醒的 reactor adaptive idle

#### 状态模型

每个 owner/data reactor 使用三个运行状态：

```text
ACTIVE
  queue != 0 或 inflight != 0 或本轮 poll 有事件
  -> 连续 busy poll

QUIET
  poll 连续 N 次为空
  -> 重新检查 queue/inflight/wake sequence

PARKED
  queue == 0 且 inflight == 0
  -> futex/condition timed wait
  -> 新 batch/message、stop 或 timeout 后回到 ACTIVE
```

核心规则：

- 只要 backend inflight 非零，就禁止 data reactor 进入 PARKED，避免拉高 completion latency。
- producer 只在 batch 从“未调度”变为“已调度”时递增 `wakeSequence` 并唤醒 reactor；不为每个请求增加 wake syscall。
- reactor 在入睡前读取 sequence、再次检查 queue/inflight，再执行 wait，防止 lost wake。
- stop、channel 初始化/释放和 direct-message fallback 都必须递增 sequence 并唤醒。
- owner 使用有界 timeout 定期服务 SPDK timer/poller；data reactor 在 inflight=0 时可以更积极地 park。
- 新的 wake/park 字段独占 cacheline，避免与 queue/inflight 热字段互相抖动。

开关建议：

- 保留当前 busy poll 作为回退：`HFS_REACTOR_WAIT_MODE=busy`。
- 新实验模式：`HFS_REACTOR_WAIT_MODE=adaptive`。
- 复用或兼容 `HFS_REACTOR_IDLE_BACKOFF_US` 作为最大 park timeout；初始只测试 5/10/20/50 us，不直接改变默认值。

优先顺序：先给 owner 加 adaptive idle，再给 data reactor 加可唤醒 park。owner 不承载 steady-state data I/O，风险和预期 CPU 收益最好。

阶段门禁：

- owner-only 模式 CPU 至少下降 15%，IOPS 回退不超过 2%。
- owner+data 模式 CPU 累计下降至少 20%，IOPS 回退不超过 3%，p99 回退不超过 10%。
- 不满足门禁则默认继续 busy poll，仅保留实验开关。

### 6.3 阶段 2：批量 queue/inflight credit accounting

当前 `hfsRuntimeDispatchDataBatch()` 在每个 item 上获取 `batchLock`，逐项执行：

- `queueDepth--`
- `inflight++`
- completion 时再次获取锁执行 `inflight--`
- `pthread_cond_broadcast(queueAvailable)`

优化为：

1. consumer 一次锁住并 detach 整个 batch。
2. 在同一临界区按 batch size 执行 `queueDepth -= batchSize`、`inflight += batchSize`。
3. completion credit 改为原子递减；只有从满 credit 变为可用 credit 时才 signal producer。
4. 保持 bounded queue、direct fallback 和 shutdown drain 语义不变。
5. 不直接恢复多生产者向 SPDK ring 提交；bug #76 已证明该方向存在 reservation stall 风险。

阶段门禁：

- 在阶段 1 基础上 CPU 再下降至少 8%～10%。
- T4～T5 p99 不增加，IOPS 不下降。
- queue depth、inflight、submit/complete 和 shutdown drain 全部闭合。

### 6.4 阶段 3：completion wake 路径按证据精简

当前 completion 总是执行 `FUTEX_WAKE_PRIVATE`。可选设计是把 completion state 改为位标志状态机：`DONE`、`WAITER_ARMED`、`TIMED_OUT`，只有应用线程确实 armed 后才 wake。

该改动必须延后，原因是现有 stats 显示绝大多数请求确实进入 futex sleep，EAGAIN 比例极低；条件 wake 的收益可能有限，而 timeout/lifetime race 风险较高。

只有满足以下条件才进入实现：

- 系统调用/perf 证明 futex wake 或 context switch 占总 CPU 至少 5%。
- 能用状态机模型证明 completion-before-wait、wait-before-completion、timeout 和 shutdown 四类竞态无 lost wake。
- CPU 或 p99 至少改善 5%，且不增加总 CPU。

`HFS_WAIT_MODE=adaptive` 继续只作为实验开关。它可能降低低并发延迟，但会增加应用 CPU，不符合本轮“CPU 优先”的默认值目标。

### 6.5 阶段 4：可选的拓扑与数据路径优化

#### 单 data reactor A/B

保留当前两个 data reactor 默认。只做同日 A/B：

- total=2：owner + 1 data reactor。
- total=3：owner + 2 data reactor。

只有单 data reactor 保留至少 90% 的吞吐、CPU/IOPS 改善至少 15% 时才考虑用于较小 CPU 或低 QD 主机。历史结果显示一个 data reactor 在高并发下吞吐不足，因此不建议直接改默认值，也不建议在请求运行过程中动态迁移 stripe/channel。

#### staging/pool

在 reactor CPU 收口后，再检查：

- 16K/64K DMA pool lease wait 是否超过 3%。
- copy-in/copy-out CPU 是否超过进程 CPU 的 5%。
- 内部 DMA pool class 数量 A/B 是否提升 IOPS/核。

保持 POSIX-only，不增加公开 registered-buffer API。

#### ticket 与 fd/metadata

- ticket/range scheduler 保持现状；只有 admission+completion 超过请求时间 5% 才做 O(1) 空列表快路径或扫描早停。
- steady-state fd/open-file fast path 只有在新 trace 中 T1～T2 仍超过总延迟 10% 时再立项。
- durability 不参与本轮优化。

## 7. 验证矩阵

### 7.1 快速 A/B

每个改动先运行：

| workload | block size | jobs/QD | runtime | repeats |
|---|---:|---:|---:|---:|
| randrw 50/50 | 4K | HFS 32 / SPDK 32 | 5 s | 3 |
| randread | 4K | HFS 16/32 | 5 s | 3 |
| randwrite | 4K | HFS 16/32 | 5 s | 3 |

未达到阶段门禁立即回滚，不进入完整矩阵。

### 7.2 正式性能矩阵

- block size：4K/8K/16K/32K/64K。
- randrw=50/50、HFS 32 jobs、raw SPDK QD32。
- 10 秒 ramp、30 秒 runtime、5 次取 median。
- shared-file 与 per-job-file 分开报告。
- trace-off/stats-off 正式样本；另跑一个 attribution 样本。
- 报告 IOPS、avg/p99、process CPU、user/sys、线程 CPU、runq、ctx/s 和 IOPS/核。

### 7.3 correctness 门禁

- `make -B app lib` 与 CMake Release。
- `scripts/db/posix_contract_test.sh`。
- `scripts/db/io_fast_path_smoke.sh`。
- ticket/runtime/channel/DMA pool 专项。
- `io_trace_smoke.sh`：`trace_dropped=0`、parent/child 闭合、`spdk_submit=spdk_complete`。
- MariaDB 和 RocksDB 缩短 smoke、正常重启与 SIGKILL recovery。

## 8. 实施顺序与提交边界

1. 测量合同与 clean baseline，不改运行时。
2. owner adaptive idle，单独提交和 A/B。
3. data reactor 可唤醒 park，单独提交和 A/B。
4. batch credit accounting，单独提交和 A/B。
5. completion wake 仅在 perf 证据满足门禁时实施。
6. 完整正式矩阵通过后，才更新正式性能基线和默认开关。

每阶段都必须保留独立回退开关；阶段提交完成后合入 `main`，并在 `main` 上确认工作树、提交和 VM 回归状态。

## 9. 风险与停止条件

- 任何 IOPS 提升不足 2% 且 CPU/复杂度增加的方案停止并回滚。
- CPU 增加超过 10% 但 IOPS 提升不足 5%的方案停止。
- adaptive park 导致 p99 增加超过 10%或出现 shutdown 超时，恢复 busy poll 默认。
- 任何 submit/complete、credit、lease 不闭合立即停止，不输出性能结论。
- 不允许用 pure spin、增加 reactor、扩大 queue 或弱化 durability 掩盖调度问题。
- 不允许把 FIO JSON worker CPU 当成 HFS 总 CPU。

## 10. 预期收益

第一阶段仅让 idle owner 可唤醒休眠，理论上可回收接近一个轮询核心，使 4K HFS 从约 394% 降到约 300%～320%，同时保持约 95K IOPS。data reactor park 主要降低空闲或突发数据库负载 CPU，对持续 QD32 的收益有限。batch credit accounting 进一步降低锁和系统 CPU。

因此这套设计的现实目标不是让完整 POSIX HFS 立即达到 raw SPDK 的单核效率，而是先把当前约四核成本压到三核以内，并保持 80%～91% 的 raw SPDK 吞吐。完成后再依据 per-thread CPU 和 TN0/TN1 数据决定是否值得继续优化 backend poll 或 completion wake。
