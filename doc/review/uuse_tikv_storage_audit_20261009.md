# UUSE × TiKV 存储绕过点审计（Shard-114，2026-10-09）

> 冻结版本：TiKV v8.5.8（`a1fdd6a59`）、rust-rocksdb（`tikv/rust-rocksdb` checkout
> `64a092f`）、raft-engine（`tikv/raft-engine` checkout `3d84405`）、vendored RocksDB
> （随 rust-rocksdb 构建）、Rust `nightly-2026-01-30`；源码位于 host4
> `/build/tikv-fork`。
>
> 依据：`doc/design/uuse.md` §21（本审计发现与 §21.2 原假设不符，需同步修订）、
> `doc/shard/uuse_tidb_shard_plan.md` §0.1。
>
> 行号以冻结 commit 为准；升级 TiKV 后必须重跑本审计。

---

## 0. 结论摘要

1. **RocksDB 侧不存在可实现的 Rust `FileSystem` trait**。TiKV v8.5.8 的
   `components/file_system` 已重构为 `std::fs` 的再导出 + io_stats/metrics/rate_limiter，
   `rocksdb::Env` 是 C++ 不透明对象（`crocksdb_env_t`），Rust 侧只有
   `Env::default()`、加密包装和 inspector 包装。要让 kvdb/raftdb 的文件 I/O 走 UUSE，
   必须在 **C++ 层实现 `rocksdb::FileSystem`**，并通过 **crocksdb FFI + rust-rocksdb
   `Env` 构造**注入。
2. **raft-engine 侧有可插拔的 Rust `FileSystem` trait**，且 v8.5.8 默认启用
   raft-engine（`raft_engine.enable = true`，无 raftdb）。接入面比 RocksDB 干净。
3. **非引擎文件仍有直用点**（snap/import/LOCK/占位文件/panic mark 等），必须逐项路由或
   fail-closed，否则 `data_dir` 会静默落宿主。
4. Patch 面比原计划大：**rust-rocksdb fork（crocksdb C API + Rust Env 构造）** +
   **TiKV（`engine_rocks`/`config`/`raft_log_engine`）** + **UUSE `tikv/shim`（C++
   FileSystem 实现 + Rust FS 实现）**。

---

## 1. 冻结版本与源码位置

| 组件 | 版本/位置 | 说明 |
|---|---|---|
| TiKV | v8.5.8 `a1fdd6a59` | `/build/tikv-fork`（host4） |
| rust-rocksdb | `tikv/rust-rocksdb` `64a092f` | Cargo git 依赖，Cargo.lock 锁定 |
| raft-engine | `tikv/raft-engine` `3d84405` | 默认 raft log 引擎 |
| RocksDB | vendored（随 rust-rocksdb） | `librocksdb_sys/rocksdb/` |
| Rust | `nightly-2026-01-30` | `rust-toolchain.toml` 固定 |

---

## 2. RocksDB 接入面（kvdb + raftdb）

### 2.1 现状链路（file:line）

- Env 工厂：`components/engine_rocks/src/lib.rs:117-123`
  `get_env(key_manager, limiter)` → `encryption::get_env(None /* base_env */, key_manager)`
  → `file_system::get_env(env, limiter)`；
- 加密包装：`components/engine_rocks/src/encryption.rs:14-26`
  （仅当启用 key manager 时包 `new_key_managed_encrypted_env`）；
- 限速 inspector 包装：`components/engine_rocks/src/file_system.rs:13-26`
  （`Env::new_file_system_inspected_env(base_env, inspector)`，base 为
  `Env::default()`）；
- 共享 env：`src/config/mod.rs:4525-4540` `build_shared_rocks_env(...)`；kv 引擎
  `opts.set_env(shared.env.clone())`（`:1547`），raftdb `opts.set_env(env)`（`:1964`）；
- 调用点：`components/server/src/server.rs:1778`、`components/server/src/server2.rs:1595`；
- SST 读路径：`components/engine_rocks/src/sst.rs:31-52`（`open_with_env`，同样经
  `get_env`）；
- 注释明确现状：`engine_rocks/src/file_system.rs:11`、`encryption.rs:13`
  “Use engine::Env directly since Env is not abstracted.”。

### 2.2 C++/FFI 能力

