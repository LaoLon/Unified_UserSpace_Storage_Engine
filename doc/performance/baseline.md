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

## 复现入口

- FIO：`ALLOW_FORMAT=1 RESULT=<dir> bash fio/run_three_way_matrix_vm.sh`
- RocksDB：`ALLOW_FORMAT=1 RESULT=<dir> bash rocksdb/run_rw32_cache1g_vm.sh`
- MySQL：`RESULT=<dir> bash mysql/run_shard105_samedisk_vm.sh`
