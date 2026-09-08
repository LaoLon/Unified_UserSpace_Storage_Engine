# HFS 阶段二性能基线

**记录日期**：2026-09-08
**本地代码基线**：`main@49aa2ab`（shard-48）
**VM 构建工作树**：`bd69b07`（通过本地同步脚本传入的 HFS 源码；远端 `.git` 未随源码覆盖）
**SPDK**：`f0b63ed866f0e4d3c56e73bd4a98d0042956147a`
**测试环境**：Ubuntu 24.04 VM，8 vCPU，15 GiB RAM；FIO 3.42-139-gae35；MariaDB 10.11.14；RocksDB `db_bench` 8.9.1。

本阶段在 VM 新增的 20 GiB VMware 虚拟磁盘 `/dev/sdb` 上建立了 GPT 分区和 ext4 文件系统，挂载点为 `/mnt/hfs-data`，挂载参数为 `rw,noatime`，文件系统标签为 `hfs-data`，UUID 为 `6fadef96-1c65-4c8b-8169-a9528a74a61f`，并已写入 `/etc/fstab`。FIO 的 ext4 对照文件、MariaDB/RocksDB 持久化 Aio 镜像及所有结果目录均使用该磁盘。

## 1. FIO 三路对比

### 1.1 测试口径

- 工作负载：`randrw`，读写比例 50/50；块大小 4K、8K、16K、32K、64K。
- 每个 case 256 MiB、运行 15 秒、单线程、`iodepth=32`；错误数均为 0。
- HFS：`libhfs.so` preload，`psync`、`O_DIRECT`，后端为进程私有 `Malloc0` bdev。
- SPDK：官方 `spdk_bdev` fio engine，使用同规格进程私有 `Malloc0` bdev。
- ext4：无 preload，普通文件位于 `/mnt/hfs-data`（`/dev/sdb1`）。

因此，HFS 与 SPDK 的结果反映内存 bdev 上的用户态路径开销，ext4 组反映新增虚拟磁盘上的文件系统路径；三者不是同一物理介质上的公平硬件吞吐对比。

### 1.2 总吞吐

总 IOPS 为读写 IOPS 之和，带宽为读写带宽之和。

| Block | HFS IOPS | HFS MiB/s | ext4 IOPS | ext4 MiB/s | SPDK IOPS | SPDK MiB/s |
|------:|---------:|----------:|----------:|-----------:|----------:|-----------:|
| 4K  | 64,928 | 253.63 | 9,029 | 35.27 | 1,883,077 | 7,355.77 |
| 8K  | 60,438 | 472.17 | 7,855 | 61.37 | 1,184,766 | 9,255.98 |
| 16K | 55,211 | 862.67 | 7,476 | 116.81 | 718,415 | 11,225.24 |
| 32K | 49,693 | 1,552.89 | 7,076 | 221.14 | 374,056 | 11,689.24 |
| 64K | 41,655 | 2,603.45 | 6,757 | 422.29 | 187,806 | 11,737.90 |

相对同轮 SPDK `Malloc0`，HFS 总 IOPS 为约 3.1%～22.2%；相对 `/dev/sdb1` ext4，HFS 为约 6.2～7.2 倍。后一个比例不能外推到真实 NVMe，因为介质和访问模型不同。

### 1.3 HFS 延迟

| Block | Read p50 | Read p99 | Write p50 | Write p99 |
|------:|---------:|---------:|----------:|----------:|
| 4K  | 14.400 | 40.192 | 14.400 | 41.216 |
| 8K  | 15.040 | 54.016 | 15.040 | 55.040 |
| 16K | 15.936 | 56.576 | 15.936 | 57.088 |
| 32K | 17.536 | 64.256 | 17.280 | 62.720 |
| 64K | 20.352 | 89.600 | 20.096 | 85.504 |

FIO 原始 JSON、日志和汇总位于 VM：`/mnt/hfs-data/performance-stage2-fio`；ext4 对照文件位于 `/mnt/hfs-data/fio-ext4`。

## 2. MariaDB 原生与 HFS Hook

### 2.1 测试口径

- 每组独立数据目录，10,000 行；每个 case 2,000 次查询、3 次迭代；并发 1、4、16。
- 读：`SELECT SUM(value) ... WHERE id BETWEEN 1 AND 10000`。
- 写：单行 autocommit `UPDATE`，多个连接竞争同一行，刻意覆盖持久化写路径；不是分片写入模型。
- 原生组数据目录在 `/mnt/hfs-data/performance-stage2-mysql/native-datadir`。
- HFS 组通过持久化 SPDK Aio 镜像 `/mnt/hfs-data/hfs-stage2-mysql.img`，挂载到 `/hfs/mysql`。

