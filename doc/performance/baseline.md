# HFS 2.0 性能基线（2026-09-26，fsync 覆盖修复回归 2026-09-27）

数据会话：UTC 2026-09-26 09:09–09:45（本地 17:09–17:45）。HFS 二进制包含
direct-buffer DMA 连续性校验修复（不可直接 DMA 的缓冲回落 bounce）。
2026-09-27 追加 fsync 覆盖修复（`fix(core): cover in-place data writes in the sync barrier`）：
原地覆盖写不再命中 `syncMetadata` 早退，barrier 必做 data flush（§10.2/§20.3）；
下方矩阵为修复前二进制（`2e21ac8d…`）测得，修复后同会话回归见末节。

## 环境与口径

| 项目 | 值 |
|---|---|
| 设备 | `VMware NVME_0002`，PCI `0000:13:00.0`；内核 `nvme` 枚举为 `/dev/nvme2n1`，SPDK bdev `Nvme0n1` |
| VM | 6 vCPU、约 7.2 GiB RAM、1024 hugepages |
| HFS | `libhfs_core.so` SHA256 矩阵 `2e21ac8d…`、修复后 `fba6fe1c…`；`fio-hfs.so` `823be5a3…`，`hfs_db_bench` `b99e83…`，`hfsctl` `7c2472…`；`hfs_nvme.conf` `89e636…`，`hfs_mysql.conf` `b9b5a3…`；`trace=false`、`stats=false` |
| SPDK / FIO | SPDK `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`；FIO 3.42 |
| 应用 | RocksDB 9.11.2；MySQL 8.0.46；sysbench 1.0.20 |
| 证据 | FIO `/home/hfs/fio-three-way-20260926c/`；RocksDB `/home/hfs/rocksdb-rw32-cache1g-20260926b{,-r2,-r3}/`；MySQL `/home/hfs/mysql-samedisk-20260926b/`；修复回归 `/home/hfs/fsync-ab-{pre3,post4}/`、`/home/hfs/fio-8j4-fsync-20260927/`、`/home/hfs/syncstats-inplace-20260926/` |

## FIO 三方矩阵（256 MiB 已写范围、randrw 50/50、10 秒、每格 3 次中位数）

总并发：ext4 与 SPDK raw 为 1 job × QD32/64；HFS 为 **1 job × QD32**（QD32 格）
或 **2 jobs × QD32**（QD64 格），`hfs_client_depth=32`、`hfs_async=1`。

### IOPS

| I/O | 总 QD | ext4+libaio | SPDK raw | HFS | HFS/raw |
|---|---:|---:|---:|---:|---:|
| 4K | 32 | 11179 | 86039 | 80538 | **93.6%** |
| 4K | 64 | 12951 | 85545 | 84313 | **98.6%** |
| 16K | 32 | 35564 | 66539 | 64058 | 96.3% |
| 16K | 64 | 9618 | 67294 | 68372 | 101.6% |
| 64K | 32 | 14629 | 38558 | 38188 | 99.0% |
| 64K | 64 | 16146 | 36767 | 35705 | 97.1% |

### p99（读/写，µs）

| I/O | 总 QD | ext4 | SPDK raw | HFS | HFS/raw 读 |
|---|---:|---:|---:|---:|---:|
| 4K | 32 | 1352/1352 | 1090/1106 | 1155/1171 | 106.0% |
| 4K | 64 | 1794/1810 | 1958/1958 | 1794/1794 | 91.6% |
| 16K | 32 | 2310/2277 | 1384/1384 | 1417/1417 | 102.4% |
| 16K | 64 | 3391/3391 | 2343/2343 | 2056/2073 | 87.8% |
| 64K | 32 | 4080/4080 | 2023/2023 | 1925/1925 | 95.1% |
| 64K | 64 | 5997/5997 | 3817/3817 | 3195/3228 | 83.7% |

### ext4 三次原始 IOPS（其余两层各轮波动 <5%）

