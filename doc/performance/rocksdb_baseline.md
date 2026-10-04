# HFS 2.0 RocksDB 基线：原生 ext4 对比 HFS（32/64 并发）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。
>
> 2026-10-04 改名后回归（FIO/RocksDB/MySQL 同会话对照，无系统性回退）：见
> `doc/performance/uuse_rename_regression_20261004.md`。

日期：2026-09-27 会话（UTC 16:38–16:59，本地 2026-09-28 00:38–00:59）。
范围：同一块 NVMe 上，用同一个 `hfs_db_bench` 二进制、同一套 key/value 生成器与
同一组 RocksDB 选项，比较**原生 Env（ext4）**与 **HFS Env** 在 32/64 并发下的
顺序写、顺序读、随机写、随机读、覆盖写校验与读写混合。本文件只描述本会话结果。

相关矩阵：FIO 见 `doc/performance/fio_baseline.md`，MySQL 见
`doc/performance/mysql_baseline.md`；三者共用本会话设备与构建。

## 1. 环境与构建

| 项目 | 值 |
|---|---|
| VM | 6 vCPU、约 7.2 GiB RAM、`HugePages_Total=1024` |
| 目标设备 | `VMware NVME_0002`，PCI `0000:13:00.0`；native 用 ext4，HFS 绑定 `uio_pci_generic`，SPDK bdev `Nvme0n1` |
| HFS 源码 | Windows 工作区 `HEAD 8dc2175` 同步后 Release 重建（`/root/code/build-verify`）；CTest `26/26` |
| RocksDB | 9.11.2（`hfs_db_bench` 链接系统库） |
| HFS 配置 | `config/hfs_nvme.conf`（`[rocksdb] worker_direct_reads=true`） |
| 证据目录 | `/home/hfs/rocksdb-native-vs-hfs-20260927`（正式）、`…-aborted1`（首次尝试，见 §5） |

## 2. 口径

- 数据集：固定 **1,280,000** 个 key（16 B key、1024 B value），每线程 `--num`：
  32 线程 = 40,000、64 线程 = 20,000，保证两种并发下数据量相同。
- RocksDB 选项：4 KiB block、`compression=none`、`use_direct_reads=1`、
  `use_direct_io_for_flush_and_compaction=1`、1 GiB block cache、WAL 开启、
  每次 Put 不强制 sync、`max_background_jobs=threads`。
- 线程固定 `worker_cpus=0,1,2,3,4,5`，进程 `taskset -c 0-5`。
- 每个 (模式, 并发) 分两个阶段，各自从**全新设备状态**开始（native 重新
  `mkfs.ext4`，HFS 重新 `format`），保证同一时刻只有一个 128 万 key 的库在线：
  1. `seq` 阶段：`fillseq` → `readseq` → `readrandom`；
  2. `rnd` 阶段：`fillrandom` → `overwrite` → `readwhilewriting`。
- `overwrite` 语义：先无计时地重放全部 key（seed），再计时覆盖写，最后对每个
  key 做无计时的逐键校验；`readwhilewriting` 为 1 个 writer + N-1 个 reader
  （writer 写 `num` 个 key）。
- 交替顺序：native-s32 → hfs-s32 → native-s64 → hfs-s64，便于观察会话漂移。

复现命令见第 6 节。

## 3. 性能矩阵（ops/s，1 KiB value）

### 3.1 32 线程

| 工作负载 | native ext4 | HFS | HFS/native |
|---|---:|---:|---:|
| fillseq（顺序写） | 87207 | 86937 | **99.7%** |
| readseq（顺序读） | 39604 | 229034 | **578.3%** |
| readrandom（随机读） | 39216 | 132505 | **337.9%** |
| fillrandom（随机写） | 84647 | 83403 | **98.5%** |
| overwrite（覆盖写+校验） | 72398 | 60828 | **84.0%** |
| readwhilewriting（读写混合） | 7329（30.5 s，异常轮） | 2470043（0.46 s） | 见 §5 注 |

