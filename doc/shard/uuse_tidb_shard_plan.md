# UUSE 2.0 TiDB/TiKV frontend Shard 开发计划（Shard-113 起）

> 日期：2026-10-09
>
> 状态：计划；Gate 0（§0.1）未完成不得动代码
>
> 事实源：`doc/design/uuse.md`（现行唯一架构/边界/接口/验收事实源；Phase 8 契约以更新后的 §21 为准）
>
> 全局规则沿用：`doc/shard/hfs2.0_shard_development_plan.md` §0（HFS 时期命名，规则继续有效）
>
> 基线依据：`doc/design/uuse.md` §16、`doc/performance/rocksdb_baseline.md`、`doc/performance/mysql_baseline.md`
>
> 编号：接 Phase 7 MySQL 的 Shard-112 之后，从 Shard-113 开始；本文定义 Phase 8（TiDB/TiKV frontend）
>
> 不包含：FUSE/POSIX preload、TiFlash、TiDB server/PD 改造、encryption-at-rest、S3/cloud FS、
> 多 engine/多进程共享 bdev、punch hole 实现（v1 按审计结论 fail-closed 或显式降级并记录）、
> 既有 RocksDB/FIO/MySQL frontend 行为变更、TiKV 上游非 seam 改动、3 副本集群（二期，见 §6）

---

## 0. 全局规则

### 0.1 前置条件（Gate 0，未完成不得动代码）

1. **设计先行**：更新 `doc/design/uuse.md`
   - §3.1 支持列表加入 TiKV frontend（TiDB 的本地存储层；TiDB server/PD 是无状态进程，
     不属于 UUSE 边界）；
   - §3.2 明确禁用项：encryption-at-rest、S3/cloud storage、FUSE/preload、TiFlash、
     fork/多进程共享 bdev、mmap 读写、`tikv-ctl` 直连 UUSE 数据目录（一期边界）；
   - 新增 §21 TiDB/TiKV frontend 契约：RocksDB Env（C++ `FileSystem` + crocksdb FFI）与
     raft-engine `FileSystem` seam、路径路由与宿主白名单、handle 与线程 registry、
     停机顺序、失败关闭、`punch_hole` 语义；
   - §16 增补 TiKV 验收（功能矩阵、崩溃断点、无宿主落盘 strace 证据）；
   - §17 增补风险（Rust FFI、双实例、线程数、信号、punch hole 空间放大）；
   - §18 增补 Phase 8。
2. **基座冻结**：TiKV 8.x LTS（当前建议 8.5；Gate 0 冻结 tag/commit 及 Cargo.lock
   锁定的 rust-rocksdb 依赖版本）在 VM 内通过 Git clone 获取并维护，不从 Windows 同步
   TiKV/Rust 源码；补丁以 `tikv/patches/` 独立管理，禁止散落修改。
3. **配置隔离**：TiKV 使用独立 `config/uuse_tidb.conf`；`config/uuse_nvme.conf`、
   `config/uuse_mysql.conf` 与 FIO/RocksDB/MySQL 正式口径保持逐字节不变。
4. **基线固化**：在 UUSE core 零改动状态下记录当前基线（CTest 全量、FIO smoke、
   `uuse_db_bench` readrandom 同会话交错 A/B、RocksDB 正式同形状 gate、MySQL 冒烟），
   作为后续回归对照；数字与证据路径写入 Shard-113 报告。

### 0.2 每个 shard 的完成定义

1. `make` 与 CMake 均可构建；UUSE core 不引用 TiKV/Rust 源；TiKV fork 只依赖
   `tikv/shim` 暴露的 C ABI（shim 由 UUSE 侧构建，依赖 `include/uuse`）；
2. 新增路径有单测或结构测试，生命周期与资源守恒测试通过；
3. VM 真实 NVMe 按 `.conf` 短回归通过；BDF 以 sysfs 拓扑为准，不从内核 `nvmeXn1`
   编号推断；
4. `spdk_submit == spdk_complete`、errors=0、无 frame/lease/ring/handle 泄漏；
5. 新增 tracepoint/计数可诊断，trace/stats off 无 per-request 分配；
6. 结构或恢复门禁失败时不得用性能数字掩盖，按 `uuse.md` §17.3 停止。

### 0.3 RocksDB/MySQL 无回退门禁（本计划的核心验收约束）

UUSE 是单进程、单 engine、bdev 独占；TiKV 与 RocksDB/MySQL 不会同进程/同盘共跑，
因此**适配本身不应影响既有 frontend 的运行时性能**。每个 shard 出口必须执行：

| 级别 | 项目 | 通过标准 | 触发条件 |
|---|---|---|---|
| L1 | CTest 全量 | 全量通过（基线数在 Shard-113 固化，随 shard 增长） | 每个 shard |
| L2 | FIO 3.42 smoke（4K randrw/verify） | errors=0、closure 闭合 | 每个 shard |
| L3 | `uuse_db_bench` readrandom 同会话交错 A/B（stats=false） | 偏差 <5% | 每个 shard |
| L4 | RocksDB fillseq/readrandom/overwrite/readwhilewriting + FIO 正式同形状 gate（1j32/8j4）；触及 core 时加 MySQL 同盘 A/B 冒烟 | 无回退（<5%）、closure 闭合 | 触及 core 的 shard |

