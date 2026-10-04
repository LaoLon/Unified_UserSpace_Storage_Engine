# RocksDB `readrandom` 根因定位报告

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

日期：2026-09-22

## 1. 结论先行

本轮严格统一 workload、`Flush + CompactRange`、RocksDB 参数、worker CPU
（8 个线程固定在 CPU2/3）后：

| 组别 | round 1 平均 | round 2 平均 | round 3 平均 |
|---|---:|---:|---:|
| Native ext4 buffered | 97,762 ops/s | 106,141 ops/s | 106,351 ops/s |
| Native ext4 direct | 83,220 ops/s | 104,705 ops/s | 104,326 ops/s |
| HFS SPDK，frontend CPU4 | 约 94,000 ops/s | 106,174 ops/s | 106,291 ops/s |

关键证据：HFS round 2/3 的 `bdev_data_read_ops=0`，性能约 106K
ops/s，与 native warm read 相同量级。frontend 从共享 CPU2 移到独立 CPU4
没有带来收益。因此当前结论选择 **D：多个因素共同存在**，但不是 frontend
固定开销主导：旧的 `Native≈1.62M` 与 `HFS≈391K` 不是同一 CPU/workload/
cold-warm 口径，不能直接作为 HFS 软件路径的倍率结论。

## 2. CPU topology 与实际绑定

正式配置为 `config/hfs_nvme.conf`：

```ini
metadata_cpu = 0
data_cpus = 1
metadata_cpus = 0
frontend_cpus = 4
```

实际启动日志（代表性 run）：

```text
[CPU MAP] metadata reactor: tid=92752 configured_cpu=0 actual_cpu=0
[CPU MAP] data reactor 0: tid=92754 configured_cpu=1 actual_cpu=1
[CPU MAP] frontend dispatcher: tid=92755 configured_cpu=4 actual_cpu=4 pin=ok
```

8 个 RocksDB worker 按 `2,3,2,2,3,3,2,3` 等价轮换方式绑定，实际均为
CPU2 或 CPU3；每个 read round 的 8 个 worker 各 4 个落在 CPU2/CPU3。

代表性 warm round 的线程观测：

| 线程 | CPU time / 1.5 s wall | voluntary CS | involuntary CS |
|---|---:|---:|---:|
| worker 8 个 | 372--376 ms/个 | 3--8/个 | 122--130/个 |

代表性 cold round：每 worker CPU time 约 408--415 ms，voluntary CS
约 631--665，involuntary CS 约 152--185；增加的 voluntary CS 与等待
实际读完成相符。

实际确认：`0000:0b:00.0` 测试结束后已恢复到 Linux `nvme` driver，
`vm.nr_hugepages=1024`，无遗留测试进程。

## 3. Benchmark configuration

| 项目 | 值 |
|---|---|
| NVMe | `0000:0b:00.0`, SPDK bdev `Nvme0n1` |
| RocksDB | 8.9.1，系统 `/usr/lib` 构建；`GetRocksVersionAsString=8.9.1` |
| workload | 160,000 unique keys；8 threads × 20,000 operations |
| key/value | key 16 B；value 100 B |
| key range | 生成索引 `[0,159999]`；本轮 Get 成功 `160000/160000` |
| key generator | big-endian index + NUL padding，和 `hfs_db_bench` 相同 |
| value generator | 前 8 B 为 index，其余为 `v`，和 `hfs_db_bench` 相同 |
| block size | 4096 B |
| block cache | 32 MiB，RocksDB LRU block cache |
| write buffer | 64 MiB |
| compression | Snappy |
| mmap | `allow_mmap_reads=0` |
| flush/compact | 两侧都执行 `DB::Flush()`，随后 `CompactRange()` |
| flush/compact direct flag | 两侧 `use_direct_io_for_flush_and_compaction=1`；HFS adapter 写侧仍保持既有 write-back 语义 |
| native buffered | `use_direct_reads=0` |
| native direct | `use_direct_reads=1` |
| HFS | `FSRandomAccessFile::use_direct_io()=true`，SPDK bdev |
| HFS frontend queue | 1024 |
| HFS reactor | metadata CPU0；1 个 data reactor CPU1 |

新增的观测器是 `rocksdb_readrandom_observer`；只增加 CPU affinity、轻量
per-process atomic counters 和观测程序，没有实现 read cache，也没有改
WAL durability、extent allocator、SPDK bdev abstraction 或 compaction policy。

### 可复现命令

构建：

```bash
cmake -S . -B build-rootcause -DCMAKE_BUILD_TYPE=Release \
  -DHFS_SPDK_ROOT=/root/code/HFS_BASE_SPDK/spdk -DROCKSDB_ROOT=/usr
cmake --build build-rootcause --parallel 4 --target rocksdb_readrandom_observer
```

HFS 设备准备及恢复：

```bash
PCI_ALLOWED=0000:0b:00.0 DEV_TYPE=NVME SKIP_HUGE=yes ./spdk/scripts/setup.sh config
trap './spdk/scripts/setup.sh reset' EXIT
```

HFS dedicated frontend：

```bash
LD_LIBRARY_PATH=$PWD/build-rootcause HFS_LOG_LEVEL=error \
taskset -c 0-4 ./build-rootcause/rocksdb_readrandom_observer \
  --mode=hfs --hfs-config=config/hfs_nvme.conf --db=/rocksdb-rootcause-hfs \
  --keys=160000 --threads=8 --cache-size=33554432 --block-size=4096 \
  --rounds=3 --worker-cpus=2,3
```

Native buffered：

```bash
LD_LIBRARY_PATH=$PWD/build-rootcause taskset -c 0-4 \
./build-rootcause/rocksdb_readrandom_observer \
  --mode=ext4-buffered --db=/mnt/hfs-data/rootcause-buffered \
  --keys=160000 --threads=8 --cache-size=33554432 --block-size=4096 \
  --direct-reads=0 --rounds=3 --worker-cpus=2,3 --disk=sdb1
```

Native direct：

```bash
LD_LIBRARY_PATH=$PWD/build-rootcause taskset -c 0-4 \
./build-rootcause/rocksdb_readrandom_observer \
  --mode=ext4-direct --db=/mnt/hfs-data/rootcause-direct \
  --keys=160000 --threads=8 --cache-size=33554432 --block-size=4096 \
  --direct-reads=1 --rounds=3 --worker-cpus=2,3 --disk=sdb1
```

