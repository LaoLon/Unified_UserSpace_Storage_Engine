# MinIO 集成（UUSE Phase 9）

本目录保存 UUSE × MinIO 集成的可复现材料：冻结版本记录、补丁栈（`patches/`）、
C ABI shim（`shim/`）、VM 内构建/部署/验收脚本。设计依据
`doc/design/uuse_minio_frontend.md`，接入点审计
`doc/review/uuse_minio_storage_audit_20261011.md`。

## 1. 冻结版本（Gate 0，2026-10-11）

| 组件 | 冻结值 | 说明 |
|---|---|---|
| MinIO | tag `RELEASE.2025-10-15T17-29-55Z`，commit `9e49d5e7a648f00e26f2246f4dc28e6b07f8c84a` | upstream 最后开源 tag（仓库 2026-04-25 归档 read-only） |
| Go | `1.24.0`（toolchain `go1.24.8`） | 冻结 commit 的 `go.mod` 要求 |
| 获取方式 | `git clone --depth 1 --branch RELEASE.2025-10-15T17-29-55Z https://github.com/minio/minio.git` | VM 内 clone；禁止从 Windows 工作区同步 |
| 许可 | AGPLv3 | 仅实验室内部使用/不分发（设计 §11 Q5） |

审计副本（2026-10-11 采集）：host2 `/build/minio-recon/minio`；采集脚本与日志
见 `temp_scripts/minio_audit_shard90.sh`、VM `/tmp/minio_audit_shard90.log`。

## 2. 状态（Shard-9.0 出口）

