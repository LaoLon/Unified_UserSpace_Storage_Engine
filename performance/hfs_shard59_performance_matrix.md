# HFS shard-59 性能矩阵与阶段收口

> 记录时间：2026-09-09
> 目标：验证 shard-54～58 的 bounce-copy、请求放大和 completion 生命周期改动，并完成
> FIO、RocksDB、MariaDB/MySQL 的统一回归。
> HFS source commit：`3443c09`；本报告脚本/文档收口 commit：`4f2d6a1`。
> SPDK：VM 内 Git 工作树，锁定 `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`。

## 测试口径

本次验收使用 VM 上持久化 Aio 镜像，避免把不同后端混为一谈：

| 项目 | 配置 |
|---|---|
| HFS | `build/lib/libhfs.so` + `LD_PRELOAD` + `Aio0` |
| native FIO | 同一 VM 的 ext4 文件，不加载 HFS |
| FIO | psync/O_DIRECT/randrw 50/50，4K/8K/16K/32K/64K，3 s |
| RocksDB | stage48 `fillseq`、`readwhilewriting`，1/4 threads，5 s |
| MariaDB | native ext4 与 HFS persistent-Aio，read/write，1/4/16 并发 |
| 统计 | stats-off 主口径；错误数必须为 0 |
| 本轮规模 | shard59 3-round short：3 轮、RocksDB 5000 条、MySQL 100 queries |

执行入口：

```bash
HFS_SHARD59_ROUNDS=3 \
HFS_SHARD59_DURATION_SECONDS=5 \
HFS_SHARD59_FIO_RUNTIME_SECONDS=3 \
HFS_SHARD59_FIO_SIZE=32m \
HFS_SHARD59_DB_NUM=5000 \
HFS_SHARD59_MYSQL_QUERIES=100 \
HFS_SHARD59_MYSQL_ITERATIONS=1 \
bash scripts/db/shard59_performance_matrix.sh \
  build/lib/libhfs.so config/hfs_db_aio.json \
  /var/tmp/hfs-shard59-3r.img /var/tmp/hfs-shard59-3r
```

## FIO：HFS 与 native ext4

结果来自同一轮完整脚本，`errors=0`：

| bs | HFS IOPS | native IOPS | HFS BW | native BW | HFS/native |
|---:|---:|---:|---:|---:|---:|
| 4K | 14,663.11 | 14,267.24 | 57.28 MiB/s | 55.73 MiB/s | 1.03x |
| 8K | 14,486.17 | 14,160.00 | 113.17 MiB/s | 110.62 MiB/s | 1.02x |
| 16K | 14,348.22 | 13,414.86 | 224.19 MiB/s | 209.61 MiB/s | 1.07x |
| 32K | 13,486.50 | 12,749.42 | 421.45 MiB/s | 398.42 MiB/s | 1.06x |
| 64K | 12,488.50 | 10,703.77 | 780.53 MiB/s | 668.99 MiB/s | 1.17x |

HFS 在本 Aio/ext4 口径下没有错误，64K 的吞吐高于 native；这不是 raw SPDK/NVMe 结论，
只用于确认 persistent-Aio 文件系统路径没有新的请求丢失或异常放大。

三轮短样本的总 IOPS 中位数如下（每轮的完整 JSON/TSV 位于 VM 的
`/var/tmp/hfs-shard59-3r/fio-aio/round-*`）：

| bs | HFS median IOPS | native median IOPS | HFS/native |
|---:|---:|---:|---:|
| 4K | 14,658.78 | 13,704.10 | 1.07x |
| 8K | 15,139.62 | 14,084.64 | 1.07x |
| 16K | 14,074.31 | 13,551.82 | 1.04x |
| 32K | 14,065.64 | 12,860.05 | 1.09x |
| 64K | 12,411.86 | 11,480.00 | 1.08x |

## RocksDB：持久化 Aio

| case | ops/sec | p50 (µs) | p99 (µs) | errors |
|---|---:|---:|---:|---:|
| fillseq | 7,018 | 144.73 | 368.86 | 0 |
| readwhilewriting-1t | 1,057,228 | 131.87 | 361.65 | 0 |
| readwhilewriting-4t | 3,861,609 | 194.01 | 523.89 | 0 |

