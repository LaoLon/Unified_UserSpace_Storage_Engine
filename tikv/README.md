# TiKV 集成（UUSE Phase 8）

本目录保存 UUSE ↔ TiKV 集成的可复现材料：TiKV fork 的补丁栈（`patches/`）、C ABI
shim（`shim/`）、VM 内构建/部署与验收脚本。设计依据见 `doc/design/uuse.md` §21，
执行计划见 `doc/shard/uuse_tidb_shard_plan.md`，存储绕过点审计见
`doc/review/uuse_tikv_storage_audit_20261009.md`。

## 1. 状态（Shard-121 收口，2026-10-10）

Phase 8 全部 shard（113–121）已完成：

| 出口 | 结果 | 证据 |
|---|---|---|
| 功能矩阵（Shard-119） | **26/26 PASS** | `doc/shard/uuse_tidb_shard_plan.md` §Shard-119 |
| 生命周期用例（Shard-118） | 6/6，ASAN 干净、TSAN 0 警告 | `shim/uuse_tikv_shim_lifecycle.cc`、`shim/tsan.supp` |
| 并发 closure（Shard-118） | 5/5（18.7 万行负载 → SIGTERM → 重启行数完整） | `run_concurrent_closure_vm.sh` |
| 性能同盘 A/B（Shard-120） | 终态：点读 98.4%、混合 tps 93.7%、p99 102.4%/109.5% 全部通过（同步窗口化后） | `doc/performance/uuse_tidb_baseline_20261009.md` §6 |
| RocksDB/FIO 无回退门禁（Shard-120） | CTest 29/29、RocksDB A/B 无 >5% 回退、FIO gate M 93.5%/E 89.9% | 同上 |

## 2. 架构（实现口径）

- **kvdb**：C++ 层实现 `rocksdb::FileSystem`（`rocksdb/uuse_filesystem.cpp`），经
  crocksdb FFI 新增 `crocksdb_uuse_env_create` 构造 `Env`（`NewCompositeEnv`）注入
  `engine_rocks`；不修改 RocksDB 本体，不实现通用 Rust `RocksDBEnv`。
- **raft-engine**：Rust `FileSystem` trait 实现（`shim/rust/uuse_file_system.rs`），
  经 `tikv/shim` C ABI 调 UUSE core；`read_dir`/`create_dir` 为 trait 增量
  （默认 fail-closed，禁止回落宿主 `std::fs`）。
- **共享 engine**：单进程一个 UUSE engine（shim 进程级单例 + adapter
  `createFileSystemWithEngine(ownsEngine=false)`），kvdb 与 raft-engine 共用；
  第二实例、跨进程、fork child 分别 `Busy`/`Busy`/`ForkedProcess`。
- 不是 POSIX 兼容层：无 preload/FUSE/mmap/Blobstore 回流；core 不新增 POSIX
  `read/write/pread/pwrite` 入口。

## 3. 版本冻结（Gate 0）

- TiKV **v8.5.8**（commit `a1fdd6a59`，`/build/tikv-fork`）；
- rust-rocksdb `64a092f`、raft-engine `3d84405`（随 `Cargo.lock` 冻结）；
- PD/TiDB/dumpling **v8.5.8**（`/build/tidb/bin`）；
- 升级版本时按 `patches/README.md` §Rebase 说明逐条重锚并重跑矩阵。

## 4. VM 内构建与运行

- 构建：`tikv/build_tikv_vm.sh`（`TIKV_REF=v8.5.8`；`CMAKE_POLICY_VERSION_MINIMUM=3.5`、
  gcc-12、`-luring`、`JOBS=2`+swap，见脚本头注释）；`tikv-server` 链接
  `libuuse_tikv_shim.so`（运行需 `LD_LIBRARY_PATH=/root/code/build-uuse`）；
- 单节点 bring-up：`tikv/run_single_node_vm.sh`（PD + TiKV + TiDB，生成
  `/build/tidb/tikv-uuse-both.toml`）；
- 验收脚本：`run_functional_matrix_vm.sh`（功能矩阵）、
  `run_concurrent_closure_vm.sh`（并发 closure）、`run_perf_ab_target_vm.sh`
  （同盘性能 A/B，宿主 ext4 侧切内核 nvme + mkfs/mount 于同一盘）；
- 前置：`0000:13:00.0` 绑 `uio_pci_generic`、`vm.nr_hugepages=1024`、UUSE 构建
  `/root/code/build-uuse`。

## 5. 配置模板

