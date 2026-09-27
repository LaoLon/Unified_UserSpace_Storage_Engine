# HFS 2.0 MySQL 基线：原生 ext4 对比 HFS（sysbench 16/32/64 线程）

日期：2026-09-27 会话（UTC 17:00:10–17:08:34，本地 2026-09-28 01:00–01:09）。
范围：同一块 NVMe 上，同一个 MySQL 8.0.46 二进制分别使用 **HFS 卷**与
**native ext4 datadir**，用 sysbench 1.0.20 准备 10 张表、每张 1 万行，按
读写混合、随机读、纯写、主键点查四类场景，在 16/32/64 线程下各跑 10 s，
并采集 P50/P95/P99 与 redo fsync 计数。本文件只描述本会话结果。

相关矩阵：FIO 见 `doc/performance/fio_baseline.md`，RocksDB 见
`doc/performance/rocksdb_baseline.md`；三者共用本会话设备与构建。

## 1. 环境与口径

| 项目 | 值 |
|---|---|
| VM | 6 vCPU、约 7.2 GiB RAM、`HugePages_Total=1024` |
| 目标设备 | `VMware NVME_0002`，PCI `0000:13:00.0`；HFS 用 `config/hfs_mysql.conf`（`max_group_commit_wait_us=0`），native 用同盘 ext4 |
| MySQL | 8.0.46（VM 内源码构建，`/home/hfs/mysql-build`） |
| sysbench | 1.0.20（系统包），0 数据预热 |
| 关键参数 | `innodb_flush_log_at_trx_commit=1`、`innodb_use_native_aio=OFF`、禁用 MyISAM、`--skip-networking` |
| 证据目录 | `/home/hfs/mysql-native-vs-hfs-20260927`（67 个日志/结果文件） |

口径：

- 每阶段重建 datadir（native 重新 `mkfs.ext4`；HFS 重新 `format` 并重建
  `/home/hfs/mysql-native-vs-hfs-hfs-datadir`），然后
  `sysbench oltp_read_write prepare --tables=10 --table-size=10000`（已验证
  `sbtest1..sbtest10` 各 10000 行）。
- 每阶段对 4 个场景 × 3 个线程级别各跑一次 `--time=10 --percentile=99
  --histogram=on`；P50/P95 由完整 histogram 计算，P99 取 sysbench 汇总值。
- 模式交替顺序 `hfs-1 → native-1 → hfs-2 → native-2`，两轮取均值；比值以
  同会话对照为准，避免跨会话设备漂移。
- 场景：`oltp_read_write`（sysbench 的约 70% 读 + 30% 写混合）、
  `oltp_read_only`（随机读）、`oltp_write_only`（纯写）、
  `oltp_point_select`（主键点查）。

复现命令见第 5 节。

## 2. 性能矩阵（TPS，HFS/native 为两轮均值之比）

### 2.1 读写混合 `oltp_read_write`（约 70% 读 + 30% 写）

| 线程 | HFS TPS（两轮） | native TPS（两轮） | HFS/native | HFS avg/P95/P99 ms | native avg/P95/P99 ms | HFS fsync/10s | native fsync/10s |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 2934.75 / 2844.45 → 2890 | 2913.11 / 2963.44 → 2938 | **98.3%** | 5.45 / 7.842 / 9.56 | 5.49 / 7.985 / 12.75 | 30652 | 33476 |
| 32 | 3647.23 / 3656.24 → 3652 | 3687.43 / 3748.20 → 3718 | **98.2%** | 8.76 / 12.752 / 15.83 | 8.67 / 13.704 / 18.61 | 22024 | 25296 |
| 64 | 3798.53 / 3848.38 → 3823 | 4072.98 / 4168.62 → 4121 | **92.8%** | 16.82 / 27.165 / 35.59 | 15.68 / 25.278 / 32.53 | 15200 | 10084 |

### 2.2 随机读 `oltp_read_only`

