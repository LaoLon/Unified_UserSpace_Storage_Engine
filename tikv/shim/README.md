# TiKV shim（UUSE C ABI + raft-engine FileSystem）

Shard-116 的 TiKV 侧接入材料。

## 组件

| 文件 | 用途 |
|---|---|
| `uuse_tikv_shim.h` / `uuse_tikv_shim.cc` | C ABI shim：engine open/close、open/remove/rename/create_dirs/exists、pread/pwrite/append/truncate/allocate/sync/file_size（错误为负 errno） |
| `uuse_tikv_shim_smoke.cc` | C ABI 真机 smoke（create/write/append/pread/truncate/sync/exists/rename/remove） |
| `rust/uuse_file_system.rs` | raft-engine `FileSystem` trait 的 UUSE 实现（经 C ABI；含 `read_dir`/`create_dir`） |
| `rust/build.rs` | 把 `libuuse_tikv_shim.so` 链接进 `raft_log_engine`（`UUSE_CORE_BUILD` 可覆盖） |
| `rust/uuse_fs_smoke.rs` | raft-engine 真机 smoke（write/sync/fetch） |
| `rust/uuse_fs_prod_smoke.rs` | 生产 wrapper 真机 smoke（`RaftLogEngine::new_with_uuse`） |
| `rust/engine.rs` | TiKV `components/raft_log_engine/src/engine.rs` 的改造副本（enum base + `new_with_uuse` + `read_dir`/`create_dir` 转发） |
| `../patches/apply-raft-engine-readdir.py` | raft-engine 目录列举/创建改走 `FileSystem` trait 的补丁 |
| `../patches/apply-uuse-config.py` | TiKV 配置门控（`[raft-engine] uuse_conf`）+ 生产调用点分支的补丁 |

## 关键实现说明

- **引用计数生命周期**：`UuseEngineState` 由引擎句柄与文件句柄共享；前端先丢 FS、后丢
  reader/writer 时（raft-engine 的字段析构顺序）不会在文件仍打开时停引擎，最后一个句柄
  释放才 `client close + drain + engine stop`（修复 gdb 定位的 `Namespace::close` 死锁）。
- **目录操作**：`read_dir` 经 `Client::getChildren` 游标分页返回 NUL 分隔文件名；
  `create_dir` 经 `Client::createDirectories`；raft-engine trait 默认实现 fail-closed，
  禁止回退宿主 `std::fs`。

## 构建（UUSE 仓库）

```bash
cmake --build build-uuse -j3 --target uuse_tikv_shim uuse_tikv_shim_smoke
```

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
```

## 配置门控（raft-engine）

TiKV `[raft-engine]` 段新增 `uuse_conf`（默认空 = POSIX 文件系统）：

```toml
[raft-engine]
dir = "/raft-engine"
uuse_conf = "/root/code/config/uuse_nvme.conf"
```

`uuse_conf` 非空时，`RaftLogEngine` 经 `ManagedFileSystem::new_with_uuse` 把日志放到
UUSE 卷；`key_manager`（加密）与该组合 fail-closed。

## 待办

- RocksDB 侧配置键与 `engine_rocks::get_env` 选择（Shard-117）；
- `swappy_allocator.rs` spill 目录一期不启用；
- 正式 fork/补丁栈（当前为 checkout 就地补丁，见 `tikv/spike/` 与 `tikv/patches/`）。