### 3.2 64 线程

| 工作负载 | native ext4 | HFS | HFS/native |
|---|---:|---:|---:|
| fillseq（顺序写） | 100198 | 97459 | **97.3%** |
| readseq（顺序读） | 40029 | 255485 | **638.3%** |
| readrandom（随机读） | 39213 | 157336 | **401.2%** |
| fillrandom（随机写） | 97939 | 94249 | **96.2%** |
| overwrite（覆盖写+校验） | 69153 | 85712 | **123.9%** |
| readwhilewriting（读写混合） | 2799664 | 2314158 | **82.7%** |

原始输出还包含 MB/s 与 micros/op（见证据目录 stdout）；例如 HFS 32 线程
`fillrandom` 82.7 MB/s、`readseq` 227.2 MB/s，64 线程 `readseq` 253.4 MB/s。

## 4. 校验与闭包证据

- `overwrite` 的逐键内容校验在全部 8 个阶段通过（脚本对
  `verification failed/Put failed/Get failed` 做失败约束）。
- HFS 4 个阶段 `hfsctl check` 均为 `corruption=none`；stderr 扫描无
  `error/failed/corruption`。
- HFS 侧为 `worker_direct_reads` 异步读路径，`[CPU MAP]` 显示
  metadata reactor=CPU0、data reactor=CPU1、frontend dispatcher=CPU3（pin=ok）。
- HFS 适配器调用计数（`hfs-rocksdb io stats`）：

| 阶段 | append_calls / append_bytes | flush_calls | sync_calls | read_calls |
|---|---:|---:|---:|---:|
| hfs-s32-seq | 334298 / 7.46 GB | 334498 | 151 | 760482 |
| hfs-s32-rnd | 992467 / 22.84 GB | 993069 | 467 | 331977 |
| hfs-s64-seq | 184411 / 7.38 GB | 184610 | 150 | 760123 |
| hfs-s64-rnd | 569591 / 22.53 GB | 570184 | 458 | 329707 |

## 5. 已知边界与失败记录

- **首次正式尝试失败（extent 边界）**：第一版协议在同一个 HFS format 上先后保留
  两个 128 万 key 的库（`seq` 与 `rnd`），`rnd` 阶段进行到 `overwrite` 时出现
  `Put failed: IO error: No space left on device: no allocatable extents across
  data devices`（断言退出码 5）。此时设备数据区约 19.2 GiB，物理空间充足，现象
  指向分配器在大量文件/预分配下的 extent 耗尽；本会话未做根因诊断。证据：
  `/home/hfs/rocksdb-native-vs-hfs-20260927-aborted1*`。
  最终协议改为每个阶段先重新 format、同一时刻只保留一个库；单独在全新 HFS 上
  复跑 `rnd` 阶段通过（探针 `/home/hfs/rocksdb-probe-rnd.*`，`rc=0`），因此矩阵
  数据未受影响。多库并存时的分配器行为仍是待查项，不影响本报告的单库结论。
- native 侧顺序读/随机读本会话为 39–40k ops/s，明显低于 HFS 的 132–255k；
  两侧读路径不同（ext4 direct 读 vs HFS worker-direct 异步读），**不能据此宣称
  任何场景的普适优势**；2026-09-28 的受控归因实验（§5.1）显示差距来自 native
  冷读并发塌陷，而非 HFS 读软件路径优势。
- `native-s32` 的 `readwhilewriting` 一轮耗时 30.5 s、聚合约 7.3k ops/s，而其余
  轮次为 0.2–0.5 s；该轮为设备停顿/后台压缩叠加的异常样本，已在矩阵中标注。
- 1 GiB block cache 相对约 1.25 GiB 逻辑数据，`readseq/readrandom` 部分命中缓存；
  所有结论限于本口径。

### 5.1 读场景差距的受控归因（2026-09-28）

