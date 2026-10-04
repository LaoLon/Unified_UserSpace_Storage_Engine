# RocksDB ext4 / HFS 统一口径对比

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

测试日期：2026-09-21。设备为同一块 `/dev/nvme0n1`，PCI BDF 为
`0000:0b:00.0`。本报告采用第二次统一复测结果；第一次复测在统计前做
iterator 扫描，预热了 block cache，已丢弃。

## 1. 口径、key 集合和 block size

| 项目                     | 原生 ext4/db_bench                         | 原 HFS/hfs_db_bench                         | 统一复测三组 |
| :----------------------- | :----------------------------------------- | :------------------------------------------- | :----------- |
| fillseq 写入操作数       | 160,000                                    | 160,000                                      | 160,000      |
| fillseq 唯一 key 数      | 20,000                                     | 160,000                                      | 160,000      |
| fillseq key 范围         | 0..19,999                                  | 0..159,999                                   | 0..159,999   |
| readrandom 逻辑 key 范围 | 0..19,999                                  | 0..159,999                                   | 0..159,999   |
| Get 请求数/命中数        | 160,000/160,000（原 db_bench 输出）        | 160,000/160,000                               | 160,000/160,000 |
| 统一复测实际请求范围     | -                                          | -                                            | 0..159,998（随机样本未取到 159,999） |
| RocksDB table block      | 4,096 B（db_bench 默认）                  | 4,096 B（TableOptions 默认）                 | 4,096 B（显式） |
| ext4 filesystem block    | 4,096 B                                    | -                                            | 4,096 B      |
| NVMe logical/physical    | 512 B / 512 B                              | HFS bdev 512 B                               | 512 B / 512 B |

