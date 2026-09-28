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

## 7. 跨矩阵分析：为什么 SPDK 设备优势没有转化为 MySQL 端到端优势

FIO 三层/四引擎矩阵显示 HFS+SPDK 在设备层远超 ext4（4K randrw QD64：
15684 → HFS 106753，6.8×），但 MySQL 同盘对照只有 84.8–102.8%。原因是
端到端性能由多层瓶颈的最小值决定，SPDK 只改善"设备"这一层：

| 层 | 本会话证据 | 后果 |
|---|---|---|
| 设备形状依赖 | 同一内核路径：4K randread QD32 为 98556（vs raw 130667，1.33×）；4K randrw QD64 只有 15684（6.8×）。ext4 的劣势集中在 4K 随机**写**（O_DIRECT 分配/日志），不是读 | FIO 的 6.8× 不代表应用 I/O 形状 |
| 读缓存 | MySQL 数据集 10 万行，远小于默认 InnoDB buffer pool（128 MiB）；`oltp_read_only`/`point_select` 基本内存命中，HFS/native 98.8–102.8% | 读场景瓶颈是 CPU，不是设备 |
| 写持久性 | `trx=1` 下每个事务等 group commit + redo fsync/barrier；写场景 fsync 增量 1.2–4.3 万/10s，读场景仅 5–18/10s。HFS 的 `File::sync` 屏障含 journal/checkpoint/元数据 + 设备 flush（早前同 VM profile：barrier ≈180–286 µs、`log_flusher` CPU 15.4% vs native 4.5%，见 Git 历史 `8dc2175:doc/performance/baseline.md`） | 写路径比的是屏障延迟与 CPU，HFS 多做的工作抵消设备优势，纯写 84.8–96.4% |
| 顺序写/flush | RocksDB `fillseq/fillrandom` 两侧都 86–99 MB/s（96–100%），受 memtable/flush/compaction CPU 与顺序带宽限制 | 设备随机能力用不上，双方持平 |
| CPU 竞争 | 6 vCPU；HFS 额外 metadata/data reactor 轮询与元数据工作 | 64 线程读写混合降至 92.8% |

反过来，**在设备真正受限的环节 HFS 确实超过 native**：RocksDB `readrandom`
（1 GiB block cache / 1.25 GiB 数据，大量 miss）为 3.4–6.4×；受控实验显示
32 线程 miss 时 HFS 每次设备读 ≈16 µs，native 同步读 ≈170–620 µs；而两侧
全缓存命中时同为 ≈320 万 ops/s。即 HFS 的优势只在"设备受限"时兑现。

结论：HFS 在 FIO 上的设备优势被缓存（读）、持久化屏障（写）、CPU（6 vCPU）
和负载形状（非 4K 随机写）逐层截断，所以 MySQL 端到端只能接近 native；
要让写路径也超过 native，需要继续降低同步屏障成本（HFS 侧已做过 journal/
批量写优化，但设备往返仍是下限）。本分析限于本 VM 与本文口径。

## 8. 大工作集对照（64 MiB buffer pool + O_DIRECT，2026-09-28）

口径：`TABLES=10 TABLE_SIZE=100000`（1M 行，InnoDB 数据约 0.6 GB）、
`--innodb-buffer-pool-size=64M`、`--innodb-flush-method=O_DIRECT`
（HFS 数据文件本来就不过页缓存，native 用 O_DIRECT 对齐口径）、
`--innodb-redo-log-capacity=256M`、`threads=32`、`hfs-1 → native-1` 交错。

| 场景 | HFS TPS（qps） | native TPS（qps） | HFS/native | HFS avg/p95/p99 ms | native avg/p95/p99 ms |
|---|---:|---:|---:|---:|---:|
| `point_select`（主键点查） | 205063.77 | 201090.60 | **102.0%** | 0.16 / 0.367 / 0.68 | 0.16 / 0.354 / 0.58 |
| `read_write`（70/30 混合） | 2507.05（50141） | 2505.26（50105） | **100.1%** | 12.75 / 19.65 / 26.68 | 12.76 / 21.89 / 27.66 |
| `write_only`（纯写） | 4957.12（29743） | 5735.86（34415） | **86.4%** | 6.45 / 10.65 / 14.73 | 5.57 / 11.04 / 17.01 |
| `read_only`（随机范围读） | 2860.38（45766） | 8207.60（131322） | **34.9%** | 11.18 / 9.73 / 287.38 | 3.89 / 5.88 / 7.98 |
| `seq_scan`（4 表 UNION 全扫，mysqlslap 平均秒） | 0.165 s | 0.149 s | **90.3%** | — | — |

观察：

- 点查和 70/30 混合在 64 MiB 池下两侧持平（热子集仍能进池）；纯写 86.4%
  （屏障成本）；全表扫描 90.3%。
- **范围读（`read_only`）HFS 只有 native 的 34.9%**，且 p99 287 ms 对
  native 8 ms。InnoDB 范围扫描是成批的连续页读，native O_DIRECT 同步 pread
  在这个形状上比 HFS seam 的逐 I/O 同步路径更高效；这与 RocksDB 冷读里
  HFS 大幅领先的形状相反，说明"设备受限"并不自动等于"HFS 更快"，还取决于
  访问形状（随机块 vs 范围页读）。
- 2M 行（数据约 1.2 GB）的同口径尝试不可用：native 侧 O_DIRECT 随机写在
  本 VM 上塌陷（`write_only` 25–445 TPS、p99 30 s，伴随内核
  `nvme0: I/O tag timeout`），HFS 侧范围读出现秒级尾部（129–205 TPS，
  疑似 extent 分配压力，见 `rocksdb_baseline.md` §8.1）；该规模未纳入结论。