| 项 | 结果 |
|---|---|
| 版本冻结 | 完成（上表） |
| 接入点审计 | 完成：`doc/review/uuse_minio_storage_audit_20261011.md`（file:line 全清单） |
| seam 策略 | A 案（`internal/uuseos` 盘层抽象 + 函数级替换）由审计证据支持；B 案细节见审计 §5 |
| UUSE core 缺口 | 收敛为 shim 换算（容量/inode）+ `remove_all`（纯加法）；无 core 破坏性改动 |
| shim C ABI（Shard-9.1） | **完成**：`minio/shim/`（engine/client/file/dir + 容量/inode 统计）；smoke 14/14、lifecycle 8/8 全过（250 真机 `Nvme0n1`）；`uusectl check corruption=none` |
| Go 盘层抽象（Shard-9.2） | **完成**：`internal/uuseos` + `cmd` 补丁栈（`patches/apply-uuse-seam.py`，30 锚点，可重放）；端到端冒烟见 9.3 行（早期"E2E pass"实为宿主 I/O，已作废）|
| 单节点功能/崩溃矩阵（Shard-9.3） | **完成**：真实路由下功能 **21/21**（含 version-list 修复：`RenameData` 的 `xioutil.ReadFile` 未路由导致旧版本元数据读不到）；崩溃重跑（真实路径）写中 kill：**59/60 已 ACK 对象完整、两轮 sweep 一致、无损坏**；末对象 kill 边界丢失记为 **RPO 边界**待后续分析 |
| 两节点分布式（Shard-9.4） | **完成（真实引擎）**：跨节点/韧性/逐节点 namespace 对称核对全过；关键坑与修复见下 |
| **四节点分布式（Shard-9.4 扩展）** | **完成**：`.250–.253` 四节点集群初始化成功；**QUAD-CROSS-PASS**（PUT@251→GET@253 字节一致、8 小对象四节点轮转交叉读写、LIST@250/@252 一致）；**QUAD-RESILIENCE-PASS**（杀 .253：读经 .250 851968B OK、写经 .251 quorum 3/4 成功；.253 重入后复测通过）；**四节点功能矩阵 21/21**（尺寸/multipart/version/bigdir 1000/copy/stat）；**HOST-GATE-PASS**（strace 四台同时挂表、期间跑完整矩阵：`/root/uuse-data` 下成功文件操作数全部为 0，trace 181–294 行/台）；四节点宿主目录均 **0 文件**；统一运维脚本 `/root/uuse-cluster-up.sh`/`-down.sh` 已部署（自动识别运行时目录） |
| **RPO 边界（Shard-9.3/9.5 前置）** | **完成并修复**：根因 = uuseos **忽略 `O_DSYNC`**（审计 §5.5 定的"Close 时一次 sync"未实现），小 xl.meta 写入（`writeAllInternal`→`openFileSync`）不落 durable → kill -9 丢末对象；修复：O_DSYNC/O_SYNC → **Close 前 `Sync()` 屏障**（错误向上传播），顺带修 `O_APPEND` 语义（EOF 写）。复验：**全部 ACK 后 kill -9 → 60/60 存活**；**写入中途 kill → 45/45 存活、0 坏**；sweep 60/60 无半成品；矩阵 21/21 无回归。**RPO 边界 = 客户端 ACK 即 durable（单盘 quorum）；未 ACK 的尾部按覆盖语义保留上一已提交版本或不可见，绝不出现撕裂对象**（与 ext4+O_DSYNC 语义对齐） |
| **性能校核（Shard-9.5，进行中）** | **Round 1**：零填充裁剪 + freeze/encode 快路径 + DMA 非零填充（PUT 47%→≈62%）。**Round 2**：shape change 可回放（barrier p50 3.9ms→0.62ms；checkpoint 由每轮 1800+ full 变为 2 full + 3–4 delta）→ **PUT 达标**：ext4 871.7/869.1 vs UUSE **900.2/901.1 MiB/s = 103.3%/103.7% ✅**，PUT p50/p99 0.98x/0.93–0.98x ✅；**GET 未达标**：2721.9/2758.0 vs 1944.7/1938.8 MiB/s（71.4%/70.3%），GET p99 0.76x/0.78x ✅。**Round 3 归因**：GET 期间 CPU idle≈78%（非 CPU 瓶颈）；读路径探针纯读峰值 5270 MiB/s@8 线程，混合负载（1MiB+4KiB）下 4KiB 读 p50 143µs→811µs → **设备级队头阻塞**（小读排在 1 MiB 传输后）；ext4 元数据读命中页缓存、UUSE 无页缓存 → 每对象多 ~2 次设备小读；按 I/O 大小分 reactor 试验为负结果（1938→1790 MiB/s，已回退）。**Round 3 缺陷修复**：checkpoint 快照与 staged 记录的非原子窗口导致 shape/Alloc 记录二次应用（四节点滚动 kill -9 后 .251/.253 mount fail-closed）→ ①`saveSnapshot` 丢弃快照覆盖的 staged 记录；②replay 幂等化（CreateInode/RemoveEntry/RenameEntry/LinkEntry/FreeInode/Alloc）；③新增幂等回归用例；④`uusectl check` 回退先释放进程锁。修复后旧卷全部挂载成功且 namespace 完整（.minio.sys + 5 buckets）。**验证**：单节点+集群矩阵 21/21、崩溃矩阵 ACK 180/180 + sweep 60/60、`corruption=none`、CTest 29/29、zero-fill 探针 19/19、四节点重建集群健康 200 + 跨节点读 60/60。**当前 gate：PUT ✅（103%），GET ❌（≈71%），GET p99 ✅**；9.5 保持开放，Round 4 方向见归因报告 §12 |
| 虚拟 NVMe 停顿根因（2026-10-12 确认） | **已坐实：VMware 虚拟 NVMe 的 MSI-X 完成中断投递缺陷**——dmesg `I/O tag N QID M timeout, completion polled`（完成存在、中断未达，内核 30s 超时后轮询）；**`pci=nomsi` 对照实验决定性**：INTx 模式下 PUT 798 MiB/s、TTFB 17ms、**0 次内核超时**（对比 MSI-X 模式 5–35MiB/s、15.1s 均匀停顿）；已排除：补丁（stock 同样复现）、IOMMU（guest 无 vIOMMU）、磁盘（裸 fio 1718MiB/s/43.5k fsync IOPS）；**workaround 已应用**：.250 grub 加 `pci=nomsi`（原配置备份 `/etc/default/grub.bak-nomsi`，`update-grub`+重启生效；不影响 UUSE/SPDK 轮询路径）；根治需宿主侧（升级虚拟化层或换 PVSCSI/virtio-blk） |
| 环境操作备注（.250 驱动切换） | uio→内核 nvme：unbind → 清 `driver_override` → `remove`+`rescan`（直接 bind nvme 会 ENODEV）；nvme→uio：`modprobe uio_pci_generic` → unbind → 置 override → 直接 bind（重启后需重新 modprobe）；驱动反复切换后虚拟 NVMe 可能需重启 VM 恢复干净状态 |
| 虚拟 NVMe 停顿根因分析（2026-10-12） | 见上"虚拟 NVMe 停顿根因（已确认）"行；`pci=nomsi` 已写入 .250 grub（备份 `/etc/default/grub.bak-nomsi`，如需恢复 MSI-X 模式：还原备份 + `update-grub` + 重启） |
| 分布式接入的两个新坑（已修） | ①**MinIO 要求所有节点 `MINIO_*` 环境值完全一致**——`MINIO_UUSE_DRIVES` 里 conf 路径不同会拒启，统一为 `/etc/uuse-minio.conf`；②**旧版 wipe 工具无 durable flush**，删除不落盘导致旧 2 盘 format.json 残留（"already being used in another erasure deployment"），须用当前版 wipe（remove→precreate→Flush） |
| 真实路由验证（本轮关键修复） | **完成**：`ROUTE-PUT-GET-PASS`（PUT/GET 字节一致）+ 宿主目录为空 + namespace 含 `.minio.sys/buckets/{bigdir,matrix}`；`UUSE_GO_BRIDGE_PASS`、`BIG-PROBE-PASS`（4K/64K/1M/5M 写读一致）、`PERSISTENCE-PASS`（优雅重启后对象可读） |
| 本轮修掉的真实缺陷 | ①挂载钩子条件错误；②`engine_close` 不保证 namespace durable（新增 `Flush`）；③`O_TRUNC` 后 offset 未重置；④短写/短读未循环；⑤env 解析顺序敏感；⑥"声明即自动挂载"自愈；⑦尾部斜杠路径 EINVAL；⑧子目录枚举未加 "/"（LIST 全空）；⑨`reliableMkdirAll` 绕过收口层；⑩4 处 `OpenFile` 未路由；⑪`RenameData` 的 `xioutil.ReadFile` 未路由（多版本丢失）；⑫分布式 `oDirect=true` 时 `CopyAligned` panic；⑬构建事故（补丁失败被管道吞退出码）；**⑭uuseos 忽略 `O_DSYNC`（RPO 缺口：末对象丢失）→ Close 前 Sync**；**⑮`O_APPEND` 未实现（EOF 写）** |
| 构建验证 | 完成：Go 1.24.8（250/251）+ 251 `/build-nvme` 构建盘 + `go build -o minio-bin .`（注意：main 在仓库根，`./cmd/` 是库包）|
| Shard-9.5 归因与优化（2026-10-12） | **归因报告**：`doc/performance/minio_shard95_attribution.md`（trace 点位、分段 p50/p99、checkpoint 写放大与数据路径 zero-fill 证据、Round 2 shape 可回放、GET 差距分析、A/B、验证与下一步）。**Round 1 优化**：`src/core/client.cpp`（zero-fill 裁剪 + truncate 增长补零）、`src/metadata/{inode_map,namespace}.{hpp,cpp}`（freeze/encode 快路径）、`src/runtime/spdk_runtime.cpp`（DMA 非零填充）、`src/common/trace.hpp`+`src/core/engine.cpp`+`src/storage/journal.cpp`（append-only 归因点位）。**Round 2 优化**：`include/uuse/journal.hpp` + `src/storage/journal.cpp`（记录变长扩展传输）、`src/metadata/namespace.cpp`（形状记录 staging + replay，`CreateInode`/`RemoveEntry`/`RenameEntry`/`LinkEntry`/`FreeInode`）、`src/common/test_hooks.hpp`（`UUSE_TEST_FORCE_CHECKPOINT`）。**脚本**：构建 `temp_scripts/minio_shard95_build250.sh`、归因 `minio_shard95_attrib3.sh` + 解析器 `minio_shard95_trace_analyze.py`、A/B `ab6_250.sh`、zero-fill 探针 `minio_shard95_zerofill_probe.cc`、读路径探针 `minio_shard95_readprobe.cc`。**验证**：矩阵 21/21（单节点+集群）、崩溃矩阵 ACK 180/180 + sweep 60/60、`corruption=none`、CTest 29/29、zero-fill 探针 19/19、读探针峰值 5270 MiB/s、四节点滚动部署后跨节点读 60/60 |
| **9.6 验收补证（2026-10-12）** | **扩展功能矩阵 24/24**（新增 multipart 断点续传 `list-parts`+continue+complete 8 MiB 内容校验、multipart abort 后对象不可见、长路径 261 字符/12 层深目录 PUT+GET）——经 .250 端点、四节点集群；**bitrot 注入**（`temp_scripts/minio_shard96_poke_tool.go`，停止单节点 MinIO 后经 shim 覆写其 shard 4 KiB）：① 1/4 shard 损坏 → 经 .250 读 **60/60 内容正确**（bitrot 检出 + 纠删重建，无静默坏数据）；② 3/4 shard 损坏 → 读**拒绝**（`Resource requested is unreadable`，fail-closed ✅）；测试对象已删除、集群 59/59 正常；**xattr 面**：四节点全量负载日志中 xattr/setattr 调用 **0 次**（v1 fail-closed 路径未被触发，风险未materialize）；**磁盘满**：Round 2 实测 ENOSPC + MinIO `minimum free drive threshold` 拒绝写入（错误面干净、无损坏）；**未做**：`mc admin heal` 后修复验证（lab 无 `mc`/madmin）、inode 上限（需 1M inode，不实用） |
| 环境备注（2026-10-12） | ①CTest 设备测试（`file_api_test`/`journal_device_test`/`data_path_device_test`）用测试配置（`journal_mb=64/checkpoint_mb=128`）对同一 NVMe 做 `format`，之后必须用目标 conf `uusectl format --yes` 再 wipe/拉起，否则 mount 报 `stored geometry does not match`；②**每台节点的 `minio-wipe` 必须同版本**：.251 曾残留旧 wipe（md5 `244f3228…`，输出 `MINIO-WIPE-PASS` 缺 RESET），wipe 不彻底留下陈旧 `format.json`，导致新集群 bootstrap 死锁在 "waiting for all servers"；重新分发当前 wipe（`a76dac02…`）后一次成型；③`/tmp/uuse.lock` 被残留 engine 占用会让 scan-only check 失败（已在 `uusectl check` 回退路径修复：先 `stop()` 释放锁） |