原生 RocksDB 8.9.1 的 `GenerateKeyFromInt` 使用大端整数并以 ASCII `0`
填充；原 HFS 代码使用同样的大端整数，但以 NUL 填充。原 HFS value 是
`'v'` 填充并在前 8 字节写入小端 index，原生 `db_bench` value 使用
RocksDB 的 `RandomGenerator`。因此原来的两次测试不仅 key 数不同，key
编码、value 内容和 SST 布局也不同。参见
[RocksDB 8.9.1 db_bench 源码](https://github.com/facebook/rocksdb/blob/v8.9.1/tools/db_bench_tool.cc)。

统一复测程序对两侧使用同一套 HFS 风格 key/value 生成器、8 个 writer、每个
writer 20,000 条，并在两侧都执行同样的 `Flush()` 和
`CompactRange()`，分别计时：

| 组别           | fillseq (s) | flush (s) | compact (s) |
| :------------- | ----------: | --------: | ----------: |
| ext4 buffered  |    0.716542 |  0.039463 |    0.000822 |
| ext4 direct    |    0.767767 |  0.042579 |    0.001601 |
| HFS            |    0.491130 |  0.039959 |    0.001118 |

原始 `621` 与 `4,898` 的 block miss 不是同口径结果。原生侧只有 20,000
个 key，HFS 侧有 160,000 个 key；统一到 160,000 个 key 后，第一轮 miss
变为：ext4 buffered `4,854`、ext4 direct `4,893`、HFS `4,880`。
相对原始差值 `4,898 - 621 = 4,277`，统一数据集后与 buffered 的差值
只剩 `26`，即原始 miss-count 差异的约 `99.4%` 消失；与 direct 相比只
剩 `13`，约 `99.7%` 消失。

作为背景，原先五次非统一 `readrandom` 均为 160,000 次请求：

| 运行       | 1     | 2     | 3     | 4     | 5     |
| :--------- | ----: | ----: | ----: | ----: | ----: |
| 原生 ext4  | 0.088 | 0.089 | 0.096 | 0.087 | 0.090 |
| 原 HFS     | 0.230 | 0.239 | 0.237 | 0.254 | 0.248 |
| 单位       | s     | s     | s     | s     | s     |

## 2. 同一进程、同一 DB 的两轮 readrandom

每组均在同一个 observer 进程、同一个 DB 上执行两轮相同的 160,000 次
Get。fill/flush/compact 后先统计 key，再清除未引用 block-cache entry，
然后执行 round 1 和 round 2。`block_cache_miss`、`block_read_*` 和 HFS
bdev 计数均为该轮增量。

| 组别          | 轮次 | time (s) | cache hit | cache miss | block read count | block read bytes | HFS bdev read ops | HFS bdev bytes |
| :------------ | ---: | --------: | --------: | ---------: | ---------------: | ---------------: | ----------------: | -------------: |
| ext4 buffered | 1    |  0.331146 |   155,146 |      4,854 |            4,854 |        3,141,489 |                 - |              - |
| ext4 buffered | 2    |  0.305898 |   160,000 |          0 |                0 |                0 |                 - |              - |
| ext4 direct   | 1    |  0.411402 |   155,107 |      4,893 |            4,893 |        3,168,549 |                 - |              - |
| ext4 direct   | 2    |  0.312664 |   160,000 |          0 |                0 |                0 |                 - |              - |
| HFS           | 1    |  0.502437 |   155,120 |      4,880 |            4,880 |        3,161,117 |             4,881 |     23,134,208 |
| HFS           | 2    |  0.327715 |   160,000 |          0 |                0 |                0 |                 0 |              0 |

三组的 block-cache miss 几乎相同，说明 621/4898 的差别来自数据集和布局，
不是 HFS 在统一 workload 下额外产生了大量 miss。HFS round 1 的 bdev
实际读取为 4,881 次、23,134,208 B；round 2 为 0 次、0 B。round 2 的
所有 Get 都命中 RocksDB block cache。

统一观测还记录到：

| 组别          | round 1 `read_block_get` (count/sum us) | round 1 `sst_read` (count/sum us) |
| :------------ | --------------------------------------: | --------------------------------: |
| ext4 buffered |                  4,854 / 128,385       |                 4,854 / 46,678    |
| ext4 direct   |                4,893 / 1,102,179      |              4,893 / 1,043,594    |
| HFS           |                4,880 / 1,744,537      |              4,880 / 1,709,823    |

## 3. CPU 分配和差距归因

执行进程使用 `taskset -c 2-3`。已有 CPU 采样显示：8 个测试 worker
实际运行在 CPU 2/3，每线程约 `16%~27%`；metadata reactor 实际在 CPU 0，
约 `93%~100%`；data reactor 实际在 CPU 3，约 `2%~11%`。因此不能写成
“所有 reactor 和测试线程都被限制在 CPU 2-3”：metadata reactor 没有落在
CPU 2-3，data reactor 与测试 worker 共享 CPU 3。

本轮没有做 CPU 隔离/反隔离的反事实实验，所以 CPU 争用的独立贡献不能从
现有数据中精确拆出。可观测的 round 1 到 round 2 首次读取惩罚为：

| 组别          | round 1 - round 2 (s) | 相对 round 1 |
| :------------ | --------------------: | ------------: |
| ext4 buffered |              0.025248 |         7.6% |
| ext4 direct   |              0.098738 |        24.0% |
| HFS           |              0.174722 |        34.8% |

round 2 是重复对齐读取场景：HFS 没有任何 bdev 读，仍比 ext4 buffered 多
`0.021817 s`，比 ext4 direct 多 `0.015051 s`。这部分包含 HFS 前端、调度、
异步路径和 CPU 争用，不能单独归因于 CPU；当前只能把它作为剩余差距的上界。

结论：存在大量实际 NVMe 读取，但只发生在统一复测的第一轮：HFS 有
`4,881` 次 bdev read、约 `22.07 MiB`；第二轮为零。原始 621/4898 差异
首先应归因于数据集/key 编码/SST 布局差异；统一后应继续查 HFS 与 ext4
的 CPU/构建/异步路径差异，同时用第二轮结果验证读缓存。当前没有证据支持
先实现缓存，本轮未实现缓存。

## 4. 可复现命令

### 构建观测程序

在 VM 内、从 HFS 自身源码构建；SPDK 源码保持 VM 内的 Git checkout：

```bash
cd /root/code/HFS_BASE_SPDK
cmake -S . -B build-rocksdb-unified-20260921 \
  -DCMAKE_BUILD_TYPE=Release -DROCKSDB_ROOT=/usr
cmake --build build-rocksdb-unified-20260921 \
  --target hfs_core hfs_rocksdb hfsctl rocksdb_unified_observer -j2
```

### ext4 buffered/direct

```bash
PROJECT=/root/code/HFS_BASE_SPDK
BUILD=$PROJECT/build-rocksdb-unified-20260921
DEV=/dev/nvme0n1
MNT=/mnt/rocksdb-unified-ext4

mkfs.ext4 -F "$DEV"
mount "$DEV" "$MNT"
rm -rf "$MNT/db"; mkdir -p "$MNT/db"

LD_LIBRARY_PATH="$BUILD" HFS_LOG_LEVEL=error taskset -c 2-3 \
  "$BUILD/rocksdb_unified_observer" \
  --group=ext4-buffered --db="$MNT/db"

umount "$MNT"
mkfs.ext4 -F "$DEV"
mount "$DEV" "$MNT"
rm -rf "$MNT/db"; mkdir -p "$MNT/db"

LD_LIBRARY_PATH="$BUILD" HFS_LOG_LEVEL=error taskset -c 2-3 \
  "$BUILD/rocksdb_unified_observer" \
  --group=ext4-direct --db="$MNT/db" --direct-reads=1
```

### HFS

```bash
PROJECT=/root/code/HFS_BASE_SPDK
BUILD=$PROJECT/build-rocksdb-unified-20260921
CONFIG=$PROJECT/config/hfs_nvme.conf
BDF=0000:0b:00.0

echo "$BDF" > /sys/bus/pci/drivers/nvme/unbind
PCI_ALLOWED=$BDF DEV_TYPE=NVME SKIP_HUGE=yes \
  "$PROJECT/spdk/scripts/setup.sh" config
LD_LIBRARY_PATH="$BUILD" HFS_LOG_LEVEL=error \
  "$BUILD/hfsctl" --config="$CONFIG" --yes format
LD_LIBRARY_PATH="$BUILD" HFS_LOG_LEVEL=error taskset -c 2-3 \
  "$BUILD/rocksdb_unified_observer" \
  --group=hfs --hfs-config="$CONFIG" --db=/rocksdb

# 完成后恢复内核 nvme 驱动
echo "$BDF" > /sys/bus/pci/drivers/uio_pci_generic/unbind || true
echo "$BDF" > /sys/bus/pci/drivers/nvme/bind || true
```

观测程序的统一参数为：`--num=20000`、`--threads=8`、16-byte key、
100-byte value、`block_size=4096`、`cache_size=33554432`、
`write_buffer_size=67108864`、Snappy、flush/compaction direct I/O 开启；
只有 ext4-direct 和 HFS 开启 direct reads。