隔离原则：

1. **前端加法**：TiKV 接入只新增 `tikv/`、shim 与 core 新 API，不改 RocksDB/FIO/MySQL
   adapter 行为；
2. **core 改动门控**：任何 core 行为改动必须配置键门控、可提交回退，并单独成 sub-shard；
3. **默认值冻结**：不改 `config/uuse_nvme.conf`/`uuse_mysql.conf`，不改 `ring_depth/
   client_depth/arena/reactor` 等既有默认值；
4. **ABI 加法优先**：只做加法；破坏性变更升 `coreAbiMajor` 并单独评审；
5. **禁止共跑**：不新增同进程多 engine、跨进程共享 bdev 或同盘双挂载路径。

高风险共享改动清单（必须 L4 门禁）：

| 改动 | 风险 | 约束 |
|---|---|---|
| core 新增 namespace/File API（`set_times`、目录同步等） | 波及 metadata/durability | 纯加法、新单测、L4 |
| per-thread client registry 复用/调整 | worker-direct 路径回归 | 只加法；RocksDB/MySQL worker-direct 回归 |
| `File::sync` 语义 | durability/epoch 模型 | 路径字节级不变；RocksDB sync=1 + TiKV WAL 回归 |
| `punch_hole`/目录 fsync | 空间回收/崩溃恢复 | v1 不做实现；只做 fail-closed 或显式降级，L4 |
| 配置 parser 新键 | 直接改变基准口径 | `uuse_tidb.conf` 独立，新键必须可选 |

### 0.4 环境与口径（结合现有实验室）

现有 5 VM 拓扑（`E:\Code\workspace.yaml`，2026-10-09）：

| VM | 规格 | 现状 | Phase 8 用途 |
|---|---|---|---|
| .250 host1 | 6 vCPU / 7.2 GiB / 3×20 GiB NVMe | 3FS storage node 10002（data1 用 NVME_0000；hugepages 当前为 0） | 不占用；二期 3 副本候选（需协调并恢复 hugepages=1024） |
| .251 host2 | 6 vCPU / 7.2 GiB / 3×20 GiB NVMe | 3FS storage node 10001 + UUSE 目标盘 | 不占用 |
| .252 host3 | 4 vCPU / 7.2 GiB（2026-10-09 实测；workspace.yaml 旧值 2 vCPU/3.3 GiB） | 3FS FUSE client；3 块数据盘空闲 | 备选 TiKV 节点 / 二期候选 |
| .253 host4 | 4 vCPU / 7.2 GiB（2026-10-09 实测；workspace.yaml 旧值 2 vCPU/3.3 GiB） | 3FS FUSE client；3 块数据盘空闲 | **单节点 TiKV+UUSE 首选**（NVME_0002 @ `0000:13:00.0`） |
| .254 host5 | 4 vCPU / 3.3 GiB / 3×20 GiB NVMe | 3FS manager（FDB/mgmtd/meta） | **PD + TiDB server** |

部署口径：

- 单节点 v1：PD + TiDB server 在 .254；TiKV + UUSE 在 .253，独占
  `0000:13:00.0`（kernel `nvme2n1`，以 sysfs 核对）；不占用 3FS 实验室已声明盘
  （.251 全部、.250 的 data1 盘），不与 3FS client 性能口径重叠运行；
- 资源预算：UUSE 需要 `vm.nr_hugepages=1024`（2 GiB）；.253 实测 4 vCPU/7.2 GiB，
  TiKV block cache/memtable 按 VM 剩余内存预算在 Shard-113 固化（无需按 3.3 GiB 口径
  裁剪）；若资源不足再克隆 host2 规格（6 vCPU/7.2 GiB）VM，并在报告中记录；
- 正式口径：真实 NVMe + `.conf`、trace/stats off、同会话交错 A/B（UUSE vs host ext4，
  同 VM 同盘）、5-run median、结果与证据写 VM `/home/uuse`；跨会话数字不得直接比较。

---

## 1. Shard 总览