## 3. 目录规划（随实现填充）

```text
minio/
├── README.md              # 本文件（冻结/构建/运行）
├── patches/               # 锚点补丁脚本 + 登记（§5；Shard-9.2 填充）
├── shim/                  # C ABI shim（uuse_minio_shim.*，已完成 Shard-9.1）
│   ├── uuse_minio_shim.h          # C ABI（engine/file/dir + 统计）
│   ├── uuse_minio_shim.cc         # 实现（每 .conf 弱注册表 + 共享 path-op client）
│   ├── uuse_minio_shim_smoke.cc   # 真机 smoke（14 项）
│   └── uuse_minio_shim_lifecycle.cc # 生命周期（8 用例）
├── internal/uuseos/       # Go 盘层抽象（Shard-9.2；cgo 桥 + 路由）
│   ├── cgo.go             # C ABI 绑定（engine/统计）
│   ├── uuse_file.go       # UUSE File 实现（7 方法）+ stat 转换
│   ├── uuse_paths.go      # 注册表路由 + ReadFile/Stat/MkdirAll/Remove/…
│   ├── dir_native_linux.go# 宿主路径的 stock getdents 枚举
│   ├── dir_native_other.go
│   ├── fallback.go        # 无 cgo 时纯 os 回退（编译可用）
│   └── uuseos_vm_test.go  # 真机桥接测试（UUSE_GO_BRIDGE_PASS）
├── build_minio_vm.sh      # Go 工具链 + minio 构建（Shard-9.2）
├── run_single_node_vm.sh  # 单节点冒烟（Shard-9.2）
├── run_cluster_vm.sh      # 4 节点集群（.250-.253，Shard-9.4）
├── run_functional_matrix_vm.sh
└── run_perf_ab_vm.sh      # 同盘 ext4↔UUSE A/B（Shard-9.5，≥95% gate）
```

