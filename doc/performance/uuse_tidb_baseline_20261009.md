# UUSE × TiKV Phase 8 基线（Shard-113 初版 + Shard-118…120 终态，2026-10-09/10）

> 本文件是 Phase 8 的**唯一基线入口**：初版（Shard-113）记录功能/性能门禁基线，
> 终态（§6）记录 TiKV 功能与性能的最终验收结果与同步窗口化结论。

> 环境：host4（.253）4 vCPU / 7.2 GiB / `vm.nr_hugepages=1024`；目标盘
> `VMware NVME_0002`（`0000:13:00.0`，序列号 `VMware NVME_0002`），
> `uio_pci_generic`；UUSE 源码 `/root/code`（135 个文件从 Windows 工作区同步，
> SPDK 未同步，commit `f0b63ed86` 与 Windows 一致）；构建 `/root/code/build-uuse`
> （CMake 4.2.3，Release）。
>
> 版本口径：RocksDB **9.11.2**（系统 `librocksdb-dev`；设计文本写 8.9.1，本机旧基线
> 也是 9.11.2，本报告按实际 9.11.2 记录）、FIO **3.42**（`/home/uuse/fio/fio`）、
> 工具链 GCC 15 / gcc-12（TiKV 构建用）。
>
> 用途：Phase 8 后续 shard 的回归对照。跨会话数字不可直接比较；shard 出口必须
> 同会话交错 A/B。

## 1. 结果摘要

| 级别 | 项目 | 结果 |
|---|---|---|
| L1 | CTest 全量 | **29/29 通过**（3.21 s） |
| L2 | FIO smoke（prep 写 + 4K randrw 50/50、2 jobs × QD32、10 s） | 读 **53476** / 写 **53629** IOPS，errors=0，closure `queued=reaped=1071153`、outstanding=0、token_mismatch=0 |
| L3/L4 | RocksDB native-vs-UUSE A/B（threads 32/64，fresh format/phase，交错） | UUSE/native：写 97–110%、读 158–781%、readwhilewriting 93–117%（见 §3） |
| L4 | FIO 正式同形状 gate（UUSE vs raw SPDK，5-run median） | M(1j32)：UUSE/raw **95.5%**、p99 比值 **0.92**；E(8j4)：**86.0%**、p99 比值 **0.20**；errors=0（见 §3b） |

## 2. 环境与口径

- 同步：`src/`、`include/`、`tests/`、`tools/`、`fio/`、`rocksdb/`、`config/`、
  `tikv/`、根构建文件（135 个），SPDK 保持 VM 内 Git 状态；
- 构建命令：
  `cmake -S /root/code -B build-uuse -DCMAKE_BUILD_TYPE=Release
  -DFIO_SOURCE_DIR=/home/uuse/fio -DROCKSDB_ROOT=/usr`；
  产物 `libuuse_core.so`、`fio-uuse.so`、`libuuse_rocksdb.so`、`uusectl`、
  `uuse_db_bench`、`uuse_ldb` 与全部测试；
- 设备绑定：`driver_override` → `uio_pci_generic`（直接 `bind` 会被拒绝）；
- 测试 BDF：CMake 从 `config/uuse_nvme.conf` 自动读取 `0000:13:00.0`；
- host4 为 4 vCPU，A/B 脚本本机适配：`nproc>=4`、`taskset -c 0-3`、
  `worker_cpus=0,1,2,3`、结果目录 `/build/uuse-baseline-rocksdb-20261009`；
- FIO smoke 命令（可复现）：
  prep：`fio --name=uuse-prep --ioengine=external:.../fio-uuse.so --thread=1
  --uuse_config=config/uuse_nvme.conf --uuse_async=1 --uuse_client_depth=32
  --filename=/fio-smoke --rw=write --bs=64k --size=256m --direct=1 --iodepth=4
  --numjobs=1 --output-format=json`；
  cell：同前，`--rw=randrw --rwmixread=50 --bs=4k --iodepth=32 --numjobs=2
  --time_based=1 --runtime=10`。

## 3. RocksDB native-vs-UUSE A/B

脚本：`rocksdb/run_native_vs_uuse_vm.sh`（本机适配版，逻辑未改）。
参数：1,280,000 keys × 1 KiB、cache 1 GiB、`use_direct_reads=1`、
`use_direct_io_for_flush_and_compaction=1`、compression none；每 phase 全新格式化，
native ext4 与 UUSE 交错执行。

### threads = 32