- RocksDB 公开接口：`include/rocksdb/env.h:161`（`explicit Env(const
  std::shared_ptr<FileSystem>& fs)`）、`:669`（`GetFileSystem()`）、`:1910`
  （`NewCompositeEnv(fs)`）；`include/rocksdb/file_system.h:270`
  （`class FileSystem : public Customizable`）、`:1399`（`FileSystemWrapper`）。
- crocksdb 现有 env 构造（`librocksdb_sys/crocksdb/crocksdb/c.h:1761-1779`）：
  `crocksdb_default_env_create` / `mem_env_create` / `ctr_encrypted_env_create` /
  `key_managed_encrypted_env_create`（`c.cc:4629`）/ `file_system_inspected_env_create`
  （`c.cc:4716`）。**没有 custom FileSystem 的 Env 构造**。

### 2.3 注入方案（待 Shard-116 实施）

1. UUSE `tikv/shim` 实现 C++ `rocksdb::FileSystem`（读写/追加/sync/truncate/allocate/
   rename/link/lock/目录/属性），经 opaque vtable 交给 crocksdb；
2. rust-rocksdb fork 增加 `crocksdb_uuse_env_create(...)`（`c.cc` 中用
   `Env(fs)`/`NewCompositeEnv(fs)` 构造）与 Rust `Env` 构造/FFI；
3. TiKV `engine_rocks::get_env` 增加配置门控的 UUSE base env（默认关闭，stock 行为
   不变）；`build_shared_rocks_env` 按 `storage.uuse.*` 选择；
4. 限速 inspector 包装可保留（包在 UUSE env 外），加密包装一期不启用。

### 2.4 风险

- rust-rocksdb fork 需要打补丁（C++/FFI/Rust 三处），维护与 rebase 成本最高；
- `FileSystem` 方法面较大，必须按 RocksDB 8.x 的 direct IO、`PositionedAppend`、
  `RangeSync`、目录 fsync、`LockFile` 语义逐项实现；
- 若 FFI 构造不可行，备选是把 UUSE FileSystem 编进 crocksdb（耦合更重，需评审）。

---

## 3. raft-engine 接入面（默认 raft log）

### 3.1 现状

- `raft_engine::Engine<F: FileSystem, P = FilePipeLog<F>>`（raft-engine
  `src/engine.rs:31`）；`open_with_file_system(cfg, fs, ...)`（`:70`）支持自定义 FS；
- `FileSystem` trait（raft-engine `src/env/mod.rs`）：`create/open/delete/rename/
  reuse/reuse_and_open/delete_metadata/exists_metadata/new_reader/new_writer`；
  `Handle { truncate, file_size, sync }`；`WriteExt { truncate, allocate }`；
- TiKV 包装：`components/raft_log_engine/src/engine.rs:131-145`
  `ManagedFileSystem { base_file_system: DefaultFileSystem, key_manager, rate_limiter }`；
  `:336` `RaftLogEngine(Arc<RawRaftEngine<ManagedFileSystem>>)`；`:344` 构造；
- 路径：`data_dir/raft-engine`（`src/config/mod.rs:3659`；raft-kv1 才用
  `data_dir/raft`，`:3650`）。

### 3.2 注入方案

- 实现 Rust `UuseFileSystem`（`FileSystem` trait + `Handle` + `WriteExt`），经 shim
  调 UUSE core；
- 将 `ManagedFileSystem.base_file_system` 改为配置选择的 `Default | Uuse`（小 patch，
  仅 `components/raft_log_engine` + config 键）；
- 加密（key manager）一期禁用；rate limiter 包装保留。

---

## 4. 非引擎文件绕过点清单