用 200,000 key × 1 KiB 的数据集在同一会话内做对照（32 线程，`taskset -c 0-5`，
`--use_direct_reads=1`，除注明外）：

| 用例 | native ext4 | HFS |
|---|---:|---:|
| `fillseq` | 115754 ops/s | 112231 ops/s |
| `readrandom` 单线程、16 MB cache（每次 Get 约 0.93 次设备读） | 18256 ops/s（54.8 µs/op） | 16227 ops/s（61.6 µs/op） |
| `readrandom` 32 线程、1 GiB cache 首轮（约 4.9 万次设备读） | 6603 ops/s（30.3 s） | 228227 ops/s（0.88 s） |
| `readrandom` 32 线程、16 MB cache 首轮（约 18.5 万次设备读） | 6282 ops/s | 67210 ops/s |
| `readrandom` 32 线程、16 MB cache 次轮 | 3219 ops/s（异常轮） | 65677 ops/s |
| `readrandom` 32 线程、1 GiB cache 次/三轮（全 block cache 命中） | 3317341 / 3271912 ops/s | 3256826 / 3144870 ops/s |
| `readrandom` 32 线程、1 GiB cache、buffered 首轮（page cache 命中） | 1263819 ops/s | — |
| FIO 同会话锚定：4K `randread` QD32 | ext4+libaio 98556 IOPS（10.1 µs/次） | raw SPDK 130667 IOPS（7.7 µs/次） |

结论：

1. **单线程设备读延迟两侧一致**（≈55–62 µs/次），**全缓存命中吞吐也一致**
   （≈320 万 ops/s）：HFS 读路径没有固有软件优势，与 2026-09-22 根因报告的
   “warm 同档”结论一致。
2. 差距只在“32 线程 + block cache miss”时出现：native 内核同步读退化到
   ≈170 µs（16 MB cache）甚至 ≈620 µs（1 GiB cache 首轮）每次设备读，
   呈负扩展；HFS 为 ≈16–17 µs/次。
3. 同会话 FIO 证明内核路径本身不慢：ext4+libaio QD32 4K `randread` 达
   98.5k IOPS（10.1 µs/次），接近 raw SPDK 的 130.7k。因此瓶颈是
   **RocksDB native 在 32 线程下的同步 `pread` 访问模式在该 VM/内核上
   的并发行为**，而不是设备或 ext4 的绝对能力；具体内核侧机制未在本轮
   做 profile，列为待查项。
4. 因此矩阵中的 3.4–6.4 倍读差距 = native 冷读并发塌陷 + 1.28M key/1 GiB
   cache 口径下的大量 miss + 会话漂移（native 内核路径跨会话波动大，
   HFS 稳定）；不能解读为 HFS 读性能的普适倍率。

证据：`/home/hfs/rocksdb-read-cause-20260928/`（各用例 stdout/stderr、
native `diskstats` 前后计数、两个 FIO 锚定 JSON、`environment.txt`）。

## 6. 复现

```bash
# VM 内（HFS 源码目录）
ALLOW_FORMAT=1 RESULT=/home/hfs/rocksdb-native-vs-hfs-<date> \
  THREADS_LIST='32 64' TOTAL_KEYS=1280000 \
  bash rocksdb/run_native_vs_hfs_vm.sh
```

脚本逐阶段校验输出行、stderr 错误、HFS io stats 与 `hfsctl check`。

## 7. 证据

- `/home/hfs/rocksdb-native-vs-hfs-20260927/`：8 个阶段 stdout、4 个 HFS `check`  日志、format/ext4-format 日志、`environment.txt` 与 `*-size.txt`。
- `/home/hfs/rocksdb-native-vs-hfs-20260927-metrics.tsv`（会话期间从 stdout 汇总）
  以及 `-aborted1`、`rocksdb-probe-rnd.*` 失败/探针证据。
- `/home/hfs/rocksdb-native-vs-hfs-20260927.run.log`：完成日志
  （`cleanup: stage=complete exit=0`）。