| I/O | 总 QD | 第 1/2/3 次 | 最大/最小 |
|---|---:|---|---:|
| 4K | 32 | 10620 / 11179 / 11265 | 1.06 |
| 4K | 64 | 12833 / 13034 / 12951 | 1.02 |
| 16K | 32 | 5506 / 36158 / 35564 | 6.57 |
| 16K | 64 | 9618 / 5691 / 9763 | 1.72 |
| 64K | 32 | 14400 / 14629 / 14760 | 1.03 |
| 64K | 64 | 16146 / 16576 / 2333 | 7.10 |

54 次正式测量 `error=0`；HFS 18 个计时轮 closure 全部 `queued=reaped`、
`outstanding=0`、`token_mismatch=0`（峰值 32/64），18 个预填充闭合（峰值 4）。

## RocksDB：32 并发、1 GiB 缓存、同盘（3 轮）

128 万 key、16 B key、1024 B value、4 KiB block、无压缩、direct I/O；
每轮 `fillseq` → `readrandom` → `overwrite` 逐键校验。

| 轮次 | native 写 | HFS 写 | native 读 | HFS 读 | native 校验 | HFS 校验 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 74307 | 76043 | 38184 | 127083 | 60245 | 54345 |
| 2 | 74104 | 75399 | 33996 | 140503 | 56933 | 73190 |
| 3 | 78747 | 78543 | 34917 | 131830 | 62865 | 60329 |
| **中位数** | **74307** | **76043** | **34917** | **131830** | **60245** | **60329** |

HFS/native（中位数之比）：写 **102.3%**，随机读 **377.6%**。三轮
`overwrite` 校验通过，`hfsctl check` 均为 `corruption=none`。

## MySQL 同盘：HFS vs native ext4（8 线程、每阶段重建 datadir）

| 阶段 | oltp 读 TPS | 读 avg/p99 ms | oltp 读写 TPS | 读写 avg/p99 ms | mysqlslap 读/更新 秒 | redo fsync 增量 |
|---|---:|---:|---:|---:|---:|---:|
| HFS-1 | 8941.60 | 0.89 / 2.52 | 1125.73 | 7.10 / 13.46 | 0.075 / 14.149 | 40109 |
| native-1 | 9255.82 | 0.86 / 2.39 | 1603.34 | 4.98 / 9.91 | 0.081 / 12.030 | 48490 |
| HFS-2 | 9096.93 | 0.88 / 2.35 | 1221.77 | 6.54 / 12.52 | 0.078 / 14.278 | 41064 |
| native-2 | 9057.49 | 0.88 / 2.48 | 1639.08 | 4.87 / 9.91 | 0.077 / 12.196 | 48024 |

两轮均值 HFS/native：只读 **98.5%**，读写混合 **72.4%**，mysqlslap 主键读
耗时 96.8%、随机更新耗时 117.3%；四轮 `innodb_flush_log_at_trx_commit=1`，
HFS 两轮 `hfsctl check` 均为 `corruption=none`。

## fsync 覆盖修复回归（2026-09-27，同会话 A/B）

修复前 `File::sync/datasync` 对原地覆盖写命中 `syncMetadata` 早退：1 线程 1500 次
覆盖写 + 每写 fsync 只有 1 次真实 flush；修复后每次 fsync 覆盖其前已完成写。

| 指标 | 修复前 | 修复后 | 变化 |
|---|---:|---:|---:|
| in-place fsync 覆盖（1×1500 覆盖写，每写 sync） | barriers 1 / flushes 1 | barriers 1500 / flushes 1504 | 契约修复 |
| in-place fsync 覆盖（8×4000） | barriers 1 / flushes 1 | barriers 4007 / flushes 4011 | 组提交合并 |
| MySQL trx=1 TPS（20 s，8 线程） | 1548.10 | 1568.44 | +1.3% |
| MySQL `barriers` / `flushes` | 31128 / 31128 | 49268 / 108204（skips 700） | — |
| RocksDB `fillseq` IOPS（HFS） | 108916 | 108293 | −0.6% |
| RocksDB `readrandom` IOPS（HFS） | 159920 | 165193 | +3.3% |
| RocksDB `overwrite` 校验 IOPS（HFS） | 96722 | 86337 | 该轮 native 校验被设备停顿拖到 28393，属噪声带 |
| 8j4 gate raw / HFS 中位 IOPS（5 轮） | 基线 115889 / 81714 | 120758 / 80268 | HFS −1.8%（<5% 门禁内） |

