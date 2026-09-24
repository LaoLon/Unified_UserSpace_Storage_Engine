# HFS 2.0 MySQL/InnoDB frontend Shard 开发计划（Shard-99 起）

> 日期：2026-09-24
>
> 状态：执行中；Shard-99/100/101/101B/102 已完成（2026-09-24），进入 Shard-103
>
> 事实源：`doc/design/hfs2.0.md`（Phase 7 范围、契约与禁用项以更新后的设计为准）
>
> 审计依据：`doc/review/hfs2_mysql_innodb_audit_20260924.md`（os_file seam、7 处绕过点）
>
> 全局规则沿用：`doc/shard/hfs2.0_shard_development_plan.md` §0
>
> 编号：接 repair-98 之后，从 Shard-99 开始；本文定义 Phase 7（MySQL/InnoDB frontend）
>
> 不包含：MyRocks 路线（独立决策）、native AIO bridge（二期）、punch hole/页压缩、
> clone、MyISAM/Aria、POSIX preload/FUSE（设计永久排除）

---

## 0. 全局规则

### 0.1 前置条件（Gate 0，未完成不得动代码）

1. **设计先行**：更新 `doc/design/hfs2.0.md`
   - §3.1 支持列表加入 MySQL/InnoDB frontend；
   - §3.2 将 `MySQL/MariaDB/InnoDB native backend：v1 不做`（`:119`）改为"Phase 7 经
     os_file seam 实现"，并明确禁用项（native AIO、punch hole/页压缩、clone、
     MyISAM/Aria、`--daemonize`、`--innodb-directories` 指向宿主）；
   - 新增 §20 MySQL/InnoDB frontend 契约章节：handle 表、路径路由、同步门面、线程
     registry、停机顺序、回落/失败关闭；
   - §17 增补风险：`File::sync` 无 data-only barrier、`struct stat` 合成、上游 rebase。
2. **基座冻结**：MySQL 8.0.46（审计版本）在 VM 内通过 Git 获取并维护，不从 Windows
   同步 MySQL 源码；补丁以独立 patch 目录/分支管理，禁止散落修改。
3. **配置隔离**：MySQL 使用独立 `.conf`（如 `config/hfs_mysql.conf`），
   `config/hfs_nvme.conf` 与 RocksDB/FIO 正式口径保持逐字节不变。
4. **基线固化**：在 HFS core 零改动状态下，先记录当前 RocksDB/FIO 正式基线
   （CTest、FIO smoke、readrandom 同会话交错 A/B、正式 gate），作为后续回归对照。

### 0.2 每个 shard 的完成定义

1. `make` 与 CMake 均可构建；HFS core 不引用 MySQL 源，MySQL fork 不反向依赖 HFS 内部头；
2. 新增路径有单测或结构测试，生命周期与资源守恒测试通过；
3. VM 真实 NVMe（`Nvme0n1`、BDF `0000:0b:00.0`）按 `.conf` 短回归通过；
4. `spdk_submit == spdk_complete`、errors=0、无 frame/lease/ring/handle 泄漏；
5. 新增 tracepoint/计数可诊断，trace/stats off 无 per-request 分配；
6. 结构或恢复门禁失败时不得用性能数字掩盖，按 `hfs2.0.md` §17.3 停止。

### 0.3 RocksDB 无回退门禁（本计划的核心验收约束）

HFS 是单进程、单 engine、bdev 独占；MySQL 与 RocksDB 不会同进程/同盘共跑，因此
**适配本身不应影响 RocksDB 运行时性能**。为保证这一点，每个 shard 出口必须执行：

| 级别 | 项目 | 通过标准 | 触发条件 |
|---|---|---|---|
| L1 | CTest 全量 | 全量通过（基线 26/26，随 shard 增长） | 每个 shard |
| L2 | FIO smoke（source-matched fio 3.42、4K randrw） | errors=0、closure 闭合 | 每个 shard |
| L3 | `hfs_db_bench` readrandom 同会话交错 A/B（stats=false） | 偏差 <5% | 每个 shard |
| L4 | RocksDB fillseq/readrandom/overwrite/readwhilewriting + 正式 8j4 gate | 无回退（<5%）、closure 闭合 | 触及 `hfs_core` 的 shard |

