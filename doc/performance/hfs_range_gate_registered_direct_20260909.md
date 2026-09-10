# rangeGate 统计与 registered-DMA/direct async 复测

日期：2026-09-09  
环境：VM SPDK NVMe（`Nvme0n1`）及 Aio 持久化 smoke  
代码构建：`make -B lib -j2`，结果通过

## 1. rangeGate wait/hold 统计

此前 `HFS_STATS_TIMING=1` 只统计小范围请求的 `HFS_STATS_LOCK_IO` bucket，达到
`HFS_IO_RANGE_GATE_THRESHOLD` 的请求直接获取 `runtime->rangeGate`，导致 64 KiB
FIO 样本把真实 gate 等待显示成零。

本轮新增独立统计类别 `range_gate`，快照字段为：

- `lock_range_gate_wait_count`
- `lock_range_gate_hold_count`
- `lock_range_gate_contention`
- `lock_range_gate_wait_ns` / `lock_range_gate_hold_ns`
- 对应 16 桶 wait/hold histogram

同步阻塞路径和 direct async try-lock 成功路径均记录 wait/hold；try-lock 失败会取消
未完成的 timing frame，避免污染下一次请求。

### NVMe 64 KiB 样本

口径：HFS `numjobs=32`、`psync`、`randrw 50/50`、`runtime=15s`、`size=256m`、
`HFS_STATS_TIMING=1`，设备为 SPDK `Nvme0n1`。快照和 FIO JSON 位于 VM：
`/var/tmp/hfs-range-gate-check-20260909/`。

| 指标 | 值 |
|---|---:|
| FIO total IOPS | 3,697.43 |
| FIO read/write p99 | 38,535.17 / 39,583.74 us |
| rangeGate wait count | 59,588 |
| rangeGate hold count | 59,588 |
| rangeGate contention | 53,900 |
| rangeGate wait total | 465,007,278,381 ns |
| rangeGate hold total | 6,834,935,917 ns |
| SPDK submit/complete | 59,757 / 59,757 |
| errors | 0 |
| bounce copy ops/bytes | 59,587 / 3,905,093,632 |
| direct ops/bytes | 0 / 0 |

累计 wait 约为 7.80 ms/次，而 hold 约为 115 us/次；这解释了 64 KiB 样本的高 p99，
也说明瓶颈位于同一 inode 的 gate 排队，而不是 SPDK 请求放大（submit/complete 已闭合）。
该样本打开了 timing，IOPS 仅用于归因，不能与 stats-off 正式性能表直接横比。

## 2. registered-DMA/direct async

执行：

```bash
HFS_STATS_PATH=/var/tmp/hfs-registered-direct-20260909/stats \
HFS_STATS_TIMING=1 \
bash scripts/db/registered_direct_window_test.sh \
  build/lib/libhfs.so config/hfs_db_aio.json \
  /var/tmp/hfs-registered-direct-20260909/data.img \
  /var/tmp/hfs-registered-direct-20260909/run
```

结果：

- `REGISTERED DIRECT WINDOW OK bytes=524288 copy_ops=0 registered=1`
- trace：`registered_children=4 copies=0`
- `io_copy_ops=0`、`io_copy_bytes=0`
- `io_direct_ops=2`、`io_direct_bytes=1048576`（512 KiB write + 512 KiB read）
- `data_reactor_2_submit/complete=6/6`
- `lock_range_gate_wait_count/hold_count=3/1`，无错误

同时执行 `registered_dma_test.sh`，注册/重复注册、异步期间 unregister 返回 `EBUSY`、
completion 后 unregister 以及数据校验全部通过，输出 `REGISTERED DMA IO OK`。

## 3. 结论

rangeGate 是普通 FIO bounce 路径在大范围请求下的主要软件排队点；该埋点修复使 wait/hold
归因完整。registered-DMA/direct async 已证明可以绕过 bounce copy，下一阶段应在保持注册
生命周期约束的前提下，增加可持续的 direct async 并发基准，再评估 gate 粒度、请求窗口和
每 inode 并行度的优化收益。