### 2.2 QPS 结果

| 层 | Workload | 并发 | 平均秒数 | QPS | HFS/原生 |
|---|---|---:|---:|---:|---:|
| native | read  | 1  | 2.007 | 996.51 | 100.0% |
| native | read  | 4  | 0.519 | 3,853.56 | 100.0% |
| native | read  | 16 | 0.345 | 5,797.10 | 100.0% |
| HFS | read  | 1  | 1.936 | 1,033.06 | 103.7% |
| HFS | read  | 4  | 0.538 | 3,717.47 | 96.5% |
| HFS | read  | 16 | 0.368 | 5,434.78 | 93.7% |
| native | write | 1  | 0.481 | 4,158.00 | 100.0% |
| native | write | 4  | 0.207 | 9,661.84 | 100.0% |
| native | write | 16 | 0.112 | 17,857.14 | 100.0% |
| HFS | write | 1  | 1.182 | 1,692.05 | 40.7% |
| HFS | write | 4  | 0.516 | 3,875.97 | 40.1% |
| HFS | write | 16 | 0.180 | 11,111.11 | 62.2% |

HFS 读路径基本接近原生，16 并发约低 6.3%；写路径明显受单行 autocommit 和 fsync/元数据提交影响，1/4/16 并发分别为原生的 40.7%/40.1%/62.2%。结果目录为 `/mnt/hfs-data/performance-stage2-mysql`。

## 3. RocksDB 持久化 Aio

### 3.1 测试口径

- `stage48_matrix.sh`，3 轮；每轮 20,000 条、value 256 B、WAL 开启、mmap 关闭、运行 60 秒。
- 后端为持久化 SPDK Aio，镜像 `/mnt/hfs-data/hfs-stage2-rocksdb.img`，数据库路径 `/hfs/rocks/benchmark`。
- `fillseq` 为预热/建库，随后执行 `readwhilewriting` 的 1 线程和 4 线程；每轮均重新格式化并校验。
- 结果中的 `errors=0`，且每个 case 的 `spdk_submit == spdk_complete`、`spdk_reject=0`。

### 3.2 三轮统计

| Case | ops/s 中位数 | 范围 | p50 中位数 (µs) | p99 中位数 (µs) | 最大 RSS (KiB) |
|---|---:|---:|---:|---:|---:|
| fillseq | 3,779 | 3,677–3,797 | 232.046 | 685.251 | 35,192 |
| readwhilewriting-1t | 555,043 | 552,783–572,910 | 238.787 | 736.338 | 106,728 |
| readwhilewriting-4t | 2,501,188 | 2,339,122–2,758,714 | 329.831 | 975.764 | 147,420 |

4 线程相对 1 线程约 4.51 倍吞吐；4 线程 p99 约 0.98 ms。三轮结果的 4 线程 ops/s 标准差约 172,769 ops/s，离散度明显高于 1 线程，后续应结合 vCPU 调度、SPDK reactor 绑核和 Aio 后端抖动继续拆分。

RocksDB 原始日志、每轮统计和汇总位于 VM：`/mnt/hfs-data/performance-stage2-rocksdb`。最终流程包含 `ldb checkconsistency`，无错误退出。

## 4. 结论与下一步

1. `/dev/sdb1` 已成为独立的持久化测试盘，根盘清理后保持约 5.9 GiB 可用，挂载盘保持约 19 GiB 可用；本轮没有再发生磁盘空间阻塞。
2. HFS 的内存 bdev FIO 延迟和 IOPS 显著优于 `/dev/sdb1` ext4，但与裸 SPDK `Malloc0` 仍有数量级差距，下一步应优先分析请求提交、同步边界、SPDK reactor/绑核和固定 DMA 资源。
3. MariaDB 读路径已接近原生；单行 autocommit 写路径仍是主要性能差距，应分别评估批量事务、group commit、顺序写合并和 metadata checkpoint 放大，不能用混合 QPS 掩盖该差异。
4. RocksDB 端到端流程在持久化 Aio 上稳定通过，4 线程吞吐有较好扩展，但轮间离散度偏高。下一轮建议固定 CPU affinity，增加原生 ext4 对照，并补充真实 NVMe/`io_uring` 或 native libaio 边界测试。

所有数据是同一 VM、同一版本和指定工作负载下的相对基线，不构成生产硬件绝对性能承诺。