隔离原则：

1. **前端加法**：MySQL adapter 只新增文件/API，不改 RocksDB adapter 行为；
2. **core 改动门控**：任何 `hfs_core` 行为改动必须配置键门控、可提交回退，并单独成
   sub-shard；
3. **默认值冻结**：不改 `config/hfs_nvme.conf`、不改 `ring_depth/client_depth/arena/
   reactor` 等既有默认值；
4. **ABI 加法优先**：只做加法；破坏性变更升 `coreAbiMajor` 并单独评审；
5. **禁止共跑**：不新增同进程多 engine、跨进程共享 bdev 或同盘双挂载路径。

高风险共享改动清单（必须 L4 门禁）：

| 改动 | 风险 | 约束 |
|---|---|---|
| data-only `datasync`（`src/core/client.cpp:1137`、`Engine::syncMetadata`） | durability/epoch 模型 | sync 路径字节级不变；RocksDB sync=1 回归 |
| punch hole（extent map/allocator） | 波及 `File::allocate`/commit | 一期不做 |
| `SpdkRuntime`/AIO/reactor | 影响所有 frontend | v1 只关 native AIO + 同步门面，不动 runtime |
| 配置 parser 默认值 | 直接改变基准口径 | MySQL 独立 `.conf`，新键必须可选 |

---

## 1. Shard 总览

| Shard | Phase | 内容 | 规模 | 依赖 | 出口门禁 |
|---|---|---|---|---|---|
| 99 | 7 | 设计更新、MySQL fork 基座、构建与基线固化 | M | - | 设计文档更新 + fork 可编译 + 基线记录 |
| 100 | 7 | HFS 侧 API 补齐（path stat、mkdir-p、statvfs、锁） | S | 99 | 新增单测 + L1–L3 |
| 101 | 7 | InnoDB os_file seam 与路径路由（读路径闭环） | L | 100 | `--initialize` + 无宿主落盘 + L1–L3 |
| 101B | 7 | server 层 `my_*` 路由（mysys open/stat/access/read/write/sync/delete/rename/mkdir/dir） | L | 101 | 启停 + 第二实例 Busy + 无宿主落盘 + L1–L3 |
| 102 | 7 | 绕过点修复 + 写/恢复路径（redo/doublewrite/undo） | L | 101B | crash matrix + `hfsctl check` + L1–L3 |
| 103 | 7 | 线程/生命周期（per-thread Client registry、停机顺序） | M | 101 | 并发负载 + clean shutdown + L1–L3 |
| 104 | 7 | 功能验收矩阵（SQL/DDL/事务/跨进程） | M | 102/103 | 功能矩阵全过 + L1–L3 |
| 105 | 7 | 性能与 durability（group commit、datasync 评估） | M | 104 | 性能目标 + L4（含 RocksDB gate） |
| 106 | 7 | 收口（支持边界、禁用项、归档） | S | 105 | 全回归 + 文档收口 |

---

## 2. Phase 7：MySQL/InnoDB frontend

### Shard-99：设计更新、MySQL fork 基座与基线固化

> 状态：已完成（2026-09-24）。设计更新（`hfs2.0.md` §3.1/§3.2/§17/§18/§19/§20）已落地；
> MySQL 8.0.46 fork 基座在 VM `/mnt/hfs-data/mysql-fork`，`mysqld` 构建成功；
> `config/hfs_mysql.conf` 与 `mysql/build_mysql_vm.sh` 入库；RocksDB/FIO 基线固化完成
> （CTest 26/26、FIO smoke err=0、readrandom A/B HFS≈native、8j4 gate raw 115.9K /
> HFS 81.7K = 70.51%、p99 ratio 0.861、errors=0）。基线报告见
> `doc/performance/hfs2_mysql_phase7_baseline_20260924.md`。

**目标**：冻结范围、基座与对照基线，避免后续用 MySQL 收益掩盖 RocksDB 回退。

**交付物**