| bench | native ops/s | UUSE ops/s | UUSE/native |
|---|---:|---:|---:|
| fillseq | 96213 | 98532 | 102.4% |
| readseq | 180285 | 285606 | 158.4% |
| readrandom | 38716 | 179405 | 463.4% |
| fillrandom | 91352 | 93206 | 102.0% |
| overwrite | 81177 | 89528 | 110.3% |
| readwhilewriting | 2053283 | 1909473 | 93.0% |

### threads = 64

| bench | native ops/s | UUSE ops/s | UUSE/native |
|---|---:|---:|---:|
| fillseq | 96852 | 95293 | 98.4% |
| readseq | 37927 | 296342 | 781.3% |
| readrandom | 38330 | 184236 | 480.7% |
| fillrandom | 93281 | 90818 | 97.4% |
| overwrite | 81988 | 85138 | 103.8% |
| readwhilewriting | 1900565 | 2229415 | 117.3% |

说明：UUSE 读路径为 worker-direct / kFSBuffer，且 native 侧含 ext4 direct IO 差异；
读倍率是真实对照结果，不作为回归门禁。回归门禁按同会话交错 A/B <5% 判定。
4 个 UUSE phase 全部 `corruption=none`，`uuse-rocksdb io stats` 齐全，无
error/failed 行。

## 3b. FIO 正式同形状 gate（UUSE vs raw SPDK）

脚本：本机适配版（`temp_scripts/uuse_host4_fio_gate.sh`）；形状 M = 1 job × QD32、
E = 8 jobs × QD4；4K randrw 50/50、256 MiB、10 s、5 轮交错、每轮全新 format/raw
prep；raw 侧 `LD_PRELOAD=spdk_bdev` + `gen_nvme.sh` JSON。两侧 `taskset -c 0-3`。

| 形状 | UUSE 读/写 IOPS（中位） | raw 读/写 IOPS（中位） | UUSE/raw | UUSE p99 | raw p99 | p99 比值 |
|---|---:|---:|---:|---:|---:|---:|
| M (1j32) | 50123 / 50049 | 52492 / 52422 | **95.5%** | 577.5 µs | 626.7 µs | **0.92** |
| E (8j4) | 43705 / 43653 | 50804 / 50753 | **86.0%** | 626.7 µs | 3162.1 µs | **0.20** |

两个形状均满足 §16.2 的 UUSE/raw ≥85% 与 p99 ≤110% 门槛，errors 全 0，5 轮无
异常。CPU% 门槛（8 vCPU 口径 ≤500%）在 host4 4 vCPU 上未采集，不得按 8 vCPU
口径引用。E 形状 raw 自身塌陷（p99 3.16 ms）与 §16.2 已记录的边界一致。

## 4. 证据路径
- `/home/uuse/uuse-baseline-20261009/`：FIO smoke（json/stderr/format）+
  `rocksdb-ab/`（33 个文件，156 K）；
- `/build/uuse-baseline-rocksdb-20261009/`：A/B 原始 stdout/stderr/format/check；
- `/build/uuse-baseline-fio-gate-20261009/`：gate 原始 json/stderr/format；
- `/tmp/uuse-ctest.log`：CTest 全量输出；
- **终态证据（§6）**：`/build/tidb/perf-ab-target-*`（同盘 A/B）、
  `/build/tidb/functional-matrix-*`、`/build/tidb/closure-*`、
  `/build/uuse-shard120-20261010/`（L1/L2）、`/build/uuse-shard120-rocksdb-20261010/`、
  `/build/uuse-shard120-fio-gate-20261010/`、`/build/tidb/trace-analysis-rw-*.txt`
  （同步归因解析）；归档
  `/home/uuse/uuse-tikv-20261010/tikv-shard120-evidence.tar.gz`；
- 本机操作脚本：`temp_scripts/uuse_host4_*.sh`、`temp_scripts/uuse_*`（Windows 工作区，
  git 忽略；可复现入口见 `tikv/run_*.sh` 与 `tikv/patches/`）。

## 5. 待补与边界

- **MySQL 冒烟（待重建 fork）**：host4 现有 `/home/uuse/mysql-build` 是 HFS 时代
  构建（`hfs::` 命名空间、链接 `libhfs_core.so.2`），与 UUSE 2.0 core
  （`uuse::`/`libuuse_core.so`）不兼容；需按 `mysql/patches/` 当前补丁栈在 VM 内
  重建 MySQL 8.0.46 fork 后执行；
- **FIO gate CPU%**：4 vCPU 环境未采集 8 vCPU 口径的 CPU 门槛；
- RocksDB 9.11.2 与设计文本 8.9.1 的口径差异需在设计/计划中注明；
- host4 根盘仅剩约 2.5 GiB，大体积证据统一写 `/build` 或 `/home/uuse` 小文件；
- TiKV 基座构建口径（v8.5.8、gcc-12、`CMAKE_POLICY_VERSION_MINIMUM=3.5`、
  `-luring` 链接）见 `tikv/README.md`；