| Shard | Phase | 内容 | 规模 | 依赖 | 出口门禁 |
|---|---|---|---|---|---|
| 113 | 8 | 设计更新、TiKV 基座、构建与基线固化 | M | - | 设计更新 + `tikv-server` 可编译 + 基线记录 |
| 114 | 8 | TiKV 存储绕过点审计与路由清单 | M | 113 | 审计报告 + 每处路由决定 + 禁用项清单 |
| 115 | 8 | UUSE 侧 API 补齐（纯加法，审计驱动） | S | 114 | 新增单测 + L1–L3 |
| 116 | 8 | C ABI shim + raft-engine `UuseFileSystem`（基础接口闭环） | L | 115 | FileSystem 单测 + smoke + 无宿主落盘 + L1–L3 |
| 117 | 8 | RocksDB Env + raft-engine 写/恢复路径 | L | 116 | 写闭环 + kill -9 恢复 + `uusectl check` + L1–L3 |
| 118 | 8 | 线程/生命周期（registry、信号、停机顺序） | M | 116 | 并发负载 + clean shutdown + 第二实例 Busy + L1–L3 |
| 119 | 8 | 功能验收矩阵（SQL/DDL/事务/备份/跨进程） | M | 117/118 | 矩阵全过 + `uusectl check` + L1–L3 |
| 120 | 8 | 性能与 durability（TiKV on UUSE vs host ext4） | M | 119 | 性能目标 + L4（RocksDB/MySQL 无回退） |
| 121 | 8 | 收口（支持边界、禁用项、归档） | S | 120 | 全回归 + 文档收口 |

---

## 2. Phase 8：TiDB/TiKV frontend

接入定位：TiDB 的持久化层是 TiKV（Rust）；v8.5.8 的本地存储是 kv RocksDB + 默认
raft-engine。Shard-114 审计（`doc/review/uuse_tikv_storage_audit_20261009.md`）确认
接入面为两处：（a）RocksDB 侧在 C++ 层实现 `rocksdb::FileSystem`，经 crocksdb FFI
构造 `Env` 并注入 `engine_rocks` 共享 env（rust-rocksdb 无通用 Rust Env trait，不存在
可直接实现的 `RocksDBEnv`）；（b）raft-engine 侧实现 Rust `FileSystem` trait 并替换
`ManagedFileSystem` 的 base FS。两者都经 `tikv/shim` C ABI 调 UUSE core，不是
FUSE/POSIX 层，也不逐文件改写 TiKV。

### Shard-113：设计更新、TiKV 基座与基线固化

> 状态：基座与基线主体完成（2026-10-09）。设计更新（`uuse.md`
> §3.1/§3.2/§16.6/§17/§18/§19/§21）已提交；TiKV v8.5.8 在 host4(.253) 构建通过
> （`tikv-server` 218 MiB，`--version` 输出 8.5.8 Community）；UUSE 在 host4 构建并
> 固化基线：CTest 29/29、FIO smoke（读 53476/写 53629 IOPS、errors=0、closure
> 闭合）、RocksDB native-vs-UUSE A/B（s32/s64）；证据见
> `doc/performance/uuse_tidb_baseline_20261009.md`。构建兼容修复：CMake 4 policy、
> GCC 15→gcc-12、io_uring `-luring`、`JOBS=2` + swap + 构建期释放 hugepages。
> FIO 正式同形状 gate 已通过（M 1j32：UUSE/raw 95.5%、p99 0.92；E 8j4：86.0%、
> p99 0.20；errors=0）。待补：MySQL 冒烟（host4 现有 fork 为 HFS 时代构建
> `hfs::`/`libhfs_core.so`，需按当前补丁重建）、资源预算回填。

**目标**：冻结范围、基座与对照基线，避免后续用 TiKV 收益掩盖 RocksDB/MySQL 回退。

**交付物**

- `uuse.md` §3.1/§3.2/§16/§17/§18 更新与新增 §21 frontend 契约章节（§0.1）；
- TiKV fork（VM 内 Git，tag/commit 与 Cargo.lock 锁定的 rust-rocksdb 依赖冻结；
  路径 `/home/uuse/tikv-fork` + `/home/uuse/tikv-build`）与可重复构建脚本
  `tikv/build_tikv_vm.sh`，产出 `tikv-server`（`tikv-ctl` 边界同期确定）；
- PD/TiDB server 版本冻结与部署基线（与 TiKV 同系列版本；二进制与启动参数记录在
  `tikv/`）；
- `config/uuse_tidb.conf`（namespace 前缀、device set、路径路由、内存预算；与
  `uuse_nvme.conf`/`uuse_mysql.conf` 隔离）；
- 基线快照（CTest、FIO smoke、`uuse_db_bench` readrandom A/B、FIO 正式同形状 gate、
  MySQL 冒烟）；
- TiKV 资源预算与部署脚本骨架 `tikv/run_*_vm.sh`。

**验收**：设计更新已提交；TiKV fork 编译通过；UUSE core 零改动；基线数字与证据路径
记录在案。

**不做**：任何 shim/API/TiKV 源改动。

### Shard-114：TiKV 存储绕过点审计与路由清单

> 状态：已完成（2026-10-09）。审计报告
> `doc/review/uuse_tikv_storage_audit_20261009.md`：真实 seam 为 RocksDB C++
> `FileSystem` + crocksdb FFI `Env`（非 Rust trait）与 raft-engine `FileSystem`
> trait；列出 10 处非引擎直用点与路由决定；设计 §21.1/§21.2 已按审计修订。

**目标**：冻结 seam 与路径路由，输出可执行的绕过点清单，避免 datadir 文件静默落宿主。

**交付物**：`doc/review/uuse_tikv_storage_audit_YYYYMMDD.md`，覆盖：