shared frontend A/B 使用同一命令，仅将配置复制后改为：

```bash
cp config/hfs_nvme.conf /var/tmp/hfs_rootcause_shared.conf
sed -i 's/frontend_cpus = 4/frontend_cpus = 2/' /var/tmp/hfs_rootcause_shared.conf
```

## 4. 数据库形态、key 命中与 flush/compaction

三组都在同一进程内执行：

```text
Open DB -> fillseq -> Flush -> CompactRange -> readrandom round 1
       -> readrandom round 2 -> readrandom round 3 -> Close DB
```

代表性结果：

| 组别 | 唯一 key | estimate-num-keys | memtable bytes | immutable | SST files | SST bytes |
|---|---:|---:|---:|---:|---:|---:|
| ext4 buffered | 160,000 | 160,000 | 2,048 | 0 | 1 | 3,211,923 |
| ext4 direct | 160,000 | 160,000 | 2,048 | 0 | 1 | 3,217,984 |
| HFS | 160,000 | 160,000 | 2,048 | 0 | 1 | 3,194,079 |

因此 `fillseq` 后已经发生 flush 和 compaction；readrandom 不是主要在
memtable 中完成。每轮请求索引都在 `[0,159999]`，三组每轮 Get 均为
`160000` 次成功、`0` 次 NotFound。

## 5. HFS 三轮 warm-read 详细结果

这是 dedicated frontend 的一轮完整输出摘要；计数器均为该 round 的增量。

| round | ops/s | p50 ns | p95 ns | p99 ns | p99.9 ns | cache hit | cache miss | frontend read | bdev read | bdev bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 93,859 | 14,001 | 27,901 | 625,712 | 9,066,181 | 155,117 | 4,883 | 4,883 | 4,884 | 23,150,592 |
| 2 | 106,548 | 14,000 | 14,900 | 51,601 | 9,064,881 | 160,000 | 0 | 0 | 0 | 0 |
| 3 | 106,777 | 14,000 | 14,801 | 50,701 | 9,064,380 | 160,000 | 0 | 0 | 0 | 0 |

round 1 同时观测到：`data_read_submit=4884`、
`data_read_complete=4884`、`queue_depth_max=8`、
`completion_count=4883`、`completion_wait_count=4881`。frontend read 与
data bdev read 不完全一一相等，差异为一条额外的 data-reactor bdev 读，
所以报告中同时保留两层计数，不用 frontend 计数冒充设备计数。

HFS 没有 adapter read cache：`frontend_cache_hit=0`，冷 round 的
`frontend_cache_miss` 等于 frontend read；warm round 不进入 HFS adapter，
因为 RocksDB block cache 在更上层直接命中。

## 6. 五次重复结果

### 6.1 HFS dedicated frontend CPU4，warm rounds

| run | round 2 ops/s | round 3 ops/s | round 2 bdev reads | round 3 bdev reads |
|---:|---:|---:|---:|---:|
| 1 | 106,329.0 | 106,171.7 | 0 | 0 |
| 2 | 106,152.5 | 106,272.6 | 0 | 0 |
| 3 | 105,559.9 | 106,389.8 | 0 | 0 |
| 4 | 106,708.2 | 106,193.9 | 0 | 0 |
| 5 | 106,118.1 | 106,428.4 | 0 | 0 |
| average | 106,173.5 | 106,291.3 | 0 | 0 |
| median | 106,152.5 | 106,272.6 | 0 | 0 |

### 6.2 HFS shared frontend CPU2，warm rounds

| run | round 2 ops/s | round 3 ops/s | round 2 bdev reads | round 3 bdev reads |
|---:|---:|---:|---:|---:|
| 1 | 106,204.6 | 105,599.5 | 0 | 0 |
| 2 | 106,857.6 | 106,837.1 | 0 | 0 |
| 3 | 106,509.4 | 106,017.8 | 0 | 0 |
| 4 | 106,124.9 | 105,959.5 | 0 | 0 |
| 5 | 106,276.2 | 106,059.4 | 0 | 0 |
| average | 106,394.5 | 106,094.7 | 0 | 0 |
| median | 106,276.2 | 106,017.8 | 0 | 0 |

综合 10 个 warm round：

```text
dedicated average = 106,232.4 ops/s
shared average    = 106,244.6 ops/s
improvement       = dedicated / shared - 1 = -0.01%
```

结论：在本实验的 warm path 上，frontend CPU2/CPU3 争用没有造成可测的
性能损失；实际 cache hit 不进入 frontend dispatcher。

### 6.3 Native buffered/direct 与 HFS 五次冷/热结果

下表的 native 设备计数来自 `/proc/diskstats` 的 `sdb1`；HFS 设备计数来自
SPDK bdev counter。

| 组别 | round 1 平均 | round 2 平均 | round 3 平均 | round 1 block miss | round 1 device reads |
|---|---:|---:|---:|---:|---:|
| ext4 buffered | 97,762.1 | 106,141.2 | 106,351.2 | 4,858 | 712--720 I/O，约 3.15 MiB |
| ext4 direct | 83,220.2 | 104,704.9 | 104,325.6 | 4,875 | 4,872--4,881 I/O，约 5.65 MiB |
| HFS SPDK | 约 94,000 | 约 106,174 | 约 106,291 | 约 4,880 | 4,874--4,884 bdev I/O，约 22.1 MiB |

native 五次逐次结果：

```text
buffered r1: 97273.6, 97340.5, 97494.1, 98482.1, 98220.3
buffered r2: 105596.0, 105820.6, 106112.1, 106176.7, 107000.8
buffered r3: 105849.1, 106175.1, 105842.2, 107115.8, 106773.6

direct   r1: 82727.2, 83477.0, 83047.9, 83778.8, 83070.2
direct   r2: 104712.3, 103776.7, 105150.1, 104803.1, 105082.1
direct   r3: 104146.4, 102732.5, 104316.4, 105023.3, 105409.6
```

HFS dedicated frontend 五次逐次结果：