- `hfs2.0.md` §3.1/§3.2/§17 更新与新增 §20 frontend 契约章节（§0.1）；
- MySQL 8.0.46 fork（VM 内 Git + patch 目录）与可重复构建脚本，产出 `mysqld`；
- `config/hfs_mysql.conf`（datadir namespace、device set、路径前缀映射；与
  `config/hfs_nvme.conf` 隔离）；
- RocksDB/FIO 基线快照（CTest、FIO smoke、readrandom 同会话交错 A/B、正式 gate）。

**验收**：设计更新已提交；fork 编译通过；HFS core 零改动；基线数字与证据路径记录在案。

**不做**：任何 `os_file`/HFS API 改动。

### Shard-100：HFS 侧 API 补齐（纯加法）

> 状态：已完成（2026-09-24）。新增 `Client::stat(path)`（含根路径）与
> `Client::createDirectories(path)`；`file_api_test` 覆盖缺失路径/类型/根路径/幂等/
> 相对路径/尾斜杠/文件组件冲突；设计 §13.1/§13.2 已同步。VM CTest 26/26、
> `file_api_test` ok；L3 readrandom A/B 与 Shard-99 基线同口径无回退
> （HFS 69.9–72.1K vs 基线 68.7–72.4K，p50 111–115 µs，全部干净退出）。
> 锁与 statvfs 按计划复用宿主锁文件与现有
> `Engine::freeDataBytes/deviceBlockSize/deviceCapacityBytes`，不新增 core API。
> 证据：`/mnt/hfs-data/hfs2-mysql-baseline-20260924/shard100/`。

**目标**：补齐审计 §6.6 的缺口，为 seam 提供最小依赖。

**交付物**

- `Client::stat(path)`（path 级 stat：size/mode/mtime，无权限模型）；
- 递归 mkdir-p（adapter 内实现或 core 提供）；
- statvfs 等价（`Engine::freeDataBytes` 包装）；
- 锁契约：宿主锁文件（RocksDB adapter `LockFile` 先例）或 HFS namespace lock；
- 全部为新增函数/新增文件，不修改现有语义。

**验收**：新增单测覆盖缺失路径、类型、只读场景；CTest 全量；L1–L3 无回退。

**不做**：data-only `datasync`、punch hole（分别在 Shard-105/不做）。

### Shard-101：InnoDB os_file seam 与路径路由（读路径闭环）

> 状态：进行中（2026-09-24）。MySQL fork 新增 `storage/innobase/os/hfs_vfs.{h,cc}`
> （engine 生命周期、路径路由含相对路径/CWD 归一化、handle 表、同步门面），
> `os0file.cc` 24 处 dispatch + `Dir_Walker::walk_posix` 递归扫描路由，innobase CMake
> 加入 C++20 编译与 `libhfs_core` 链接，`innodb_init`/`innodb_shutdown` 生命周期钩子；
> `mysqld` 构建通过，`--initialize` 在 HFS datadir 上执行且 `initialize_rc=0`，
> InnoDB 文件（ibdata1/mysql.ibd/undo/dblwr/#innodb_redo）不再落宿主，host datadir
> 仅剩 server 层 `auto.cnf`/证书/空目录。补丁：`mysql/patches/hfs_seam_shard101.patch`
> （+240 行，另含两个新文件）。
>
> **新发现（阻塞 start/stop）**：`mysqld` 正常启动时 server 层（mysys `my_*`）用宿主
> syscall 校验 DD 文件，strace 证据 `access(".//mysql.ibd", F_OK) = -1 ENOENT` →
> `Failed to find valid data directory`。审计只覆盖了 `storage/innobase`，server 层
> `my_*`（`my_open/my_stat/my_access/my_read/my_write/my_sync/my_close/my_delete/
> my_rename/my_mkdir/my_rmdir/dir_*/my_realpath`）也必须对 HFS 前缀路径路由，否则
> `--initialize` 之后的正常启动无法完成。该工作新增为 **Shard-101B（server 层 my_* 路由）**，
> 完成后才能执行 start/stop 与第二实例 Busy 验收。
>
> 待办（已由 Shard-101B 完成）：mysys 路由补齐后，`--initialize` rc=0、启停、
> 第二实例 Busy 与 L1–L3 回归均已通过；已知边界：`posix_fallocate` 告警属 Shard-102。
> 证据：`/mnt/hfs-data/hfs2-mysql-baseline-20260924/shard101/`。

