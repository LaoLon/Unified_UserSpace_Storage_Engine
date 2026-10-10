# TiKV shim（UUSE C ABI + raft-engine FileSystem）

Shard-116 的 TiKV 侧接入材料。

## 组件

| 文件 | 用途 |
|---|---|
| `uuse_tikv_shim.h` / `uuse_tikv_shim.cc` | C ABI shim：engine open/close、open/remove/rename/create_dirs/exists、pread/pwrite/append/truncate/allocate/sync/file_size（错误为负 errno） |
| `uuse_tikv_shim_smoke.cc` | C ABI 真机 smoke（create/write/append/pread/truncate/sync/exists/rename/remove） |
| `uuse_tikv_shim_lifecycle.cc` | Shard-118 生命周期用例（engine 先关+文件后关、线程退出持文件、重复建链、共享句柄、fork -ECHILD、参数防护） |
| `rust/uuse_file_system.rs` | raft-engine `FileSystem` trait 的 UUSE 实现（经 C ABI；含 `read_dir`/`create_dir`） |
| `rust/build.rs` | 把 `libuuse_tikv_shim.so` 链接进 `raft_log_engine`（`UUSE_CORE_BUILD` 可覆盖） |
| `rust/uuse_fs_smoke.rs` | raft-engine 真机 smoke（write/sync/fetch） |
| `rust/uuse_fs_prod_smoke.rs` | 生产 wrapper 真机 smoke（`RaftLogEngine::new_with_uuse`） |
| `rust/engine.rs` | TiKV `components/raft_log_engine/src/engine.rs` 的改造副本（enum base + `new_with_uuse` + `read_dir`/`create_dir` 转发） |
| `../patches/apply-raft-engine-readdir.py` | raft-engine 目录列举/创建改走 `FileSystem` trait 的补丁 |
| `../patches/apply-uuse-config.py` | TiKV 配置门控（`[raft-engine] uuse_conf`）+ 生产调用点分支的补丁 |
| `../patches/apply-uuse-rocksdb-config.py` | RocksDB 侧门控（`[storage] uuse-conf` + `engine_rocks::get_env_with_uuse`）的补丁 |
| `../patches/apply-uuse-shared-engine.py` | 单进程共享 UUSE engine（shim 单例 + adapter `createFileSystemWithEngine` + crocksdb 接线）的补丁 |
| `../patches/apply-uuse-validation-skip.py` | `uuse-conf` 设置时跳过宿主 raft data set 校验（Shard-117）的补丁 |
| `../patches/apply-uuse-shutdown-sync.py` | 关闭时保留 engines 并依次 sync kvdb/raft，避免进程退出丢尾部（Shard-118）的补丁 |
| `../patches/apply-raft-engine-sync-fix.py` | 修复 raft-engine `Engine::sync()` 空 batch 空操作（Shard-118）的补丁 |
| `../patches/apply-uuse-apply-sync.py` | apply 批次强制同步，保持持久 applied 跟上压缩（Shard-118）的补丁 |

## 关键实现说明

- **单进程共享引擎（Shard-117）**：`uuse_tikv_engine_open` 按 `.conf` 路径维护进程级
  单例（weak_ptr 注册表）；RocksDB adapter 经 `createFileSystemWithEngine` 复用同一
  engine（`ownsEngine=false`，不接管生命周期），crocksdb 句柄进程级保活；kvdb 与
  raft-engine 因此共用一个 UUSE engine。
- **宿主校验跳过（Shard-117）**：`uuse-conf` 设置时跳过 `RaftDataStateMachine` 的
  宿主数据校验（引擎数据在 UUSE 卷，宿主路径看不到；补丁
  `apply-uuse-validation-skip.py`）。
- **WAL SyncWAL（Shard-117）**：RocksDB `SyncWAL()` 要求 WAL 文件
  `IsSyncThreadSafe()==true`；adapter 的 `UuseWritableFile` 已声明（UUSE
  `File::sync` 是 namespace 级屏障），否则 TiKV bootstrap 报
  `SyncWAL() is not supported for this implementation of WAL file`。
- **非引擎直用点（Shard-117）**：`uuse-conf` 设置时跳过宿主 `data_dir/LOCK` 与
  reserve 占位（UUSE 进程锁是互斥手段；宿主 reserve 不保护 UUSE 卷）；panic mark、
  `last_tikv.toml`、RocksDB/raftdb `info-log-dir` 归宿主白名单；补丁
  `apply-uuse-nonengine-closeout.py`。
