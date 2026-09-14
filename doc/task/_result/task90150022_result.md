# task90150022 执行结果

执行对象：Huizareth
执行日期：2026-09-15
任务范围：P0-1 Profiling 修正、P0-2 五层数据路径基线；未进入 P0-3 或任何正式数据面优化。

## 1. 结论

本轮已经完成并验证：

- T0～T11 单调 profiling，以及每个 `HfsIoRequest` 独立 child profile 和 parent 聚合。
- `messageSubmitLock`、ticket、futex completion、DMA direct/staging、copy-in/copy-out 统计。
- A1 raw SPDK bdev benchmark、A2 raw SPDK Blobstore benchmark、A3 HFS internal data path benchmark。
- A4 完整 HFS POSIX 路径的 4K `psync`、`numjobs=32` 正式读写样本：每项 10 秒 ramp、30 秒 runtime、3 次取 median。
- A0 raw SPDK NVMe 已补做只读能力/短测：设备由 SPDK 通过 PCIe `0000:0b:00.0` 接管，`spdk_nvme_perf` 可用；未在 HFS 所有的裸设备上执行 raw randwrite，避免破坏当前文件系统状态。

关键判断：当前最值得优先处理的是每请求 completion 到应用线程恢复的同步边界，其次是 reactor ingress/message queue 和 POSIX open-file/inode 上下文；ticket 冲突在读路径为 0、写路径比例很低，staging copy 有成本但不是第一热点。

## 2. 环境与口径

| 项目 | 值 |
|---|---|
| HFS 基线提交 | `add07228a663ffefd528b280ec5896be2cbab4b6` |
| SPDK | `f0b63ed866f0e4d3c56e73bd4a98d0042956147a` |
| VM | Ubuntu，8 vCPU，1 NUMA node |
| CPU | Intel Core i5-14600KF |
| NVMe | PCI `0000:0b:00.0`，SPDK bdev `Nvme0n1` |
| PCI driver | `uio_pci_generic` |
| raw NVMe tool | `/root/code/HFS_BASE_SPDK/spdk/build/bin/spdk_nvme_perf` |
| hugepages | 1024 × 2 MiB |
| HFS POSIX workload | FIO `psync`，`--thread=1 --numjobs=32`，4K，`direct=0` |
| HFS mount prefix | `/fio/` |
| 正式 HFS 结果 | `/mnt/hfs-data/task90150022-formal-4k-32/` |
| profiling 结果 | `/mnt/hfs-data/task90150022-profile-copyticks/`、`...-write/` |

`--thread=1` 是必要的测试口径：项目已有 owner PID fork 防护，FIO 的 process-mode `numjobs=32` 会绕过 HFS 路由；线程模式仍然保留 32 个同步 POSIX worker。

## 3. P0-1 Profiling 实现

### T0～T11

| 阶段 | 定义 |
|---|---|
| T0 | POSIX API entry |
| T1 | fd/open-file/inode context ready |
| T2 | ticket/range admission ready |
| T3 | buffer ready；write 已完成 copy-in，read 已准备 direct/staging buffer |
| T4 | request enqueue/message submit complete |
| T5 | target reactor dequeues request |
| T6 | `spdk_blob_io_read/write` submit |
| T7 | SPDK completion callback enters |
| T8 | completion state publish and wake issued |
| T9 | application wait primitive returns |
| T10 | read copy-out、request cleanup、ticket completion complete |
| T11 | POSIX API return |

`src/hfs_profile.[ch]` 使用 stack-owned parent profile；每个 `HfsIoRequest` 保存自己的 `HfsIoChildProfile`，child 在跨线程 enqueue/dequeue/submit/complete/wake/wait-return 边界写入私有 tick，再由 parent 以 first/last 规则聚合，避免 pipeline、vector 和 RMW child 互相覆盖。

采样模式采用 `HFS_TRACE_SAMPLE=100`，无 parent 的普通事件和未命中 parent/request 的事件统一丢弃；每线程 ring 扩大到 16384。最终 probe 均满足：

```text
profile_invalid=0
trace_dropped=0
spdk_submit=spdk_complete
errors=0
```

### 4K / 32-job profiling 的阶段 p99（timing on）

profile tick frequency 为 2.5 GHz；下表单位为微秒，完整 p50/p95/p99 输出由 `scripts/db/hfs_profile_summary.py` 生成。

