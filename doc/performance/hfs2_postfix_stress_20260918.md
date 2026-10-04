# HFS 2.0 修复后压测与原生 SPDK 对比（2026-09-18）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

日期：2026-09-18  
设备：真实 `Nvme0n1`，PCI BDF `0000:0b:00.0`，`uio_pci_generic`，`vm.nr_hugepages=1024`  
构建：VM `/root/code/HFS_BASE_SPDK/build-repair98`（Release）  
证据：

- 正式 5-run gate：`/mnt/hfs-data/hfs2-postfix-gate-20260918`
- 压测矩阵：`/mnt/hfs-data/hfs2-stress-20260918`（`stress.tsv` + 各 case JSON/日志）
- 优化复测（§6）：`/mnt/hfs-data/hfs2-topology-20260918`、
  `/mnt/hfs-data/hfs2-postfix-gate2-20260918`、`/mnt/hfs-data/hfs2-teardown-check-20260918`
- 新正式口径 5-run gate（§7）：`/mnt/hfs-data/hfs2-postfix-gate3-20260918`

## 1. 结论

- 修复 #36（shard inode 槽位线性扫描）后，HFS 在正式 4K shared-file randrw 口径下
  达到 raw SPDK 的 `88.28%`（5-run 中位），首次通过 §16.2 的 ≥85% 吞吐门禁；
  读写 p99 为 raw 的 `1.144x`，仍略高于 1.10x 门禁。
- 压测矩阵显示：4K 读/写 HFS 为 raw 的 90.8%/95.2%，p99 差距 ≤4.3%；
  64K randrw 与 4K QD=128 场景 HFS 反超 raw（120.8%/120.2%），p99 也优于 raw。
- CPU：raw 约 `0.94` core，HFS 约 `3.2-3.4` cores；HFS 的 reactor 固定到 CPU
  0/1/2，会绕过 `taskset -c 2-3` 的限制，因此该 CPU 对比不是同核数口径。
- 内存：raw 峰值 RSS 约 `21 MiB`，HFS 约 `314 MiB`（含 `max_inodes=1M` 预分配的
  inode 槽位数组与 DMA arena 元数据），属后续可优化项。

## 2. 正式 5-run gate（4K randrw 50/50、256MiB、QD=32、10s）

- raw：SPDK fio plugin `spdk_bdev`，`--iodepth=32 --numjobs=1`；
- HFS：external `fio-hfs.so`，`--hfs_async=1 --numjobs=32 --iodepth=1`；
- 两者均 `taskset -c 2-3`、每次 HFS 前重新 format。

| 指标 | raw | HFS | 对比 |
|---|---:|---:|---:|
| 总 IOPS（5 次中位） | 66935.2 | 59091.4 | 88.28% |
| 读 p99 | 1253.4us | 1433.6us | 1.144x |
| 写 p99 | 1253.4us | 1433.6us | 1.144x |
| errors | 0 | 0 | PASS |
| closure | — | accepted=terminal=published，child_submit=child_complete，queued=reaped，outstanding=0，token_mismatch=0，peak=32 | PASS |

修复前同口径为 HFS `9012 IOPS`（raw `65650`，比值 `13.7%`），根因与修复见
`doc/bug.md` #36。

## 3. 压测矩阵（单次 10s，任务同口径）

| case | layer | IOPS | HFS/raw | 读 p99 (us) | 写 p99 (us) | cores | peak RSS (MiB) | err |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 4K randrw QD32 | raw | 88302 | — | 864 | 864 | 0.94 | 20.7 | 0 |
| 4K randrw 32j×1 | HFS | 82262 | 93.2% | 889 | 889 | 3.29 | 313.7 | 0 |
| 4K randread QD32 | raw | 106354 | — | 750 | — | 0.92 | 20.7 | 0 |
| 4K randread 32j×1 | HFS | 96525 | 90.8% | 782 | — | 3.35 | 313.7 | 0 |
| 4K randwrite QD32 | raw | 79733 | — | — | 930 | 0.94 | 20.7 | 0 |
| 4K randwrite 32j×1 | HFS | 75907 | 95.2% | — | 946 | 3.27 | 313.7 | 0 |
| 64K randrw QD32 | raw | 42037 | — | 1516 | 1532 | 0.92 | 20.8 | 0 |
| 64K randrw 32j×1 | HFS | 50762 | 120.8% | 1434 | 1466 | 3.16 | 314.0 | 0 |
| 4K randrw QD128 | raw | 87465 | — | 2605 | 2605 | 0.94 | 20.8 | 0 |
| 4K randrw 32j×4 | HFS | 105144 | 120.2% | 2572 | 2605 | 3.35 | 315.3 | 0 |

