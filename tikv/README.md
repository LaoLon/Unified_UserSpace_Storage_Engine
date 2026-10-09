# TiKV 集成（UUSE Phase 8）

本目录保存 UUSE ↔ TiKV 集成的可复现材料：TiKV fork 的补丁集、VM 内构建/部署脚本。
设计依据见 `doc/design/uuse.md` §21，执行计划见 `doc/shard/uuse_tidb_shard_plan.md`。

## 状态（Shard-113）

- 设计更新（§3.1/§3.2/§16.6/§17/§18/§19/§21）已完成；
- 本目录为骨架：构建脚本与部署脚本在 VM 内首次运行后回填实测命令；
- Shard-114（存储绕过点审计）、Shard-115（UUSE 侧 API 补齐）、Shard-116（shim +
  `UuseFileSystem`）尚未开始；在 shim 落地前，`run_single_node_vm.sh` 只是形状文档，
  不构成可运行路径。

## 接入方式

TiKV 的本地存储（kvdb/raftdb 两个 RocksDB 实例、`db/`、`raft/`、`snap/`、`import/`
与 `lock`）以 UUSE 作为文件存储：在 TiKV `components/file_system` 的 `FileSystem`
trait 上新增 `UuseFileSystem`（Rust + C ABI shim），由 `components/engine_rocks` 的
`RocksDBEnv` 适配到 rust-rocksdb `Env`。不是 POSIX/FUSE 层，也不修改 RocksDB 本体。

## 版本冻结（Gate 0）

- TiKV tag/commit：由 `TIKV_REF` 指定（未设置时构建脚本拒绝运行）；
- rust-rocksdb 依赖版本由 TiKV `Cargo.lock` 锁定，随 tag/commit 一并冻结；
- PD/TiDB server 使用与 TiKV 同系列版本，二进制与启动参数记录在本目录。

## VM 内构建

```bash
export TIKV_REF=<frozen-tag-or-commit>
bash tikv/build_tikv_vm.sh
```

- 源码：`/home/uuse/tikv-fork`；构建目录：`/home/uuse/tikv-build`；
- 依赖：Rust toolchain（冻结版本）、gcc/g++、cmake/make、libssl-dev、pkg-config；
  TiKV 源码必须在 VM 内通过 Git 获取与维护，禁止从 Windows 工作区同步；
- 网络受限时通过 `~/.cargo/config.toml` 配置 crates 镜像，不写入脚本。

## 运行（单节点 v1，见执行计划 §0.4）

- PD + TiDB server：.254；TiKV + UUSE：.253（独占 `0000:13:00.0`，以 sysfs 核对）；
- UUSE `.conf`：`config/uuse_tidb.conf`；正式口径为真实 NVMe + trace/stats off；
- 不占用 3FS 实验室已声明盘，不与 3FS client 性能口径重叠运行。

## 补丁集

`patches/` 为空（Shard-116 起按层加入）；补丁必须幂等、可单独回退，改动限定
FS 工厂 + shim + 独立 patch 目录。
