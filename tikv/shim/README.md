# TiKV shim（UUSE C ABI + raft-engine FileSystem）

Shard-116 的 TiKV 侧接入材料。

## 组件

| 文件 | 用途 |
|---|---|
| `uuse_tikv_shim.h` / `uuse_tikv_shim.cc` | C ABI shim：engine open/close、open/remove/rename/create_dirs/exists、pread/pwrite/append/truncate/allocate/sync/file_size（错误为负 errno） |
| `uuse_tikv_shim_smoke.cc` | C ABI 真机 smoke（create/write/append/pread/truncate/sync/exists/rename/remove） |
| `rust/uuse_file_system.rs` | raft-engine `FileSystem` trait 的 UUSE 实现（经 C ABI） |
| `rust/build.rs` | 把 `libuuse_tikv_shim.so` 链接进 `raft_log_engine`（`UUSE_CORE_BUILD` 可覆盖） |
| `rust/uuse_fs_smoke.rs` | raft-engine 真机 smoke（write/sync/fetch） |

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
```

## 待办

- `ManagedFileSystem` 集成：把 `base_file_system` 换成配置选择的
  `UuseFileSystem`（含加密/限速包装），使 TiKV 生产路径生效（Shard-116/117）；
- 生命周期告警 `client destroyed without close after drain`（Shard-118）；
- 正式 fork/补丁栈（当前为 checkout 就地补丁，见 `tikv/spike/`）。