### 3.1 shim 构建/运行（250 实测，2026-10-11）

```bash
# 250（构建机 /root/code，SPDK 就绪；新构建目录 build-minio）
cd /root/code && mkdir -p build-minio && cd build-minio
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target uuse_minio_shim uuse_minio_shim_smoke uuse_minio_shim_lifecycle -j 4

# 设备前置（一次性）：0000:13:00.0 → uio_pci_generic，vm.nr_hugepages=1024
# （脚本 temp_scripts/minio_prep_250.sh；会破坏该盘既有卷数据）

# 验收
./uusectl --config=/root/code/config/uuse_nvme.conf format --yes
export LD_LIBRARY_PATH=$PWD
./uuse_minio_shim_smoke     /root/code/config/uuse_nvme.conf /minio-shim-smoke
./uuse_minio_shim_lifecycle /root/code/config/uuse_nvme.conf /minio-shim-lifecycle.dat
./uusectl --config=/root/code/config/uuse_nvme.conf check | tail -1   # corruption=none
```

> 实测记录：smoke `MINIO_SHIM_SMOKE_PASS`（version `uuse-minio-shim/0.1 core-abi-3`）；
> lifecycle `MINIO_SHIM_LIFECYCLE_PASS`（8/8，含 fork fail-closed、dir 句柄保活、
> 容量统计 total≈free≈19.2 GiB、inode 统计 4/1000000）；`corruption=none`。
> 已知实现边界：`OpenFlags::Create` 不自动创建父目录（`resolveParent` fail-closed），
> MinIO 侧必须先 `mkdir_all`——与 MinIO 自身创建目录的逻辑一致。