stage48 矩阵返回 `DATABASE BENCHMARK OK` 和 `STAGE48 MATRIX OK`。三轮中位数为
fillseq 7,404 ops/s、readwhilewriting-1t 1,077,791 ops/s、readwhilewriting-4t
3,681,855 ops/s。它不能替代 shard-35 的 60 秒正式吞吐数据，作用是确认 shard-58
之后的 vector/RMW/resize 路径没有破坏 WAL、flush/compact 和读写并发。

## MariaDB/MySQL：persistent-Aio 对照

| workload | concurrency | native QPS | HFS QPS | HFS/native |
|---|---:|---:|---:|---:|
| read | 1 | 1,086.96 | 1,000.00 | 0.92x |
| write | 1 | 6,250.00 | 847.46 | 0.14x |
| read | 4 | 3,846.15 | 3,571.43 | 0.93x |
| write | 4 | 11,111.11 | 1,960.78 | 0.18x |
| read | 16 | 5,882.35 | 4,347.83 | 0.74x |
| write | 16 | 20,000.00 | 6,666.67 | 0.33x |

初始化与 workload 均返回 `MYSQL STAGE2 OK`。写 QPS 差距仍主要反映 HFS 的 POSIX
元数据、同步和持久化提交成本，不能用 FIO 的 Aio 对照直接推导数据库性能。

三轮中位数的关键对照：

| workload | concurrency | native QPS | HFS QPS | HFS/native |
|---|---:|---:|---:|---:|
| read | 1 | 1,075.27 | 1,041.67 | 0.97x |
| write | 1 | 5,882.35 | 833.33 | 0.14x |
| read | 4 | 3,846.15 | 3,448.28 | 0.90x |
| write | 4 | 14,285.71 | 1,923.08 | 0.13x |
| read | 16 | 5,882.35 | 4,761.90 | 0.81x |
| write | 16 | 25,000.00 | 6,666.67 | 0.27x |

## completion 生命周期修复验证

shard-59 前的 FIO JSON 路径曾稳定触发 `libhfs.so` SIGSEGV。根因是
`hfsIoDone()` 调用 `hfsRequestComplete()` 唤醒同步 waiter 后继续读取栈 request。修复后：

- FIO `--output-format=json` 32 MiB/4K/randrw/3 s：退出码 0，JSON 正常生成；
- shard-58 request reduction、I/O error injection、shard-59 FIO/RocksDB/MySQL：均无
  段错误，错误数为 0；
- completion trace 在唤醒 waiter 前导出，SPDK submit/complete 继续闭合。

## Direct-NVMe 状态

本轮脚本按前置条件检查 direct-NVMe：

```text
SKIP direct NVMe: spdk_fio_plugin_missing; no_nvme_device;
```

当前 VM 没有 `/dev/nvme*`，且缺少 `/root/code/spdk/build/fio/spdk_bdev`，因此没有把
Aio/Malloc 数字冒充 NVMe。真实 NVMe 的上一阶段 stats-off 结果仍保存在
[`stage2_nvme_native_baseline.md`](stage2_nvme_native_baseline.md)，其中已记录 HFS/raw
SPDK 的 QD32 差距、p99 和退出阶段限制。具备设备直通、SPDK FIO plugin 后，应使用同一
脚本补跑至少三轮并回填本文件的 NVMe 表格。

## 阶段结论

- shard-59 的可执行回归矩阵完成：Aio FIO、RocksDB、MariaDB/MySQL 全绿；
- shard-58 的请求收敛没有引入数据错误，completion UAF/段错误已修复；
- direct-NVMe 属于环境阻塞项，已明确记录为 SKIP，不影响代码回归结论，也不宣称达到
  4K QD32 的 NVMe 性能门槛；
- 后续若要继续追 NVMe 差距，优先补齐设备和 raw SPDK plugin，再按本文件的固定口径
  保存三轮原始 JSON、trace、stats、CPU/RSS 和 error log。