**目标**：让 InnoDB 的 open/read/size/status/dir 全部经过 HFS，形成最小可启动闭环。

**交付物**

- `storage/innobase/os/hfs_vfs.{h,cc}`：handle 表（`os_file_t`/`pfs_os_file_t` 由 fd
  改为 handle 索引）、路径路由（datadir/Fil_path 前缀 → HFS namespace，其余走 POSIX）、
  seam 函数实现（审计 §5.1 除 AIO 外的全部）；
- `SyncFileIO::execute` 改为 HFS read/write；`os_file_set_nocache` no-op；
  `os_file_get_status/exists` 走 `Client::stat`；
- `innodb_use_native_aio=OFF`（模拟 AIO）；socket/pid/error log/tmpdir/binlog 保持宿主
  绝对路径；
- 路径路由与 handle 表计数可诊断（trace/stats off 零开销）。

**验收**：`mysqld --initialize` 完成、`mysqld` 启停正常；strace/审计确认 datadir 内
文件不落宿主；第二实例返回 `Busy`；L1–L3 无回退。

**不做**：写路径持久化结论、性能结论、AIO bridge。

### Shard-101B：server 层 `my_*` 路由（mysys）

> 来源：Shard-101 收尾时发现 server 层 DD 校验走宿主 syscall
> （`access(".//mysql.ibd", F_OK)` → ENOENT），`--initialize` 后无法正常启动。
>
> 状态：已完成（2026-09-24）。mysys 路由覆盖 `my_open/my_create/my_close/my_read/
> my_write/my_sync/my_seek/my_chsize/my_stat/my_fstat/my_access/my_realpath/my_delete/
> my_rename/my_mkdir/my_setwd/my_chmod/my_fopen`；POSIX `my_access` 宏改为实函数；
> `fopencookie` 读写/seek 流覆盖 stdio 路径；`MY_NABP` 语义补齐；`hfs_vfs` 迁入 mysys
> 编译（C++20 + 全局 `libhfs_core` 链接与 build rpath）；SSL 密钥 `rename` 改为
> `my_rename`、RSA 密钥读取 `fopen` 改为 `my_fopen`。
>
> 验收（2026-09-24，全部通过）：`mysqld --initialize` rc=0；正常启动到达
> `ready for connections`；SIGTERM 正常退出；第二实例被 HFS 进程锁拒绝
> （`another engine already holds '/tmp/hfs2.lock'`，失败关闭）；datadir 全在 HFS
> （宿主 `/hfs/mysql` 不存在）；RSA 密钥警告消除。回归：L1 CTest 26/26 +
> `file_api_test`、L2 FIO verify/plain smoke `error=0` 且 closure 闭合、
> L3 readrandom A/B HFS 70.8–72.3K vs native 69.0–69.5K（无回退）。
> 遗留（Shard-102）：InnoDB `posix_fallocate` 告警（有 `os_file_set_size` 回退，
> 非致命）；server 层其余裸 syscall 审计。补丁：
> `mysql/patches/hfs_seam_shard101b.patch`（2276 行 / 27 文件，无调试插桩）。

**目标**：让 mysys 文件 API 对 HFS 前缀路径路由到同一 `hfs_vfs`，使 `mysqld`
正常启停、第二实例 Busy、datadir 不再出现 server 层宿主文件。

**交付物**

- mysys dispatch：`my_open/my_create/my_close/my_read/my_write/my_pread/my_pwrite/
  my_sync/my_fsync/my_stat/my_fstat/my_access/my_delete/my_rename/my_mkdir/my_rmdir/
  my_realpath/dir_*`（仅当路径/句柄属于 HFS 前缀时路由，其余走宿主）；
- `hfs_vfs` 补齐 mysys 需要的包装（`MY_STAT` 合成、FILE-like 语义、错误码映射）；
- server 层生命周期：日志/socket/pid/binlog 明确使用宿主绝对路径（不路由）。

**验收**：`--initialize` 后 `mysqld` 正常启动/停止；第二实例返回 Busy；datadir 中
InnoDB 与 server 层文件均在 HFS；L1–L3 无回退。

