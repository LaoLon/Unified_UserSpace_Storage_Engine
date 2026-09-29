# HFS 2.0 Stage A–D 合并回归（2026-09-29）

日期：2026-09-29
范围：Stage A–D 四个优化阶段合并后的功能与性能回归；同一 VM、同一设备、
同一构建（`HEAD 4243a05`，`/root/code/build-verify` Release）。
相关设计：`doc/design/hfs2.0.md`；优化台账：`doc/review/hfs2_architecture_optimization_review_20260928.md`。

## 1. 环境与范围

| 项目 | 值 |
|---|---|
| VM | 6 vCPU、约 7.2 GiB RAM、`HugePages_Total=1024` |
| 目标设备 | `VMware NVME_0002`，PCI `0000:13:00.0`，SPDK bdev `Nvme0n1` |
| 构建 | `HEAD 4243a05` 同步后 Release 重建；全量 CTest **29/29** |
| 口径 | 本文件是**缩减回归**：FIO 8j4 正式 gate 全量 5 轮；RocksDB/MySQL 用较小数据集与单线程档位；绝对值不跨会话比较 |

## 2. FIO 8j4 正式 gate（5 轮交错，中位数）

口径：4K `randrw` 50/50、256 MiB、10 s、`taskset -c 0-5`；HFS = 8 jobs × iodepth 4
（`hfs_client_depth=32`），raw = 1 job × iodepth 32。

| 侧 | 中位 IOPS | p99 读/写（µs） |
|---|---:|---|
| SPDK raw | 113043.3 | 561.2 / 561.2 |
| HFS | 82180.5 | 643.1 / 643.1 |

- **HFS/raw = 72.70%**（§16.2 目标 ≥85%，未达标）
- **p99 比值 = 1.146**（目标 ≤1.10，未达标）
- 每轮 HFS closure 完整（示例 `queued=819975=reaped`、`outstanding=0`、
  `token_mismatch=0`、`completed_error=0`）

## 3. RocksDB 缩减回归（32 线程，20 万 key，64 MiB block cache）

| 工作负载 | native ext4 | HFS | HFS/native |
|---|---:|---:|---:|
| fillseq | 108779 | 112341 | 103.3% |
| readseq | 6563 | 274901 | 4189%（native 冷读并发塌陷） |
| readrandom | 6429 | 92957 | 1446%（同上） |
| fillrandom | 111073 | 109570 | 98.6% |
| overwrite | 102703 | 110629 | 107.7% |
| readwhilewriting | 3148197 | 2829024 | 89.9% |

- HFS 侧 `hfsctl check` `corruption=none`；两侧 stderr 无 error/failed/corruption。
- 读侧大倍率是 native 在该 VM 上的已知冷读退化，不作为普适优势结论
  （见 `doc/performance/rocksdb_baseline.md` §5.1）。

## 4. MySQL 缩减回归（32 线程，10 表 × 1 万行，`trx=1`）

| 场景 | HFS TPS | native TPS | HFS/native | HFS p99（ms） | native p99（ms） |
|---|---:|---:|---:|---:|---:|
| read_write | 4193.0 | 4183.3 | 100.2% | 13.95 | 17.01 |
| read_only | 10288.3 | 10223.7 | 100.6% | 5.47 | 6.09 |
| write_only | 7461.3 | 8876.6 | **84.1%** | 9.39 | 12.75 |
| point_select | 258983.6 | 260445.4 | 99.4% | 0.34 | 0.35 |

- HFS 阶段 `hfsctl check`：`corruption=none`，checkpoint 与 journal 闭合。
- `write_only` 与历史基线一致（84.7–87.2%），差距仍在设备 barrier 往返；
  Stage C 的同步 ingress 针对的是回退/增长写路径，不改 trx=1 的 barrier 下限。

## 5. 结论

1. **功能与生命周期无回退**：CTest 29/29；FIO closure、RocksDB 错误扫描、
   MySQL/HFS check 全部干净。
2. **FIO 8j4 正式 gate 仍未通过**（72.7% / 1.146×），与历史 0.764/0.702 同档；
   Stage B→C 的同会话 A/B 提升（+6.6%）未改变该 gate 的达标状态。
   该 gate 的剩余差距仍是本文件之后的优化目标。
3. **RocksDB/MySQL 行为与既有基线一致**：写 ≈100%、读受设备/native 冷读影响、
   MySQL 纯写受 barrier 限制。
4. 缩减回归不能替代 §16.2 的完整矩阵与多盘 gate。

## 6. 与 `fio_baseline.md` 的对照（2026-09-29 追加）

**同会话回退检查（权威口径）**：pre-Stage-A 树（`/root/code-baseline`，即
`fio_baseline.md` 同代代码）vs 当前树，HFS-only、1 job × QD32、5/3 轮交错：

| 形状 | pre-A IOPS | 当前 IOPS | 当前/pre-A | p99 读/写比值 |
|---|---:|---:|---:|---:|
| 4K QD32 | 98478.6 | 102887.4 | **+4.5%** | 0.96 / 0.95 |
| 16K QD32 | 82698.4 | 84570.2 | **+2.3%** | 0.99 / 0.98 |

**跨会话与文档数字对照（仅趋势）**：

| 形状 | 文档 HFS | 当前 HFS | 文档 raw | 当前 raw | 文档 HFS/raw | 当前 HFS/raw |
|---|---:|---:|---:|---:|---:|---:|
| 4K QD32 | 91081 | ~103000 | 95986 | 114737 | 94.9% | ~90–91% |
| 4K QD64 | 96534 | 109312 | 94092 | 103224 | 102.6% | 105.9% |

结论：

1. **HFS 没有下降**：同会话 A/B 显示当前树比 pre-A 树快 4.5%（4K）/2.3%（16K），
   p99 同步改善 2–5%；Stage A–D 的净效果为正。
2. 文档与本会话的绝对值都上升（HFS +13%、raw +10–20%），说明设备/会话状态不同；
   4K QD32 比值从 94.9% 降到约 90–91% 是 raw 在本会话增益更大造成的跨会话假象
   （同代 pre-A 代码在本会话测得的 HFS 也从 91.1k 升到 98.5k），不构成回退证据。
3. 4K QD64 的比值反而从 102.6% 升到 105.9%，p99 比值从 ~0.88 改善到 ~0.81。
4. `fio_baseline.md` 自身已声明"不同会话或 VM 的绝对 IOPS 不直接比较回退"；
   本节对照只用于趋势判断，正式验收仍按 §2 的同会话口径。

证据：`/home/hfs/regression-check-preA-20260929`（pre-A vs 当前同会话 A/B）、
`/home/hfs/retest-1j32-20260929`（1j32 读写分离）、`/home/hfs/retest-2j32-20260929`
（QD64 2j32）。

## 7. 证据

- `/home/hfs/regression-stageabcd-fio-20260929/`：10 个计时 JSON + prep、closure stderr、`environment.txt`、`spdk-nvme.json`
- `/home/hfs/regression-stageabcd-rocksdb-20260929/`：4 个阶段 stdout/stderr、format 日志、`*-check.log`
- `/home/hfs/regression-stageabcd-mysql-20260929/`：2 阶段 initialize/prepare/4 场景日志、`*-results.tsv`、`*-hfs-check.log`、`environment.txt`