```text
hfs r1: 94630.8, 94248.3, 94100.6, 94229.8, 94683.7
hfs r2: 106329.0, 106152.5, 105559.9, 106708.2, 106118.1
hfs r3: 106171.7, 106272.6, 106389.8, 106193.9, 106428.4

hfs r1 average=94378.6, median=94248.3 ops/s
```

RocksDB 8.9.1 的 public ticker 中没有 `BLOCK_READ_COUNT/TIME` 这两个
统一 ticker；本轮不伪造这两个数字，使用 `BLOCK_CACHE_DATA_MISS/HIT`、
native `/proc/diskstats` 和 HFS frontend/data/bdev 分层计数。这里的
`block miss` 是 data-block cache miss，不是 Get miss；所有 Get 都成功。

## 7. 实际 NVMe 读取证据

存在大量实际 NVMe 读取的证据只出现在 cold round：

```text
HFS round 1: bdev_data_read_ops=4,884
             bdev_data_read_bytes=23,150,592 (~22.1 MiB)
HFS round 2: bdev_data_read_ops=0
HFS round 3: bdev_data_read_ops=0
```

native direct round 1 也有约 4,872--4,881 个 `sdb1` read I/O；native
buffered round 1 则由 Linux page cache/readahead/merge 吸收了大部分块读，
只看到约 712--720 个底层 I/O。故 native buffered 的旧 1.6M 数字不能当作
NVMe baseline。

## 8. 根因分解与下一步

| 因素 | 本轮证据 | 判断 |
|---|---|---|
| 数据集/阶段差异 | 旧结果是 native buffered cold/warm 不明、HFS cold；统一后两侧 warm 都约 106K | 对旧 4.13× 差距贡献很大，旧结果不可直接归因 |
| 首次读取 | HFS cold 有 4,884 bdev reads/22.1 MiB；warm 为 0；native buffered 有 page-cache/readahead 差异 | cold I/O 是主要差异来源之一 |
| 重复对齐读取 | 三轮同一 DB/同一 block cache；round2/3 全部 cache hit | HFS 没有内部 cache，但不会重复进入 HFS；重复读取成本消失 |
| frontend CPU 争用 | shared CPU2 vs dedicated CPU4，warm 平均仅约 -0.01% | 不是当前主要原因，不应先重构 dispatcher |
| worker CPU 争用 | 8 worker 压在 CPU2/3，每个 warm worker 约 25% CPU，约 122--130 involuntary CS | 是严格绑核下的共同吞吐上限；旧 1.62M 与本轮不具同口径 |
| 固定 software path | bdev=0 时 HFS 与 native warm 同量级；且 cache hit 不进入 frontend | 没有证据表明存在 4× 固定 HFS software overhead |

因此下一步优先级是：

1. 用相同 CPU 拓扑比较 native buffered/direct 与 HFS 的 **cold direct-read**；
2. 若要解释旧 1.62M 数字，先复现旧测试的 worker affinity、线程数和 cache
   状态，再做 CPU/构建差异分析；
3. 暂不重构 frontend/data reactor，也不实现缓存。

## 9. 大 working-set 尝试边界

使用 5,242,880 keys × 1,024 B value、32 MiB cache、4 KiB block、无压缩
尝试 storage-bound 组。ext4 数据目录达到约 5.4 GiB，随后进入 RocksDB
manual compaction；在有限观察窗口内尚未进入三轮 readrandom，因此没有把该
不完整 run 写入性能结论。测试进程已停止，临时目录已清理，未影响上述小数据集
结果。

## 10. 大 working-set / cache 不可容纳复测

### 10.1 口径与命令

本轮使用 524,288 个唯一 key，key 16 B、value 1,024 B、4 KiB block、32 MiB
RocksDB block cache、无压缩。最终 SST 为 9 个、553,129,614 B，约为 block
cache 的 16.5 倍；每轮执行 524,288 次随机 Get，全部命中。

三组使用同一观测器和同一 worker 绑定（8 个 worker 固定 CPU2/3）：

```bash
taskset -c 2-4 ./build-rootcause/rocksdb_readrandom_observer \
  --mode=ext4-buffered --db=/mnt/hfs-data/rootcause-large-buffered-512m \
  --keys=524288 --threads=8 --key-size=16 --value-size=1024 \
  --cache-size=33554432 --block-size=4096 --direct-reads=0 \
  --compression=none --rounds=3 --worker-cpus=2,3 --disk=sdb1 \
  --drop-os-cache-before-read=1

taskset -c 2-4 ./build-rootcause/rocksdb_readrandom_observer \
  --mode=ext4-direct --db=/mnt/hfs-data/rootcause-large-direct-512m \
  --keys=524288 --threads=8 --key-size=16 --value-size=1024 \
  --cache-size=33554432 --block-size=4096 --direct-reads=1 \
  --compression=none --rounds=3 --worker-cpus=2,3 --disk=sdb1

LD_LIBRARY_PATH=./build-rootcause taskset -c 0-4 \
  ./build-rootcause/rocksdb_readrandom_observer --mode=hfs \
  --hfs-config=./config/hfs_nvme.conf --db=/rootcause-large-hfs-512m \
  --keys=524288 --threads=8 --key-size=16 --value-size=1024 \
  --cache-size=33554432 --block-size=4096 --direct-reads=1 \
  --compression=none --rounds=3 --worker-cpus=2,3 --disk=nvme0n1
```

HFS 命令执行前用 `PCI_ALLOWED=0000:0b:00.0 DEV_TYPE=NVME ... setup.sh config`
绑定 NVMe，进程退出后执行 `setup.sh reset`。实际 CPU map 为 metadata=0、data=1、
frontend=4、RocksDB worker=2/3，均打印并核对成功。

### 10.2 三轮结果