- `components/file_system`：`FileSystem` trait 全方法与 `EngineFileSystem` 工厂选择点；
- `engine_rocks`：`RocksDBEnv`/`Env` 接线点、kvdb/raftdb 创建、WAL/MANIFEST/CURRENT/
  atomic flush/rename/link/dir fsync、`LOCK` 文件、fallocate/punch hole/mmap/direct IO
  选项；
- `components/server`/`components/raftstore`/`components/sst_importer`/`components/backup`/
  `cmd/tikv-ctl`：`data-dir`、`snap`、`import`、encryption 元数据目录（若启用）、`lock`、
  日志/status 的 `std::fs`/`std::io` 直用点；
- 每处路由决定：UUSE namespace / 宿主白名单（log/status/socket/pid）/ fail-closed；
- 禁用项清单与探针（encryption、cloud、mmap、punch hole、`tikv-ctl` 一期边界）。

**验收**：审计覆盖 100% 直用点；无"未决路由"；设计 §21 与之同步。

**不做**：任何代码改动。

### Shard-115：UUSE 侧 API 补齐（纯加法）

> 状态：已完成（2026-10-09，映射结论，无 core 改动）。见审计 §9：raft-engine
> `FileSystem` 全部方法可由现有 `Client/File` API 覆盖；RocksDB 侧复用现有
> `rocksdb/uuse_filesystem.cpp` adapter（52 个 override，LockFile 宿主锁 +
> flock、punch_hole 默认 fail-closed）；RocksDB 8.10.2 头文件适配归 Shard-116
> spike。若 spike 发现缺口，按 §0.3 纯加法单独成 sub-shard。

**目标**：按审计补齐 `FileSystem` 实现所需的最小依赖，不修改现有语义。

**交付物**（预计，以 Shard-114 为准）：现有 `Client::open/remove/rename/link/
getChildren/stat/createDirectories` 与 `File::read/write/append/allocate/truncate/
datasync/sync/stat/allocatedBytes` 已覆盖大部分；预计补齐 `set_times`（mtime/ctime）、
目录同步语义（映射到 metadata barrier）、`punch_hole` 显式 fail-closed 声明、`copy`
与递归 `remove_dir_all`（adapter 内实现）。

**验收**：新增单测覆盖缺失路径、类型、幂等、错误映射；CTest 全量；L1–L3 无回退。

**不做**：data-only `datasync`、punch hole 实现、任何已有 API 语义变更。

### Shard-116：C ABI shim + raft-engine `UuseFileSystem`（基础接口闭环）

> 状态：进行中（2026-10-09）。RocksDB 侧**真机通过**：`tikv/spike/` 的
> `apply-uuse-filesystem.py` 把真实 UUSE adapter 编进 `librocksdb_sys`，crocksdb
> `crocksdb_uuse_env_create` + Rust `Env::uuse`；`engine_rocks` 示例在 uio 绑定的
> NVMe 上 format 后打开 DB、put/get/flush 成功（`uuse env ok`，I/O 经 UUSE，
> checkpoint 落盘，closure 闭合）。C ABI shim（`tikv/shim/`，CMake 目标
> `uuse_tikv_shim` + smoke）真机 `SHIM_SMOKE_PASS`：engine mount、create/write/
> append/pread/truncate/sync/exists/rename/remove 全部通过。raft-engine Rust
> `UuseFileSystem`（`tikv/shim/rust/`）真机 `RAFT_ENGINE_SMOKE_PASS written=47
> fetched=2`：`Engine::open_with_file_system` + `write(sync)` + `fetch_entries_to`
> 全走 UUSE。生产 wrapper 改造（`ManagedFileSystem` enum base + `RaftLogEngine::
> new_with_uuse`，见 `tikv/shim/rust/engine.rs`）**真机通过**：
> `RAFT_ENGINE_PROD_SMOKE_PASS written=47 fetched=2`（`RaftLogEngine::new_with_uuse`
> + `ManagedFileSystem`）。同轮修复两个生产阻塞：
> （a）raft-engine `std::fs::read_dir` 绕过——trait 增加 `read_dir`/`create_dir`
> （默认 fail-closed），UUSE FS 经 `getChildren`/`createDirectories` 实现，
> `ManagedFileSystem` 转发（补丁 `tikv/patches/apply-raft-engine-readdir.py`）；
> （b）析构顺序死锁——shim 引擎状态改为引用计数（文件句柄持引用，最后一个关闭才
> 停引擎），修复 `Namespace::close` futex 挂起。**配置门控完成**：TiKV
> `[raft-engine] uuse_conf`（默认空）+ 两处生产调用点分支到 `new_with_uuse`；
> `cargo check -p tikv` rc=0，生产 smoke 复测通过。**RocksDB 侧门控完成**：
> `[storage] uuse_conf` + `engine_rocks::get_env_with_uuse`（kvdb/raftdb 共享 env），
> `cargo check -p tikv` rc=0。其余待做：非引擎直用点（LOCK/panic mark/占位文件）
> 路由（Shard-117）、真实 `tikv-server` bring-up 与 kill -9 恢复、`swappy_allocator`
> spill 目录一期不启用、正式 fork/补丁栈。