| 线程 | HFS TPS（两轮） | native TPS（两轮） | HFS/native | HFS avg/P95/P99 ms | native avg/P95/P99 ms | HFS fsync/10s | native fsync/10s |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 9812.91 / 10066.21 → 9940 | 9864.38 / 9760.12 → 9812 | **101.3%** | 1.63 / 2.662 / 3.43 | 1.62 / 2.615 / 3.30 | 16 | 15 |
| 32 | 9500.51 / 9672.15 → 9586 | 9642.41 / 9760.39 → 9701 | **98.8%** | 3.36 / 4.91 / 5.67 | 3.31 / 4.91 / 5.77 | 6 | 5 |
| 64 | 8209.67 / 8608.28 → 8409 | 8032.39 / 8529.50 → 8281 | **101.5%** | 7.78 / 10.844 / 12.98 | 7.95 / 11.242 / 15.0 | 8 | 6 |

### 2.3 纯写 `oltp_write_only`

| 线程 | HFS TPS（两轮） | native TPS（两轮） | HFS/native | HFS avg/P95/P99 ms | native avg/P95/P99 ms | HFS fsync/10s | native fsync/10s |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 4773.13 / 4764.71 → 4769 | 4613.82 / 5282.77 → 4948 | **96.4%** | 3.35 / 5.09 / 6.55 | 3.46 / 5.879 / 9.73 | 32233 | 38245 |
| 32 | 6537.02 / 6636.05 → 6587 | 7534.80 / 7895.88 → 7715 | **85.4%** | 4.89 / 7.985 / 10.84 | 4.24 / 7.842 / 13.22 | 30626 | 39832 |
| 64 | 7682.95 / 7922.61 → 7803 | 9047.61 / 9347.40 → 9198 | **84.8%** | 8.32 / 15.268 / 20.74 | 7.06 / 13.46 / 18.95 | 28012 | 12846 |

### 2.4 主键点查 `oltp_point_select`

| 线程 | HFS TPS（两轮） | native TPS（两轮） | HFS/native | HFS avg/P95/P99 ms | native avg/P95/P99 ms | HFS fsync/10s | native fsync/10s |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 245986.12 / 253585.27 → 249786 | 240709.78 / 245094.35 → 242902 | **102.8%** | 0.06 / 0.12 / 0.23 | 0.07 / 0.134 / 0.27 | 18 | 18 |
| 32 | 238356.52 / 235152.86 → 236755 | 245474.62 / 248638.85 → 247057 | **95.8%** | 0.13 / 0.256 / 0.46 | 0.13 / 0.252 / 0.46 | 7 | 6 |
| 64 | 218270.15 / 218363.79 → 218317 | 223955.78 / 224781.40 → 224369 | **97.3%** | 0.29 / 0.608 / 1.01 | 0.29 / 0.587 / 0.97 | 7 | 5 |

### 2.5 逐轮原始 TPS

| 场景 | 线程 | hfs-1 | native-1 | hfs-2 | native-2 |
|---|---:|---:|---:|---:|---:|
| read_write | 16 | 2934.75 | 2913.11 | 2844.45 | 2963.44 |
| read_write | 32 | 3647.23 | 3687.43 | 3656.24 | 3748.20 |
| read_write | 64 | 3798.53 | 4072.98 | 3848.38 | 4168.62 |
| read_only | 16 | 9812.91 | 9864.38 | 10066.21 | 9760.12 |
| read_only | 32 | 9500.51 | 9642.41 | 9672.15 | 9760.39 |
| read_only | 64 | 8209.67 | 8032.39 | 8608.28 | 8529.50 |
| write_only | 16 | 4773.13 | 4613.82 | 4764.71 | 5282.77 |
| write_only | 32 | 6537.02 | 7534.80 | 6636.05 | 7895.88 |
| write_only | 64 | 7682.95 | 9047.61 | 7922.61 | 9347.40 |
| point_select | 16 | 245986.12 | 240709.78 | 253585.27 | 245094.35 |
| point_select | 32 | 238356.52 | 245474.62 | 235152.86 | 248638.85 |
| point_select | 64 | 218270.15 | 218363.79 | 223955.78 | 224781.40 |

## 3. 扩展性与延迟观察

- TPS 相对 16 线程的扩展（两轮均值）：