**不做**：性能结论、AIO bridge。

### Shard-102：绕过点修复 + 写/恢复路径

> 状态：已完成（2026-09-24，核心验收）。修复 7 处绕过点：`fil0fil.cc` posix_fallocate
> ×2 → `hfs_vfs::setSize`；`fsp0file.cc` fstat → `hfs_vfs::fstatHandle`；`row0log.cc`
> ftruncate ×2 → `hfs_vfs::truncate`；`buf0dump.cc` stdio/unlink/rename →
> `my_fopen/my_delete/my_rename`；`srv0start.cc` monitor unlink → `my_delete`；
> `ddl0ddl.cc` posix_fadvise → HFS 句柄 no-op。`pathIsHfs` 增加前缀预判，工具进程
> 不再因环境变量触发挂载尝试。
>
> 验收（真实 NVMe）：initialize rc=0；SQL CRUD/DDL（CREATE DATABASE/TABLE、INSERT、
> ALTER ADD COLUMN、UPDATE、DELETE、SELECT）rc=0 且结果正确；干净停机；重启后数据
> 持久（COUNT 正确）；写负载中 kill -9 后重启恢复、`hfsctl check` rc=0
> `corruption=none`；datadir 全在 HFS；`posix_fallocate` 告警清零；L1 CTest 26/26 +
> `file_api_test`、L2 FIO verify/plain smoke `error=0`、L3 readrandom A/B
> HFS 70.1–72.0K vs native 68.5–69.7K 无回退。证据：
> `/mnt/hfs-data/hfs2-mysql-baseline-20260924/shard102/`。
> 遗留：双写 torn page / 并发 online DDL 日志回收的深水区崩溃矩阵未专门构造
> （补丁已在位），并入 Shard-104 功能矩阵补测。

**目标**：关闭审计 §4 的全部绕过点，并让 redo/doublewrite/undo 写入与恢复闭环。

**交付物**

- #1/#2 `posix_fallocate` → `os_file_allocate`（`File::allocate`）；
- #3 `fstat` → `os_file_get_size/status`；#4 `ftruncate` → `os_file_truncate`；
- #5 `buf0dump` stdio/unlink/rename → `os_file_*`（或一期关 dump/load 并在文档标注临时
  边界）；#6 monitor file unlink → `os_file_delete` 或宿主路径；
- #7 `posix_fadvise` no-op；
- redo（`#innodb_redo`）、doublewrite、undo 写入与 `innodb_flush_method=O_DIRECT`
  对齐路径打通；`hfsctl check` 对 MySQL 卷输出正常。

**验收**：SQL CRUD + DDL（含 online DDL 日志回收）通过；kill -9 崩溃矩阵（含双写
torn page、redo 恢复、未完成 DDL）后重启可恢复；`hfsctl check` 无 corruption；
L1–L3 无回退。

**不做**：并发扩展与性能调优（Shard-103/105）。

### Shard-103：线程与生命周期

**目标**：让 InnoDB 多后台线程（page cleaner、log writer、AIO handler、purge、DDL）
在 HFS 上有确定的生命周期。

**交付物**

- per-thread `hfs::Client` registry（复用 RocksDB worker-direct 的 retire/fallback
  机制）：拥有线程自建自关；context 抢先 retire 时 TLS 析构为 no-op；
- 停机顺序：停新提交 → drain outstanding → close Client → 停 dispatcher → engine stop；
  任一层失败 fail-closed；
- 资源上限：线程数 × `maxOutstanding` × 队列容量按配置校验，超限回落共享 dispatcher
  并计数/日志；
- ASAN/TSAN 生命周期用例（线程退出时仍有 outstanding、engine stop 时仍有 client、
  反复建链/销毁、建链失败回落）。

**验收**：多线程 mysqlslap/sysbench 负载下 closure 闭合、无 UAF；反复启停与 SIGKILL
正常；第二进程 `Busy`；L1–L3 无回退。

### Shard-104：功能验收矩阵

**目标**：以用户可见语义收口功能正确性。

**交付物**