| # | 位置 | 用途 | 路由决定 |
|---|---|---|---|
| 1 | `components/server/src/common.rs:171-185`（`init_fs`） | `data_dir/LOCK`：`File::create` + `try_lock_exclusive` | UUSE namespace lock 或与 UUSE 进程锁合并；失败必须 fail-closed |
| 2 | `components/server/src/common.rs:147-170`、`423-437` | `$TMPDIR/TIKV_LOCK_FILES/*` 冲突地址锁 | 宿主白名单（保持 host，不进入 UUSE） |
| 3 | `components/server/src/common.rs:193-216` | `data_dir/SPACE_PLACEHOLDER_FILE` + `reserve_space_for_recover` | UUSE 等价物（`File::allocate`）或 fail-closed；禁止静默 no-op |
| 4 | `components/server/src/common.rs:186-192` | `data_dir/panic_mark_file` | UUSE namespace（同 datadir 语义） |
| 5 | `src/server/snap.rs`、`src/server/tablet_snap.rs:39` | snap 文件传输（`OpenOptions`） | `data_dir/snap` → UUSE namespace |
| 6 | `components/sst_importer/src/util.rs:33-101`、`sst_importer.rs:309/353/636` | import 文件（hard_link/copy_and_sync/read_dir/remove_dir_all） | `data_dir/import` → UUSE namespace；外部导入源路径保持宿主 |
| 7 | `components/backup/src/*` | 备份输出（本地/S3） | 宿主或对象存储白名单；不默认进 UUSE |
| 8 | `cmd/tikv-ctl` | 直接打开 data dir | 一期禁用（fail-closed） |
| 9 | 日志/status/socket/pid | 运维输出 | 宿主白名单 |
| 10 | encryption 元数据（`encryption` crate） | 密钥文件 | 一期禁用（`plaintext`），启用即 fail-closed |
| 11 | raft-engine `src/file_pipe_log/mod.rs:111`、`src/file_pipe_log/pipe_builder.rs:269`、`src/swappy_allocator.rs:471` | 日志文件目录列举（`std::fs::read_dir`） | **已修复（Shard-116）**：`FileSystem` trait 增加 `read_dir`/`create_dir`（默认 fail-closed），`UuseFileSystem` 经 `getChildren`/`createDirectories` 实现，`ManagedFileSystem` 转发；`file_pipe_log/mod.rs` 与 `pipe_builder.rs` 改走 FS（补丁 `tikv/patches/apply-raft-engine-readdir.py`）；`swappy_allocator.rs`（spill 目录）一期不启用 |
| 12 | TiKV `components/tikv_util/src/config.rs`（`RaftDataStateMachine::validate`、`raftengine_exists`/`raftdb_exists`）、`engine_rocks::RocksEngine::exists` | 启动前 raft data set / kvdb 存在性校验（宿主路径） | **已修复（Shard-117）**：`uuse-conf` 设置时跳过宿主校验（补丁 `apply-uuse-validation-skip.py`）；实测 kill -9 重启持久通过 |
| 13 | TiKV 单进程双引擎 | kvdb（RocksDB env）与 raft-engine 各建一个 UUSE engine | **已修复（Shard-117）**：shim 进程级单例（weak_ptr 注册表）+ adapter `createFileSystemWithEngine`（`ownsEngine=false`）+ crocksdb 接线；实测双引擎同开 UUSE、SQL 写入/kill -9 恢复通过（补丁 `apply-uuse-shared-engine.py`） |
| 14 | RocksDB `DBImpl::SyncWAL` | WAL 文件 `IsSyncThreadSafe()` 默认 false → bootstrap `NotSupported` | **已修复（Shard-117）**：`UuseWritableFile::IsSyncThreadSafe()=true`（UUSE `File::sync` 为 namespace 级屏障） |

---

## 5. 路径路由与宿主白名单（目标态）

| 路径 | 内容 | 路由 |
|---|---|---|
| `storage.data-dir/db` | kv RocksDB | UUSE（RocksDB Env） |
| `storage.data-dir/raft-engine` | raft log（默认） | UUSE（raft-engine FileSystem） |
| `storage.data-dir/raft` | raftdb（仅 raft-kv1） | UUSE（RocksDB Env） |
| `storage.data-dir/snap` | snapshot 文件 | UUSE |
| `storage.data-dir/import` | SST ingest | UUSE |
| `storage.data-dir/LOCK`、`panic_mark_file`、`SPACE_PLACEHOLDER_FILE` | 进程锁/标记/占位 | UUSE namespace（LOCK 与 UUSE 进程锁共同 fail-closed） |
| `$TMPDIR/TIKV_LOCK_FILES/*` | 冲突地址锁 | 宿主白名单 |
| 日志/status/socket/pid、备份/导入输出目录 | 运维与外部产物 | 宿主/对象存储白名单（显式配置） |

---

## 6. 禁用项与探针

- encryption-at-rest、S3/cloud FS、mmap 读写、多进程共享 bdev、`tikv-ctl` 直连
  UUSE 数据目录：配置出现即启动失败；
