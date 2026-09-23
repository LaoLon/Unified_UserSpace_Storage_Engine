# RocksDB/HFS CPU 隔离定位

测试设备为同一块 `/dev/nvme0n1`，BDF `0000:0b:00.0`。统一 workload
为 8 个 worker、每个 worker 20,000 个 key，总计 160,000 个唯一 key，
key 16 B、value 100 B。每个 timed read 前在同一进程内完成一次 warm-up，
然后运行 60 秒热读；每组重复 5 次。

前两版观测器分别因结束快照过早、主线程忙等而丢弃；本文只采用最终版
`cpu-isolation-20260922` 批次。观测器和 bdev 计数器为本轮临时测量代码，
测试完成后已从生产构建中清理。

## 1. CPU 配置

| 项目                    | 共享组       | 隔离组       |
| :---------------------- | :----------- | :----------- |
| data reactor            | CPU 3        | CPU 4        |
| 测试 worker             | CPU 2/3      | CPU 2/3      |
| 进程允许 CPU            | CPU 2/3/4    | CPU 2/3/4    |
| metadata reactor 观测   | CPU 0，约 100% | CPU 0，约 100% |

进程必须允许 CPU 4，否则 `taskset -c 2-3` 会使 data reactor 无法迁移到
CPU 4。测试 worker 在观测程序内部显式固定到 CPU 2/3，因此 worker 口径
没有变化。

## 2. 热读结果

每一行均为约 60 秒；`cache_miss=0`、`bdev_data_read_ops=0`、
`bdev_data_read_bytes=0` 在 10 次 timed read 中全部成立。

| Run | 共享 CPU 3 ops | 共享 ops/s | 隔离 CPU 4 ops | 隔离 ops/s |
| ---: | --------------: | ---------: | --------------: | ---------: |
| 1 | 6,178,622 | 102,960.870 | 6,856,820 | 114,262.299 |
| 2 | 6,173,740 | 102,878.342 | 6,853,288 | 114,200.516 |
| 3 | 6,275,258 | 104,571.229 | 6,859,228 | 114,300.565 |
| 4 | 6,186,571 | 103,097.060 | 6,870,004 | 114,479.864 |
| 5 | 6,226,967 | 103,765.816 | 6,851,262 | 114,168.550 |
| **平均** | **6,208,232** | **103,454.664** | **6,858,120** | **114,282.359** |

隔离 CPU 4 后平均吞吐提升 `10.47%`，中位数从 `103,097.060` 提升到
`114,262.299 ops/s`，提升 `10.83%`。所有 Get 成功命中，实际请求范围
均为 `0..159999`。

## 3. 线程 CPU 和上下文切换

以下为 5 次运行中每个线程的观测范围；CPU 百分比由
`/proc/<pid>/task/<tid>/stat` 的 user/system jiffies 计算，context switch
为 `/proc/<tid>/status` 的阶段增量。

| 线程组 | CPU | 每线程 CPU | voluntary cs | nonvoluntary cs |
| :----- | --: | :--------- | :----------- | :-------------- |
| 共享 worker | 2 | 24.73%–25.05% | 1,008–1,127 | 7,872–11,565 |
| 共享 worker | 3 | 21.42%–21.72% | 664–752 | 50,882–53,148 |
| 隔离 worker | 2 | 24.60%–25.05% | 595–703 | 8,004–11,299 |
| 隔离 worker | 3 | 24.73%–25.02% | 603–736 | 8,017–11,599 |
| 共享 data reactor | 3 | 13.30%–13.45% | 192,152–193,668 | 0–3 |
| 隔离 data reactor | 4 | 29.02%–29.95% | 304,067–312,872 | 17–39 |

关键变化是 CPU 3 上 worker 的 nonvoluntary context switch 从约
`50.9k–53.1k` 降到 `8.0k–11.6k`，同时 CPU 3 worker 利用率从约
`21.5%` 恢复到约 `24.9%`。data reactor 从 CPU 3 移到 CPU 4 后自身
利用率升高，说明原先确实受到了 CPU 3 争用。

SPDK reactor 的 OS `comm` 在该构建中继承为 `rocksdb_cpu_iso`，因此
metadata/data 线程不能仅靠 `comm` 名称区分；data reactor 通过配置目标
CPU 及对应的 busy thread（CPU 3/CPU 4）确认。另有一个 CPU 0 上约 100%
的 runtime 线程，两组均存在，未随 data reactor 配置改变。

## 4. bdev 读取和结论

10 个 timed read 的 bdev 计数均为：

```text
bdev_control_read_ops = 0
bdev_control_read_bytes = 0
bdev_data_read_ops = 0
bdev_data_read_bytes = 0
RocksDB block_cache_miss = 0
```

因此本次性能差异不是 NVMe 读延迟造成的，而是热读路径上的 CPU 争用：
data reactor 与 CPU 3 上的测试 worker 共享时，worker 发生大量非自愿切换；
移到 CPU 4 后平均吞吐提升约 10.47%。

## 5. 可复现命令

构建：

```bash
cd /root/code/HFS_BASE_SPDK
cmake -S . -B build-rocksdb-cpu-isolation-20260921 \
  -DCMAKE_BUILD_TYPE=Release -DROCKSDB_ROOT=/usr
cmake --build build-rocksdb-cpu-isolation-20260921 \
  --target hfs_core hfs_rocksdb hfsctl hfs_db_bench \
  rocksdb_cpu_isolation_observer -j2
```

配置差异：

```bash
cp config/hfs_nvme.conf shared.conf
cp config/hfs_nvme.conf isolated.conf
sed -i 's/^data_cpus = .*/data_cpus = 3/' shared.conf
sed -i 's/^data_cpus = .*/data_cpus = 4/' isolated.conf
```

统一填充数据：

```bash
BUILD=/root/code/HFS_BASE_SPDK/build-rocksdb-cpu-isolation-20260921

LD_LIBRARY_PATH=$BUILD HFS_LOG_LEVEL=error taskset -c 2-3 \
  $BUILD/hfs_db_bench --hfs-config=config/hfs_nvme.conf --db=/rocksdb \
  --num=20000 --threads=8 --key_size=16 --value_size=100 \
  --benchmarks=fillseq --use_direct_io_for_flush_and_compaction=1
```

单次 60 秒热读命令：

```bash
LD_LIBRARY_PATH=$BUILD HFS_LOG_LEVEL=error taskset -c 2-4 \
  $BUILD/rocksdb_cpu_isolation_observer \
  --hfs-config=shared.conf --db=/rocksdb --seconds=60

LD_LIBRARY_PATH=$BUILD HFS_LOG_LEVEL=error taskset -c 2-4 \
  $BUILD/rocksdb_cpu_isolation_observer \
  --hfs-config=isolated.conf --db=/rocksdb --seconds=60
```

观测器内部将 8 个测试 worker 固定到 CPU 2/3；每次启动先 warm-up，随后
清零 bdev 计数并进入 60 秒 timed read。