| 组别 | round | ops/s | block miss | 实际设备读次数 | 实际读字节 | 读字节吞吐 |
|---|---:|---:|---:|---:|---:|---:|
| ext4 buffered | 1 | 29,773.7 | 494,214 | 88,501 | 550,150,144 | 29.8 MiB/s |
| ext4 buffered | 2 | 48,296.7 | 493,945 | 0 | 0 | 0 |
| ext4 buffered | 3 | 48,691.6 | 493,999 | 0 | 0 | 0 |
| ext4 direct | 1 | 10,422.7 | 494,141 | 494,145 | 2,327,140,864 | 44.1 MiB/s |
| ext4 direct | 2 | 10,432.5 | 493,750 | 493,750 | 2,325,151,232 | 44.1 MiB/s |
| ext4 direct | 3 | 10,473.4 | 493,819 | 493,819 | 2,325,532,672 | 44.3 MiB/s |
| HFS SPDK | 1 | 19,598.9 | 494,077 | 495,057 bdev | 4,097,286,144 | 145.8 MiB/s |
| HFS SPDK | 2 | 19,583.9 | 493,894 | 494,882 bdev | 4,095,623,168 | 145.7 MiB/s |
| HFS SPDK | 3 | 19,695.4 | 493,938 | 494,926 bdev | 4,096,065,536 | 146.9 MiB/s |

HFS 三轮的 frontend/data/bdev 增量如下，证明每轮都重新到达设备：

```text
round 1: frontend_read=494077, data_submit=495057, bdev_read=495057,
         bdev_bytes=4097286144, frontend_cache_hit=0
round 2: frontend_read=493894, data_submit=494882, bdev_read=494882,
         bdev_bytes=4095623168, frontend_cache_hit=0
round 3: frontend_read=493938, data_submit=494926, bdev_read=494926,
         bdev_bytes=4096065536, frontend_cache_hit=0
```

### 10.3 结论与口径限制

1. “RocksDB block cache 装不下”已经被验证：三组每轮约 494K block miss；但
   native buffered 的 round2/3 设备读为 0，说明这些 miss 被 Linux page cache
   供应，不能用其 48K ops/s 代表真实 NVMe cold-read 成本。
2. HFS 没有内部 read cache 命中（`frontend_cache_hit=0`），round1--3 都约
   495K bdev read、约 4.096 GiB bdev bytes；因此 HFS 的大 working-set 场景确实
   是持续设备读，而不是误测成缓存命中。
3. 在本轮环境中，HFS 约 19.6K ops/s，native direct 约 10.4K ops/s；但 native
   ext4 使用 `/dev/sdb1`，HFS 使用 raw `nvme0n1`，不是同一个 Linux block
   device。这个差值只能说明当前两条 I/O 路径的观测结果，不能作为同一物理盘的
   最终性能排名。
4. 与旧的 native buffered 约 1.6M、HFS 约 391K 结果相比，新的大 working-set
   复测显示最主要的误差来源仍是 native buffered 的 Linux page cache；HFS
   frontend CPU 已隔离到 CPU4，且本轮没有证据表明 frontend contention 是首要
   瓶颈。下一步若要做最终绝对性能结论，需要让 native ext4 和 HFS 使用同一物理
   NVMe（或至少同一设备性能基线），再重复这组三轮测试。

## 11. 同一块 NVMe 的顺序复测

为消除上一节的设备差异，本轮先将 `0000:0b:00.0` 对应的
`/dev/nvme0n1` 格式化为 ext4，native 两组均使用该设备；随后卸载 ext4，
清除 ext4 签名，绑定 SPDK，用 `hfsctl --yes format` 在同一设备上重新建立
HFS volume，再运行 HFS。测试结束后设备已由 `setup.sh reset` 恢复到内核
`nvme` 驱动；`nvme0n1` 无 Linux 挂载点。

测试 workload、RocksDB 8.9.1、32 MiB block cache、553,129,614 B SST、
524,288 次随机 Get/轮、CPU2/3 worker 绑定均保持不变。

| 组别 | round 1 ops/s | round 2 ops/s | round 3 ops/s |
|---|---:|---:|---:|
| 同盘 ext4 buffered | 38,968.7 | 48,284.7 | 48,476.4 |
| 同盘 ext4 direct | 22,077.9 | 22,126.4 | 22,048.0 |
| 同盘 HFS SPDK | 19,638.8 | 19,556.0 | 19,705.9 |

对应设备读计数：

| 组别 | round | block miss | 设备读次数 | 设备读字节 |
|---|---:|---:|---:|---:|
| ext4 buffered | 1 | 494,043 | 88,504 | 550,051,840 |
| ext4 buffered | 2 | 493,876 | 0 | 0 |
| ext4 buffered | 3 | 493,889 | 0 | 0 |
| ext4 direct | 1 | 494,133 | 494,133 | 2,326,931,968 |
| ext4 direct | 2 | 493,847 | 493,847 | 2,325,580,800 |
| ext4 direct | 3 | 493,878 | 493,878 | 2,325,724,160 |
| HFS SPDK | 1 | 494,114 | 495,104 bdev | 4,097,462,272 |
| HFS SPDK | 2 | 493,921 | 494,909 bdev | 4,095,827,968 |
| HFS SPDK | 3 | 493,886 | 494,874 bdev | 4,095,549,440 |

同盘结果说明：

1. HFS 三轮都真实读取 NVMe，`frontend_cache_hit=0`，约 494K block miss
   对应约 495K bdev read；Linux `nvme0n1` 计数为 0 是因为 SPDK 绕过了
   Linux block layer，不能解释成没有设备读。
2. 同盘 HFS 平均约 19.63K ops/s，native direct 平均约 22.08K ops/s，HFS
   低约 11.1%。因此 HFS 在真实同盘 cold/read-heavy 场景不是数量级更慢，
   但存在可测的读路径开销。
3. 同盘 native buffered 的 round2/3 仍然约 48K ops/s 且设备读为 0，继续
   证明旧 native 高性能主要受 Linux page cache 影响，而不是 NVMe cold-read
   性能。

## 12. HFS 读放大定向观测

针对上一节约 1.76 倍的 bdev 字节差异，在实际 RocksDB 异步路径
`AsyncDataService::prepare` 增加了轻量计数器，覆盖 frontend 请求、planner
child/span 和 bdev 读取。使用同一块 `0000:0b:00.0`、同一 workload，单轮执行：

```bash
LD_LIBRARY_PATH=./build-rootcause taskset -c 0-4 \
  ./build-rootcause/rocksdb_readrandom_observer --mode=hfs \
  --hfs-config=./config/hfs_nvme.conf \
  --db=/rootcause-large-hfs-read-amp-async-20260922 \
  --keys=524288 --threads=8 --key-size=16 --value-size=1024 \
  --cache-size=33554432 --block-size=4096 --direct-reads=1 \
  --compression=none --rounds=1 --worker-cpus=2,3 --disk=nvme0n1
```

