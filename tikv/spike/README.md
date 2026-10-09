# Shard-116 spike：crocksdb `Env(fs)` 注入链路

状态：**已通过（2026-10-09，host4）**。

## 目的

在写真正的 UUSE FileSystem 之前，先验证 TiKV RocksDB 侧的注入链路：

```
TiKV/engine_rocks (Rust)
  └─ Env::uuse_spike(base_path)
      └─ crocksdb FFI crocksdb_uuse_spike_env_create(base_path)
          └─ rocksdb::NewCompositeEnv(UuseSpikeFileSystem)
              └─ rocksdb::FileSystemWrapper（路径前缀映射 → 默认 POSIX FS）
```

## 材料

- `apply-spike-env-ffi.py`：对 rust-rocksdb cargo checkout（`64a092f`）就地打补丁：
  - `librocksdb_sys/crocksdb/crocksdb/c.h`：声明 `crocksdb_uuse_spike_env_create`；
  - `librocksdb_sys/crocksdb/c.cc`：`UuseSpikeFileSystem : rocksdb::FileSystemWrapper`
    （17 个 override 做路径映射；`Name()` 必须实现）+ `NewCompositeEnv(fs)`；
  - `librocksdb_sys/src/lib.rs`：FFI 声明；
  - `src/rocksdb.rs`：`Env::uuse_spike(base_path)`；
- `uuse_env_spike.rs`：TiKV `components/engine_rocks/examples/` 示例（v0.3 API：
  `DBOptions`、`Writable`、`FlushOptions`、`DBVector`）。

> 说明：spike 采用 checkout 就地补丁（原文件备份在 VM `/build/spike-backup/`）；
> Shard-116 正式实现改为独立 fork + `tikv/patches/` 有序补丁栈。

## 构建与运行（host4）

```bash
source /build/tikv-env.sh          # 代理、gcc-12、rustup、CMAKE_POLICY_VERSION_MINIMUM
cd /build/tikv-fork
CARGO_TARGET_DIR=/build/tikv-build \
  cargo rustc -p engine_rocks --release --example uuse_env_spike --jobs 2 \
  -- -C link-arg=-luring           # RocksDB io_uring 需要 -luring（与 tikv-server 相同）
mkdir -p /build/uuse-spike-root
/build/tikv-build/release/examples/uuse_env_spike /build/uuse-spike-root /spike-db
```

## 结果（2026-10-09）

```text
spike ok: mapped dir /build/uuse-spike-root/spike-db
files: ["000008.log", "000009.sst", "CURRENT", "IDENTITY", "LOCK",
        "MANIFEST-000005", "OPTIONS-000007"]
spike rc=0
```

结论：

1. `crocksdb Env(fs)` 构造**足够**（`NewCompositeEnv` 即可，不需要内部
   `CompositeEnvWrapper` 头文件）；
2. 自定义 `FileSystem` 确实接管了 RocksDB 的全部文件 I/O（SST/WAL/MANIFEST/
   CURRENT/OPTIONS/LOCK/IDENTITY 都落在映射目录）；
3. TiKV workspace 能编译并运行该 Env（`engine_rocks` example）；
4. 踩坑记录：`rocksdb::Env` 在 8.10.2 仍是抽象类（必须 `NewCompositeEnv`）；
   `Customizable::Name()` 必须实现；旧版 rust-rocksdb API（`DBOptions`/`Writable`/
   `DBVector`/`FlushOptions`）；最终链接需要 `-luring`。

## 下一步（Shard-116 主体）

- 把 `UuseSpikeFileSystem` 替换为真正的 UUSE FileSystem（复用/适配
  `rocksdb/uuse_filesystem.cpp` 到 8.10.2 头文件 + C 回调桥）；
- raft-engine `UuseFileSystem`（Rust `FileSystem` trait）+ `ManagedFileSystem`
  base 配置选择；
- 正式 fork rust-rocksdb 并把补丁整理为 `tikv/patches/` 有序补丁栈。
