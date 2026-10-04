# HFS 2.0 MySQL 基线：原生 ext4 对比 HFS（sysbench 16/32/64 线程）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

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
  不是设备停顿或 extent 压力；已在 §10 定位并修复。
- 2M 行规模仍不重测：§8 已记录 native O_DIRECT 随机写在该规模下伴随
  设备 timeout 塌陷，需单独方法学。

证据：`/home/hfs/mysql-native-vs-hfs-20260928/`（标准矩阵 4 阶段）、
`/home/hfs/mysql-large-1m-20260928/`（大工作集 2 阶段）；
均为 `cleanup: stage=complete exit=0`。

## 10. `read_only` 根因定位与修复（Shard-108，2026-09-28）

### 10.1 现象与定位

大工作集（64 MiB buffer pool + O_DIRECT）下 HFS `read_only` 只有 native 的
15.1%，而线程缩放实验给出关键线索（`/home/hfs/mysql-readonly-diag-20260928`）：

| 线程 | HFS TPS | HFS avg/p50/p95/p99/max ms | native TPS | native p99/max ms |
|---:|---:|---|---:|---|
| 1 | 820.84 | 1.22 / 1.16 / 1.70 / 2.07 / 4.29 | 849.19 | 2.14 / 5.95 |
| 8 | 6511.64 | 1.23 / 1.18 / 2.03 / 2.48 / 11.65 | 8026.55 | 2.22 / 5.66 |
| 32 | **4556.19** | 7.00 / 3.30 / 5.99 / **147.61 / 889.92** | 8440.49 | 6.67 / 16.43 |
| 32，`--range_selects=off` | 22087.03 | 1.45 / 1.32 / 2.48 / 3.43 / 8.7 | 20764.71 | 2.91 / 10.46 |

- 1/8 线程两侧持平；32 线程 + 范围扫描才塌陷，纯点查（范围开关关闭）正常
  → 问题在"高并发范围读"的 I/O 提交路径，不在设备或单次读成本。
- seam 计数器：`async_reads=28621 async_writes=34532 async_fallbacks=294533`
  → **约 91% 的 I/O 走了同步回退**。
- `perf`（32 线程）：connection 线程 **64.2% 采样在 `spdk_ring_enqueue`**，
  其中 63.9% 在 `rte_wait_until_equal_32` 自旋——同步回退路径每个 I/O 调用
  一次 `spdk_thread_send_msg` 投递到单一 data reactor，消息环满后生产者自旋
  等待，造成 CPU 空转与百毫秒级长尾。
- 代码根因：`hfs_vfs::asyncIo` 要求当前线程已有 `t_holder`（线程 client），
  而 `threadClient()` 只在文件 open/create 路径被调用；32 个连接线程大多
  没有 client，异步快路径不可用，全部落到 `file->read().wait()` 同步面。

### 10.2 修复与验证

修复（`mysql/patches/hfs_seam_shard108_async_lazy.patch`，`build_mysql_vm.sh`
按标记应用）：`asyncIo` 在首次 I/O 时惰性调用 `threadClient()` 建立本线程
client；共享溢出 client 仅服务元数据，不参与异步 poll，因此超限线程仍安全
回退。修复后 `thread_clients=140`、`async_reads=315801 async_writes=228960
async_fallbacks=47580`（异步覆盖 ~92%）。

同会话 A/B（同一 HFS datadir，1/8/32 线程）：

| 线程 | 修复前 TPS | 修复后 TPS | 修复前 p99 / max ms | 修复后 p99 / max ms |
|---:|---:|---:|---|---|
| 1 | 820.84 | 823.02 | 2.07 / 4.29 | 2.03 / 3.15 |
| 8 | 6511.64 | 7025.40 | 2.48 / 11.65 | 2.39 / 4.32 |
| 32 | 4556.19 | **8752.81** | **147.61 / 889.92** | **7.98 / 18.82** |

修复后大工作集矩阵（10 表 × 10 万行、64 MiB pool + O_DIRECT、32 线程）：

| 场景 | HFS（修复前） | HFS（修复后） | native | 修复后比值 |
|---|---:|---:|---:|---:|
| read_only | 1250.58（15.1%） | **7947.85** | 8309.30 | **95.7%** |
| point_select | 236302.67 | 203505.04 | 205061.74 | 99.2% |
| write_only | 4915.24 | 5270.95 | 5628.51 | 93.6% |
| read_write | 2533.81 | 2571.36 | 536.70（30 s 设备停顿，不可比） | — |
| seq_scan（平均秒） | 0.157 | 0.154 | 0.148 | 104.1%（HFS 慢 4%） |