- **崩溃 RPO ≤5 ms**（同步窗口化的语义边界）与 fast-reorg ingest 未闭环等
  TiKV 侧边界见 §6.4 与 `tikv/README.md` §6。

## 6. TiKV 终态：功能与性能（Shard-118…120，2026-10-10）

### 6.1 功能与 durability 出口

| 出口 | 结果 | 证据 |
|---|---|---|
| 功能矩阵（Shard-119） | **26/26** | `/build/tidb/functional-matrix-*` |
| 并发 closure（Shard-118） | **5/5**（~20 万行 SIGTERM 重启持久、`uusectl check corruption=none`） | `/build/tidb/closure-*` |
| 生命周期用例（Shard-118） | 6/6，ASAN 干净、TSAN 0 警告 | `tikv/shim/uuse_tikv_shim_lifecycle.cc` |
| L1 CTest | 29/29 | `/build/uuse-shard120-20261010/ctest.txt` |
| L2 FIO smoke | 读 50238 / 写 50346，errors=0，closure 闭合 | 同上 |
| L3/L4 RocksDB A/B（640k keys，同盘交错） | 写 96.1–107.7%、混合读写 95.7–112.6%，无 >5% 回退 | `/build/uuse-shard120-rocksdb-20261010/` |
| L4 FIO gate | M 93.5%（p99 0.96）、E 89.9%（p99 0.20），errors=0 | `/build/uuse-shard120-fio-gate-20261010/` |

### 6.2 同盘 TiKV A/B（UUSE vs 宿主 ext4，同一目标 NVMe，5 轮交错）

最终口径（worker/apply 5 ms 同步窗口）：

| 基准 | UUSE 中位 | 宿主中位 | 比值 | 门槛 | 判定 |
|---|---:|---:|---:|---|---|
| point_select tps | 18534 | 18838 | **98.4%** | ≥95% | PASS |
| point_select p99 | 1.30 ms | 1.27 ms | 102.4% | ≤110% | PASS |
| read_write tps | 378 | 404 | **93.7%** | ≥85% | PASS |
| read_write p99 | 33.12 ms | 30.26 ms | 109.5% | ≤110% | PASS |

三变体对比（绝对值为同日会话参考；比率为同会话口径）：

| 变体 | fsync 屏障/60 s | 同步累计 | 点读比值 | 混合 tps 比值 | 混合 p99 比值 |
|---|---:|---:|---:|---:|---:|
| always-sync（Shard-118 修复版） | 55,169 | 17.4 s | 94.9% | 90.9% | 107.5% |
| apply 窗口 1 ms | 57,540 | 16.3 s | 97.8% | 93.3% | 109.4% |
| **worker+apply 窗口 5 ms** | **21,856** | **5.3 s** | **98.4%** | **93.7%** | 109.5% |
| （崩溃前会话，always-sync） | — | — | 97.9% | 89.5% | 117.6% |

### 6.3 同步路径 trace 归因（read_write 60 s，`trace_sample=1`）

- 主来源是 **async write worker**（`components/raftstore/src/store/async_io/write.rs`）：
  apply-state 的 kvdb 写与 raft 日志写原为每次 raft append 硬编码 `sync=true`
  （~51k 屏障/60 s，p50 182 µs / p99 1.68 ms）；apply 线程的 `write_to_db` 不是主导
  路径。
- 修复补丁：`tikv/patches/apply-uuse-worker-sync-window.py`（worker 两处 5 ms 窗口）、
  `apply-uuse-apply-sync.py`（apply 线程 5 ms 窗口）。
- 窗口后：屏障 55k→**21.8k**、同步累计 17.4 s→**5.3 s**；主源 50,855→10,822。
- 原始解析：`/build/tidb/trace-analysis-rw-*.txt`（原始 trace 已清理）。

### 6.4 边界（RPO）

- 同步窗口使单节点实验室的**崩溃 RPO ≤5 ms**（apply-state 与 raft 日志）；优雅退出
  仍无条件全同步（**RPO=0**，shutdown 双引擎 sync 保留）；对比 ext4 页缓存掉电窗口
  ~5 s 仍严格。
- 其余边界（fast-reorg ingest 未闭环、`tikv-ctl` 禁用、错误面透传等）见
  `tikv/README.md` §6 与 `doc/review/uuse_tikv_storage_audit_20261009.md` §10。