8j4 五轮 HFS 全部 closure 闭合、`error=0`；MySQL 两轮 `hfsctl check` 均
`corruption=none`；RocksDB 两种模式 `overwrite` 逐键校验通过。修复后
`libhfs_core.so` SHA256 `fba6fe1c…`。

### barrier 相位与合并（2026-09-27）

相位计时（`HFS_DIAG_SYNC_STATS=1`，MySQL trx=1 会话）：barrier avg 286 µs，其中
data_flush 95、ckpt_snapshot 16、ckpt_write 111、ckpt_flush 44、sb_write 90、
sb_flush 35 µs；带 checkpoint 的 barrier ≈ 391 µs，模型 flush 数 108,902 对实测
108,204。合并后（checkpoint/superblock 延迟 flush，每设备单次 flush）：

| 指标 | 合并前 | 合并后 | 变化 |
|---|---:|---:|---:|
| MySQL trx=1 TPS（同会话相邻两轮） | 1568.03 | 1592.69 | +1.6% |
| barrier avg µs | 286 | 237 | −17% |
| device flush 命令（会话） | 108176 | 52853 | −51% |
| ckpt_write / sb_write µs | 111 / 90 | 182 / 109 | 合并后写入成为 barrier 首个设备 I/O |
| ckpt_flush / sb_flush µs | 44 / 35 | 0 / 0 | 由 device flush 覆盖 |

门禁：CTest 26/26；Shard-104 crash matrix（在线 DDL + torn page 双 SIGKILL）通过、
两轮 `hfsctl check` `corruption=none`；RocksDB rw32 HFS fillseq 102.7%、readrandom
408%、`overwrite` 校验 111.7%（对 native）；8j4 HFS 中位 79577 / raw 121547（0.655），
较上一轮 HFS −0.9%（<5%）。合并后 `libhfs_core.so` SHA256 `1b31d870…`；证据
`/home/hfs/barrier-ab-{premerge,postmerge}/`、`/home/hfs/mysql-shard104-crash-20260927/`、
`/home/hfs/rocksdb-rw32-cache1g-20260927-merge/`、`/home/hfs/fio-8j4-merge-20260927/`。

### checkpoint/superblock 批量写（2026-09-27）

`JournalDevice::writeBatch` 把 checkpoint 记录与 superblock 一次提交（并发在飞、只付
一次唤醒）；`SpdkRuntime::controlWriteBatch` 负责批量控制写。

| 指标 | 合并后 | 批量写后 | 变化 |
|---|---:|---:|---:|
| MySQL trx=1 TPS（同会话相邻两轮） | 1598.43 | 1615.76 | +1.1% |
| barrier avg µs | 237 | 179–184 | −24% |
| meta_write µs（两条记录） | 182 + 109 = 291 | 191–197 | −34% |
| device_flush µs | 49 | 59–61 | — |
| ckpt_snapshot µs | 16 | 16 | — |

累计（修复前 → 批量写）：MySQL trx=1 1568.03 → 1615.76 TPS（同会话相邻轮 +3.0%）；
barrier 286 → 179 µs。门禁：CTest 26/26；Shard-104 crash matrix 通过、
`hfsctl check` `corruption=none`；RocksDB rw32 HFS fillseq 103.6%、readrandom 400%、
校验 153%；8j4 HFS 中位 80861 / raw 121663（0.665）。批量写后 `libhfs_core.so`
SHA256 `b40d3831…`；证据 `/home/hfs/barrier-ab-{merge2,batch2,batch1}/`、
`/home/hfs/rocksdb-rw32-cache1g-20260927-batch/`、`/home/hfs/fio-8j4-batch-20260927/`。

## MySQL 新口径：fsync 修复 + barrier 合并 + 批量写后（2026-09-27）

同会话同盘协议（`mysql/run_shard105_samedisk_vm.sh`，每阶段重建 datadir、无预热）：