## 8. 大工作集对照（64 MiB block cache，2026-09-28）

口径：`TOTAL_KEYS=1200000`（16 B key、1 KiB value，约 1.2 GB 数据）、
`CACHE_BYTES=67108864`（64 MiB，数据/缓存 ≈19×）、`THREADS_LIST=32`，
其余选项与 §2 相同；每个阶段仍从全新设备状态开始。

| 工作负载 | native ext4 | HFS | HFS/native |
|---|---:|---:|---:|
| fillseq（顺序写） | 103752 | 105625 | **101.8%** |
| readseq（顺序读） | 37654 | 257934 | **685.0%** |
| readrandom（随机读） | 33518 | 61457 | **183.4%** |
| fillrandom（随机写） | 102048 | 103717 | **101.6%** |
| overwrite（覆盖写+校验） | 85497 | 77056 | **90.1%** |
| readwhilewriting（混合） | 15489（30.1 s，异常轮） | 2783772（0.39 s） | 见 §5 注 |

观察：缓存缩到 64 MiB 后，读场景几乎全部落到设备——HFS 顺序读 6.9×、
随机读 1.8× native；写入仍受 flush/compaction CPU 限制，两侧 ≈100%；
`overwrite` 90.1%。这与 §5.1 的结论一致：HFS 的优势只在设备受限读路径兑现。

### 8.1 大库 extent 边界（已修复）与 2M key 复测

同一脚本在更大数据集上曾可复现 HFS 提前 `NoSpace`：

| 数据集 | native | HFS（修复前） | HFS（修复后） |
|---|---|---|---|
| 1.2M key | 两阶段通过 | 两阶段通过（上表） | — |
| 1.5M key | 两阶段通过 | `rnd` 阶段 Flush/Compact 失败（rc=7） | `rc=0`，`orphans=0` |
| 2.0M key | 两阶段通过 | `overwrite` 中 `Put failed: no allocatable extents`（rc=5） | 全矩阵 `rc=0`，`orphans=0` |

根因（bug.md #41）：RocksDB worker-direct 读路径在
`DirectWorkerEntry::files` 里按路径缓存 `shared_ptr<hfs::File>`，文件删除/
改名后缓存不失效，已 unlink 的 inode（orphans）因缓存句柄保持
`openCount>0`，其 extent 永不回收；19.24 GiB 的 extent 池在 live 数据仅
2 GB 时被 ~300 个孤儿 inode 耗尽。修复：`DeleteFile`/`RenameFile` 后
`dropDirectPath` 丢弃缓存句柄，并让 dispatcher 的 `retireHandle` 及时
唤醒释放退役状态（详见 bug.md #41）。

修复后 2M key（约 2 GB 数据、64 MiB cache、32 线程）复测：

| 工作负载 | native ext4 | HFS | HFS/native |
|---|---:|---:|---:|
| fillseq（顺序写） | 96463 | 102997 | **106.8%** |
| readseq（顺序读） | 61137 | 264775 | **433.1%** |
| readrandom（随机读） | 46527 | 59964 | **128.9%** |
| fillrandom（随机写） | 95168 | 99246 | **104.3%** |
| overwrite（覆盖写+校验） | 79716 | 89271 | **112.0%** |
| readwhilewriting（混合） | 2018137 | 2564877 | **127.1%** |

修复后 HFS 侧 `committed` 仅 ~2043/19704 extents、`orphans=0`、alloc/freed
平衡，`hfsctl check` `corruption=none`；4/4 阶段
`cleanup: stage=complete exit=0`。证据：
`/home/hfs/rocksdb-large-2m-fix-20260928`（修复后矩阵）、
`/home/hfs/rocksdb-fix-verify-20260928`（1.5M/2M 复现验证）、
`/home/hfs/rocksdb-diag-1_5m-20260928`（orphans 诊断）、
`…-aborted2m`、`…-1_5m-20260928-aborted`（修复前失败证据）。