### 3.2 MinIO fork 构建/冒烟（251 实测，2026-10-11）

```bash
# 打补丁（24 个锚点，幂等可重放；对冻结 tag 的干净树执行）
python3 minio/patches/apply-uuse-seam.py /build-nvme/minio-fork
gofmt -w cmd/xl-storage.go cmd/os-instrumented.go cmd/os_unix.go internal/uuseos/*.go

# 构建（注意：main 在仓库根 main.go，./cmd/ 是库包；cgo 外链需要 -luuse_core + rpath）
cd /build-nvme/minio-fork
CGO_ENABLED=1 CGO_LDFLAGS="-L$PWD/minio/shim -luuse_core" \
  go build -p 2 -ldflags "-r $PWD/minio/shim" -o minio-bin .
chmod +x minio-bin

# 运行（一台机一个 UUSE 盘；锚目录需先建好）
export MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin
export MINIO_UUSE_DRIVES="/root/uuse-data=/root/code/config/uuse_nvme.conf"
export LD_LIBRARY_PATH=$PWD/minio/shim
./minio-bin server /root/uuse-data --address :9000 --console-address :9001
```

> 实测记录（251，真机 `Nvme0n1`）：`MINIO_UUSE_E2E_PASS`（bucket/PUT/GET 字节一致/
> LIST/remove）+ `PERSISTENCE-PASS`（重启后对象可读）。运行前置：`0000:13:00.0`
> 绑 `uio_pci_generic`、`vm.nr_hugepages=1024`、锚目录 `/root/uuse-data` 存在。
> 注意事项：`/tmp` 与 `/build-nvme` 的 noexec 会拦 cgo 链接测试与产物执行
> （本环境 `/build-nvme` 已 remount `exec`）；`GOCACHE` 清过一次（曾缓存过
> “把 cmd 当库编译”的错误产物——用 `go build .` 而非 `./cmd/`）。

## 4. VM 内构建（Shard-9.2 起执行）

前置（2026-10-11 实测更新）：

- **Go 工具链已安装**：host2 `.251`（`/usr/local/go`，go1.24.8 linux/amd64）；
  环境变量持久化在 `/etc/profile.d/go-minio.sh`：
  `GOPATH=/build-nvme/gopath`、`GOMODCACHE=/build-nvme/gocache/mod`、
  `GOCACHE=/build-nvme/gocache/build`、`GOPROXY=https://goproxy.cn,direct`；
- **构建盘**：251 的 `nvme1n1`（空白盘，20G）格式化为 ext4（LABEL
  `minio-build`）挂载 `/build-nvme`（fstab 持久化）；Go 缓存 3.9G 在盘上，
  余量 15G——正式构建（含 cgo + 补丁）空间充足；
