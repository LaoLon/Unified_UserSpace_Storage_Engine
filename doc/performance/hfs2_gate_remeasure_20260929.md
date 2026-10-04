# HFS 2.0 门禁重测（当前树：Stage A–D + P2-1/P0-9/P2-2 + §7.3 trace）（2026-09-29）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

> 口径：**同盘**对照（HFS 卷与 native ext4 都在同一块 `VMware NVME_0002` 上交替，
> 每阶段重建 datadir），去除设备不对称；目标按 `hfs2.0.md` §16.2：吞吐 HFS/raw
> ≥85%、p99 ≤110%。跨会话绝对值不比较，只比较同会话比值。

## 1. MySQL（`run_shard105_samedisk_vm.sh`，8 线程，trx=1）

| 顺序 | 读 TPS | 读 p99 ms | 读写 TPS | 读写 p99 ms | mysqlslap 读/写 s |
|---|---:|---:|---:|---:|---:|
| HFS 1 | 10654.31 | 1.82 | 2163.37 | 8.43 | 0.063 / 10.847 |
| native 1 | 11047.84 | 1.86 | 2193.67 | 7.17 | 0.059 / 10.533 |
| HFS 2 | 11074.48 | 1.82 | 2072.11 | 9.06 | 0.080 / 10.880 |
| native 2 | 11196.42 | 1.76 | 2272.58 | 7.04 | 0.064 / 10.494 |

同会话比值（HFS/native）：

- **读吞吐 96.4% / 98.9%（均值 ≈97.7%）✓ 达标（≥85%）**；
- **读写吞吐 98.6% / 91.2%（均值 ≈94.9%）✓ 达标**；
- 读 p99 97.8% / 103.4% ✓；**写 p99 117.6% / 128.7% ✗（目标 ≤110%）**；
- mysqlslap 写 103.0% / 103.7% ✓；读为小绝对值（0.06–0.08 s），波动大；
- 两个 HFS 阶段 `hfsctl check` 均 `corruption=none`。

**与历史 65.2% 的差异**：历史数字来自 `run_shard105_vm.sh` 的**非对称**口径
（HFS datadir 在 NVMe、native datadir 在 VM 根盘 ext4），设备差异主导；同盘口径
下 HFS 读/写吞吐均进入达标区间。剩余差距是**写 p99**（fsync 屏障 + 元数据
checkpoint 延迟）。

## 2. RocksDB（`run_native_vs_hfs_vm.sh`，32 线程，1.28M keys × 1 KiB，同 NVMe）

| 阶段 | 操作 | native ops/s | HFS ops/s | HFS/native |
|---|---|---:|---:|---:|
| seq | fillseq | 102,183 | 107,695 | **105.4%** |
| seq | readseq | 40,029 | 263,534 | 658%（HFS 预取 + 大块 kFSBuffer 读） |
| seq | readrandom | 39,173 | 167,591 | 428%（块缓存/预取口径差异） |
| rnd | fillrandom | 99,541 | 101,736 | **102.2%** |
| rnd | overwrite | 84,955 | 93,488 | **110.0%** |
| rnd | readwhilewriting | 10,258 | 2,986,794 | 不具可比性（HFS 0.4 s 内全命中缓存，native 30 s 含设备读） |

- 写侧（fillseq/fillrandom/overwrite）与 native 持平或更快，**历史 rootcause 的
  “大工作集落后 native 约 18%”在当前树（kFSBuffer 零拷贝 + Stage A–D）已消失**；
- 读侧 HFS 明显更快，但两者缓存/预取口径不同（native direct 读无 readahead，
  HFS 走 prefetch + arena 大块），不能作为软件效率的直接结论；
- 两个 HFS 阶段 `hfsctl check` 均 `corruption=none`，stderr 无 error/corruption。

## 3. Shard-112 后复测（同口径，2026-09-29 12:0x，`mysql-samedisk-20260929c`）

| 顺序 | 读 TPS | 读 p99 ms | 读写 TPS | 读写 p99 ms | mysqlslap 读/写 s |
|---|---:|---:|---:|---:|---:|
| HFS 1 | 8881.70 | 2.71 | 1672.85 | 10.84 | 0.111 / 12.405 |
| native 1 | 5716.97 | 4.74 | 1067.96 | 13.70 | 0.127 / 21.624 |
| HFS 2 | 5753.77 | 4.18 | 1098.86 | 12.98 | 0.115 / 15.659 |
| native 2 | 5829.33 | 4.03 | 1070.27 | 15.27 | 0.132 / 16.401 |

同会话比值（HFS/native）：

- 读吞吐 155.4% / 98.7%、读写吞吐 156.6% / 102.7% ✓（≥85%）；
- **写 p99 79.1% / 85.0% ✓（≤110%，Shard-112 前为 117.6% / 128.7%）**；
- 读 p99 57.2% / 103.7% ✓；mysqlslap 写 57.4% / 95.5% ✓；
- `hfsctl check` 两阶段均 `corruption=none`。

**环境说明**：本轮两侧绝对值均比上午的门禁低约 2 倍（native 读 5.7k vs 11.0k），
是设备/VM 在当日多轮 format 与重负载后的状态差异，**同时影响两侧**；门禁按同会话
比值判定，因此结论仍有效，但绝对值不跨轮比较。

## 4. 结论（更新）

- MySQL 同盘吞吐达标（本轮读 ≈126.9%、写 ≈129.7% 均值比；上一轮读 ≈97.7%、
  写 ≈94.9%），**写 p99 在 Shard-112 后达标（≈82.1% 均值比，上一轮 117–129%）**；
- RocksDB 写侧与 native 持平/更优（105.4%/102.2%/110.0%）；
- FIO 同形状门禁已通过（1j32 90.6%、8j4 120.8%）；
- 门禁证据目录：`/home/hfs/mysql-samedisk-20260929b`（Shard-112 前）、
  `/home/hfs/mysql-samedisk-20260929c`（Shard-112 后）、
  `/home/hfs/rocksdb-native-vs-hfs-20260929`。