| 层次/计数器 | 数值 |
|---|---:|
| readrandom operations | 524,288 |
| ops/s | 19,661.0 |
| RocksDB block cache miss | 494,012 |
| frontend read ops | 494,012 |
| frontend request bytes | 4,096,671,744 |
| frontend exact 4 KiB / 8 KiB / other | 1 / 481,870 / 12,141 |
| planner requests | 494,012 |
| planner request / child / span bytes | 4,096,671,744 / 4,096,671,744 / 4,096,671,744 |
| direct / head-RMW / tail-RMW / zero children | 494,993 / 0 / 0 / 0 |
| planner child span 4 KiB / 8 KiB / other | 1,934 / 480,947 / 12,112 |
| data read submit / complete | 494,993 / 494,993 |
| bdev read ops / bytes | 494,993 / 4,096,671,744 |
| bdev block size | 512 B |

这次结果确认：

1. HFS planner 没有额外的字节读放大：`request_bytes = child_bytes = span_bytes =
   bdev_bytes`，head/tail RMW 也都是 0。
2. HFS 看到的约 4.096 GiB 并不是 extent 对齐或 data reactor 隐式扩展产生的，
   而是上游实际提交的请求本身已经约 4.096 GiB；其中 481,870 次是 8 KiB
   请求。HFS 只是把这些请求原样提交到 bdev。
3. 因此可以优化，但优化点首先不在 SPDK bdev 或 extent allocator，而在确认
   为什么 HFS RocksDB adapter/异步读路径形成了大量 8 KiB 请求，并与 native
   direct 的实际 read size 做同层级对比。当前证据不支持“HFS planner 把 4 KiB
    请求放大成 8 KiB”的结论。

## 13. Native read size 与 adapter alignment A/B

为确认 8 KiB 请求的来源，native ext4 direct 在同一块 NVMe、同一 workload
下使用 strace 统计 readrandom 阶段实际 syscall 返回字节数，并扣除了
DB open/shape 基线。native readrandom 的主要 read size 为：

| native 实际 read size | 次数 | 字节数 |
|---:|---:|---:|
| 4,608 B | 396,450 | 1,826,841,600 |
| 5,120 B | 97,679 | 500,116,480 |
| 其他少量 syscall | 24 | 34,781 |
| 合计 | 494,153 | 2,326,992,861 |

native 的这些长度都是 512 B 的整数倍，但不是 4 KiB 的整数倍。随后只改变
HFS adapter 的 GetRequiredBufferAlignment：

| HFS adapter 对外 alignment | frontend 请求 | frontend bytes | planner span / bdev bytes | head/tail RMW |
|---:|---:|---:|---:|---:|
| 4,096 B（正式配置） | 481,870 次为 8 KiB | 4,096,671,744 B | 4,096,671,744 B | 0 / 0 |
| 512 B（A/B） | 主要为 4,608/5,120 B | 2,326,425,088 B | 4,096,667,648 B | 432,468 / 432,011 |

512 B A/B 中 planner 统计为 direct=135,684、head_rmw=432,468、
tail_rmw=432,011、bdev_read_ops=1,000,163。这说明：RocksDB 会根据
adapter 对外声明的 alignment 调整提交给 FSReadRequest 的长度；HFS
adapter 本身没有把 4,608/5,120 B 改写成 8 KiB。正式的 4 KiB alignment
则避免了 HFS 4 KiB planner 的边缘 RMW，但代价是上游请求被上取整到 8 KiB。

因此，旧版 8 KiB 的直接原因已经确认：GetRequiredBufferAlignment() = 4096
参与了 RocksDB direct-read 请求的对齐。单独改成 512 B 会把读放大从“上游请求
上取整”转移成 HFS planner 的 head/tail RMW，不能作为最终修复。下一节实现了
独立的 native-sector read planner，避免这两种放大。

## 14. Native-sector read fast path 实现与验证

本轮按以下顺序实现：

1. 写路径、extent 布局和元数据仍使用原有 4 KiB planner。
2. RocksDB 随机读 adapter 对外声明 512 B alignment。
3. 异步读 planner 使用运行时 NVMe native block size；当前单盘为 512 B。
4. planner child 携带显式 `physicalByte`，backend 不再强制把读地址解释为
   4 KiB planner block。
5. 512 B 对齐的 4,608/5,120 B 请求直接提交；本节基线不做请求合并。
6. 非对齐边界才使用 512 B span 的 bounce buffer 和拷贝回用户 buffer。

构建和单测：

```bash
cmake --build build-rootcause --parallel 4 \
  --target io_planner_test spdk_async_backend_test rocksdb_readrandom_observer
ctest --test-dir build-rootcause --output-on-failure \
  -R 'io_planner_test|spdk_async_backend_test'
```

结果：2/2 单测通过。

同一块 NVMe `0000:0b:00.0`、同一 workload：524,288 keys、8 threads、
16B key、1024B value、32MiB block cache、4KiB RocksDB block、无压缩、
direct reads、worker CPU 2/3、HFS frontend CPU 4。

| round | native direct ops/s | native NVMe read bytes | HFS fast-path ops/s | HFS bdev read bytes | HFS head/tail RMW |
|---:|---:|---:|---:|---:|---:|
| 1 | 21,842.5 | 2,327,246,336 | 19,528.4 | 2,326,734,336 | 0 / 0 |
| 2 | 21,873.0 | 2,325,670,400 | 19,304.7 | 2,325,498,880 | 0 / 0 |
| 3 | 21,889.6 | 2,325,577,216 | 19,293.3 | 2,325,261,312 | 0 / 0 |
| average | 21,868.4 | 2,326,164,651 | 19,375.5 | 2,325,831,509 | 0 / 0 |

HFS fast-path 每轮的读路径计数：

| round | block cache miss | frontend reads | frontend bytes | planner direct children | planner span bytes | bdev read ops |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 494,094 | 494,094 | 2,326,734,336 | 495,075 | 2,326,734,336 | 495,075 |
| 2 | 493,828 | 493,828 | 2,325,498,880 | 494,812 | 2,325,498,880 | 494,812 |
| 3 | 493,776 | 493,776 | 2,325,261,312 | 494,762 | 2,325,261,312 | 494,762 |

