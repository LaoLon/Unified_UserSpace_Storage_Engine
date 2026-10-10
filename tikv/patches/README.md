# UUSE × TiKV 补丁栈（Shard-116…118）

冻结版本：TiKV **v8.5.8**（`docs/` 记录 commit `a1fdd6a59`）、rust-rocksdb
`64a092f`、raft-engine `3d84405`（cargo git checkout）。所有 patch 脚本在 **VM 内**
执行，目标路径写死为 `/build/tikv-fork`（TiKV）与
`/build/cargo/git/checkouts/raft-engine-aebf03c5dccc7789/3d84405`（raft-engine）；
原始文件备份到 `/build/spike-backup/`。

## 应用顺序

> **前置 1（rust-rocksdb checkout）**：先应用两个 spike 前序补丁
> （`tikv/spike/apply-spike-env-ffi.py` → `tikv/spike/apply-uuse-filesystem.py`），
> 表 3 的 shared-engine 补丁依赖它们建立的 `crocksdb_uuse_env_create` /
> `Env::uuse` 锚点。
> **前置 2（raft_log_engine 文件安装）**：表 1 的 config 补丁把生产调用点改为
> `RaftLogEngine::new_with_uuse`，该构造函数与 UUSE FileSystem 由以下文件提供
> （从 `tikv/shim/rust/` 安装，非锚点补丁）：
> - `engine.rs` → `components/raft_log_engine/src/engine.rs`（含 `new_with_uuse`）
> - `uuse_file_system.rs` → `components/raft_log_engine/src/uuse_file_system.rs`
> - `build.rs` → `components/raft_log_engine/build.rs`（链接 `libuuse_tikv_shim`）
> - `lib.rs` 增加 `pub mod uuse_file_system;`
>
> 完整重建顺序：cargo fetch → spike-env-ffi → uuse-filesystem → （下表 1–9）→
> raft_log_engine 文件安装 → 构建。

| # | 补丁 | 目标 | 作用 | 引入 |
|---|---|---|---|---|
| 1 | `apply-uuse-config.py` | TiKV `src/config/mod.rs`、`components/raft_log_engine` 生产调用点 | `[raft-engine] uuse_conf` 配置门控 + `new_with_uuse` 分支 | Shard-116 |
| 2 | `apply-uuse-rocksdb-config.py` | TiKV `components/engine_rocks` | `[storage] uuse-conf` 门控 + `get_env_with_uuse`（UUSE base env） | Shard-116 |
| 3 | `apply-uuse-shared-engine.py` | rust-rocksdb `c.cc`/`lib.rs` + TiKV `components/engine_rocks` | `crocksdb_uuse_env_create` 使用 shim 进程级单例 engine；adapter `createFileSystemWithEngine`（`ownsEngine=false`） | Shard-117 |
| 4 | `apply-uuse-validation-skip.py` | TiKV `src/config/mod.rs` | `uuse-conf` 设置时跳过宿主 raft data set 校验 | Shard-117 |
| 5 | `apply-uuse-nonengine-closeout.py` | TiKV `components/server/src/common.rs` 等 | 跳过宿主 `data_dir/LOCK` 与 reserve 占位；`info-log-dir` 白名单 | Shard-117 |
| 6 | `apply-raft-engine-readdir.py` | raft-engine `src/env/mod.rs`、`src/file_pipe_log/*` | `FileSystem` trait 增加 `read_dir`/`create_dir`（默认 fail-closed），目录列举改走 trait | Shard-116 |
| 7 | `apply-raft-engine-sync-fix.py` | raft-engine `src/engine.rs` | `Engine::sync()` 直连 `pipe_log.sync(LogQueue::Append)`（原实现经空 batch 提前返回为空操作） | Shard-118 |
| 8 | `apply-uuse-shutdown-sync.py` | TiKV `components/server/src/server.rs` | `run_impl` 保留 engines（信号线程用 clone）；`TiKV::stop()` 依次 `KvEngine::sync()` + `RaftEngine::sync()` | Shard-118 |
| 9 | `apply-uuse-apply-sync.py` | TiKV `components/raftstore/src/store/fsm/apply.rs` | apply 批次同步：有界窗口（5 ms，`sync_log_hint` 仍强制、shutdown 无条件同步），持久 applied 保持在 raft 日志压缩安全边界内 | Shard-118/120 |

> 说明：`apply-uuse-apply-sync.py` 生成窗口版（5 ms）。对已应用 always-sync 版的
> checkout，用 `apply-uuse-apply-sync-window.py` 做增量替换（Shard-120 trace 归因后
> 的优化：55k 屏障/60s → 5 ms 窗口按 apply 间隔合并）。

> 注：表 3 的 shared-engine 补丁假设 spike 前序补丁已插入
> `crocksdb_uuse_spike_env_create`（spike 版）文本；若 spike 脚本的插入文本与
> filesystem 补丁的替换锚点漂移（2026-10-10 重建实测），按锚点报错修正 spike
> 脚本的 `result->rep` 行（必须为 `rocksdb::NewCompositeEnv(fs).release()`）。

## 幂等与回退

- 每个脚本先备份原文件（`/build/spike-backup/<prefix>_<path>`），`patch()`
  断言锚点唯一（`anchor count != 1` 即失败），可重复执行；
- 回退：从 `/build/spike-backup/` 恢复对应文件并重建；raft-engine 改动需
  `cargo clean -p raft-engine --release` + 清 `.fingerprint/raft-engine-*` 后重建
  （cargo 不感知 git checkout 源码变化，Shard-118 实测）；
- 补丁只改 TiKV/raft-engine 侧接线，**不改 RocksDB/FIO/MySQL adapter 行为**，
  不改 core 语义（Shard-118 的 core 改动为 `src/core/data_path.cpp` 完成回调
  notify 位置，属独立 bug 修复，不在本目录）。

## Rebase 说明（版本升级时）

1. 锚点字符串基于冻结版本；升级 TiKV/raft-engine 后按 `anchor count` 报错定位，
   逐条更新锚点并重新验证 shim/生命周期；
2. rust-rocksdb 的 `c.cc`/`lib.rs` 改动集中在 `crocksdb_uuse_env_create` 与
   `Env::uuse`；若上游签名变化，保持 C ABI（`uuse_tikv_shim.h`）不变；
3. `UUSE_CORE_BUILD` / `LD_LIBRARY_PATH` 指向 UUSE 构建目录；shim 动态库
   `libuuse_tikv_shim.so` 由 UUSE 仓库构建（`uuse_tikv_shim` target）。

## 验证入口

- `tikv/run_single_node_vm.sh`：单节点 bring-up（PD + TiKV + TiDB）；
- `tikv/run_functional_matrix_vm.sh`：Shard-119 功能矩阵（26 项）；
- `tikv/run_concurrent_closure_vm.sh`：Shard-118 并发 closure（5 项）；
- `tikv/run_perf_ab_target_vm.sh`：Shard-120 同盘性能 A/B（sysbench）。