- **引用计数生命周期**：`UuseEngineState` 由引擎句柄与文件句柄共享；前端先丢 FS、后丢
  reader/writer 时（raft-engine 的字段析构顺序）不会在文件仍打开时停引擎，最后一个句柄
  释放才 `client close + drain + engine stop`（修复 gdb 定位的 `Namespace::close` 死锁）。
- **client 线程（Shard-118）**：core 要求 `Client::close` 在创建线程（frontend thread）
  调用，而最后句柄可能由任意线程释放；engine state 因此自带专用 client 线程——client
  在该线程创建与关闭，路径操作仍由调用线程经 mutex 串行执行（core 允许），文件数据操作
  经 `File` 在调用线程自驱动。生命周期用例验证：worker 线程退出后其打开的文件仍可由
  main 线程关闭，engine 随最后句柄干净停止（日志 `client closed: open files=0`）。
- **元数据持久化语义**：namespace 元数据（create/remove）随 `File::sync` 的 durable
  epoch 落盘；跨挂载可见性依赖该 sync（生命周期用例按此实现）。
- **目录操作**：`read_dir` 经 `Client::getChildren` 游标分页返回 NUL 分隔文件名；
  `create_dir` 经 `Client::createDirectories`；raft-engine trait 默认实现 fail-closed，
  禁止回退宿主 `std::fs`。

## 构建（UUSE 仓库）

```bash
cmake --build build-uuse -j3 --target uuse_tikv_shim uuse_tikv_shim_smoke uuse_tikv_shim_lifecycle
```

TSAN 复跑：`TSAN_OPTIONS=suppressions=tikv/shim/tsan.supp`（仅压制 SPDK 未插桩的
`controlSubmitTrampoline`/`submitControlIo` 边界帧；UUSE/shim 代码保持干净）。

## TiKV fork 内的落点

- `components/raft_log_engine/src/uuse_file_system.rs`（新增）
- `components/raft_log_engine/src/lib.rs` 增加 `pub mod uuse_file_system;`
- `components/raft_log_engine/build.rs`（新增）
- `components/raft_log_engine/examples/uuse_fs_smoke.rs`（新增）

## 真机结果（host4，2026-10-09）

前置：`0000:13:00.0` 绑 `uio_pci_generic`、`uusectl format --yes`、
`hugepages=1024`、`LD_LIBRARY_PATH=/root/code/build-uuse`。

```text
SHIM_SMOKE_PASS
RAFT_ENGINE_SMOKE_PASS written=47 fetched=2
RAFT_ENGINE_PROD_SMOKE_PASS written=47 fetched=2   # RaftLogEngine::new_with_uuse
SHIM_LIFECYCLE_PASS                                  # 6 lifecycle cases (Shard-118)
```

Sanitizer（Shard-118）：生命周期用例 ASAN 干净；TSAN 复跑 0 警告
（`TSAN_OPTIONS=suppressions=tikv/shim/tsan.supp`，仅压制 SPDK 未插桩边界）。

## 配置门控（raft-engine 与 RocksDB）

TiKV `[storage]` 与 `[raft-engine]` 段新增 `uuse_conf`（默认空 = POSIX 文件系统）：

```toml
[storage]
data-dir = "/tikv-data"
uuse_conf = "/root/code/config/uuse_nvme.conf"   # kvdb（与 raftdb）共享 env

[raft-engine]
dir = "/tikv-data/raft-engine"
uuse_conf = "/root/code/config/uuse_nvme.conf"   # raft 日志
```

任一 `uuse_conf` 非空时，对应引擎的文件放到 UUSE 卷；`key_manager`（加密）与该
组合 fail-closed。注意：`data_dir/LOCK`、panic mark、占位文件等非引擎直用点尚未
路由（审计 §4），一期 bring-up 需要宿主目录存在（后续 Shard-117 关闭）。

## 待办

- 非引擎直用点（LOCK/panic mark/占位文件）路由或 fail-closed（Shard-117）；
- `swappy_allocator.rs` spill 目录一期不启用；
- 正式 fork/补丁栈（当前为 checkout 就地补丁，见 `tikv/spike/` 与 `tikv/patches/`）。