| run | 只读 TPS | 读 avg/p99 ms | 读写 TPS | 读写 avg/p99 ms | mysqlslap 读/更新 s | redo fsync |
|---|---:|---|---:|---|---:|---:|
| hfs-1 | 10770.89 | 0.74 / 1.79 | 1680.33 | 4.76 / 9.22 | 0.056 / 12.090 | 45723 |
| native-1 | 11533.86 | 0.69 / 1.64 | 2299.48 | 3.48 / 6.91 | 0.058 / 10.535 | 68434 |
| hfs-2 | 10622.66 | 0.75 / 1.86 | 1680.26 | 4.76 / 9.06 | 0.067 / 12.211 | 45373 |
| native-2 | 10938.11 | 0.73 / 1.73 | 2306.10 | 3.47 / 6.91 | 0.069 / 10.524 | 69104 |

同会话定向 profile（预热协议，`perf record` 自耗 + fsync 计数）：HFS 1577.84 TPS /
avg 5.07 ms / 1.21 fsync per txn，`log_write_up_to` 自耗 24.4%、内核
`_raw_spin_unlock_irqrestore` 14.7%、SPDK 轮询 3.1%；native 2326.84 / 3.44 ms /
2.31 fsync，`log_writer` 23.1%、`log_write_up_to` 6.9%、内核自旋 6.7%。即 HFS 的
fsync 次数只有 native 一半，但 committer 多出约 25pp 的自旋（≈2 核/6 vCPU），
对应吞吐差距；根因是 redo 同步写（~140 µs）与 barrier（~180 µs）拉长等待。
证据 `/home/hfs/mysql-profile-20260927/`。

只读 **95.2%**、读写 **73.0%**（旧基线 98.5% / 72.4%）。预热同会话对照
（2×10 s warmup + 20 s trx=1）：HFS 1640.15 TPS / avg 4.88 / p99 9.56，native
2297.35 / 3.48 / 6.79，**HFS/native 71.4%**。即写路径比值在冷/热两种口径下一致
（71–73%），fsync 修复与 barrier 优化未改变该比值：修复把"跳过 flush"换成正确落盘，
合并/批量写把 barrier 从 286 µs 降到 ~180 µs，两者相抵。剩余差距
（+1.4 ms/txn）不在 barrier（约 0.23 ms/txn），而在 seam 同步 I/O 与 InnoDB
log 等待/自旋、CPU 争用。证据 `/home/hfs/mysql-samedisk-20260927-postfsync/`、
`/home/hfs/mysql-warm-ab-20260927/`。

## seam worker-direct 快路径（2026-09-27，Shard-106）

seam 的 `read/write` 改为 per-thread client `submitRead/submitWrite` + 同线程 `poll`
（File 由 owner client 持有；core 的 file 归属校验放宽到同一 engine，增长写与 submit
失败回落同步门面）。同会话双向 A/B（8 线程 trx=1、2×10 s warmup）：

| 轮次 | pre-async TPS | seam-async TPS | 变化 | async 计数 |
|---|---:|---:|---:|---|
| pre → post | 1642.05 | 1656.59 | +0.9% | reads 339 / writes 126949 / fallbacks 46036 |
| post → pre（反序） | 1631.26 | 1662.11 | +1.9% | reads 337 / writes 127118 / fallbacks 46063 |
| v2（owner-file 模型，反序 post→pre） | 1625.40 | 1650.40 | +1.5% | reads 337 / writes 126677 / fallbacks 46132 |

两序均值约 **+1.4%**；v2 停机干净（`engine stopped`、复挂载 `clean=1`）。定向 profile
（预热协议）显示 committer 自旋几乎未降：`log_write_up_to` 自耗 24.4% → 24.0%、
内核自旋 14.7% → 12.6%，即写延迟不是自旋主因；剩余 HFS 特有开销集中在 fsync barrier
与 InnoDB group-commit 等待结构。

### `max_group_commit_wait_us` 0 vs 200（2026-09-27，同会话四相位）

| 配置 | TPS（两相位） | avg ms | barriers | flushes | checkpoints |
|---|---|---:|---:|---:|---:|
| 0 | 1829.23 / 1788.37 | 4.37 / 4.47 | 166707 / 167897 | 166709 / 167899 | 43704 / 43757 |
| 200 | 1662.26 / 1649.40 | 4.81 / 4.85 | 48509 / 48247 | 48511 / 48249 | 32592 / 32371 |

