# FIO 基线对比（HFS vs ext4）

> 记录时间：2026-09-02（shard-23）
> 数据文件：`build/fio_results/*.raw`（含 HFS 与 ext4 两组）

## 环境

| 项 | 值 |
|---|---|
| VM | 192.168.1.250，Ubuntu（内核 6.x x86-64），2 核 |
| fio | 3.42-139-gae35 |
| HFS 存储 | SPDK malloc bdev 256MB（纯内存），cluster 1MB |
| ext4 | 根文件系统 LVM ext4（磁盘） |
| 引擎 | psync（pread/pwrite，O_DIRECT） |
| HFS 前缀 | /hfs/fio（libhfs LD_PRELOAD，单进程） |
| ext4 目录 | /tmp/fio-ext4 |

## 命令

```bash
# HFS（LD_PRELOAD 单进程）
bash scripts/fio/run_fio.sh build-cmake/libhfs.so build/fio_results

# HFS + ext4 对照（第三参数为 ext4 目录）
bash scripts/fio/run_fio.sh build-cmake/libhfs.so build/fio_results /tmp/fio-ext4
```

- randrw：4K 随机混合读写（rwmixread=50），iodepth=32，64MB 文件，10s
- seq：128K 顺序读写（rw=rw 混合作业），iodepth=32，64MB 文件，10s

## 结果（2026-09-02，runtime=10s）

| 场景 | HFS read IOPS | HFS write IOPS | HFS BW | ext4 read IOPS | ext4 write IOPS | ext4 BW | 比值（HFS/ext4） |
|---|---|---|---|---|---|---|---|
| randrw 4K | 12880 | 12882 | 52.8 MB/s | 3617 | 3624 | 14.8 MB/s | ~3.6x |
| seq 128K | 9810 | 9790 | 1.29 GB/s | 2541 | 2536 | 333 MB/s | ~3.9x |

## 结果 2：1GB 随机读写（bs 4k~64k，2026-09-02，runtime=15s）

配置：`bash scripts/fio/run_randrw_1g.sh`（HFS：LD_PRELOAD + 1.5GB malloc bdev；ext4：/tmp/fio-ext4-1g；SPDK：spdk_bdev 插件）

| bs | HFS IOPS | HFS BW (MB/s) | ext4 IOPS | ext4 BW (MB/s) | 比值 |
|---|---|---|---|---|---|
| 4k | 12602 | 49.2 | 3555 | 13.9 | 3.55x |
| 8k | 12721 | 99.4 | 3437 | 26.9 | 3.70x |
| 16k | 12494 | 195.2 | 3615 | 56.5 | 3.46x |
| 32k | 12001 | 375.0 | 3320 | 103.8 | 3.61x |
| 64k | 11056 | 691.0 | 3247 | 203.0 | 3.40x |

## 结果 3：1GB 随机读写 —— HFS vs ext4 vs 裸 SPDK bdev（2026-09-02，runtime=15s）

SPDK bdev 用官方 fio 插件（`LD_PRELOAD=/root/code/spdk/build/fio/spdk_bdev`，ioengine=spdk_bdev，--thread=1，iodepth=32，同一 malloc bdev 配置），作为"无文件系统层"的上限参考。

| bs | HFS IOPS | ext4 IOPS | SPDK bdev IOPS | HFS/SPDK |
|---|---|---|---|---|
| 4k | 12788 | 3270 | 24656 | 0.52 |
| 8k | 12224 | 3421 | 25089 | 0.49 |
| 16k | 12133 | 3218 | 24578 | 0.49 |
| 32k | 11685 | 2840 | 22821 | 0.51 |
| 64k | 10864 | 2911 | 20879 | 0.52 |

- HFS 相对裸 bdev 约为一半 IOPS（文件系统层开销：路径解析/inode 表/锁/同步引擎），相对 ext4 仍 3.5~4x（内存盘 vs 磁盘）。
- 口径说明：SPDK bdev 为异步引擎（iodepth=32），HFS/ext4 为 psync 同步——差距包含"同步 vs 异步"因素。
- 结果文件：`build/fio_1g_results/{hfs,ext4,spdk}_<bs>.raw`。

## 结论

1. HFS（malloc bdev 纯内存）在 4K 随机与 128K 顺序场景均快于磁盘 ext4 约 3.6~3.9 倍，
   符合"内存盘 vs 磁盘 + O_DIRECT"的预期差距（ext4 受磁盘介质限制，HFS 无 I/O 落盘）。