| 阶段 | randread p99 | randwrite p99 |
|---|---:|---:|
| T0-T1 | 3.330 | 1.888 |
| T1-T2 | 247.249 | 207.577 |
| T2-T3 | 12.081 | 16.204 |
| T3-T4 | 16.335 | 12.420 |
| T4-T5 | 204.594 | 331.185 |
| T5-T6 | 0.912 | 0.927 |
| T6-T7 | 894.028 | 1,153.564 |
| T7-T8 | 8.508 | 9.703 |
| T8-T9 | 309.711 | 271.534 |
| T9-T10 | 334.627 | 246.814 |
| T10-T11 | 13.268 | 12.928 |

样本数分别为 randread 2657、randwrite 2133，均 `profile_invalid=0`、`trace_dropped=0`。

### 分层统计

下表来自对应 timing-on stats 快照；`*_ticks` 保留程序输出字段名，message/ticket/futex 累计时长由 stats 的 monotonic-ns 计时器记录，profile/copy ticks 使用 SPDK tick。

| 指标 | randread | randwrite |
|---|---:|---:|
| `spdk_submit / complete` | 265816 / 265816 | 213447 / 213447 |
| `message_submit_lock_acquire` | 200340 | 149380 |
| owner/data message | 81 / 200259 | 97 / 149283 |
| lock wait p50/p95/p99 ns | 32 / 128 / 4096 | 32 / 64 / 1024 |
| lock hold p50/p95/p99 ns | 256 / 512 / 2048 | 256 / 512 / 1024 |
| ticket conflict/dependency/real wait | 0 / 0 / 0 | 425 / 425 / 425 |
| ticket scan items | 7651226 | 6290887 |
| active ticket peak | 32 | 32 |
| futex attempt/success/EAGAIN/EINTR | 265777 / 265750 / 27 / 0 | 213436 / 213417 / 19 / 0 |
| completion-to-wake ticks | 704511918 | 589843886 |
| wake-to-application-resume ticks | 59136995924 | 42440533790 |
| DMA direct / staging count | 0 / 265728 | 0 / 213343 |
| copy-in ops/bytes/ticks | 0 / 0 / 0 | 213343 / 873852928 / 236121273 |
| copy-out ops/bytes/ticks | 265728 / 1088421888 / 607623369 | 0 / 0 / 0 |

读写都没有走 direct caller buffer；当前 workload 100% 使用 staging。写路径出现少量 ticket dependency，读路径没有真实 ticket wait。

## 4. P0-2 五层基线

以下 A1/A2/A3 为 4K、3 秒、单次代表性测量，用于层间归因；A2 的 qd1/8/32/64 已覆盖读写。它们不是 A4 的 30 秒正式吞吐基线。

### A1 raw SPDK bdev

| QD | randread IOPS | randwrite IOPS |
|---:|---:|---:|
| 1 | 35,852.67 | 32,859.33 |
| 8 | 111,951.67 | 92,357.67 |
| 32 | 147,518.00 | 104,818.33 |
| 64 | 144,897.00 | 97,850.00 |

所有样本 `errors=0`。实现文件为 `scripts/db/task90150022_bdev_bench/`。

### A2 raw SPDK Blobstore

| QD | randread IOPS | randwrite IOPS |
|---:|---:|---:|
| 1 | 35,120.00 | 32,317.33 |
| 8 | 105,972.33 | 87,248.67 |
| 32 | 139,180.33 | 96,398.67 |
| 64 | 136,630.33 | 91,467.33 |

所有样本 `errors=0`；benchmark 使用真实 `spdk_blob_io_read/write`，block size 会按 blob io unit 对齐。

### A3 HFS internal data path

直接调用 `HfsFile` 的 positioned read/write，保留 HFS ticket/reactor/blob 数据路径，绕过 preload、fd lookup 和 POSIX metadata。4K、3 秒结果如下：

| worker threads | randread IOPS | randwrite IOPS |
|---:|---:|---:|
| 1 | 83,178.84 | 65,316.68 |
| 8 | 62,616.71 | 53,434.51 |
| 32 | 83,178.84 | 65,316.68 |
| 64 | 91,749.99 | 69,912.53 |

所有有效样本 `errors=0`。A3 benchmark 由 `HFS_INTERNAL_BENCH=1` 触发，位于 `src/hfs_main.c`。

### A4 完整 HFS POSIX

正式 trace-off 口径：4K、`psync`、32 个线程、10 秒 ramp、30 秒 runtime、每个读写方向 3 次。

| workload | run 1 | run 2 | run 3 | median |
|---|---:|---:|---:|---:|
| randread IOPS | 91.3k | 92.2k | 94.3k | **92.2k** |
| randwrite IOPS | 70.6k | 71.0k | 70.9k | **70.9k** |

6 个正式样本均 `err=0`，FIO `short=0`，`dropped=0`。原始文件为 VM 的 `/mnt/hfs-data/task90150022-formal-4k-32/randread-r{1,2,3}.txt` 和 `randwrite-r{1,2,3}.txt`。