0 的 barrier/flush 是 200 的 3.4×，吞吐仍 **+9.2%**：窗口是纯延迟项，每次 barrier 已真实
落盘，设备可吸收更多 flush。`config/hfs_mysql.conf` 已改为 `max_group_commit_wait_us = 0`。

per-file `File::sync` 目标 epoch 实验（同日，已回退）：checkpoints 45148 → 41519（−8%）、
TPS 1841.04 → 1825.49（−0.8%，噪声内）。checkpoint 主要来自 fsync 文件自身的增长
（数据/undo/doublewrite），不是跨文件连带，per-file 语义无法也不应跳过；证据
`/home/hfs/mysql-perfile-sync-ab-20260927/`。

门禁（core 校验放宽 + v2 seam + gcw=0）：CTest 26/26；Shard-104 crash matrix 通过、
`hfsctl check` `corruption=none`；RocksDB rw32 HFS fillseq 105.8%、readrandom 411%、
`overwrite` 校验 107.6%（对 native）；8j4 HFS 中位 79825 / raw 121426（0.657）、
5 轮 closure 全闭合。证据 `/home/hfs/mysql-seam-v2-ab-20260927/`、
`/home/hfs/mysql-gcw-v2-20260927/`、`/home/hfs/mysql-seam-v2-check-20260927/`、
`/home/hfs/rocksdb-rw32-cache1g-20260927-v2/`、`/home/hfs/fio-8j4-v2-20260927/`、
`/home/hfs/mysql-shard104-crash-20260927-v2/`。

## 最终口径（2026-09-27 晚，fsync 修复 + barrier 合并/批量 + seam 异步 + gcw=0）

同盘协议（每阶段重建 datadir、无预热）：

| run | 只读 TPS | 读 avg/p99 ms | 读写 TPS | 读写 avg/p99 ms | mysqlslap 读/更新 s | redo fsync |
|---|---:|---|---:|---|---:|---:|
| hfs-1 | 11073.01 | 0.72 / 1.73 | 1919.76 | 4.16 / 10.46 | 0.058 / 10.901 | 70556 |
| native-1 | 11266.50 | 0.71 / 1.70 | 2287.40 | 3.49 / 6.79 | 0.064 / 10.566 | 67983 |
| hfs-2 | 10512.78 | 0.76 / 1.79 | 1893.58 | 4.22 / 10.27 | 0.062 / 11.010 | 71343 |
| native-2 | 10865.98 | 0.74 / 1.73 | 2297.99 | 3.48 / 7.17 | 0.069 / 10.508 | 69228 |

只读 **97.5%**、读写 **83.2%**（此前 95.2% / 73.0%，旧基线 98.5% / 72.4%）；mysqlslap
单行更新耗时已到 native 的 96–104%。预热同会话 profile：HFS 1769.19 TPS / avg 4.52 ms
vs native 2196.78 / 3.64 ms（**80.5%**），fsync/txn 2.39 vs 2.34。

| 自耗 | HFS | native | 说明 |
|---|---:|---:|---|
| `log_write_up_to` | **8.68%** | 8.61% | committer 等待自旋已持平（此前 24.4%） |
| `log_flusher` | **15.37%** | 4.53% | 剩余 HFS 特有项：barrier 的 on-CPU 工作（51KB 快照 + CRC + control 路径） |
| `log_writer` | 6.07% | 22.79% | HFS 写线程阻塞在同步 I/O，采样不到 |
| 内核自旋 | 9.21% | 6.27% | — |
| SPDK 轮询 | 4.33% | — | reactor 轮询（无中断） |

证据 `/home/hfs/mysql-samedisk-20260927-final/`、`/home/hfs/mysql-profile-20260927b/`。

## 元数据 journal 化（2026-09-27，Shard-107）

`SET_SIZE`/`ALLOC`/`FREE`/`SET_TIMES`/`TRUNCATE` 以 compact record 在 barrier 中一次连续写
落盘（与数据 flush 合并），namespace 形状变化仍走全量 checkpoint。同会话 A/B（8 线程
trx=1、2×10 s warmup）：

| 指标 | pre-journal | journal | 变化 |
|---|---:|---:|---:|
| MySQL trx=1 TPS | 1908.37 | 1926.61 | +1.0% |
| checkpoints（会话） | 47310 | **99** | −99.8% |
| journal transactions | — | 48111 | — |
| device flushes | 182221 | 190165 | +4.4% |