2. HFS 顺序 128K 达 1.28GB/s，接近 malloc bdev 内存带宽的量级；随机 4K 约 52.8MB/s。
3. 对比口径说明：本基线并非"同介质公平对比"（HFS 是内存盘），仅验证拦截层全链路
   （open/ftruncate/pread/pwrite/fsync/stat）在 fio 负载下可用且数字合理；后续若换
   持久化 bdev（文件/用户态 NVMe），需重新采数。
4. 已知限制：fio 的 libaio 引擎（io_submit 走系统调用绕过拦截）暂不支持，基线使用
   psync；每进程 malloc bdev 独立，跨进程数据不共享，HFS 数字为单进程基线。

## shard-31 请求级 I/O 回归（2026-09-04）

配置：malloc bdev、O_DIRECT、psync、iodepth=32、单进程 `thread=1`、runtime=10s；结果由
`scripts/fio/run_fio.sh` 从 fio JSON 输出 IOPS、带宽和 completion latency p50/p99。

| 场景 | read IOPS | write IOPS | read BW | write BW | read p50/p99 (ns) | write p50/p99 (ns) |
|---|---:|---:|---:|---:|---:|---:|
| randrw 4K | 6839.52 | 6806.22 | 28.01 MB/s | 27.88 MB/s | 58112 / 120320 | 58112 / 119296 |
| seq 128K | 5772.32 | 5750.12 | 756.59 MB/s | 753.68 MB/s | 73216 / 142336 | 73216 / 142336 |

该结果用于验证 shard-31 的请求级上下文、固定 DMA pool、向量 I/O 和并发 positioned I/O
链路；与 2026-09-02 的旧基线不作跨后端绝对性能结论。fio 的 fork job 模式不作为 HFS
并发验收口径，避免复用父进程 SPDK 状态。

## shard-35 性能回归与阶段收口（2026-09-05）

### FIO：Malloc bdev 同口径回归

环境：VM Ubuntu，8 vCPU，fio 3.42-139-gae35；HFS 使用 Malloc bdev，`psync`、`O_DIRECT`、
`iodepth=32`、单进程 `thread=1`、10s。命令：

```bash
bash scripts/fio/run_fio.sh build-cmake/libhfs.so /var/tmp/hfs-shard35-fio-results
```

结果（延迟单位 ns）：

| 场景 | read IOPS | write IOPS | read BW | write BW | read p50/p99 | write p50/p99 | error |
|---|---:|---:|---:|---:|---:|---:|---:|
| randrw 4K | 6471.75 | 6436.36 | 25.28 MB/s | 25.14 MB/s | 61184 / 136192 | 61184 / 132096 | 0 |
| seq 128K | 5675.93 | 5653.03 | 709.49 MB/s | 706.63 MB/s | 74240 / 146432 | 74240 / 144384 | 0 |

相对本文件 shard-31 的同配置结果，randrw read/write IOPS 分别下降约 5.4%/5.4%，seq
read/write IOPS 分别下降约 1.7%/1.7%，均未超过 10% 门槛。该比较只适用于 Malloc bdev
微基准；不能与持久化 AIO/NVMe 数据库吞吐混比。

### FIO：256MiB 三路对比（2026-09-05）

环境：同一 VM、8 vCPU、fio 3.42-139-gae35；`randrw` 50/50、`O_DIRECT`、单线程、15s，
块大小覆盖 4K/8K/16K/32K/64K，测试范围统一为 256MiB。无 LD 组使用 ext4 普通文件，
SPDK 组使用官方 `spdk_bdev` fio engine + `Malloc0`，HFS 组使用当前 `libhfs.so` +
`LD_PRELOAD` + Malloc bdev。SPDK/HFS 为进程私有内存 bdev，和 ext4 组不属于同一物理存储介质，
因此该表用于链路开销/吞吐对比，不作真实盘绝对性能结论。