修复后 `read_only` 的 p95/p99（6.91/9.39 ms）与 native（5.67/8.13 ms）同档；
`hfsctl check` 仍为 `corruption=none`。证据：
`/home/hfs/mysql-readonly-diag-20260928`（修复前定位与 perf）、
`/home/hfs/mysql-readonly-lazyfix-20260928`（A/B）、
`/home/hfs/mysql-large-1m-lazyfix-20260928`（修复后矩阵）。

## 11. seam 批量提交优化（Shard-109，2026-09-28）

### 11.1 实现

`hfs_vfs::aioBatch`（`mysql/patches/hfs_seam_shard109_aio_batch.patch`）：一次把整批
HFS I/O 通过本线程 client 提交（每批最多 16 条在途），再用一个 poll 循环收割全部
完成，替代"每 I/O 一次提交+等待"；任何提交失败或请求错误都会 drain 已提交项后
回退到原来的逐 slot 路径。调用点在 InnoDB 模拟 AIO 的合并批处理
（`SimulatedAIOHandler::io()`）：当批内 ≥2 个 slot 且都是 HFS 句柄、非 punch-hole
时走批量路径。

### 11.2 覆盖与结果（HFS-only 大工作集，两次运行取均值）

`async_batch_ios≈12.2k / 529k` 个异步 I/O（约 **2.3%**）：模拟 AIO 只在
`merge_adjacent` 合并到同文件相邻 slot 时才形成批，随机读/点查多为单 slot，
不进入批量路径。两次运行对比 Shard-108 基线：

| 场景 | Shard-108 | Shard-109（两次均值） | 变化 |
|---|---:|---:|---:|
| read_only | 7947.85 | 8071.64 | +1.6% |
| point_select | 203505.04 | 202513.22 | −0.5% |
| write_only | 5270.95 | 5156.12 | −2.2% |
| read_write | 2571.36 | 2568.20 | −0.1% |
| seq_scan（平均秒） | 0.154 | 0.155 | +1.0% |

结论：批处理实现正确，但在本负载下覆盖率低，端到端变化在会话噪声范围内
（±2%），不作为独立性能结论；`read_only` 的 p99 略降（9.39→8.43/8.74 ms）。
非相邻 slot 的扩展（`fill_batch`）已试验并回退：覆盖率提升到 7.5% 但读场景
回退 2–5%（证据 `/home/hfs/mysql-large-1m-batchfill-20260928`），未保留。

证据：`/home/hfs/mysql-large-1m-batch-20260928`、
`/home/hfs/mysql-large-1m-batch2-20260928`（两次确认）；
`mysql-large-1m-batchfill-20260928`（回退的扩展实验）。

## 12. 最终矩阵（Shard-108/109 后，2026-09-28）

覆盖顺序（全表扫描）、随机（点查/范围读）、读、写、读写混合；HFS/native 同会话
交错。本轮无新增内核 `nvme0: I/O tag timeout`（计数 92 不变），
`innodb_flush_log_at_trx_commit=1`，HFS `hfsctl check` 均 `corruption=none`。

### 12.1 标准数据集（10 表 × 1 万行，默认 buffer pool，两轮均值）

| 场景 | 线程 | HFS TPS | native TPS | HFS/native | HFS avg/p95/p99 ms | native avg/p95/p99 ms |
|---|---:|---:|---:|---:|---:|---:|
| read_only（随机读） | 16 | 11286.84 | 11308.05 | 99.8% | 1.42 / 2.18 / 2.61 | 1.40 / 2.18 / 2.66 |
| read_only | 32 | 10671.32 | 10817.19 | 98.7% | 3.00 / 4.41 / 5.18 | 2.94 / 4.33 / 5.09 |
| read_only | 64 | 9740.69 | 9891.79 | 98.5% | 6.57 / 9.06 / 10.65 | 6.48 / 8.90 / 10.27 |
| point_select（随机点查） | 16 | 270314.99 | 275321.51 | 98.2% | 0.06 / 0.10 / 0.15 | 0.06 / 0.10 / 0.15 |
| point_select | 32 | 262170.82 | 265061.66 | 98.9% | 0.12 / 0.21 / 0.32 | 0.12 / 0.20 / 0.32 |
| point_select | 64 | 240291.92 | 251802.51 | 95.4% | 0.28 / 0.49 / 0.77 | 0.25 / 0.45 / 0.67 |
| write_only（纯写） | 16 | 5262.26 | 5634.09 | 93.4% | 3.08 / 4.74 / 6.21 | 2.84 / 4.82 / 10.84 |
| write_only | 32 | 7511.66 | 8571.85 | 87.6% | 4.26 / 7.04 / 9.22 | 3.78 / 6.91 / 13.22 |
| write_only | 64 | 9227.39 | 10290.62 | 89.7% | 6.98 / 12.98 / 17.63 | 6.26 / 11.65 / 16.41 |
| read_write（70/30 混合） | 16 | 3126.24 | 3479.23 | 89.9% | 5.07 / 7.30 / 9.56 | 4.62 / 6.79 / 10.09 |
| read_write | 32 | 4112.40 | 4160.16 | 98.9% | 7.81 / 11.45 / 14.21 | 7.78 / 12.08 / 16.71 |
| read_write | 64 | 4255.90 | 4638.56 | 91.8% | 14.99 / 24.38 / 31.94 | 13.90 / 22.69 / 29.19 |
| seq_scan（4 表全扫，平均秒） | 16 | 0.009 s | 0.009 s | 94.7% | — | — |
| seq_scan | 32 | 0.018 s | 0.019 s | 92.1% | — | — |
| seq_scan | 64 | 0.040 s | 0.035 s | 114.5% | — | — |