**目标**：建立最小可用闭环：`tikv/shim` 在 UUSE namespace 上完成
create/open/read/write/rename/delete/目录等基础操作；raft-engine 经
`UuseFileSystem` 使用 UUSE；RocksDB 侧完成 crocksdb FFI `Env(fs)` 注入 spike。

**交付物**

- UUSE 侧 `tikv/shim/`：`libuuse_tikv_shim`（C ABI，opaque engine/client/file/dir
  handle、Status/errno 映射、线程 registry 骨架、版本与 ABI 检查）；
- raft-engine 侧 Rust `UuseFileSystem`（`FileSystem` trait + `Handle` + `WriteExt`，
  `build.rs` 链接 shim）与 `ManagedFileSystem` base 的配置选择 patch；
- RocksDB 侧 spike：crocksdb FFI `Env(fs)` 构造 + C++ `FileSystem` 最小实现
  （create/open/read/write/sync/rename/delete/目录），验证注入链路；
- 路径路由：`storage.data-dir` → UUSE namespace；日志/status/socket/pid → 宿主白名单；
- 无宿主落盘校验脚本（strace 证据）。

**验收**：raft-engine `FileSystem` 单测与独立 smoke（经 shim 的 create/write/read/
rename/delete/dirs）通过；RocksDB spike 能经 UUSE Env 打开/关闭引擎目录（不要求完整
启动）；`data_dir` 内文件不落宿主；L1–L3 无回退。

**不做**：写/恢复结论、性能结论、`tikv-ctl` 支持。

### Shard-117：RocksDB Env + raft-engine 写/恢复路径

> 状态：核心链路通过（2026-10-09/10）。**共享引擎**（kvdb + raft-engine 同开 UUSE）
> + 宿主校验跳过 + `SyncWAL` 修复后，单节点全链路真机通过：
> PD+TiKV+TiDB 启动（tikv 2s / tidb 4s）→ SQL 写入 `n=3` → kill -9 后 2s 重启 →
> 数据持久（`alpha,beta,gamma`）→ 宿主 `db/`、`raft-engine/` 目录无引擎数据 →
> `uusectl check` checkpoint 有效（txid 76/77）、`corruption=none`。
> 补丁：`apply-uuse-shared-engine.py`、`apply-uuse-validation-skip.py`；脚本
> `tikv/run_single_node_vm.sh`。审计第 12/13/14 条均已修复。
> **非引擎直用点收尾（2026-10-10）**：`uuse-conf` 时跳过宿主 `data_dir/LOCK` 与
> reserve 占位（补丁 `apply-uuse-nonengine-closeout.py`）；RocksDB/raftdb
> `info-log-dir` 显式落宿主日志目录；自包含脚本复测通过（`uuse engines: 1`、
> SQL/kill -9/`corruption=none`），宿主 `tikv-data` 仅剩 `last_tikv.toml` +
> raft-engine `LOCK`/挂起探针（审计第 15 条白名单）。
> 其余待做：`swappy_allocator` spill 目录一期不启用、功能矩阵（Shard-119）、
> 性能与 durability（Shard-120）、正式 fork/补丁栈。

**目标**：让 RocksDB 与 raft-engine 的写、flush、compaction、恢复全部经 UUSE 闭环。

**交付物**

- RocksDB Env 接线：kv 与 raftdb 共享 UUSE env；C++ `FileSystem` 覆盖 direct IO/
  RangeSync/目录 fsync/LockFile；
- raft-engine `UuseFileSystem` 写/恢复闭环（`Handle.sync`、`WriteExt.allocate`）；
- WAL/MANIFEST/CURRENT/atomic flush/rename/link/dir fsync 路径打通；
- `data_dir/LOCK`、panic mark、占位文件与 UUSE 进程锁语义：第二实例 fail-closed；
- `fallocate`（`File::allocate`）与 direct IO 对齐；`punch_hole` 按 114 结论处理；
- 按 114 清单修复绕过点；kill -9 断点恢复脚本。

**验收**：最小 PD+TiKV 启动下完成 TiKV 写负载并重启持久；kill -9 后 TiKV 可恢复且
无已提交写丢失；`uusectl check` `corruption=none`；`data_dir` 无宿主落盘；L1–L3
无回退。

**不做**：并发扩展与性能调优（Shard-118/120）。

### Shard-118：线程与生命周期