| bs | 无 LD 总 IOPS | SPDK 总 IOPS | HFS 总 IOPS | 无 LD BW | SPDK BW | HFS BW | 错误 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 4K | 7,101.99 | 50,022.53 | 12,972.10 | 27.74 MiB/s | 195.40 MiB/s | 50.67 MiB/s | 0 |
| 8K | 6,145.86 | 48,113.46 | 12,591.09 | 48.01 MiB/s | 375.89 MiB/s | 98.37 MiB/s | 0 |
| 16K | 6,270.65 | 46,526.90 | 12,346.04 | 97.98 MiB/s | 726.98 MiB/s | 192.91 MiB/s | 0 |
| 32K | 6,151.79 | 43,958.40 | 12,455.84 | 192.24 MiB/s | 1,373.70 MiB/s | 389.24 MiB/s | 0 |
| 64K | 6,063.86 | 39,729.35 | 11,755.48 | 378.99 MiB/s | 2,483.08 MiB/s | 734.72 MiB/s | 0 |

HFS 相对 SPDK 的总 IOPS 为 25.9%～29.6%；相对无 LD ext4 为 182.7%～204.9%。HFS
4K/64K 的读 p50/p99 为 61.2/120.3µs、64.3/127.5µs，写 p50/p99 为 61.2/119.3µs、
63.7/171.0µs。原始 JSON 和日志保存在 VM 的
`/var/tmp/hfs-fio-256m-compare-20260904-173808`。

### RocksDB：持久化 AIO benchmark

命令：

```bash
HFS_BENCH_NUM=20000 HFS_BENCH_DURATION_SECONDS=60 \
  bash scripts/db/database_benchmark.sh build-cmake/libhfs.so /hfs \
  config/hfs_db_aio.json /var/tmp/hfs-shard35-benchmark.img \
  /var/tmp/hfs-shard35-benchmark
```

环境记录：Ubuntu 6.8.0-138-generic，8 vCPU，RocksDB `db_bench 8.9.1`，1 GiB Aio 镜像，
256-byte value，WAL 开启，mmap 关闭。延迟来自 `rocksdb.db.write.micros`，单位为 µs。

| case | ops/sec | p50 (µs) | p99 (µs) | elapsed | CPU | errors |
|---|---:|---:|---:|---:|---:|---:|
| fillseq | 2491 | 337.26 | 1493.22 | 8.82s | 120% | 0 |
| readwhilewriting-1t | 63854 | 345.09 | 972.45 | 60.54s | 222% | 0 |
| readwhilewriting-4t | 235831 | 379.63 | 1164.78 | 61.24s | 518% | 0 |

4 线程吞吐约为 1 线程的 3.69 倍。4 线程 DMA active peak 为 4，SPDK submit/complete 为
290185/290185，reject=0；1 线程对应 active peak=1、316517/316517、reject=0。fd/meta
锁、DMA pool 和 SPDK 计数器仅在设置 `HFS_STATS_PATH` 时开启，默认关闭；本次报告中三组
测试的 HFS error counter 均为 0。

本阶段数据库验收脚本已分别完成三轮完整 MySQL/MariaDB 与 RocksDB 流程；本次收口又在
最终统计代码上完成了 MariaDB 持久化恢复 smoke、RocksDB 缩短持久化恢复 smoke，以及全套
preload/fd/元数据/I/O/POSIX/runtime 回归。

阶段结论：性能回归门槛满足，MySQL/MariaDB-compatible server 与 RocksDB 的单进程持久化
兼容路径可以标记为 supported；多个 HFS 进程同时挂载同一后端、mmap、native libaio 和
io_uring 仍明确不支持。

## shard-39 可观测性与公平 scaling smoke（2026-09-06）

脚本：`scripts/db/hfs_scaling_benchmark.sh`。VM 为 Ubuntu 6.8.0-138-generic、8 vCPU、
fio 3.42-139-gae35；HFS 使用 `build/cmake-release/libhfs.so`、Malloc bdev、LD_PRELOAD
同步 `psync`，raw 使用 VM 内 `/root/code/spdk/build/fio/spdk_bdev`。SPDK 固定为
`f0b63ed866f0e4d3c56e73bd4a98d0042956147a`，HFS 与 raw 均为独立进程，raw QD=1 与 HFS
有效 QD=1 对齐。为快速验收，本轮采用 16 MiB、5 s、HFS 线程 1、raw QD 1、metadata count 1；
脚本默认值仍覆盖线程 1/2/4/8/16、QD 1/4/16/32、metadata 1K/10K/60K。