小数据集全表扫描只有 9–40 ms 且完全驻留缓存，比值波动大，仅作顺序访问参考。

### 12.2 大工作集（10 表 × 10 万行，64 MiB pool + O_DIRECT，32 线程）

| 场景 | HFS | native | HFS/native | HFS avg/p95/p99 ms | native avg/p95/p99 ms |
|---|---:|---:|---:|---:|---:|
| read_only（范围读） | 7771.58 | 8088.97 | **96.1%** | 4.11 / 6.91 / 9.22 | 3.95 / 5.88 / 7.70 |
| point_select | 198292.61 | 213115.40 | 93.0% | 0.16 / 0.46 / 1.12 | 0.15 / 0.32 / 0.52 |
| write_only | 5406.84 | 4457.45 | **121.3%** | 5.91 / 9.91 / 13.70 | 7.17 / 15.00 / 20.00 |
| read_write | 2598.27 | 65.23（22 s 设备停顿，不可比） | — | 12.30 / 18.95 / 22.69 | 490.35 / 31.95 / 22034.77 |
| seq_scan（4 表全扫，平均秒） | 0.150 s | 0.162 s | 92.6% | — | — |

- **`read_only` 从修复前的 15.1% 恢复到 96.1%**（Shard-108 效果），p95/p99
  与 native 同档；`seq_scan` 本轮到 92.6%（HFS 快 7.4%，上次为慢 4%，均在小
  样本噪声内）。
- `write_only` 本会话 native 侧较慢（p99 20 ms），HFS 反而快 21%；跨会话只比
  比值趋势，不比较绝对值。
- `read_write` 的 native 轮出现 22 s 停顿（`max=22034.77 ms`），该格不可比；
  HFS 侧保持 2598 TPS / p99 22.7 ms。设备停顿是本 VM 的已知间歇问题，本轮
  dmesg 未新增 timeout 记录，但运行期确实发生。

证据：`/home/hfs/mysql-native-vs-hfs-final-20260928`（标准 4 阶段）、
`/home/hfs/mysql-large-1m-final-20260928`（大工作集 2 阶段），
均 `cleanup: stage=complete exit=0`。

### 12.3 `read_only`/`read_write` 复测（4 阶段交错，2026-09-28）

针对 §12.2 中 native `read_write` 的 22 s 停顿，对两个场景做 4 阶段交错复测
（10 表 × 10 万行、64 MiB pool + O_DIRECT、32 线程）：

| 场景 | 模式 | 第 1 轮 TPS | 第 2 轮 TPS | 均值 | avg ms（两轮） | p99 ms（两轮） | max ms（两轮） |
|---|---|---:|---:|---:|---:|---:|---:|
| read_only | HFS | 7799.07 | 7945.65 | 7872.36 | 4.10 / 4.02 | 9.06 / 9.22 | 30.1 / 35.4 |
| read_only | native | 8424.53 | 7904.12 | 8164.33 | 3.79 / 4.04 | 7.56 / 8.13 | 25.5 / 27.7 |
| read_write | HFS | 2374.77 | 2262.58 | 2318.68 | 13.46 / 14.13 | 27.66 / 30.26 | 82.2 / 93.1 |
| read_write | native | 2053.10 | **26.04**（30.8 s 墙钟，max 30299 ms） | — | 15.57 / 1228.64 | 34.33 / 30469.9 | 211.4 / 30298.9 |

- `read_only`：HFS/native = **96.4%**，两轮稳定，与 §12.2 的 96.1% 一致；
  p99 9.06/9.22 ms 对 native 7.56/8.13 ms。
- `read_write`：native 第二轮再次出现 **30 s 设备停顿**（dmesg 新增 1 次
  `nvme0: I/O tag timeout`，92→93），该轮不可比；干净轮对比
  **HFS 2374.77 vs native 2053.10（115.7%）**，HFS p99 27.66 对 native 34.33 ms。
  即混合读写在本口径下 HFS 不落后，native 内核路径的间歇停顿是主要风险。
- 两阶段 HFS `hfsctl check` 均 `corruption=none`，`flush_log_at_trx_commit=1`。

证据：`/home/hfs/mysql-large-1m-rw-ro-20260928`（4 阶段）。