- **网络**：`proxy.golang.org` 在本 lab 不可达（下载卡死）；`goproxy.cn`
  与 `go.dev`/`github.com` 直连可用——Go 模块必须走 `goproxy.cn` 镜像；
- **源码基线**：`/build-nvme/minio-src`（冻结 tag 的完整拷贝，可作 Shard-9.2
  的 patch 工作树；原始浅克隆 `/build/minio-recon` 已清理）；
- **251 的 NVMe 备注**（探索记录）：`nvme0n1` 空白；`nvme1n1` = 构建盘（LABEL
  `minio-build`）；`nvme2n1` 含旧 MySQL 测试数据（303M，未挂载，**勿格式化**，
  保留为未来故障注入的现成裸盘数据；MinIO 测试用盘由 Shard-9.4 的 3FS 暂停
  流程统一处理）；
- **配置保底**：清理前从 `code-uuse` 保留了 `uuse_nvme.conf`/`uuse_nvme_m2.conf`
  到 `/root/code/config/minio-preserved/`；
- **根盘**：251 清理后剩 7.4G（57%）；250/253 清理后各剩 ~6.1G；252 清理后
  剩 7.0G（59%）；
- 3FS 暂停步骤见设计 §6.3（.250 hugepage 复位、.251 uio 绑定、4 盘 format）。

```bash
# 1) Go（已装，以下为复现步骤）
curl -fsSL -o /tmp/go1.24.8.tar.gz https://go.dev/dl/go1.24.8.linux-amd64.tar.gz
rm -rf /usr/local/go && tar -C /usr/local -xzf /tmp/go1.24.8.tar.gz
source /etc/profile.d/go-minio.sh

# 2) 冻结源码（工作树已就位：/build-nvme/minio-src）
git clone --depth 1 --branch RELEASE.2025-10-15T17-29-55Z \
  https://github.com/minio/minio.git /build-nvme/minio-src

# 3) 依赖预热（goproxy.cn；实测 ~2.1G 缓存）
cd /build-nvme/minio-src && go mod download all

# 4) 打补丁栈（Shard-9.2 起）
bash minio/patches/apply-all.sh /build-nvme/minio-src

# 5) 构建（cgo 链接 libuuse_minio_shim.so；低并行防 OOM）
cd /build-nvme/minio-src && CGO_ENABLED=1 go build -p 2 -o minio ./cmd/
```

> 构建验证记录（2026-10-11）：`go build -p 2 ./cmd/` rc=0，全量冷编译约
> 55 秒（缓存预热后增量 46 秒）；产出 71M 二进制。cgo+补丁构建留待 Shard-9.2。

运行（单节点冒烟）：

```bash
LD_LIBRARY_PATH=/root/code/build-uuse \
  ./minio server /mnt/uuse1/data \
  --address :9000 --console-address :9001
```

## 5. 补丁栈登记（Shard-9.2 起填充，2026-10-12 定稿）

单一补丁脚本 `patches/apply-uuse-seam.py`（锚点字符串替换，幂等可重放；对
冻结 tag `RELEASE.2025-10-15T17-29-55Z` 的干净树执行；缺锚点或非唯一即失败）。
共 **4 个文件、39 处替换** + 每文件 import 注入（`internal/uuseos`）：

| 文件 | 替换数 | 内容（锚点） |
|---|---|---|
| `cmd/xl-storage.go` | 26 | `openFileDirect/openFileSync/openFile` 三出口句柄化；`ReadFile`/`ReadFileStream`/`readMetadataWithDMTime`/`readAllDataWithDMTime`/`bitrotVerify`/`writeAllDirect` open 路由；`writeAllDirect` 的 `CopyAligned` 原生句柄保护与 UUSE 分支；write 句柄类型；healing 标记读、`format.json` 读、`getDiskInfo`、`ListDir`；`RenameData`（含 legacy）dst xl.meta 读路由；engine 挂载钩子（`newXLStorage` 早于任何盘访问） |
| `cmd/os-instrumented.go` | 7 | `RemoveAll`、`MkdirAll`、`Rename`、`Lstat`、`Remove`、`Stat`、`Mkdir` 分流 |
| `cmd/os_unix.go` | 4 | `readDirFn`、`readDirWithOpts`、`osMkdirAll`、`access` 分流 |
| `cmd/metacache-walk.go` | 2 | 目录项元数据读、legacy 元数据读路由 |