CPU/RSS 为按 `/proc/<pid>/task/*/stat` 与 `VmRSS` 采样的进程级统计；`cores` 为
采样窗口内 CPU 时间除以墙钟时间。

## 4. 口径与边界

1. **CPU 核数不同口径**：HFS `metadata_cpu=0`、`data_cpus=auto` 会把 metadata/data
   reactor 固定到 CPU 0/1/2，覆盖 `taskset` 初始亲和性；raw plugin 保持在 CPU 2-3。
   因此上表 CPU 列不能直接当作同核数实现成本对比。若要做 CPU gate 的公平对照，
   需要让两侧使用同一 cpuset（例如把 reactor 配到 2/3 并禁止越界 pin）。
2. **raw 基线波动**：同日更早的 repair98 gate raw 中位为 `108487 IOPS`，本轮为
   `66935`；设备/VM 状态差异明显。gate 比值取同场次测量，跨场次绝对值不可直接比较。
3. **p99 仍有差距**：正式口径 HFS 读写 p99 超门禁 4.4%；压测中 4K 读/写差距
   ≤4.3%，64K 与高 QD 场景 HFS 更优。
4. **RSS 差异**来自 `max_inodes=1,000,000` 的 inode 槽位预分配（每个 shard
   500k 槽）与 arena/元数据，不是请求路径泄漏；后续可按需懒分配或按容量收敛。
5. 本报告为修复后的非正式压测与对比，不替代 Shard-81 的正式五次 gate；正式报告
   仍需补齐 CPU、RSS、读写分离 p99 和统一拓扑后归档。

## 5. 复现命令（VM）

```text
# raw
taskset -c 2-3 env LD_PRELOAD=/root/code/HFS_BASE_SPDK/spdk/build/fio/spdk_bdev \
  /root/code/fio-3.42/fio --name=raw --ioengine=spdk_bdev --thread=1 \
  --spdk_conf=<gen_nvme.sh json> --filename=Nvme0n1 --bs=4k --size=256m \
  --rw=randrw --rwmixread=50 --direct=1 --iodepth=32 --numjobs=1 \
  --group_reporting=1 --time_based=1 --runtime=10 --output-format=json

# HFS（每次前 hfsctl format）
taskset -c 2-3 /root/code/fio-3.42/fio --name=hfs \
  --ioengine=external:/root/code/HFS_BASE_SPDK/build-repair98/fio-hfs.so \
  --thread=1 --hfs_config=/root/code/HFS_BASE_SPDK/config/hfs_nvme.conf \
  --hfs_async=1 --hfs_client_depth=32 --numjobs=32 --iodepth=1 --bs=4k \
  --size=256m --rw=randrw --rwmixread=50 --direct=1 --group_reporting=1 \
  --time_based=1 --runtime=10 --output-format=json
```

## 6. 优化复测：cpuset 亲和、inode 池化与拓扑对照（2026-09-18 晚）

本轮实现三项变更：

1. `spdk_runtime.cpp` 的 `pinToCpu` 在 pin 前检查线程当前 cpuset，目标 CPU
   越界时跳过并记录 `cpu pin to N skipped`，不再让 pin 覆盖 `taskset` 限制；
2. `async_data_service.cpp` inode 槽位池化：槽位数从 `max_inodes/shard` 改为
   `min(max_inodes/shard, operation_pool)`（默认 4096），`inodeCount` 仍按
   `max_inodes` 限流；RSS 从约 `314 MiB` 降到 `88-96 MiB`；
3. fio cleanup 对 `Engine::stop()` 有界重试、失败不再释放引擎（bug #37，修复
   停机期 data reactor UAF）。

拓扑矩阵（单次 10s，4K randrw 50/50，QD 合计 32，均 `taskset -c 2-3`）：

