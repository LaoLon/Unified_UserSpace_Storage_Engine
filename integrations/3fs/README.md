# 3FS 集成（UUSE 本地引擎适配）

本目录保存 UUSE ↔ 3FS 集成的可复现材料：3FS fork 的补丁集、实验室构建流程和
运行说明。设计依据见 `doc/design/uuse_3fs_local_engine.md`。

## 3FS 源码与版本

- 上游：`https://github.com/deepseek-ai/3FS.git`
- 部署基线 commit：`22fca04`（ReleaseVersion `250228-dev-1-999999-22fca045`）
- 源码必须在 VM 内通过 Git 获取与维护；Windows 工作区不承载 3FS 源码本体。

## 实验室构建环境（.251，Ubuntu 26.04）

| 组件 | 版本/说明 |
|---|---|
| clang | `clang-17` 17.0.6（CMake 主编译器） |
| gcc | `gcc-12`/`g++-12` 12.5.0（Rust workspace 编译器 + clang `--gcc-install-dir` 目标） |
| rustc/cargo | apt `rustc`/`cargo` 1.93（构建 chunk_engine workspace） |
| cmake/ninja | 系统版本；CMake 4.x 需 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` |
| 依赖 | `build-essential gcc-12 g++-12 libstdc++-12-dev clang-17 clang-format-17 cmake ninja-build pkg-config libuv1-dev liblz4-dev liblzma-dev libdouble-conversion-dev libdwarf-dev libunwind-dev libaio-dev libgflags-dev libgoogle-glog-dev libgtest-dev libgmock-dev libgoogle-perftools-dev libssl-dev libboost-all-dev libfuse3-dev libibverbs-dev libnuma-dev python3-pip rustc cargo` |
| FDB 客户端 | `foundationdb-clients_7.3.77-1_amd64.deb`（`/usr/lib/libfdb_c.so`） |
| 构建盘 | `/build` = VMware `NVME_0001`，ext4 label `build` |

## 构建流程

1. 克隆并检出基线，拉取 submodule，应用上游补丁：

   ```bash
   git clone https://github.com/deepseek-ai/3FS.git /build/3fs
   cd /build/3fs && git checkout 22fca04
   git submodule update --init --recursive
   bash ./patches/apply.sh          # 上游 rocksdb/folly 补丁
   bash integrations/3fs/patches/apply-lab-compat.sh
   ```

2. Rust workspace（chunk_engine）：

   ```bash
   cd /build/3fs
   CC=gcc-12 CXX=g++-12 cargo build --release
   ```

3. CMake 配置（clang-17）：

   ```bash
   cmake -S . -B build \
     -DCMAKE_C_COMPILER=clang-17 -DCMAKE_CXX_COMPILER=clang++-17 \
     -DCMAKE_C_FLAGS="--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/12" \
     -DCMAKE_CXX_FLAGS="--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/12" \
     -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG -Wno-error" \
     -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG -Wno-error" \
     -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
     -DSHUFFLE_METHOD=g++11 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
     -DCMAKE_EXE_LINKER_FLAGS="-Wl,--allow-shlib-undefined" \
     -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--allow-shlib-undefined"
   ```

4. 构建目标（首次构建中 Arrow 的 bundled deps 需要 `build-fixes.sh` 修复后重试；
   构建环境必须导出 gcc-12、实验室代理与 CMake 4 的 policy 放宽变量，否则
   vendored RocksDB 会用 gcc 15 编译失败、Arrow 下载会挂起、旧版 bundled deps
   会 configure 失败）：

   ```bash
   export CC=gcc-12 CXX=g++-12
   export http_proxy=http://192.168.1.105:12450 https_proxy=http://192.168.1.105:12450
   export CMAKE_POLICY_VERSION_MINIMUM=3.5
   cmake --build build -j3 --target meta_main mgmtd_main storage_main \
     hf3fs_fuse_main admin_cli monitor_collector_main
   ```

## 补丁集

- `patches/apply-lab-compat.sh`：源码级兼容补丁（clang-17 flags、Boost 1.90
  Boost.System 移除、SerDeser trivially-copyable、UsrbIo/Uuid/ResyncWorker uuid
  API、liburing `open_how` 兼容、clang-format 回退）。
- `patches/build-fixes.sh`：构建期修复（Arrow bundled deps 的
  `cmake_minimum_required` 提升、thrift 0.16.0 tarball 下载），失败后重跑构建。
- `patches/apply-uuse-chunkstore.sh`：安装 M2.1 的 `UuseChunkStore` 核心与
  smoke 目标（源码在 `uuse/`）；含 M2.2 读路径与 M3.1 sync 路径修复。
- `patches/apply-uuse-syncpaths.sh`：M3.1 sync/recycle 路径的 UUSE 分支
  （`getAllMetadata`/`getAllMetadataMap`/`reportUnrecycledSize`/`addChunkSize`/
  migrate 守卫/punchHole 接线）。独立脚本，可补装到旧版本安装的源码树（幂等）。
  注意：`UuseChunkStore` 的 slot 池为 16 文件 × 128 槽/类（6144 槽）；回收依赖
  本补丁的 punchHole 接线 + journal `reserved2` 持久化的 recycleState。
- `patches/apply-uuse-oplogs.sh`：UUSE 后端每 op 成功日志（`uuse update ok` /
  `uuse commit ok` / `uuse read ok`，含 chunk/ver/len），用于 op 级追踪对账。
- 注意：`uuse_chunkstore_smoke` 目标构建需 `CC=gcc-12 CXX=g++-12`，否则 cargo
  指纹失配会触发 Rust 重编（用默认 c++ 编译 vendored RocksDB 失败）。

## M2.1/M2.2 UuseChunkStore（核心已通过，接线已构建）

- 源码：`uuse/UuseChunkStore.{h,cc}`、`uuse/UuseChunkStoreSmoke.cc`、
  `uuse/CMake.uuse-chunkstore.cmake`。
- 安装（含 M2.2 的 `StorageTarget` 第三后端接线：PhysicalConfig
  `use_uuse_engine/uuse_conf`、create/load/update/commit/query 分支）：

  ```bash
  bash integrations/3fs/patches/apply-uuse-chunkstore.sh /build/3fs
  cd /build/3fs && cmake -S . -B build
  cmake --build build -j3 --target storage_main uuse_chunkstore_smoke
  ```

- 运行（目标 NVMe 绑 uio + hugepages；.251 用 `dma_arena_mb = 256` 的配置）：

  ```bash
  uuse_chunkstore_smoke <uuse.conf> /uuse-store/t<targetId> 1   # 写入+COW+删除+query+update/commit+sync
  uuse_chunkstore_smoke <uuse.conf> /uuse-store/t<targetId> 2   # 新进程重放 journal 校验恢复
  ```

- 实测（2026-10-05，.251 NVME_0002）：`SMOKE1_PASS live=3 free=189 used=4784128`、
  `SMOKE2_PASS live=3 free=189 used=4784128`（64K/512K/4M 写入+CRC、COW 版本递增、
  update/commit 语义（自动递增版本、陈旧版本拒绝、commit 持久化）、删除不复活、
  journal 重放守恒）。
- 接线状态（2026-10-05）：`storage_main` 已带 UUSE 后端构建通过；UUSE target 由
  `target.toml` 的 `use_uuse_engine = true` + `uuse_conf` 选择，数据落在 UUSE
  卷的 `/uuse-store/t<targetId>`；读路径已在 `AioStatus::collect` 内联直读
  （无 fd，替代 io_uring；`aioPrepareRead` 准备元数据，`readUuseChunk` 读入
  `localbuf`）。服务级验证：新 `storage_main` 同时加载 2 个 chunk_engine
  target + 1 个 UUSE target（日志 `uuse store loaded target 101000100103`，
  `Load storage target TargetId(101000100103)`），稳定运行后恢复 systemd 服务。
- M2.3 单链 E2E 已通过（2026-10-05）：客户端 4/16MiB 写读 md5 全对、UUSE 用量
  精确吻合（25,755,648）、SIGKILL 重启后读取一致；链路由通过上传
  ChainTableId 1 的新版本（含 UUSE chain）实现。
- 待做：`StorageTargets::create` 路径（admin `create-target`）尚未带 UUSE 选项；
  读路径异步化（`Client::submitRead`）。

## 部署说明

构建产物经 `strip` 后分发到各节点 `/opt/3fs/bin`（lab 打包脚本见
`temp_scripts/lab_3fs_package.sh`）。M2 的 `UuseChunkStore` 后端改动将以新的
patch 文件加入本目录，并在单 storage 节点以 `engine_type` 开关做 A/B。