> 状态：进行中（2026-10-10）。已核实/完成：
> （a）**信号**：TiKV 自身用 `signal_hook` 注册进程级 handler（专用 signal 线程），
> 实测 SIGTERM 走 `signal_handler.rs` graceful shutdown（矩阵日志 `receive signal
> 15, starting graceful shutdown...`），SPDK 线程空掩码不影响，无需额外转发；
> （b）**生命周期缺陷（已修）**：core 要求 `Client::close` 在创建线程（frontend
> thread）调用；shim 共享 client 原先由首个调用线程创建、engine state 可能由任意
> 线程析构 → close 被拒（`client destroyed without close`）且元数据未收尾。修复：
> engine state 自带专用 client 线程，client 创建/关闭都在该线程，路径操作仍由调用
> 线程经 mutex 串行（core 允许），文件数据操作经 `File` 在调用线程自驱动；
> （c）**元数据持久化语义**：namespace create/remove 随 `File::sync` 的 durable
> epoch 跨挂载可见（用例按此实现）；
> （d）新增 `uuse_tikv_shim_lifecycle` 6 用例（engine 先关+文件后关、线程退出持文件、
> 8 次重复建链、共享句柄、fork 子进程 -ECHILD、参数防护），真机
> `SHIM_LIFECYCLE_PASS`，ASAN 干净；TSAN 复跑（`tikv/shim/tsan.supp` 仅压制 SPDK
> 未插桩的控制路径边界帧）**0 警告**；
> （e）TSAN 抓到并修复 core `ParentLatch` 锁外 notify（栈 condvar UAF 窗口，
> bug.md #48）；设备测试补 `RESOURCE_LOCK uuse_device`（bug.md #49），
> `ctest -j2` 29/29 通过；
> （f）并发负载 closure 复现「SIGTERM 后重启报 raft 日志缺条目」：根因三层——
> UUSE 无内核页缓存且 TiKV 正常写路径异步 fsync（raft 日志 `sync=false`、kvdb apply
> `need_sync=false`）；raft-engine `Engine::sync()` 空 batch 空操作；engines 被
> `take()` 进信号线程使 `stop()` 无法访问。修复补丁栈：`apply-raft-engine-sync-fix.py`
> （`Engine::sync` 直连 `pipe_log.sync`）、`apply-uuse-shutdown-sync.py`（保留 engines +
> 关闭 sync 双引擎）、`apply-uuse-apply-sync.py`（apply 批次强制同步）。宿主 ext4
> 同负载对照通过，确认差异在 UUSE 持久化语义。
> 出口结果（2026-10-10）：closure `PASS=5 FAIL=0`（187,740 行 → SIGTERM → 重启行数完整
> → `uusectl check corruption=none`）；矩阵回归 26/26；`uuse_tikv_shim_lifecycle`
> 6/6（ASAN 干净、TSAN 0 警告）；`ctest -j2` 29/29。边界：kill -9 仍可能丢未 sync
> 尾部（与 ext4 掉电语义对齐）；apply 同步吞吐代价由 Shard-120 量化。

**目标**：让 TiKV 后台线程（raftstore/apply/read pool/backup/import/compaction）在 UUSE
上有确定的生命周期。

**交付物**

- per-thread `uuse::Client` registry（惰性创建、weak 注册、线程退出自关、上限回落共享
  并计数；复用 MySQL Shard-106/108 的 worker-direct 机制）；
- 进程级 fileOwnerClient（所有打开的 `File` 由同一 client 持有）；
- 信号转发：在 TiKV 信号处理注册后安装（SPDK 线程空信号掩码问题，MySQL Shard-103
  先例），SIGTERM/SIGINT 转发；
- 停机顺序：停新提交 → drain → close clients → 关 handle → engine stop，任一层失败
  fail-closed；fork/`Busy` 语义；
- ASAN/TSAN 生命周期用例（线程退出时仍有 outstanding、engine stop 时仍有 client、
  反复建链/销毁、建链失败回落）。

**验收**：多线程读写负载 closure 闭合、无 UAF；反复启停与 SIGTERM/SIGKILL 正常；
第二实例 `Busy`；L1–L3 无回退。

### Shard-119：功能验收矩阵

> 状态：**功能矩阵全过（2026-10-10）**：`PASS=26 FAIL=0`（结果目录
> `/build/tidb/functional-matrix-20261010-062537`，含 matrix.log/dump/uusectl
> check 证据）；矩阵入口固化于 `tikv/run_functional_matrix_vm.sh`。
> 第一轮矩阵发现并修复：
> （a）`ALTER TABLE ... ADD INDEX` 触发 SST importer 在 `import/` 下建文件 →
> namespace 父目录不存在 → `NotFound` panic；adapter `Create` 打开时自动创建
> namespace 父目录链（审计第 6 条部分修复）；
> （b）fast-reorg ingest 路径未闭环（importer 宿主 FS 写 SST + engine ingest 找不到
> → `IngestSST Io(NotFound)` 重试循环）；矩阵以 `tidb_ddl_enable_fast_reorg=OFF`
> 走经典回填并记录该边界（审计 §8 待决）。
> 矩阵覆盖：CRUD、DDL（add/drop column/index）、乐观/悲观事务与重复键拒绝、
> ADMIN CHECK、region split、GC safe point、kill -9/SIGTERM 重启持久、跨进程
> reopen、dumpling 备份恢复、第二实例/encryption fail-closed、`uusectl check
> corruption=none`。

**目标**：以用户可见语义收口功能正确性。