```toml
[storage]
data-dir = "/build/tidb/tikv-data"
uuse-conf = "/root/code/config/uuse_nvme.conf"   # 正式单盘口径；kvdb/raftdb
reserve-space = "0GB"                             # 宿主 reserve 不保护 UUSE 卷
reserve-raft-space = "0GB"

[raft-engine]
dir = "/build/tidb/tikv-data/raft-engine"
uuse-conf = "/root/code/config/uuse_nvme.conf"

[log]
file.filename = "/build/tidb/logs/tikv-uuse-both.log"   # 宿主白名单

[rocksdb]
info-log-dir = "/build/tidb/logs/rocksdb"               # 宿主白名单
```

`uuse-conf` 非空即启用 UUSE；缺省清空 = POSIX 文件系统。配置键为 kebab-case
（`uuse-conf`、`reserve-space`），`config/uuse_tidb.conf` 为独立口径。

## 6. 支持边界（Shard-121 交付物）

**支持范围**：单节点实验室部署（PD + TiKV + TiDB，单 store）；kvdb 与 raft-engine
（默认）落在 UUSE；SQL CRUD/DDL（经典回填）/乐观与悲观事务/`ADMIN CHECK`/region
split/GC/kill -9 与 SIGTERM 重启/跨进程 reopen/dumpling 备份恢复。

**禁用项（配置出现即 fail-closed）**：encryption-at-rest、S3/cloud FS、mmap 读写、
多进程共享 bdev、`tikv-ctl` 直连 UUSE 数据目录（一期）。

**已知边界（记录在案，不静默降级）**：

| 边界 | 现象/影响 | 规避与去向 |
|---|---|---|
| fast-reorg ingest 未闭环 | `ADD INDEX` 走 SST importer（宿主 FS 写 SST + engine ingest 找不到）→ `IngestSST Io(NotFound)` 重试 | 设 `tidb_ddl_enable_fast_reorg=OFF` 走经典回填；路由待修（审计第 6 条） |
| 混合读写 p99 | 终态 109.5%（≤110% 门槛内；早期会话 117.6% 已收敛） | worker/apply 同步窗口化（5 ms）已实施；trace 归因与前后对比见 `doc/performance/uuse_tidb_baseline_20261009.md` §6 |
| 崩溃 RPO（kill -9） | 未 sync 尾部丢失窗口 ≤5 ms（apply-state 与 raft 日志；优雅退出 RPO=0） | 单节点实验室口径；worker/apply 5 ms 同步窗口（`apply-uuse-worker-sync-window.py`、`apply-uuse-apply-sync.py`）；对比 ext4 页缓存 ~5 s 仍严格 |
| kill -9 durability | 未 sync 的尾部随进程退出丢失（与 ext4 掉电语义对齐） | 优雅退出（SIGTERM）已由双引擎 sync 闭合；不要依赖 kill -9 持久性 |
| 错误面 | 第二实例/挂载失败统一报 `uuse env create failed`，丢失底层 errno | 审计 §8 待办：shim 透传 last-error |
| 宿主白名单 | `data_dir/LOCK`、panic mark、`last_tikv.toml`、raft-engine `LOCK`、日志/status | 运维机制，无引擎数据；见审计 §4 行 1–5、9、15 |
| MySQL 同盘冒烟 | 未执行：fork 停留 HFS 时代状态、`/` 根盘仅 2.1 GiB、全量重建需数小时 | 独立会话按 `mysql/build_mysql_vm.sh`（更新 `UUSE_CORE_ROOT`）执行 |
| 备份输出 | dumpling/BR/lightning 输出默认宿主/对象存储 | 显式配置，不默认进 UUSE（审计 §4 行 7） |

## 7. 补丁栈

`patches/`（9 个，应用顺序、幂等/回退与 rebase 说明见 `patches/README.md`；
raft-engine 两处改动作用于 cargo git checkout，改后需清 raft-engine 指纹重建）。

## 8. 证据归档

- 功能矩阵/closure/性能 A/B：`/build/tidb/functional-matrix-*`、
  `/build/tidb/closure-*`、`/build/tidb/perf-ab-target-*`；
- 门禁：`/build/uuse-shard120-20261010/`、`/build/uuse-shard120-rocksdb-20261010/`、
  `/build/uuse-shard120-fio-gate-20261010/`；
- 汇总文档：`doc/performance/uuse_tidb_baseline_20261009.md`（§6 终态）、
  `doc/review/uuse_tikv_storage_audit_20261009.md`、`doc/bug.md` #48–#50。
