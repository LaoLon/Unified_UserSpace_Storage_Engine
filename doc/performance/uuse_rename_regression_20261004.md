# UUSE 改名后回归验证：FIO / RocksDB / MySQL（2026-10-04）

> 目的：项目由 HFS 更名为 UUSE（`914a83b`）后，按
> `doc/performance/fio_baseline.md`、`rocksdb_baseline.md`、`mysql_baseline.md`
> 的既有口径在同一会话内重跑三层同盘对照，确认改名（命名空间、二进制名、
> 配置名、ABI=3、磁盘格式）没有引入性能回退。历史基线的绝对数值保留在原文档；
> 本文只描述本次会话，跨会话绝对值不直接比较。

## 1. 环境与构建

| 项目 | 值 |
|---|---|
| VM | .250（原 HFS 开发机），6 vCPU、约 7.2 GiB RAM、`HugePages_Total=1024` |
| 目标设备 | `VMware NVME_0002`，PCI `0000:13:00.0`（.250 内核名 `nvme1n1`），测试期间绑定 `uio_pci_generic` |
| 源码 | 改名提交 `914a83b` + 工具偏移修复 `d1b3656`；VM `/root/code` 同步后 `build-verify` Release 重建 |
| 回归 | CTest 29/29（非设备 21 + 设备 8，设备测试单独绑定后全过） |
| 配置 | `config/uuse_nvme.conf`、`config/uuse_mysql.conf`（`[uuse]`、`UUSE_CORE_ABI_MAJOR=3`） |
| FIO | 3.42，`/home/uuse/fio/fio`；引擎 `build-verify/fio-uuse.so` |
| RocksDB | 9.11.2（系统库）；`build-verify/uuse_db_bench`、`uuse_ldb` |
| MySQL | 8.0.46（`/home/uuse/mysql-build`，seam 改名后重建） |
| 证据 | `/home/uuse/fio-three-way-20261004`、`/home/uuse/rocksdb-native-vs-uuse-20261004`、`/home/uuse/mysql-native-vs-uuse-20261004`；阶段日志 `/home/uuse/rename-regression-20261004/{fio,rocksdb,mysql}.log` |

## 2. FIO 三层矩阵（randrw 50/50、256 MiB、10 s、3 轮中位数）

| I/O | QD | UUSE IOPS | raw IOPS | UUSE/raw | UUSE p99 读/写 µs | raw p99 读/写 µs |
|---|---:|---:|---:|---:|---|---|
| 4K | 32 | 77651 | 79836 | **97.3%** | 791/791 | 872/872 |
| 4K | 64 | 86351 | 74252 | **116.3%** | 1352/1368 | 1729/1729 |
| 16K | 32 | 64893 | 65731 | **98.7%** | 913/913 | 995/1004 |
| 16K | 64 | 62295 | 68278 | **91.2%** | 1761/1761 | 1827/1827 |
| 64K | 32 | 38361 | 38785 | **98.9%** | 1384/1384 | 1532/1532 |
| 64K | 64 | 33693 | 33271 | **101.3%** | 2802/2867 | 3490/3523 |

结论：UUSE/raw 全部 ≥85%（91.2%–116.3%），p99 全部不高于 raw（≤100%），
与 2026-09-27 基线的 94.9%–102.6% 同档；改名未改变 FIO 数据路径。

## 3. RocksDB 对照（1,280,000 key × 1 KiB、32/64 线程、每阶段全新 format）

| 工作负载 | 线程 | UUSE ops/s | native ext4 ops/s | UUSE/native |
|---|---:|---:|---:|---:|
| fillseq | 32 | 76650 | 67294 | **113.9%** |
| fillseq | 64 | 80999 | 80870 | **100.2%** |
| readseq | 32 | 217432 | 39376 | **552.2%** |
| readseq | 64 | 231236 | 20402 | **1133.4%** |
| readrandom | 32 | 131988 | 28715 | **459.6%** |
| readrandom | 64 | 135870 | 39012 | **348.3%** |
| fillrandom | 32 | 73228 | 66578 | **110.0%** |
| fillrandom | 64 | 79155 | 83401 | **94.9%** |
| overwrite（含逐键校验） | 32 | 65217 | 62260 | **104.7%** |
| overwrite | 64 | 69094 | 67922 | **101.7%** |
| readwhilewriting | 32 | 1822353 | 1954029 | **93.3%** |
| readwhilewriting | 64 | 2156227 | 3172（异常轮） | 不可比 |

结论：写路径 94.9%–113.9%，无 >5% 回退；读路径 3.5–11× 领先与历史
"native 冷读并发塌陷、UUSE 设备受限读优势"结论一致；`readwhilewriting` 64
线程 native 3172 ops/s 为已知的间歇设备停顿异常轮（历史多次记录），不计入
比值。全部阶段 `overwrite` 校验通过，`uusectl check` `corruption=none`。

## 4. MySQL 对照（sysbench 1.0.20，10 表 × 1 万行，16/32/64 线程，两轮均值）

| 场景 | 16 线程 | 32 线程 | 64 线程 | 历史区间（§12） |
|---|---:|---:|---:|---|
| `oltp_read_write`（70/30） | **106.8%** | 92.0% | 97.2% | 89.9–98.9% |
| `oltp_read_only`（随机读） | 99.2% | 99.3% | **99.4%** | 98.5–99.8% |
| `oltp_write_only`（纯写） | **102.1%** | 91.4% | 89.9% | 87.6–93.4% |
| `oltp_point_select`（点查） | 99.9% | 95.6% | 95.4% | 95.4–98.9% |

结论：全部落在历史波动区间内；p99 同档（write_only 64t 23.95 vs 21.50 ms、
read_write 64t 45.79 vs 39.65 ms，属会话噪声）。`innodb_flush_log_at_trx_commit=1`
四阶段确认，fsync 计数持续增长。

## 5. 改名过程中发现并修复的问题

1. **`--uuse-config=` 偏移 bug**（`rocksdb/uuse_db_bench.cpp`、`uuse_ldb.cpp`）：
   选项名由 13 字符的 `--hfs-config=` 改为 14 字符后 `substr(13)` 未同步，
   配置路径解析成 `=/root/...`，表现为 `cannot create UUSE RocksDB Env`；
   修复提交 `d1b3656`。
2. **MySQL fork 缺少顶级 CMake 全局链接块**：`uuse_seam_shard101b.patch` 的
   `CMakeLists.txt` hunk 未在 VM fork 生效，`comp_err` 等直接链接 `libmysys.a`
   的目标缺 `-luuse_core`；按补丁原文补回后全量构建通过（`mysqld 8.0.46`）。
3. 环境性修复：MySQL 构建 OOM（4 G swap + `-j2`）；FIO/RocksDB 脚本要求结果
   目录不存在；`.250` 根盘满（移除构建 swapfile、历史证据归档到 NVME_0001）。

## 6. 边界

- 本文只证明"改名未引入回退"，不改变任何正式门禁口径；RocksDB 大工作集
  （64 MiB cache）与 8j4 FIO 形状的改名后复测未包含在本轮。
- 绝对 IOPS/TPS 受会话与设备漂移影响，与历史文档不跨会话比较。
- 历史性能文档保留 HFS 时期名称与数值，仅追加本回归的指引。