| case | IOPS | 占 raw | r_p99 (us) | cores | peak RSS (MiB) |
|---|---:|---:|---:|---:|---:|
| raw QD32 | 82955 | 100% | 995 | 0.92 | 20.7 |
| official conf 32j×1 | 57746 | 69.6% | 1384 | 2.75 | 95.8 |
| official conf 8j×4 | 61072 | 73.6% | 1909 | 2.78 | 90.1 |
| 1 reactor + pin 2/3，32j×1 | 75413 | 90.9% | 979 | 2.27 | 93.5 |
| **1 reactor + pin 2/3，8j×4** | **88472** | **106.7%** | **881** | **2.35** | **87.9** |
| 2 reactors + pin 2/3，32j×1 | 58610 | 70.7% | 1303 | 2.75 | 95.8 |
| 2 reactors + pin 2/3，8j×4 | 61772 | 74.5% | 1892 | 2.78 | 90.0 |

正式 5-run gate（gate2，同一 raw 基线 `83094 IOPS / 938us`）：

| 口径 | HFS IOPS（中位） | 占 raw | HFS p99（中位） | vs raw |
|---|---:|---:|---:|---:|
| official conf 32j×1 | 58902 | 70.9% | 1237.0us | 1.319x |
| 1 reactor + pin 2/3，8j×4 | 88646 | 106.7% | 856.1us | 0.913x |

要点：

- 在 2 核 cpuset 下，1 个 data reactor + 把 metadata/data 固定到 2/3 明显优于
  official conf 的 auto 拓扑（reactor 目标 CPU 越界后不受控地与其他线程争核）；
  8j×4（8 线程 × QD4）比 32j×1 更优，5 次运行 IOPS `87360-90732`、p99
  `815-864us`，吞吐与 p99 双双通过 §16.2 门禁。
- gate2 显示 official conf 口径比值 `70.9%`，但 HFS 绝对值为 `58902 IOPS`，与
  上一轮 gate 的 `59091 IOPS` 基本一致；差异来自 raw 基线从 `66935` 升到
  `83094`。比值门禁对 raw 场次状态敏感，跨场次比较应以同场次 raw 为准。
- 10 次 HFS 运行中有 2 次出现 `engine stop retried after: async data operations
  are still outstanding`，均在 200×10ms 重试内成功停机；该瞬态属 fio 并发
  cleanup 与 `Engine::stop` 的 `outstanding()` 检查窗口，已由 #37 的重试+不释放
  语义兜底，无崩溃。
- 结论：已采用 1 reactor + cpuset 对齐 + 8j×4 作为正式口径：
  `config/hfs_nvme.conf` 更新为 `data_reactors=1`、`metadata_cpu=2`、`data_cpus=3`，
  `doc/design/hfs2.0.md` §16.2 与 Shard-81 口径同步更新；official conf 原 32j×1
  口径保留为历史对比。item 3（唤醒批量化压 p99）暂缓，待有更大 p99 余量需求时再评估。

## 7. 新正式口径 5-run gate（gate3，official `config/hfs_nvme.conf`）

正式入口：`config/hfs_nvme.conf`（1 reactor、pin 2/3）+ HFS 8 jobs × iodepth 4，
raw 1 job × iodepth 32，其余同 §2；证据 `/mnt/hfs-data/hfs2-postfix-gate3-20260918`。

| 指标 | raw | HFS | 对比 |
|---|---:|---:|---:|
| 总 IOPS（5 次中位） | 78318.4 | 87837.1 | **112.15%** |
| 读 p99（中位） | 1044.5us | 888.8us | **0.851x** |
| 写 p99（中位） | 1056.8us | 897.0us | **0.849x** |
| errors | 0 | 0 | PASS |
| closure | — | accepted=terminal=published=904568，child_submit=child_complete，queued=reaped，outstanding=0，token_mismatch=0，peak=32 | PASS |

- 5 次 HFS 中 4 次 IOPS `84542-90448`、p99 `832-913us`；run=2 出现一次场次性
  抖动（`65861 IOPS`、p99 `1909us`，同场 raw 也回落到 `73605`、p99 `1401us`），
  中位与门禁不受影响；§6 已记录同类设备/宿主瞬态抖动现象。
- 本口径为 2026-09-18 优化复测后的正式结果，替代 §2 的 32j×1 gate 作为当前
  主 gate 证据；§2/§3 保留为历史对比与压测矩阵。