**交付物**

- 单节点部署脚本（PD + TiDB + TiKV）与功能矩阵：SQL CRUD、索引、DDL（含 ADD
  COLUMN/INDEX）、乐观/悲观事务、`ADMIN CHECK`、region split 与 GC；
- 重启持久、kill -9 恢复、跨进程顺序 reopen；
- 备份/恢复 smoke（BR/dumpling/lightning；输出落宿主或对象存储的边界明确）；
- 禁用项 fail-closed 探针（encryption/cloud/mmap/第二实例/`tikv-ctl`）；
- `uusectl check` 证据归档。

**验收**：矩阵全过；禁用项明确拒绝或记录降级；`uusectl check` 通过；L1–L3 无回退。

**不做**：性能结论（Shard-120）、3 副本（二期）。

### Shard-120：性能与 durability

> 状态：**完成（2026-10-10，混合 p99 记录边界）**。结果
> `doc/performance/uuse_tidb_shard120_20261010.md`，证据
> `/build/tidb/perf-ab-target-20261010-151716`、`/build/uuse-shard120-*`。
> （a）同盘 TiKV A/B（同一目标 NVMe、8 线程 × 60 s、5-run median、交错）：
> 点读 tps **97.9%**（≥95% 通过）、p99 101.8%；混合 tps **89.5%**（≥85% 通过）、
> **p99 117.6%（>110%，记录边界）**。边界根因：Shard-118 的 apply 强制同步
> （UUSE 无页缓存，撤销即数据丢失路径）；ext4 侧 apply 异步不受影响。可选收敛：
> group commit/atomic flush 优化 apply-state barrier（独立 core sub-shard，未做）。
> （b）L1 CTest 29/29；L2 FIO smoke 读 50238/写 50346、errors=0、closure 闭合；
> L4 FIO gate M 93.5%（p99 0.96）、E 89.9%（p99 0.20）、errors=0。
> （c）RocksDB native-vs-UUSE A/B（640k keys、32/64 线程、同盘交错）：写 96.1–107.7%、
> 混合读写 95.7–112.6%，无 >5% 回退；无 error/corruption。
> （d）group commit 评估：core 既有 `SyncBatch` + `max_group_commit_wait_us=200`
> 合并窗口；Shard-118 未改 `File::sync`/`syncMetadata` 路径（字节级不变）。
> 未做：MySQL 同盘冒烟（fork 未按补丁栈重建，Shard-121 处理）；apply 同步的吞吐
> 收敛优化（group commit sub-shard）。

**目标**：验证 TiKV 性能可接受，同时证明 RocksDB/MySQL 无回退。

**交付物**

- 同会话交错 A/B（TiKV on UUSE vs TiKV on host ext4，同 VM 同盘）：sysbench
  `oltp_point_select`/`oltp_read_write`、go-tpc tpcc 或 ycsb、TiKV 内置 metrics；
- p50/p99、设备 data I/O、CPU map、WAL/commit 延迟归因；
- group commit/atomic flush 评估（**独立 core sub-shard**）：实现必须保证
  `File::sync` 路径字节级不变，且通过 L4；
- RocksDB 全量回归（L1–L4）+ MySQL 冒烟。

**验收**：点读 ≥95% host、混合读写 ≥85%、p99 ≤110%（同会话 5-run median）；
RocksDB/MySQL 既有 gate 偏差 <5%；closure 闭合。未达目标必须记录边界，不得以
RocksDB/MySQL 回退换取 TiKV 收益。

**停止条件**：core 改动导致 RocksDB/MySQL 回退 >5% → 立即回退该 core 改动，TiKV
维持现有 barrier。

### Shard-121：收口

> 状态：**完成（2026-10-10；MySQL 冒烟记录边界）**。
> （a）支持边界文档：`tikv/README.md`（状态/架构/版本冻结/构建运行/配置模板/
> 支持范围/禁用项/已知边界/补丁栈/证据）；
> （b）回填：`doc/design/uuse.md` §21.6 实现状态、审计
> `doc/review/uuse_tikv_storage_audit_20261009.md` §10 逐条最终状态；
> （c）补丁清单与 rebase 说明：`tikv/patches/README.md`（9 个补丁、幂等/回退/
> 版本升级锚点说明）；
> （d）证据归档：`/home/uuse/uuse-tikv-20261010/tikv-shard120-evidence.tar.gz`
> （1.7 MB：性能 A/B、closure、功能矩阵、RocksDB A/B、FIO gate、L1/L2）；
> （e）最终回归：功能矩阵 26/26（收口复跑）+ Shard-118/120 的 closure/生命周期/
> CTest/RocksDB/FIO 全绿；无 preload/FUSE/Blobstore 回流（架构为显式 seam，
> 见 `tikv/README.md` §2）。
> **边界**：MySQL 同盘冒烟未执行（fork 停留 HFS 时代、根盘 2.1 GiB、全量重建需
> 数小时）——按 `mysql/build_mysql_vm.sh` 独立会话执行；其余待办见审计 §10。