收益小的原因：barrier 的**设备往返数不变**（journal 写 + flush ≈ checkpoint 写 + flush），
profile 显示 `log_flusher` 自耗 15.37% → 15.59% 未变，即 flusher CPU 跟随 fsync **延迟**
（设备往返）而非序列化工作量；journal 化省掉的是 51 KB 全量快照序列化与 CRC。

修复的 replay 缺陷：checkpoint 可能落在 reserve 之后、commit 之前，`ALLOC` replay 原先
直接 `markInitialized` 会报 "extent is not reserved"；现按 checkpoint 的 allocator 状态
逐级推进（FREE → `markReserved` → INITIALIZED → COMMITTED）。复现脚本证据
`/home/hfs/journal-replay-repro-20260927/`（fillseq → 重挂载 replay → `transactions=1`、
`corruption=none`）。

异步 journal writer（write-behind）评估（同日，已回退）：1809.90 vs 1957.94 TPS（−5.8%）——
barrier 仍需等 append 的设备往返，额外线程与每次 mutation 的唤醒是净损失。回退后保留
Journal 内部串行化（append/checkpoint/replay/flush 共用锁）与 checkpoint 失败重试修复
（失败后 `requireCheckpoint()`，否则已消费的 staged 记录会丢）。

门禁（最终态）：CTest 26/26；Shard-104 crash matrix 通过、`hfsctl check` `corruption=none`；
RocksDB rw32 HFS fillseq 104.8%、readrandom 411%、`overwrite` 校验 109.5%（对 native）；
8j4 HFS 中位 80282 / raw 121734（0.660）、5 轮 closure 全闭合。证据
`/home/hfs/mysql-perfile-sync-ab-20260927/{pre2journal,post2journal}/`、
`/home/hfs/rocksdb-rw32-cache1g-20260927-journal3/`、`/home/hfs/fio-8j4-journal2-20260927/`、
`/home/hfs/mysql-shard104-crash-20260927-journal/`。

## 收口口径（2026-09-27，最终二进制）

MySQL 比值强烈依赖设备会话速度（同一 VM 内 native 写 TPS 可在 1.5K–2.3K 间波动），
因此并列两个会话；绝对值不可跨会话比较，比值才是口径。

| 会话 | 协议 | HFS | native | 比值 |
|---|---|---:|---:|---:|
| 快设备（native 读写 ~2.3K） | shard-105 同盘 只读 | 10793 | 11066 | 97.5% |
| 快设备 | shard-105 同盘 读写 trx=1 | 1907 | 2293 | 83.2% |
| 快设备 | 预热 trx=1（2×10 s + 20 s） | 1958 | 2297 | 85.2% |
| 慢设备（native 读写 ~1.5K） | shard-105 同盘 只读 | 8562 | 8777 | 97.6% |
| 慢设备 | shard-105 同盘 读写 trx=1 | 1476 | 1504 | 98.2% |
| 慢设备 | 预热 trx=1 | 1483 | 1629 | 91.1% |

只读稳定在 **97.5–97.6%**；读写比值在设备受限时收敛到 ~98%，在设备快、CPU/延迟主导时
为 **83–85%**。证据 `/home/hfs/mysql-samedisk-20260927-final{2}/`、
`/home/hfs/mysql-warm-ab-20260927/`。

### 优化台账（本轮全部改动，按时间序）

| 改动 | 结果 | 状态 |
|---|---|---|
| fsync 覆盖修复（原地写必须落盘） | 契约修复；同会话 +1.3% | 保留 |
| barrier 合并（ckpt+sb 单次 flush） | flush 命令 −51%、barrier 286→237 µs、+1.6% | 保留 |
| checkpoint/superblock 批量写 | meta_write 291→191 µs、+1.1% | 保留 |
| seam worker-direct 异步（owner-file 模型） | 每 I/O 140→71 µs、+1.5% | 保留 |
| `max_group_commit_wait_us = 0` | +9.2% | 保留 |
| 元数据 journal 化 | checkpoints −99.8%、+1.0% | 保留 |
| per-file `File::sync` 目标 epoch | −0.8%（噪声内），语义降级 | 回退 |
| 异步 journal writer（write-behind） | −5.8% | 回退 |

