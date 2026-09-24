# HFS 2.0 Phase 7（MySQL/InnoDB）RocksDB/FIO 基线快照

时间：2026-09-24  
状态：Shard-99 出口基线，已采集（HFS core 零改动）  
目的：为 Phase 7 后续 shard 提供同口径回归对照，防止 MySQL 适配引入 RocksDB/FIO 回退。

## 1. 环境与构建

| 项目 | 值 |
|---|---|
| 设备 | `Nvme0n1`，PCI BDF `0000:0b:00.0` |
| 配置 | `config/hfs_nvme.conf`（`metadata_cpus=0`、`data_reactors=1`、`data_cpus=1`、`frontend_cpus=3`、`[rocksdb] worker_direct_reads=true`） |
| 构建目录 | `/root/code/HFS_BASE_SPDK/build-baseline-20260924`（Release） |
| CMake flags | `-DCMAKE_BUILD_TYPE=Release -DHFS_SPDK_ROOT=<spdk> -DFIO_SOURCE_DIR=/root/code/fio-3.42 -DROCKSDB_ROOT=/usr` |
| 源码指纹 | `src/core/client.cpp` = `ee347267339d68abb3a78812133d87e7`；`rocksdb/hfs_filesystem.cpp` = `4444d6724282d60594935bd07ee637a2`；`config/hfs_nvme.conf` = `f26968508ca6068da3e26bc354fbed21`（与 HEAD `eb1b1f4` 一致） |
| 工具 | `/root/code/fio-3.42/fio`（source-matched）、`spdk/build/fio/spdk_bdev`、`rocksdb_readrandom_observer` |
| 证据目录 | `/mnt/hfs-data/hfs2-mysql-baseline-20260924/` |

## 2. L1：CTest

`ctest --test-dir build-baseline-20260924`：**26/26 passed**（1.98 s）。
证据：`/mnt/hfs-data/hfs2-mysql-baseline-20260924/ctest.log`。

## 3. L2：FIO smoke（HFS engine）

| 场景 | 结果 |
|---|---|
| `numjobs=1`、`iodepth=4`、4K randrw、3 s、`--verify=crc32c --verify_fatal=1 --end_fsync=1` | rc=0，`error=0`，`queued=reaped=181894`，`outstanding=0` |
| `numjobs=2`、`iodepth=4`、4K randrw、3 s | rc=0，`error=0`，`queued=reaped=217785`，`outstanding=0` |

注意：`numjobs=2` 共享文件同时开 `--verify` 会让两个 job 互相校验对方写入，
fio 报 `error=84`（EILSEQ）属拓扑假阳性；verify 口径固定为 `numjobs=1`。
证据：`smoke1.json`、`smoke2.json`、`smoke1.stderr`、`smoke2.stderr`。

## 4. L3：RocksDB readrandom 同会话交错 A/B

协议：`--keys=524288 --threads=8 --key-size=16 --value-size=1024 --cache-size=33554432
--block-size=4096 --direct-reads=1 --compression=none --worker-cpus=4,5`；native ext4
direct 与 HFS 同会话交错，`stats=false`，进程 `taskset -c 0-5`。

| 组别 | ops/s | p50 (ns) | p95 (ns) | p99 (ns) | 退出 |
|---|---:|---:|---:|---:|---|
| native_begin | 68,183.1 | 112,594 | 198,422 | 271,868 | rc=0 |
| HFS S1 | 72,327.2 | 110,943 | 158,842 | 241,494 | clean |
| HFS D1 | 72,411.5 | 111,093 | 157,686 | 243,257 | clean |
| HFS S2 | 71,915.5 | 111,851 | 159,904 | 243,848 | clean |
| HFS D2 | 72,212.6 | 112,030 | 156,359 | 236,453 | clean |
| HFS S3 | 71,741.4 | 111,503 | 162,639 | 264,525 | clean |
| HFS D3 | 71,289.8 | 112,639 | 160,304 | 252,760 | clean |
| HFS SA1/DA1（active） | 68,664.4 / 69,888.6 | 115,214 / 115,095 | — | 249,352 / 240,918 | clean |
| HFS SA2/DA2（active） | 70,085.0 / 70,200.4 | 114,457 / 114,548 | — | 243,590 / 241,067 | clean |
| HFS S4/D4（drift） | 70,717.6 / 70,897.1 | 113,709 / 112,890 | — | 246,579 / 255,728 | clean |
| native_end | 69,486.5 | 109,259 | 201,438 | 292,986 | rc=0 |

结论：

1. HFS readrandom 与 native ext4 direct 等价（HFS 71.3–72.4K vs native 68.2–69.5K），
   p50 约 111 µs vs 109–113 µs；
2. 所有 HFS 运行干净退出（`killed=no`），无 W2 挂死；
3. 由于 `[rocksdb] worker_direct_reads=true`，shared/direct 组实际都走 worker-direct；
   该组数据只作为默认配置基线，不用于 shared/direct 归因；
4. HFS 侧设备读计数为 0 是 SPDK 绕过内核计数所致，不是无读。

证据：`read-latency-ab.tsv`、`ab-*.log`。

## 5. L4：8j4 正式 gate 基线（同会话 5-run）

协议：4K randrw 50/50、256 MiB、10 s、5-run median、trace/stats off、`taskset -c 0-5`；
raw = SPDK fio plugin `1 job × iodepth 32`；HFS = `fio-hfs.so`、`hfs_async=1`、
`hfs_client_depth=32`、`8 jobs × iodepth 4`。

| 指标 | raw SPDK | HFS | 比值 |
|---|---:|---:|---:|
| IOPS 中位 | 115,889.211 | 81,713.729 | 0.705102 |
| p99 中位 (µs) | 602.112 | 518.144 | 0.860544 |
| errors | 0 | 0 | — |

五次 HFS 全部 closure 闭合（`accepted=terminal=published`、`child_submit=child_complete`、
`queued=reaped`、`outstanding=0`、`token_mismatch=0`、`busy=0`）。
**该比值是当前基线参考值，不是性能门禁结论**；后续 shard 回归以同一脚本与口径比较，
偏差 <5% 视为无回退。

证据：`gate/run.log`、`gate/raw-*.json`、`gate/hfs-*.json`、`gate/hfs-*.stderr`。

## 6. 复现

```text
python E:\Code\test_script\baseline_build_test_20260924.sh   # VM: build + CTest + smoke
python E:\Code\test_script\baseline_smoke_rerun_20260924.sh  # VM: verify/plain smoke
python E:\Code\test_script\baseline_gate_20260924.sh         # VM: L4 5-run gate
python E:\Code\test_script\run_baseline_ab_20260924.py       # W0 readrandom A/B
```

设备在每次运行结束后恢复内核 `nvme` 驱动；大体积证据保留在 VM `/mnt/hfs-data`。

## 7. 备注

- 首轮 smoke 使用 `numjobs=2 + verify` 得到 `error=84`，已按 §3 修正口径；
- 首轮 A/B 因 `build-baseline-20260924` 漏配 `-DROCKSDB_ROOT=/usr` 未构建
  `rocksdb_readrandom_observer`（rc=127），已重配重跑；后续构建脚本必须带该 flag。
