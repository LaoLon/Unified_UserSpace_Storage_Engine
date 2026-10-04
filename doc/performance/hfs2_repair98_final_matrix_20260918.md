# HFS 2.0 Repair-98 最终修复矩阵与性能 gate

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

日期：2026-09-18  
状态：修复实现与验证矩阵完成；正式性能 gate 仍失败，不宣称 Shard-81 性能通过

## 范围与环境

本报告收口 Repair-93～Repair-98：

- Repair-93：Release 构建不再默认对 `fio-hfs` 使用 `-O0 -g`；
- Repair-94：trace format v2、TSC 校准、请求级 shard/bdev/parent 关联；
- Repair-95：FIO 外部 completion 使用统一的 Client wait path；
- Repair-96：固定容量 request lookup、精确 token/generation 回收；
- Repair-97：`hfs_file_per_job=1` 的 per-worker 文件拓扑诊断；
- Repair-98：真实 NVMe 拓扑证据、全量回归和五次 raw/HFS gate。

本轮 Windows 工作区提交为 `197eccc`（Repair-97）。VM 使用工作区同步的 HFS
源码；SPDK/FIO plugin 只在 VM 内维护。正式设备和环境为：

```text
device:       Nvme0n1
PCI BDF:      0000:0b:00.0
driver:       uio_pci_generic during test, nvme after release
hugepages:    1024
CPU:          2-3
FIO:          3.42
build:        CMake Release, HFS_FIO_DEBUG=OFF
```

Repair-98 期间发现 VM 上已有 SPDK FIO plugin 与当前 fio 3.42 的 ABI/build 状态
不一致：raw plugin 读取不到 `thread=1`。在 VM 内执行当前 SPDK checkout 的
`./configure --with-fio=/root/code/fio-3.42 && make -j2` 后，global/job/both 三种
thread 参数布局的 raw smoke 均通过。正式 raw 命令将 `LD_PRELOAD` 只施加到 FIO
进程，不施加到 `taskset`。

## 全量回归

Repair-98 Release 构建和回归结果：

```text
CTest:                24/24 passed
spdk_runtime_test:    10/10 passed
hfsctl .conf validate: passed
Release compile DB:   no unexpected -O0
```

## Repair-97 真实 per-job 拓扑证据

拓扑诊断使用临时配置：`data_reactors=2`、`trace=true`、`trace_sample=64`，
`hfs_file_per_job=1`、8 个 FIO worker、4 KiB randread、2 秒。每次 format 后
使用根目录文件 `/repair98-topology.dat`，避免依赖 format 后不存在的 `/fio` 父目录。

结果目录：

```text
/mnt/hfs-data/hfs2-repair98-final-20260918-103719
```

关键结果：

```text
trace files:                 64
trace records:               20646
manifest:                    format=2, closed=true, dropped=0
ShardMap records:            14
distinct file ids:           7
distinct file/shard pairs:   7
distinct shards:             2 (shard 0 and shard 1)
```

`hfs_trace_dump` 同时输出 request/parent/shard/bdev 四段延迟，且
`recorded - dropped == file records` 成立。Topology FIO closure 为
`accepted=921=terminal=published`、`child_submit=child_complete=921`、
`queued=reaped=921`、`outstanding=0`、`token_mismatch=0`。因此 per-job 文件确实
打开了多个 HFS file 并覆盖两个 shard；这只是拓扑和归因证据，不替代 shared-file
正式 gate。

## 正式五次 raw/HFS gate

两侧均使用真实 NVMe、CPU 2-3、4 KiB `randrw` 50/50、256 MiB、10 秒、五次。

- raw：SPDK FIO plugin、`config/hfs_nvme.json`、`iodepth=32`、`numjobs=1`；
- HFS：`config/hfs_nvme.conf`、external `fio-hfs.so`、`iodepth=1`、
  `numjobs=32`、`hfs_client_depth=32`、`hfs_file_per_job=0`；
- trace/stats：关闭；
- HFS 每次运行前 destructive `hfsctl format --yes`；
- FIO 文件：根目录 `/repair98-shared.dat`，因为正式 format 后 namespace 只有 root。

| layer | run | total IOPS | read p99 (us) | write p99 (us) | errors | HFS queued=reaped |
|---|---:|---:|---:|---:|---:|---:|
| raw | 1 | 108812.719 | 651.264 | 659.456 | 0 | — |
| raw | 2 | 109503.750 | 634.880 | 643.072 | 0 | — |
| raw | 3 | 108051.695 | 651.264 | 659.456 | 0 | — |
| raw | 4 | 108486.751 | 667.648 | 675.840 | 0 | — |
| raw | 5 | 107962.804 | 643.072 | 651.264 | 0 | — |
| HFS | 1 | 69049.980 | 3555.328 | 3555.328 | 0 | 690776 |
| HFS | 2 | 68799.580 | 3620.864 | 3620.864 | 0 | 688271 |
| HFS | 3 | 53378.249 | 3686.400 | 3686.400 | 0 | 533996 |
| HFS | 4 | 61086.774 | 3620.864 | 3653.632 | 0 | 611051 |
| HFS | 5 | 65667.333 | 3588.096 | 3555.328 | 0 | 656936 |

中位数：

```text
raw median total IOPS:       108486.751325
HFS median total IOPS:        65667.333067
HFS/raw IOPS ratio:             0.605303  (60.5303%)
raw median p99:                  655.360 us
HFS median p99:                 3620.864 us
HFS/raw p99 ratio:                 5.525x
```

五次 HFS 的 async closure 均满足：

```text
accepted = terminal = published = child_submit = child_complete
queued = reaped
outstanding = 0
completed_error = 0
token_mismatch = 0
plan_fail = 0
prealloc_failure = 0
```

设备在测试结束后已恢复为 kernel `nvme` 驱动，`/dev/nvme0n1` 可见。

## Gate 判定

| 条件 | 判定 | 证据 |
|---|---|---|
| HFS/raw total IOPS >= 85% | FAIL | `60.5303%` |
| read/write p99 <= raw × 110% | FAIL | `5.525x` |
| raw/HFS errors == 0 | PASS | 十次 JSON 均为 0 |
| HFS accepted/terminal/published 闭合 | PASS | 五次均相等 |
| child submit/complete 闭合 | PASS | 五次均相等 |
| queued/reaped 闭合 | PASS | 五次均相等 |
| outstanding/token mismatch 为 0 | PASS | 五次均为 0 |
| per-job topology 覆盖两个 shard | PASS | `ShardMap` 14 条、shard 0/1 |
| trace manifest 完整 | PASS | format v2、closed、dropped=0 |
| NVMe 释放与环境恢复 | PASS | `uio_pci_generic` → `nvme` |

结论：Repair-93～98 的修复实现、错误/生命周期证据、真实 NVMe 拓扑验证和正式
五次 gate 已完成；但性能目标本身仍未达到，Shard-81 正式性能 gate 继续保持
未完成。当前数据能够确认问题不是请求丢失、completion 泄漏、token 错配、错误
计数或设备绑定问题；不能仅凭本矩阵把剩余 39.47% 吞吐损失和 4.525 倍额外 p99
归因到单一代码阶段。后续性能修复必须以本目录的 raw/HFS JSON、closure、trace
和固定命令为基线。