| 场景 | HFS 16/32/64 | native 16/32/64 |
|---|---|---|
| read_write | 100% / 126.4% / 132.3% | 100% / 126.5% / 140.2% |
| read_only | 100% / 96.4% / 84.6% | 100% / 98.9% / 84.4% |
| write_only | 100% / 138.1% / 163.6% | 100% / 155.9% / 185.9% |
| point_select | 100% / 94.8% / 87.4% | 100% / 101.7% / 92.4% |

- 本 VM 只有 6 个 vCPU：16 线程后读写混合基本饱和（32→64 仅 +5%/+11%）；
  读型场景（`read_only`、`point_select`）在 32/64 线程不升反降，属 CPU 抢占；
  `write_only` 仍随线程增长（组提交收益），native 侧增幅更大。
- P95/P99 随线程数上升（队列变长）：读写混合 HFS P99 9.56→15.83→35.59 ms，
  native 12.75→18.61→32.53 ms；点查在 64 线程下 P99 仍约 1 ms。
  16/32 线程下两侧 P95/P99 基本同档（互有高低），64 线程时写入路径的差距
  体现在 TPS（92.8%）而不是 P99 异常放大。
- HFS/native 汇总：读写混合 92.8–98.3%、随机读 98.8–101.5%、纯写 84.8–96.4%、
  点查 95.8–102.8%；纯写是唯一稳定低于 native 的场景，与 redo 同步路径有关。

## 4. 校验证据

- 两个 HFS 阶段 `hfsctl check` 均 `corruption=none`；四个阶段
  `innodb_flush_log_at_trx_commit=1` 均确认。
- `prepare` 日志确认 10 张表（`sbtest1..sbtest10`）各 10000 行。
- 所有阶段 redo fsync 计数在写场景持续增长（证明同步路径在运行）；
  `*—results.tsv` 记录每轮的 fsync 前后计数。
- `mysqld` 日志仅有无网络场景下的 SSL warning，无错误；证据目录含每阶段
  `cpu-map.tsv`，datadir 重建日志与体积记录。

## 5. 边界与复现

边界：

- VM 为 6 vCPU；16/32/64 线程的“线性扩展”结论只在这种资源规模下成立，
  不能外推到更大 CPU 配置。
- 两轮交替取均值不能消除设备会话级波动（历史文档记录同 VM native 写入
  TPS 可在 1.5K–2.3K 间波动）；跨会话只比较比值，不比较绝对值。
- 本矩阵没有做预热、profile 或 CPU 隔离；P50/P95 来自 sysbench histogram 桶值，
  P99 来自 sysbench 汇总，二者精度受工具限制。

复现：

```bash
# VM 内（HFS 源码目录）
PHASES='hfs-1 native-1 hfs-2 native-2' TABLES=10 TABLE_SIZE=10000 DURATION=10 \
  THREADS_LIST='16 32 64' \
  SCENARIOS='read_write read_only write_only point_select' \
  RESULT=/home/hfs/mysql-native-vs-hfs-<date> \
  bash mysql/run_native_vs_hfs_sysbench_vm.sh
```

单场景命令（每阶段内部使用）：

```bash
sysbench --db-driver=mysql --mysql-user=root --mysql-socket="$SOCKET" \
  --mysql-db=hfs_perf --tables=10 --table-size=10000 oltp_read_write prepare
sysbench --db-driver=mysql --mysql-user=root --mysql-socket="$SOCKET" \
  --mysql-db=hfs_perf --tables=10 --table-size=10000 \
  --threads=<16|32|64> --time=10 --percentile=99 --histogram=on \
  <oltp_read_write|oltp_read_only|oltp_write_only|oltp_point_select> run
```

## 6. 证据

- `/home/hfs/mysql-native-vs-hfs-20260927/`：48 个 sysbench 场景日志、4 个
  prepare 日志、4 个 initialize 日志、mysqld 日志、cpu-map、durability、
  `*-results.tsv`、`*-hfs-check.log` 与 datadir 体积。
- `/home/hfs/mysql-native-vs-hfs-20260927-metrics.tsv`：本报告使用的
  TPS/QPS/avg/P50/P95/P99/fsync 汇总。
- `/home/hfs/mysql-native-vs-hfs-20260927.run.log`：完成日志
  （`cleanup: stage=complete exit=0`）。