- 结论：buffer pool 缩小后 MySQL 没有复现 RocksDB 式的 HFS 读优势；HFS 只在
  点查/混合场景持平、写略低。MySQL 的范围读要超过 native，需要优化 HFS seam
  的批量/范围读路径，而不是设备本身。

证据：`/home/hfs/mysql-large-bp64m-1m-20260928c/`（5 场景日志、prepare、
durability、`*-results.tsv`、`*-hfs-check.log` 为 `corruption=none`）；
2M 行尝试证据：`mysql-large-bp64m-20260928`、`…-20260928b`。

## 9. 2026-09-28 复测（RocksDB extent 修复后，同口径）

复测动机：确认 extent 修复（`bug.md` #41，仅 RocksDB worker-direct 路径）
未改变 MySQL 结论，并刷新同会话数值。本轮设备无新增内核停顿
（`nvme0: I/O tag timeout` 计数 91 → 91），四阶段 `innodb_flush_log_at_trx_commit=1`，
HFS `hfsctl check` 均 `corruption=none`。

### 9.1 标准矩阵（10 表 × 1 万行，默认 buffer pool，HFS/native 各两轮均值）

| 场景 | 线程 | HFS TPS | native TPS | HFS/native | HFS avg/p95/p99 ms（第 1 轮） | native avg/p95/p99 ms（第 1 轮） |
|---|---:|---:|---:|---:|---:|---:|
| read_write | 16 | 3206.53 | 3381.31 | 94.8% | 4.86 / 7.17 / 12.08 | 4.68 / 6.79 / 11.65 |
| read_write | 32 | 4000.69 | 4178.96 | 95.7% | 8.31 / 12.30 / 15.27 | 7.67 / 12.30 / 17.01 |
| read_write | 64 | 4302.47 | 4694.69 | 91.6% | 15.03 / 24.38 / 31.37 | 13.57 / 22.28 / 29.19 |
| read_only | 16 | 10501.38 | 10674.86 | 98.4% | 1.52 / 2.52 / 3.36 | 1.52 / 2.43 / 3.07 |
| read_only | 32 | 10387.04 | 10623.54 | 97.8% | 3.07 / 4.49 / 5.18 | 3.02 / 4.41 / 5.09 |
| read_only | 64 | 9485.51 | 9599.57 | 98.8% | 6.71 / 10.46 / 12.98 | 6.65 / 9.39 / 11.45 |
| write_only | 16 | 5294.15 | 6337.23 | 83.5% | 3.03 / 4.65 / 6.09 | 2.48 / 3.96 / 6.67 |
| write_only | 32 | 7467.17 | 8812.14 | 84.7% | 4.21 / 6.91 / 9.22 | 3.68 / 6.55 / 12.75 |
| write_only | 64 | 8884.79 | 10187.91 | 87.2% | 7.24 / 13.22 / 18.28 | 6.44 / 12.08 / 17.01 |
| point_select | 16 | 255488.76 | 261312.81 | 97.8% | 0.06 / 0.11 / 0.17 | 0.06 / 0.11 / 0.17 |
| point_select | 32 | 256754.54 | 263607.79 | 97.4% | 0.12 / 0.21 / 0.33 | 0.12 / 0.21 / 0.34 |
| point_select | 64 | 239836.40 | 240970.28 | 99.5% | 0.27 / 0.46 / 0.69 | 0.26 / 0.46 / 0.72 |

与 §2（2026-09-27）一致：只读 97.8–98.8%、读写混合 91.6–95.7%、
纯写 83.5–87.2%、点查 97.4–99.5%；绝对 TPS 因会话设备速度不同略高，
比值口径不变。

### 9.2 大工作集复测（10 表 × 10 万行，64 MiB buffer pool + O_DIRECT，32 线程）

| 场景 | HFS | native | HFS/native | HFS avg/p95/p99 ms | native avg/p95/p99 ms |
|---|---:|---:|---:|---:|---:|
| point_select | 236302.67 | 203945.97 | 115.9% | 0.14 / 0.354 / 0.81 | 0.16 / 0.342 / 0.56 |
| read_write | 2533.81 | 2533.80 | 100.0% | 12.62 / 19.30 / 24.38 | 12.61 / 21.89 / 26.68 |
| write_only | 4915.24 | 5578.56 | 88.1% | 6.50 / 10.84 / 14.46 | 5.73 / 11.65 / 16.71 |
| read_only（范围读） | 1250.58 | 8287.09 | **15.1%** | 25.42 / 167.44 / 493.24 | 3.86 / 5.77 / 7.70 |
| seq_scan（4 表 UNION，平均秒） | 0.157 s | 0.153 s | 97.5% | — | — |

- 点查/混合与 §8 一致（持平或略优）；纯写 88.1%。
- **范围读再次塌陷且更差（15.1%，p95/p99 达 167/493 ms）**，而本轮
  **没有任何内核 NVMe timeout**：说明这是 HFS seam 范围读路径自身的
  间歇性长尾（p50 4.25 ms 与 native 3.62 ms 接近，差距集中在尾部），
  不是设备停顿或 extent 压力；列为后续优化项（批量/范围读路径）。
- 2M 行规模仍不重测：§8 已记录 native O_DIRECT 随机写在该规模下伴随
  设备 timeout 塌陷，需单独方法学。

证据：`/home/hfs/mysql-native-vs-hfs-20260928/`（标准矩阵 4 阶段）、
`/home/hfs/mysql-large-1m-20260928/`（大工作集 2 阶段）；
均为 `cleanup: stage=complete exit=0`。