- MySQL 功能矩阵脚本：`--initialize`/bootstrap、SQL CRUD、事务与隔离级别、DDL（含
  online/instant）、外键/全文/空间索引 smoke、binlog、逻辑备份/恢复（mysqldump）、
  跨进程重启、`mysqlcheck`；
- 禁用项 fail-closed 探针：页压缩、clone、MyISAM/Aria、`--daemonize`、native AIO；
- 跨进程顺序 reopen 与 crash recovery 证据归档。

**验收**：矩阵全过；禁用项明确拒绝；`hfsctl check` 通过；L1–L3 无回退。

### Shard-105：性能与 durability

**目标**：验证 MySQL 性能可接受，同时证明 RocksDB 无回退。

**交付物**

- 性能对标（同会话交错）：mysqlslap 读/写、sysbench OLTP、`innodb_flush_log_at_trx_commit=1`
  组提交；记录 p50/p99、设备读/写量、CPU map；
- data-only `datasync` 评估（**独立 core sub-shard**）：仅在确有必要时实施；实现必须
  保证 `File::sync` 路径字节级不变，且通过 L4；
- RocksDB 全量回归（L1–L4）。

**验收**：MySQL 读性能≈native（参考 HFS 1.x MariaDB：读 93.7–103.7%、单行写
40.1–62.2%，以本机实测为准）；RocksDB 正式 gate 与 readrandom A/B 偏差 <5%；
closure 闭合。

**停止条件**：core 改动导致 RocksDB 回退 >5% → 立即回退该 core 改动，MySQL 维持
namespace barrier；不得以 MySQL 收益换取 RocksDB 回退。

### Shard-106：收口

**交付物**

- MySQL 支持边界文档（版本、禁用项、参数模板、已知边界）；
- `hfs2.0.md` 状态更新与 `doc/review/hfs2_mysql_innodb_audit_20260924.md` 实现结果回填；
- 性能基线与证据归档（`/mnt/hfs-data`）、补丁清单与 rebase 说明。

**验收**：全回归（MySQL + RocksDB/FIO）通过；支持边界明确；无 preload/FUSE/Blobstore
回流；无未记录临时边界。

---

## 3. 风险与停止条件

| 风险 | 影响 | 缓解 |
|---|---|---|
| `File::sync` 无 data-only barrier | redo per-commit 延迟 | Shard-105 组提交先行；core 改动必须过 L4 |
| 绕过点遗漏 | datadir 文件落宿主/失败 | 审计清单 + 运行期 strace 校验（每 shard） |
| 线程/停机顺序缺陷 | 挂起/UAF | Shard-103 registry + ASAN/TSAN |
| 上游 rebase 冲突 | 维护成本 | 改动限定 seam + 7 绕过点 + 独立 patch 目录 |
| fork/初始化顺序 | bootstrap 失败 | install/initialize 与 server 串行；禁 `--daemonize` |
| GPLv2 fork | 对外分发合规 | 内部使用可接受；对外需合规评审 |
| 环境漂移 | 错误性能结论 | 同会话交错 A/B、native 首尾校准（沿用 20260923 协议） |

停止条件：`hfs2.0.md` §17.3 全部条目；追加：崩溃恢复失败、双写/redo 不一致、
datadir 文件落到宿主、RocksDB L4 回退 >5%、MySQL 路径出现 data mismatch。

---

## 4. 证据与文档产物

- 审计：`doc/review/hfs2_mysql_innodb_audit_20260924.md`；
- 设计：`doc/design/hfs2.0.md`（§3/§17/§20）；
- 每个 shard 的结果报告：`doc/task_result/`（按 AGENTS.md 任务规则）或
  `doc/performance/`、`doc/review/` 归档；
- 大体积证据：VM `/mnt/hfs-data`；
- 基线对照：Shard-99 固化的 RocksDB/FIO 快照。

## 5. 与主计划的关系

`doc/shard/hfs2.0_shard_development_plan.md` 的 Phase 0-6（Shard-68~85）及其"不包含
MySQL/InnoDB"约束在 Phase 0-6 内继续有效；本计划是 repair-98 之后的 Phase 7 扩展，
入口条件为 §0.1 的设计更新完成。两计划共用同一事实源、完成定义与停止条件。