**交付物**

- TiKV 支持边界文档（版本、禁用项、参数模板、`tikv-ctl`/备份边界、已知边界）；
- `uuse.md` §21 状态回填与 `doc/review/uuse_tikv_storage_audit_YYYYMMDD.md` 实现结果
  回填；
- 性能基线与证据归档（`/home/uuse`）、补丁清单与 rebase 说明。

**验收**：全回归（TiKV + MySQL + RocksDB/FIO）通过；支持边界明确；无
preload/FUSE/Blobstore 回流；无未记录临时边界。

---

## 3. 风险与停止条件

| 风险 | 影响 | 缓解 |
|---|---|---|
| Rust FFI 生命周期/UB | 崩溃/UAF | 显式 C ABI + opaque handle + 版本检查；ASAN/TSAN 用例 |
| 绕过点遗漏 | `data_dir` 文件落宿主/失败 | Shard-114 审计清单 + 每 shard strace 校验 |
| kvdb/raftdb 双实例 + 高线程数 | registry/资源超限 | per-thread registry 上限 + 共享回落 + 计数（复用 Shard-106/108） |
| `File::sync` 无 data-only barrier | WAL per-commit 延迟 | 组提交先行；core 改动必须过 L4 |
| punch hole 不支持 | 空间放大或 compaction 失败 | 按 114 结论 fail-closed 或显式降级并记录；禁止静默 no-op |
| SPDK 线程空信号掩码 | SIGTERM 默认动作杀死 TiKV、跳过停机 | Shard-118 信号转发（MySQL Shard-103 先例） |
| rust-rocksdb fork patch（crocksdb FFI/Rust Env） | 上游 rebase 成本 | 改动限定新增 FFI 构造 + 独立 patch 目录；版本冻结 |
| RocksDB C++ FileSystem 方法面（direct IO/RangeSync/目录 fsync/LockFile） | 功能缺口 | 按审计 §2.2 逐项实现；punch hole 一期 fail-closed |
| `data_dir/LOCK`/panic mark/占位文件直用点 | 宿主落盘/双挂载 | 审计 §4 清单逐项路由；strace 校验 |
| 上游 rebase 冲突 | 维护成本 | 改动限定 FS 工厂 + shim + 独立 patch 目录；版本冻结 |
| 资源不足（4 vCPU/7.2 GiB 实测） | OOM/不稳定 | Shard-113 资源预算；必要时克隆 6 vCPU VM |
| 与 3FS 实验室冲突 | 误占盘/性能污染 | §0.4 盘与时段约束；不占用已声明盘 |
| Rust/TiKV 构建依赖（protoc/crates 镜像） | 基座无法构建 | Gate 0 固化 toolchain 与镜像；构建在 VM 内完成 |
| 环境漂移 | 错误性能结论 | 同会话交错 A/B、native 首尾校准 |

停止条件：`uuse.md` §17.3 全部条目；追加：TiKV 路径 data mismatch、崩溃恢复失败
（store 无法启动或丢失已提交写）、`data_dir` 文件落到宿主、RocksDB/MySQL L4 回退
>5%、禁用项静默降级。

---

## 4. 证据与文档产物

- 审计：`doc/review/uuse_tikv_storage_audit_YYYYMMDD.md`（Shard-114）；
- 设计：`doc/design/uuse.md`（§3/§16/§17/§18/§21）；
- 每个 shard 的结果报告：`doc/task_result/`（按 AGENTS.md 任务规则）或
  `doc/performance/`、`doc/review/` 归档；
- 大体积证据：VM `/home/uuse`；
- 基线对照：Shard-113 固化的 CTest/FIO/RocksDB/MySQL 快照；
- 集成产物：`tikv/`（`patches/`、`build_tikv_vm.sh`、`run_*_vm.sh`、`shim/`）。

---

## 5. 与主计划的关系

`doc/shard/hfs2.0_shard_development_plan.md` 的 Phase 0-6（Shard-68~85）及其"不包含
MySQL/InnoDB"约束继续有效；`doc/shard/hfs2.0_mysql_innodb_shard_plan.md` 的 Phase 7
（Shard-99~112）结论继续有效。本计划是 Phase 7 之后的 Phase 8 扩展，入口条件为
§0.1 的设计更新完成；三计划共用同一事实源、完成定义与停止条件。TiKV 与 MySQL 的
接入模式一致：不改 core 架构，只在前端 seam 处注入 UUSE，并以 RocksDB/MySQL 无回退
门禁约束所有 core 改动。

---

## 6. 二期（可选，不在 Phase 8 出口内）

- 3-TiKV 集群：3 台 VM 各一个 TiKV+UUSE（候选 .253、.252、.250（需协调 3FS 实验室）
  或克隆 VM），PD 三副本/单 PD，验证 RF3 raft、PD 调度、单节点故障切换与恢复；
- TiFlash/CDC/对象存储不作为 UUSE 适配目标；
- 入口条件：Shard-121 收口完成，且单节点性能/durability 目标达标。