### A0 与测试隔离说明

此前结果文档因检查了错误的 SPDK 构建路径，将 A0 误判为 `SKIP/ENVIRONMENT`；本次复核已纠正。PCI `0000:0b:00.0` 的内核绑定为 `uio_pci_generic`，因此系统不会创建 `/dev/nvme0n1`，这正是 SPDK 用户态接管 NVMe 后的预期现象；设备本身可由 raw SPDK 工具直接枚举。

使用 `/root/code/HFS_BASE_SPDK/spdk/build/bin/spdk_nvme_perf -q 32 -o 4k -w randread -t 3 -r 'trtype:PCIe traddr:0000:0b:00.0'` 完成只读短测，结果为：

```text
Attached to NVMe Controller at PCIE (0000:0b:00.0) [15ad:07f0]
Total: 383038.74 IOPS, 1496.25 MiB/s, average latency 83.53 us
command exit: 0
```

本次没有在 HFS 当前持有的裸 NVMe 上执行 raw randwrite；完整 A0 读写矩阵仍不属于本轮已完成的正式基线。

A2 调用 `spdk_bs_init()`，按 SPDK 语义会在底层设备上初始化新的 Blobstore super block/metadata；不能在 HFS 已格式化的同一 NVMe 状态上运行后继续直接挂载 HFS。实际执行顺序为：A2 在 raw Blobstore 状态采样，随后 `HFS_FORMAT=1` 重新建立 HFS，再执行 A3/A4。该问题已记录为 `doc/bug.md` 条目 85。

## 5. 由数据得到的后续优先级

在不改变 POSIX correctness 的前提下，建议顺序调整为：

1. **P1-2 completion wait**：T8-T9 平均约 90～98 µs，且 wake-to-application-resume 累计远高于 completion callback 到 wake，说明同步 POSIX 边界是首要软件热点。
2. **P0-3 Reactor Ingress**：T4-T5 p99 为 205～331 µs；messageSubmitLock 单次 p99 只有 1～4 µs，但消息排队/目标 reactor 调度整体仍有明显尾延迟。
3. **P1-1 steady-state open-file fast path**：T1-T2 p99 为 208～247 µs，说明 fd/open-file/inode context 和相关路径需要压缩，但优先级低于 completion wait。
4. **P1-3 staging copy**：当前直通为 0、读写全部 staging；copy tick 累计可测，但单次 memcpy 相对 T6-T7 和 T8-T9 较小，应在前两项优化后用 A/B 验证。

ticket scheduler 暂不作为第一优化点：randread conflict/dependency/real-wait 全为 0，randwrite 也只有 425 次真实等待；ticket admission scan 仍需保留在后续分析中，但不能仅依据 `ticket_conflict=0` 推断 ticket 没有成本。

## 6. 验证与已知未完成项

已通过：

- VM `make -B app lib`。
- VM CMake Release configure/build。
- A1/A2 benchmark 强制编译。
- A4 profiling：`profile_invalid=0`、`trace_dropped=0`、SPDK submit/complete 闭合、errors=0。
- A0 raw SPDK NVMe 只读短测：设备枚举成功，命令正常退出，`383038.74 IOPS`、`1496.25 MiB/s`。
- `scripts/db/posix_contract_test.sh`。
- `scripts/db/io_fast_path_smoke.sh`。

本任务明确要求的完整 16K/64K、`numjobs=1/8`、每项 3 次的 HFS 30 秒矩阵，以及 A1/A2/A3 在全部块大小上的扩展矩阵，本轮没有冒充已完成；当前文档把它们保留为下一次性能采样工作，而不是据此启动 P0-3 架构重构。

## 7. 主要改动文件

- `src/hfs_profile.[ch]`：parent/child profile 与 T0～T11 聚合。
- `src/hfs_trace.[ch]`：parent profile trace、采样闭合、16384 ring。
- `src/hfs_stats.[ch]`：message/ticket/futex/copy/direct/staging/ticks 统计。
- `src/hfs_runtime.[ch]`、`src/hfs_io.c`、`src/hfs_lib.c`、`src/hfs_ticket.c`：埋点接入。
- `scripts/db/hfs_profile_summary.py`：profile 单调性与分位数汇总。
- `scripts/db/task90150022_bdev_bench/`：A1 raw bdev benchmark。
- `scripts/db/task90150022_blob_bench/`：A2 Blobstore benchmark。
- `src/hfs_main.c`：A3 internal data-path benchmark。
- `doc/bug.md`：新增条目 81～85，记录本轮发现和修复/规避过程。