### 已知边界（profile 证据）

- 剩余 HFS 特有开销：barrier 的设备往返（耐久性必需，2 次实验证明无法再省）、
  SPDK 轮询 ~0.35 核（无中断）、内核自旋 +2.9pp；committer 等待自旋已与 native 持平
  （`log_write_up_to` 8.68% vs 8.61%）。
- 环境：设备会话间吞吐波动可达 ±40%，且有间歇性 NVMe 停顿（dmesg timeout）；
  任何性能结论必须以同会话交错测量为准。

## 新磁盘形态基线（2026-09-27 晚，当前环境）

VM 磁盘形态变更后另建基线（不与此前环境直接比较）。环境：3 块 20 GiB NVMe
（`VMware NVME_0000/0002/0001`，目标盘 `VMware NVME_0002` 仍在 `0000:13:00.0`，容量由
2.5 GiB 变为 20 GiB）；6 vCPU、1024 hugepages 不变。稳定性探针 3×10 s SPDK raw 4K
randwrite QD32 = **69628 / 68524 / 69229 IOPS（离散 1.6%）**，此前同格可达 49×。

### FIO 三方矩阵（3 轮中位数）

| I/O | 总 QD | ext4+libaio | SPDK raw | HFS | HFS/raw |
|---|---:|---:|---:|---:|---:|
| 4K | 32 | 8986 | 76034 | 70097 | 92.2% |
| 4K | 64 | 18559 | 76569 | 77091 | 100.7% |
| 16K | 32 | 32359 | 60455 | 58318 | 96.5% |
| 16K | 64 | 9842 | 62240 | 61338 | 98.6% |
| 64K | 32 | 12591 | 35795 | 33950 | 94.8% |
| 64K | 64 | 3577 | 33860 | 31716 | 93.7% |

每格 3 轮离散：raw/HFS 1.01–1.20（多数 ≤1.06）；**ext4+libaio 仍离散（最高 7.6×）**，
属 ext4/内核侧，不影响 HFS/raw 口径。

### RocksDB 32 并发、1 GiB cache（3 轮）

| 轮次 | native 写 | HFS 写 | native 读 | HFS 读 | native 校验 | HFS 校验 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 67598 | 67918 | 86786 | 111812 | 52710 | 57469 |
| 2 | 69058 | 67764 | 35726 | 109563 | 56039 | 57640 |
| 3 | 67999 | 68428 | 31981 | 113064 | 53918 | 59866 |
| **中位数** | **67999** | **67918** | **35726** | **111812** | **53918** | **57640** |

HFS/native（中位数之比）：写 **99.9%**，随机读 **313%**，校验 **106.9%**。注意本轮
**native ext4 侧离散**（readrandom 31.9–86.8K，2.7×）而 HFS 稳定（109.6–113.1K，1.03），
与 FIO 的 ext4+libaio 离散现象一致。三轮 `hfsctl check` `corruption=none`，journal
replay 跨挂载正常（`transactions=1`）。

### MySQL 同盘 + 预热

| 协议 | HFS | native | 比值 |
|---|---:|---:|---:|
| shard-105 同盘 只读 | 7982 | 8194 | 97.4% |
| shard-105 同盘 读写 trx=1 | 1377 | 1395 | 98.7% |
| 预热 trx=1（2×10 s + 20 s） | 1326.83（avg 6.02 / p99 10.84） | 1406.25（5.68 / 11.24） | 94.4% |

证据 `/home/hfs/device-probe-20260927/`、`/home/hfs/fio-three-way-20260927-newshape/`、
`/home/hfs/rocksdb-rw32-cache1g-20260927-newshape/`、
`/home/hfs/mysql-samedisk-20260927-newshape/`、`/home/hfs/mysql-warm-newshape-20260927/`。

## 复现入口

- FIO：`ALLOW_FORMAT=1 RESULT=<dir> bash fio/run_three_way_matrix_vm.sh`
- RocksDB：`ALLOW_FORMAT=1 RESULT=<dir> bash rocksdb/run_rw32_cache1g_vm.sh`
- MySQL：`RESULT=<dir> bash mysql/run_shard105_samedisk_vm.sh`