| engine/workload | bs | threads/QD | IOPS | p50/p99 (ns) | CPU | RSS (KiB) | errors | submit/complete | effective QD |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| HFS randread | 4K | 1 | 62,815 | 14,912/38,144 | 129% | 20,404 | 0 | 318,346/318,346 | 1 |
| HFS randrw | 4K | 1 | 62,248 | 14,912/40,704; write 14,784/42,240 | 129% | 20,404 | 0 | 315,453/315,453 | 1 |
| HFS randwrite | 4K | 1 | 62,190 | — | 128% | 20,396 | 0 | 311,090/311,090 | 1 |
| HFS seqread | 128K | 1 | 40,600 | 22,400/67,072 | 131% | 20,788 | 0 | 203,281/203,281 | 1 |
| HFS seqwrite | 128K | 1 | 40,343 | — | 130% | 20,660 | 0 | 201,793/201,793 | 1 |
| raw read | 4K | QD 1 | 2,939,094 | 9,096/402 | 95% | 20,476 | 0 | — | 1 |
| raw write | 4K | QD 1 | 2,951,115 | — | 95% | 20,352 | 0 | — | 1 |
| raw read | 128K | QD 1 | 347,815 | 1,116/322 | 95% | 20,592 | 0 | — | 1 |
| raw write | 128K | QD 1 | 254,172 | — | 95% | 20,468 | 0 | — | 1 |

Metadata count=1 measured create/stat/readdir/rename/unlink as 0.000740/0.000007/0.000008/
0.000621/0.000664 s. The stats-off/on pair was 63,446.91/62,540.80 IOPS, or 1.43% measured
instrumentation overhead. With `HFS_STATS_TIMING=1`, a 2-job 1 s run emitted six per-thread
shards, runtime queue peak 2, lock wait/hold histograms, and metadata log/image/page bytes;
default timing remains off to keep the stats-on hot path within the 3% budget.

## shard-40 fd lookup smoke（2026-09-06）

在同一 VM、同一持久化 Aio bdev 上，使用 `scripts/db/fd_lookup_benchmark.sh`，4 KiB
randread、iodepth=1、每轮 1 秒、16 MiB 文件，统计 1/4/16/64 个 fio 线程。结果为：

| 线程数 | IOPS | error | CPU | RSS (KiB) |
|---:|---:|---:|---:|---:|
| 1 | 15,971.03 | 0 | 110% | 20,680 |
| 4 | 31,687.31 | 0 | 116% | 20,936 |
| 16 | 33,928.07 | 0 | 119% | 22,840 |
| 64 | 27,048.76 | 0 | 215% | 34,488 |

该烟测用于确认分桶 fd registry 在高并发和 fd 复用下没有错误或请求丢失，不作为跨硬件
绝对性能承诺；完整对比仍需固定 CPU 亲和、bdev 类型和 fio 版本后执行。

额外用 shard-39 源码作为 baseline，在 VM 复用同一 SPDK、同一持久化 Aio、4 KiB
randread/4 threads/iodepth=1/5 s 口径重跑：baseline 31,131.97 IOPS，shard-40 为
31,062.79 IOPS（-0.22%，error 均为 0）。`HFS_STATS_TIMING=1` 的 4 线程样本记录
fd wait/hold 196,381/196,381、contention 33，metadata contention 30/97,173，
runtime queue wait 3.13 s；因此后续收益应从 owner 队列和 metadata/I/O 区间并发中取得，
不将本轮结果解释为绝对吞吐提升。

## shard-41 per-inode/区间 I/O 并发（2026-09-06）

环境：同一 VM、8 vCPU、Ubuntu 6.8.0-138-generic、fio 3.42-139-gae35；持久化 Aio bdev，
`LD_PRELOAD` + psync、O_DIRECT、4 KiB randrw、iodepth=1、每轮 1 s、16 MiB 文件；统计显式
开启 `HFS_STATS_TIMING=1`。脚本为 `scripts/db/inode_io_benchmark.sh`，线程矩阵为 1/2/4/8/16。

| 线程 | IOPS | CPU | RSS (KiB) | runtime queue peak | IO contention | metadata contention | error |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 14,085.91 | 110% | 20,776 | 1 | 226 | 4 | 0 |
| 2 | 24,451.55 | 114% | 20,648 | 2 | 299 | 14 | 0 |
| 4 | 26,999.00 | 121% | 20,916 | 4 | 476 | 35 | 0 |
| 8 | 29,893.11 | 122% | 21,028 | 7 | 866 | 50 | 0 |
| 16 | 30,085.91 | 123% | 22,960 | 15 | 1,605 | 110 | 0 |