- 探针：`encryption.data_encryption_method != plaintext` → 拒绝；`storage.uuse.enabled
  = false` 时保持 stock；第二实例 `LOCK`/UUSE 进程锁 → `Busy`/fatal；
- 每个 shard 出口用 strace 校验 `data_dir` 内文件不落宿主。

---

## 7. 对设计与计划的影响（必须更新）

1. `doc/design/uuse.md` §21.1/§21.2 的 “`components/file_system` FileSystem trait /
   `RocksDBEnv`” 描述与 v8.5.8 事实不符，改为：
   - RocksDB：C++ `rocksdb::FileSystem` + crocksdb FFI `Env` 构造 + rust-rocksdb `Env`；
   - raft-engine：Rust `FileSystem` trait 实现；
2. `doc/shard/uuse_tidb_shard_plan.md`：
   - Shard-114 状态标记完成（本报告）；
   - Shard-116 交付物加入 rust-rocksdb crocksdb FFI patch 与 C++ FileSystem 实现；
   - Shard-117 同时覆盖 RocksDB Env 与 raft-engine FS 的写/恢复路径；
   - 风险表加入 rust-rocksdb fork patch 维护、C++ FileSystem 方法面、
     LOCK/placeholder/panic_mark 路由；
3. Shard-115 的 API 补齐清单以本报告 §2/§3 的方法面为准（raft-engine `WriteExt.
   allocate/truncate`、`Handle.sync/file_size`；RocksDB FileSystem 的 direct IO/
   `RangeSync`/目录 fsync/`LockFile`）。

## 8. 未决问题

- ~~crocksdb FFI 增加 `Env(fs)` 构造是否足够~~：已验证足够（`NewCompositeEnv(fs)`，
  2026-10-09 spike，见 `tikv/spike/`）；不需要内部 `CompositeEnvWrapper` 头文件；
- RocksDB `FileSystem` 的 `punch_hole`/`fallocate` 语义与 UUSE 能力映射；
- snap/import 的 hard_link/copy_and_sync 在 UUSE namespace 的等价实现
  （`Client::link` 已有；copy 走 read/write）；
- raft-kv1（raftdb）是否需要一期支持，或一期只支持默认 raft-engine（建议后者）。

## 9. Shard-115 API 映射结论（2026-10-09）

结论：**UUSE core 无需新增 API**；两个 seam 的依赖都能由现有公开 API 覆盖，
RocksDB 侧直接复用现有 adapter 实现（差异只在 8.10.2 头文件适配，属 Shard-116）。

### 9.1 raft-engine `FileSystem` → UUSE API

| raft-engine | UUSE |
|---|---|
| `create(path)` | `Client::open(path, Create\|Truncate)` |
| `open(path, perm)` | `Client::open(path)` / `Create` |
| `delete(path)` | `Client::remove(path)` |
| `rename` / `reuse` | `Client::rename`（reuse 默认实现即 rename） |
| `delete_metadata` / `exists_metadata` | 加密禁用；默认实现（Ok/false） |
| `new_reader(handle)` | `File::read(buffers, offset)`（pread 语义） |
| `new_writer(handle)` | `File::write(buffers, offset)` / `append` |
| `Handle.truncate` | `File::truncate` |
| `Handle.file_size` | `File::stat().logicalSize` |
| `Handle.sync` | `File::sync` |
| `WriteExt.truncate` | `File::truncate` |
| `WriteExt.allocate` | `File::allocate` |

### 9.2 RocksDB `FileSystem` → 复用现有 adapter

- 现有 `rocksdb/uuse_filesystem.cpp`（52 个 override，含 `GetFileModificationTime`、
  `LockFile/UnlockFile` 宿主锁 + `flock`、`punch_hole` 继承默认 fail-closed）已覆盖
  接口；core API 使用面：`client->open/getChildren/link/remove/rename/submitRead/
  poll/close`、`engine->acquireDmaBuffer/releaseDmaBuffer/freeDataBytes/stop`、
  `file->stat/sync`；
- TiKV vendored RocksDB 是 **8.10.2**（adapter 目标 9.11.2），需要按 8.10.2 头文件
  适配（可选接口差异，如 `SupportedOps`/`kFSBuffer` 的有无），属 Shard-116 spike；
- 不新增 core API；若 spike 发现缺口，按 shard 计划 §0.3 纯加法单独成 sub-shard。