HFS 每轮均为：`head_rmw=0`、`tail_rmw=0`、`request_bytes=span_bytes=
bdev_bytes`。planner child 数比 frontend request 数多约 981，是跨 extent 边界
拆分造成的额外 child，不是额外字节读取。

相对旧版 4KiB adapter/planner 口径，bdev 读取从约 4.096GiB 降到约 2.326GB，
读字节放大基本消除；但 HFS 仍比 native direct 慢约 11.4%：

```text
19,375.5 / 21,868.4 - 1 = -11.4%
```

因此下一步仍应继续分析 frontend dispatcher、message/completion 和 data reactor
固定开销；相邻 child 合并结果见下一节。本轮没有修改 cache、extent allocator 或
SPDK bdev。

## 15. 相邻 512B span 合并验证

在 native-sector fast path 之上增加了受限合并：同一个父读请求内，如果相邻
planner child 满足以下条件，则合并为一个 child：

- 同一设备；
- 逻辑地址连续；
- 物理字节地址连续且均为 512 B 对齐；
- 用户 buffer 连续；
- 两个 child 都是 direct，不包含 RMW 或 hole；
- 合并后不超过 1 MiB。

合并保留一个父请求的 completion 语义，不跨独立 RocksDB `FSReadRequest` 合并，
因此不会改变当前异步 handle 生命周期。

单元测试新增了跨连续 extent 的 512 B direct child 合并检查；VM 构建和
`io_planner_test` 均通过。

测试命令：

```bash
PCI_ALLOWED=0000:0b:00.0 DEV_TYPE=NVME SKIP_HUGE=yes \
  ./spdk/scripts/setup.sh config
LD_LIBRARY_PATH=./build-rootcause taskset -c 0-4 \
  ./build-rootcause/hfsctl --config=./config/hfs_nvme.conf --yes format
LD_LIBRARY_PATH=./build-rootcause HFS_LOG_LEVEL=error taskset -c 0-4 \
  ./build-rootcause/rocksdb_readrandom_observer --mode=hfs \
  --hfs-config=./config/hfs_nvme.conf --db=/read-coalescing-20260922 \
  --keys=524288 --threads=8 --key-size=16 --value-size=1024 \
  --cache-size=33554432 --block-size=4096 --direct-reads=1 \
  --compression=none --rounds=3 --worker-cpus=2,3 --disk=nvme0n1
```

同一块 NVMe、同一 workload 的 3 轮结果：

| round | ops/s | frontend reads | coalesced children | bdev read ops | bdev read bytes | head/tail RMW |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 19,755.4 | 494,209 | 882 | 494,304 | 2,327,304,192 | 0 / 0 |
| 2 | 19,664.7 | 493,960 | 887 | 494,055 | 2,326,126,592 | 0 / 0 |
| 3 | 19,739.6 | 493,995 | 885 | 494,090 | 2,326,288,384 | 0 / 0 |
| average | 19,719.9 | 494,054.7 | 884.7 | 494,149.7 | 2,326,573,056 | 0 / 0 |

每轮 `planner_span_bytes = bdev_data_read_bytes`，说明合并没有引入读字节
放大；`bdev_read_ops - frontend_read_ops` 约为 95，主要来自仍然不能跨越的
非连续 extent 边界。相对未合并 fast path 的 19,375.5 ops/s，本轮约提升
1.8%，但该幅度仍需要多轮重复测试才能确认，不足以解释 HFS 与 native direct
之间约 11% 的剩余差距。

本轮结论：extent 边界 child 合并有效降低了约 885 次/轮 backend submission，
但主要瓶颈仍在 frontend/message/completion 软件路径。跨多个独立 RocksDB
请求的队列级合并尚未实现，后续只有在证明请求邻接率足够高后再考虑。

## 16. 按计划的安全优化与同盘复测

### 16.1 本轮改动

本轮只做读路径定位和低风险优化，没有实现 cache，也没有修改 metadata allocator、
journal、extent allocator、WAL durability 或 compaction 策略：

1. `stats=false` 时关闭 bdev/queue-depth/阶段计时等诊断计数的原子操作和遍历，
   避免测量代码污染性能路径。
2. frontend 队列从空变非空时才 `notify_one()`，减少同一批 MultiRead 的重复唤醒。
3. 对单 child、读请求复用 16 KiB DMA lease，减少常见随机读的 lease 分配。
   复用明确限制为 `state.childCount == 1`；合并后的多 child 请求仍使用独立 lease。

曾尝试对所有 child 复用同一 lease，统计运行暴露出并发 child 覆盖 DMA buffer、
导致 SST checksum mismatch 的问题，已撤回为上述单 child 限制。修正后 profile
运行 `hits=524288` 且无 checksum mismatch。

### 16.2 同一块 NVMe 的有效性能结果

之前部分表格虽然写成“同一块 NVMe”，但复核发现历史 native 运行使用的是
`/dev/sdb1`（`/mnt/hfs-data`），HFS 使用的是 PCI `0000:0b:00.0` 对应的
`/dev/nvme0n1`。因此旧表只能作为优化趋势，不能作为严格的同盘比较。以下为
重新格式化并顺序使用同一 `/dev/nvme0n1` 的有效结果。

配置：RocksDB 8.9.1，524,288 keys，8 threads，16B key，1024B value，32MiB
block cache，4KiB RocksDB block，无压缩，direct reads，worker CPU 2/3；HFS
metadata CPU 0、data CPU 1、frontend CPU 4；性能运行 `stats=false`。

HFS 命令：

```bash
wipefs -a -f /dev/nvme0n1
PCI_ALLOWED=0000:0b:00.0 DEV_TYPE=NVME SKIP_HUGE=yes \
  ./spdk/scripts/setup.sh config
LD_LIBRARY_PATH=./build-rootcause taskset -c 0-4 \
  ./build-rootcause/hfsctl --config=./config/hfs_nvme.conf --yes format
LD_LIBRARY_PATH=./build-rootcause taskset -c 0-4 \
  ./build-rootcause/rocksdb_readrandom_observer --mode=hfs \
  --hfs-config=./config/hfs_nvme.conf --db=/same-nvme-hfs-512m \
  --keys=524288 --threads=8 --key-size=16 --value-size=1024 \
  --cache-size=33554432 --block-size=4096 --direct-reads=1 \
  --compression=none --rounds=3 --worker-cpus=2,3 --disk=nvme0n1
```