- 句柄抽象（`internal/uuseos`，仓库内新增）：`File` 接口
  `Read/ReadAt/Write/Seek/Truncate/Stat/Sync/Close`（drop-in，含 `Fd()`/`Name()`）；
  `uuse_paths.go` 注册表路由 + `ReadFile`/`Stat`/`MkdirAll`/`Remove`/`Rename` 分流；
  `dir_native_linux.go` 宿主路径枚举；`fallback.go` 无 cgo 回退；
- 应用步骤（严格退出码）：
  `python3 patches/apply-uuse-seam.py <minio-tree>` →
  `gofmt -w cmd/xl-storage.go cmd/os-instrumented.go cmd/os_unix.go cmd/metacache-walk.go internal/uuseos/*.go`；
- 构建：`CGO_ENABLED=1 CGO_LDFLAGS="-L<shim-dir> -luuse_core" go build -p 2 -ldflags "-r <shim-dir>" -o minio-bin .`
  （**main 在仓库根**，`./cmd/` 是库包）；
- 上游已归档，无 rebase 需求；改动与上游文件分离登记（本表 + 脚本内锚点注释）。

## 6. 支持边界（Shard-9.6 冻结，2026-10-12）

**支持范围**

- 拓扑：4 节点单 erasure set（.250/.251/.252/.253，每节点 1 个 UUSE 盘，单盘
  `Nvme0n1`@`0000:13:00.0`，per-node `.conf`；`MINIO_UUSE_DRIVES` 全节点一致）；
  单节点（1 盘）用于开发/冒烟与同盘 A/B，不作为分布式验收替代；
- MinIO 版本：`RELEASE.2025-10-15T17-29-55Z`（Go 1.24.8）；补丁栈见 §5；
- 已验收能力：S3 基本对象/尺寸（0–12 MiB+）、multipart、range、versioning、
  bigdir 列举（1000+）、copy、remove、stat（矩阵 21/21，单节点与四节点）；
  跨节点读写、单节点宕机韧性、集群 heal、滚动重启、kill -9 恢复（ACK 即
  durable）、四节点同时 strace 无宿主落盘（HOST-GATE-PASS）；
- 性能（2026-10-12 同盘 A/B，8s 腿×2×2、INTx、warp 回环 QD16 1 MiB、errors=0）：
  **PUT 103.3%/103.7%（gate ✅）**、PUT p50 0.98x、p99 0.93–0.98x；
  **GET 71.4%/70.3%（gate ❌，开放）**、GET p99 0.76x/0.78x（✅）；
  分布式 A/B gate 尚未按口径测量（网络受限，需同时报告绝对值）。

**已知边界（记录，不作豁免）**

- **UUSE 无宿主页缓存**：GET 冷读行为与 ext4 不同（ext4 元数据读命中页缓存）；
  实测 GET 差距主要来自**设备对小读的队头阻塞**（4 KiB 读排在 1 MiB 传输后）
  与每对象多次小读，见 `doc/performance/minio_shard95_attribution.md` §10/§12；
- xattr（盘级写/删计数）v1 fail-closed + 日志（决策 Q2 未启用持久计数）；
- `flock` 语义由 engine 进程锁覆盖（`.minio.sys` 锁文件不做真 flock）；
- 每节点 1 盘（单 erasure set 的最小拓扑）；4 盘/节点需要先按 §6.2 收敛
  `dma_arena_mb` 等资源预算；
- `.minio.sys/batch-jobs` 的 `dir_open` ENOENT 日志噪音（与 stock 未对照，
  疑似 benign）；
- 上游归档：无安全修复，仅实验室内部分发（AGPLv3，决策 Q4）。

**禁用项（fail-closed）**：gateway/NAS/S3-over-S3、SSE/KMS、ILM tier、
Kafka/NATS 事件、mmap、多进程共享同一 UUSE 卷、宿主路径写 datadir。

**未完成（9.5 开放项）**：GET 吞吐 gate；分布式 A/B gate；验收矩阵缺证据项
（bitrot 静默位翻转注入、`mc admin heal --dry-run`、multipart 断点续传/abort、
超长路径、inode 上限错误面、xattr/KMS/ILM fail-closed 行为验证）。