4 线程为 1 线程的约 1.92 倍，尚未达到阶段 3.0 倍扩展目标；queue peak、metadata contention
随线程增加，表明主瓶颈已经从 fd 全局锁转移到同步 owner queue、逐请求 completion 和
metadata write-back。该表与 shard-39/40 的 randread 基线不是同一 workload，不用于跨阶段
绝对吞吐宣称；后续优化应优先处理 shard-42 的索引和 shard-45 的异步提交。

### shard-41 owner batch 修复迭代

数据 I/O 现在可通过 owner-side batch dispatcher 合并并发请求的 owner queue 通知，
`HFS_RUNTIME_BATCH=0` 保留原始路径用于 A/B。VM 上 5 秒 Aio/psync/O_DIRECT/4 KiB randrw
测得 4 线程 batch on 24.35k IOPS、off 24.73k IOPS，差异在当前单 owner/统计开销下不构成
绝对吞吐收益；其价值是消除每请求一次通知的结构性放大，并为后续异步 API 保留批量入口。
fast-I/O smoke、8 线程 runtime stress 和 Make/CMake 构建均通过，错误数为 0。

## shard-53 数据面优化阶段收口（2026-09-09）

本轮以 `main@0a79cbf`、VM 内 SPDK 锁定 commit
`f0b63ed866f0e4d3c56e73bd4a98d0042956147a` 为基线。Make/CMake Release、fast I/O、4-reactor
stripe（`266/266`）、DMA pool backpressure（lease `128/128`）、线程 1/2/4/8/16/32 并发矩阵、
注册 DMA 和 reactor=2/8 scaling 全部通过，错误数为 0；注册 DMA 输出为
`REGISTERED DMA IO OK direct=1 duplicate=EEXIST lifetime=closed`。

当前版本的 RocksDB 缩短持久化回归也通过：5000 条 fillseq、readrandom、overwrite、
readwhilewriting、WAL、flush/compact、SIGKILL recovery、跨进程 reopen 和 `ldb checkconsistency`
完成，输出 `ROCKSDB PERSISTENT RECOVERY OK`。MariaDB/MySQL 的完整初始化、SQL、事务、正常
重启、SIGKILL 恢复和 4/16 并发客户端结果继续引用 shard-33～35 的三轮持久化证据；本轮没有
把 CRLF helper 或正常 shutdown 长等待样本混入性能表。

NVMe 结果仍使用上一轮 stats-off、相同 QD 口径的有效样本：4 KiB QD32 HFS/raw SPDK
read/write 为 `110.4k/164.2k`、`37.0k/114.9k IOPS`；64 KiB QD32 为
`14.9k/60.6k`、`13.5k/46.6k IOPS`，HFS p99 `72～85 ms`，raw SPDK `约 1.8～2.0 ms`。
本次 VM 缺少可用的 SPDK FIO engine，且没有获得可覆盖 NVMe 测试盘授权，因此没有生成新的
FIO raw 文件；该限制是环境边界，不是 HFS 性能结果，后续 fresh run 必须先补齐 engine、
设备授权和脚本换行清理。

阶段结论：shard-49～53 的代码和回归已经收口；HFS 支持单进程持久化 AIO/NVMe 上的
MySQL/MariaDB-compatible server、RocksDB、同步 POSIX 和 async libhfs。普通 POSIX buffer
仍可能走 bounce copy，多个 HFS 进程共享挂载、mmap、native libaio/io_uring 继续不在支持范围。

## shard-59 性能矩阵收口（2026-09-09）

新增 `scripts/db/shard59_performance_matrix.sh`，统一输出持久化 Aio 下的 HFS/native
FIO、RocksDB 和 MariaDB/MySQL 结果。三轮短样本使用 FIO 3 s、RocksDB 5 s、MySQL
100 queries，所有 workload errors=0；详细表格见
[`performance/hfs_shard59_performance_matrix.md`](../../performance/hfs_shard59_performance_matrix.md)。

本轮同时修复了 FIO JSON 收尾时 `hfsIoDone()` completion waiter 唤醒后的 request UAF，
以及 MariaDB bootstrap 强制回收后的 backend flock 启动竞争。当前 VM 没有 NVMe 设备和
SPDK FIO plugin，direct-NVMe 项按脚本输出 SKIP；不得把 Aio/Malloc 结果用作 NVMe 替代。