native direct 命令：

```bash
./spdk/scripts/setup.sh reset
mkfs.ext4 -F -L same_nvme /dev/nvme0n1
mount -o noatime /dev/nvme0n1 /mnt/same-nvme
LD_LIBRARY_PATH=./build-rootcause taskset -c 2-4 \
  ./build-rootcause/rocksdb_readrandom_observer --mode=ext4-direct \
  --db=/mnt/same-nvme/same-nvme-native-opt \
  --keys=524288 --threads=8 --key-size=16 --value-size=1024 \
  --cache-size=33554432 --block-size=4096 --direct-reads=1 \
  --compression=none --rounds=3 --worker-cpus=2,3 --disk=nvme0n1
umount /mnt/same-nvme
```

| backend | round 1 ops/s | round 2 ops/s | round 3 ops/s | average ops/s | p50 average | p95 average |
|---|---:|---:|---:|---:|---:|---:|
| HFS/SPDK | 17,919.2 | 17,872.7 | 17,744.5 | 17,845.5 | 446.6 µs | 691.2 µs |
| native ext4 direct | 21,820.2 | 21,824.4 | 21,824.1 | 21,822.9 | 359.6 µs | 512.5 µs |

HFS/native = `81.8%`，native/HFS = `1.22x`，剩余差距为 `18.2%`。相比本轮
安全 lease 复用前的同盘 3-round 基线约 `17,547 ops/s`，当前结果约提升 `1.7%`；
该提升来自一次 3-round A/B，仍应通过更多重复轮次确认，不能视为最终收益。

native direct 每轮 Linux 设备读约 `493,855--494,174` 次、`2.326--2.327 GB`；
HFS 性能运行关闭统计，因此不从该运行读取 bdev 计数，读路径计数使用下一节的
同 workload `stats=true` 定向 profile。

### 16.3 读放大和软件阶段证据

`stats=true` profile 不是性能分数（统计和阶段计时会降低吞吐），但用于确认读
路径是否正确。结果：

| counter | value |
|---|---:|
| RocksDB block cache miss | 494,042 |
| HFS frontend read ops | 494,042 |
| planner request bytes | 2,326,518,784 |
| planner span bytes | 2,326,518,784 |
| bdev read ops | 494,042 |
| bdev read bytes | 2,326,518,784 |
| read bounce children / bytes | 494,042 / 2,326,518,784 |
| read copy-out ops / bytes | 494,042 / 2,326,518,784 |
| head/tail RMW | 0 / 0 |
| coalesced children | 982 |

planner、bdev、bounce 和 copy-out 字节完全相等，没有证据表明当前仍有额外的
字节读放大；982 个相邻 child 合并也没有增加设备字节数。真正明显的固定成本是
用户 buffer 不能直接作为 DMA 目标：`read_direct_user_children=0`，即所有设备读
都经过 HFS DMA bounce buffer，再 copy-out 到 RocksDB buffer。

profile 的阶段累计值约为：frontend queue wait `180 µs/read`、frontend
submit-to-I/O completion `242 µs/read`、completion wait `492 µs/wait`，copy-out
约 `4.1 µs/read`。这些阶段彼此有重叠，不能直接相加，但说明剩余差距主要在
worker→frontend→data reactor→completion 的软件链路和等待，而不是读字节放大。

### 16.4 当前判断和下一步

CPU map 已确认 metadata/data/frontend/workers 分别为 `0/1/4/2,3`，frontend
没有与 RocksDB worker 共用 CPU 2/3；因此当前 18.2% 差距不能再主要归因于 frontend
绑核冲突。当前结论是：

- 同盘下 HFS 已达到 native direct 的约 81.8%，但尚未超过 native；
- 单 child lease 复用和队列唤醒优化收益很小，说明 allocator/notify 不是主瓶颈；
- 没有读字节放大，且 cold read 的设备读量与 native 同量级；
- 主要瓶颈仍是每个请求都经过 frontend dispatcher、data reactor 和 completion
  wait，以及 bounce buffer/copy-out；
- 要超过 native，下一步应优先验证 worker 直达 data-reactor ring、批量 completion
  和真正的 registered-DMA/zero-copy user buffer 路径，再以同一块 NVMe、`stats=false`
  的多轮结果验收。本轮不重构这些模块。

## 17. worker 直达 ring、批量 completion 与 DMA 验证

### 17.1 worker 直达 data-reactor ring

新增实验开关 `HFS_ROCKSDB_DIRECT_WORKER_READS=1`。每个 RocksDB worker 在线程本地
创建一个 HFS `Client`，由该 worker 打开文件、调用 `Client::submitRead()`，并在同一
worker 调用 `Client::poll()`；因此请求不再经过共享 frontend dispatcher，直接发布到
data-reactor ring。默认不开启，生产路径不变。

定向验证使用真实 `0000:0b:00.0`、`Nvme0n1`，16,384 keys、8 workers、16B key、
1024B value、4KiB RocksDB block、32MiB cache、worker CPU 2/3、metadata CPU 0、
data CPU 1、frontend CPU 4。为避开已有 `Engine::stop()` 与 RocksDB 后台线程退出的
锁竞争，本次路径验证执行 `fillseq + Flush`，使用 `--skip-compaction=1`，不作为最终
性能基准。

```bash
PCI_ALLOWED=0000:0b:00.0 DEV_TYPE=NVME SKIP_HUGE=yes ./spdk/scripts/setup.sh config
LD_LIBRARY_PATH=./build-rootcause taskset -c 0-4 \
  ./build-rootcause/hfsctl --config=/var/tmp/direct-ring-small-20260922.conf --yes format
LD_LIBRARY_PATH=./build-rootcause \
  HFS_ROCKSDB_DIRECT_WORKER_READS=1 HFS_ROCKSDB_COMPLETION_BATCH=64 \
  taskset -c 0-4 ./build-rootcause/rocksdb_readrandom_observer --mode=hfs \
  --hfs-config=/var/tmp/direct-ring-small-20260922.conf \
  --db=/hfs-direct-ring-small-20260922-b --keys=16384 --threads=8 \
  --key-size=16 --value-size=1024 --cache-size=33554432 --block-size=4096 \
  --direct-reads=1 --compression=none --rounds=1 --skip-compaction=1 \
  --worker-cpus=2,3 --disk=nvme0n1
```

