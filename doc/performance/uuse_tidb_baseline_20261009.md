# UUSE × TiKV Phase 8 基线（Shard-113，2026-10-09）

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
- 本机操作脚本：`temp_scripts/uuse_host4_*.sh`（Windows 工作区，git 忽略）。

## 5. 待补与边界

- **MySQL 冒烟（待重建 fork）**：host4 现有 `/home/uuse/mysql-build` 是 HFS 时代
  构建（`hfs::` 命名空间、链接 `libhfs_core.so.2`），与 UUSE 2.0 core
  （`uuse::`/`libuuse_core.so`）不兼容；需按 `mysql/patches/` 当前补丁栈在 VM 内
  重建 MySQL 8.0.46 fork 后执行；
- **FIO gate CPU%**：4 vCPU 环境未采集 8 vCPU 口径的 CPU 门槛；
- RocksDB 9.11.2 与设计文本 8.9.1 的口径差异需在设计/计划中注明；
- host4 根盘仅剩约 2.5 GiB，大体积证据统一写 `/build` 或 `/home/uuse` 小文件；
- TiKV 基座构建口径（v8.5.8、gcc-12、`CMAKE_POLICY_VERSION_MINIMUM=3.5`、
  `-luring` 链接）见 `tikv/README.md`。