读轮结果：

| ops/s | Get hits | frontend reads | direct-worker reads | direct completions | bdev reads | bdev bytes | copy-out |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 43,032.9 | 16,384/16,384 | 0 | 4,040 | 4,040 | 4,040 | 19,005,952 | 4,040 |

CPU map 显示 8 个 RocksDB worker 实际运行于 CPU 2/3，data reactor 实际运行于 CPU 1，
frontend dispatcher 实际运行于 CPU 4。`frontend_read_ops=0` 且 direct worker 的提交、
completion 计数相等，证明本轮没有 fallback 到共享 frontend；所有 Get 命中且没有
checksum/error 输出，证明 direct Client 的 handle 生命周期和数据返回正确。

### 17.2 completion batch A/B

关闭 direct-worker 开关，保持其余配置不变，分别设置
`HFS_ROCKSDB_COMPLETION_BATCH=1` 与 `64`。两组均为真实 NVMe、16,384 keys、
`fillseq + Flush`、不 compaction 的路径实验：

| completion batch | ops/s | block cache miss | frontend reads | bdev reads | bdev bytes | completion waits | completion wait ns |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 22,451.7 | 4,056 | 4,056 | 4,056 | 19,082,752 | 4,056 | 5,249,626,809 |
| 64 | 45,459.5 | 4,054 | 4,054 | 4,054 | 19,072,512 | 4,053 | 2,251,827,792 |

batch=64 相对 batch=1 为 `2.02x`；两组设备读量相同，说明收益来自 completion
批量 drain、frontend 等待和唤醒次数下降，而不是减少了设备工作。该结果是小 workload
的定位结果，正式性能还需在不触发 shutdown 锁竞争的完整 workload 上重复。

### 17.3 registered-DMA / zero-copy

核心 API 的真实 NVMe 验证：

```bash
cmake --build build-rootcause --parallel 4 \
  --target file_api_test data_path_device_test
ctest --test-dir build-rootcause --output-on-failure \
  -R '^(file_api_test|data_path_device_test)$'
```

结果为 `2/2 passed`。其中 `file_api_test` 使用
`engine->acquireDmaBuffer()`，验证单个异步读和 32 个 DMA buffer 的
`poll(min=32,max=32)`；`data_path_device_test` 进一步确认：

```text
datapath stats: submits=29 direct=2 bounce=25 copy=10557400
data_path_device_test: ok
```

所以 HFS core 的 registered-DMA/zero-copy 路径已经在真实 NVMe 上生效。可是 RocksDB
adapter 传入的 `FSReadRequest::scratch` 是 RocksDB 普通内存，当前 profile 仍是
`read_direct_user_children=0`、全部 `read_bounce_children` 和 copy-out；不能仅靠把
请求长度补齐到 512B 就变成 zero-copy。要让 RocksDB 读真正走 zero-copy，需要增加
RocksDB buffer allocator/registration 生命周期契约；在没有这个契约前，强行对任意
scratch 调 `spdk_vtophys` 会有 buffer 生命周期和 DMA 可达性风险。

### 17.4 阶段结论

本轮已经验证：

1. worker 直达 data-reactor ring 可行，并成功绕过共享 frontend；
2. completion batch=64 在小 workload 上约降低一半 completion-wait 时间，并带来约
   2 倍吞吐；
3. core registered-DMA/zero-copy 可行，但当前 RocksDB adapter 尚未把 RocksDB 用户
   buffer 注册为 DMA buffer；
4. 因而下一步应先解决完整 workload 的安全 shutdown/compaction 锁竞争，再用
   `stats=false` 做 direct-worker 与 shared-frontend 的同盘多轮性能对比；zero-copy
   则需要先设计并验证 buffer ownership/registration API。

## 18. 同一块 NVMe 的重新对比

本次重新格式化并顺序使用同一块 `/dev/nvme0n1`（PCI `0000:0b:00.0`）：

- HFS/SPDK：`config/hfs_nvme.conf`、metadata CPU 0、data CPU 1、frontend CPU 4；
  worker CPU 2/3；`stats=false`；
- HFS worker-direct：额外设置
  `HFS_ROCKSDB_DIRECT_WORKER_READS=1 HFS_ROCKSDB_COMPLETION_BATCH=64`；
- native：ext4 direct，`use_direct_reads=1`，同一 RocksDB workload；
- 524,288 keys，8 threads，16B key，1024B value，32MiB block cache，4KiB block，
  无压缩，3 轮，每轮 524,288 次 Get。

完整命令入口：

```bash
python E:/Code/test_script/run_same_nvme_direct_compare_20260922.py
python E:/Code/test_script/run_same_nvme_hfs_shared_20260922.py
```

读性能结果：

| backend | round 1 ops/s | round 2 ops/s | round 3 ops/s | average ops/s | 相对 native |
|---|---:|---:|---:|---:|---:|
| HFS/SPDK shared frontend | 17,673.6 | 17,831.7 | 17,605.7 | 17,703.7 | 80.6% |
| HFS/SPDK worker-direct | 17,757.5 | 17,481.5 | 17,656.7 | 17,631.9 | 80.3% |
| native RocksDB/ext4 direct | 21,978.5 | 21,954.1 | 21,961.0 | 21,964.5 | 100.0% |

所有轮次均为 `524,288/524,288` 命中。native/HFS shared 为 `1.24x`，
native/HFS worker-direct 为 `1.25x`；worker-direct 相对 shared frontend 为
`-0.4%`，在本次 3 轮误差范围内，没有证明完整 workload 上有收益。

因此当前结论是：同盘、同 workload 下 HFS 约达到 native direct 的 80%，但本轮
worker 直达 ring 没有突破 native，也没有显著超过默认 frontend 路径。下一步需要
针对完整 workload 采集 direct/shared 两侧的阶段计数（当前性能运行关闭 stats），
重点确认 RocksDB 请求分布、data-reactor queue depth 和 completion 批量实际形成的
批次；不能仅根据小 workload 的 batch=64 A/B 结果推断大 workload 一定同比提升。