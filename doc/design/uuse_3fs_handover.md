# UUSE 3FS 本地引擎 — 交接文档（2026-10-07）

> 面向新会话接手者。事实源：本文 + `doc/design/uuse_3fs_local_engine.md`（M1–M3 全部
> 结论与证据链）+ 各 `integrations/3fs/patches/*` 补丁。先读本文，再按需查设计文档。

---

## 1. 环境现状（5 台 VM）

| VM | 角色 | 运行内容 | 备注 |
|---|---|---|---|
| **.254** | 管理节点 | FDB + `mgmtd_main` + `meta_main` | 不挂载文件系统；admin_cli 入口 |
| **.251** | 存储节点 10001 | **手动启动的重建版 `storage_main`**（非 systemd） | stock target 101/102/107（`/storage/data1` XFS，NVME_0000）+ **UUSE target 108**（裸盘 NVME_0002） |
| **.250** | 存储节点 10002 | stock `storage_main`（systemd） | target 201/202/207/208（`/storage/data1` XFS，NVME_0000） |
| **.252** | 客户端 | `hf3fs_fuse_main`，挂载 `/3fs/uuse` | 基准测试客户端 |
| **.253** | 客户端 | `hf3fs_fuse_main`，挂载 `/3fs/uuse` | 基准测试客户端 |

磁盘（两台存储相同）：NVME_0000 = `/storage/data1`（XFS，stock 用）；
NVME_0001 = `/build`（.251 构建盘）；NVME_0002（BDF `0000:13:00.0`）= UUSE 裸盘（.251）。

### 链与路由（RF2，副本跨节点）

| 链 | 副本 | 状态 | 用途 |
|---|---|---|---|
| **900100004** | 101000200107(.250 stock) + 101000100107(.251 stock) | SERVING | **原生对照链** |
| **900100005** | 101000200108(.250 stock) + **101000100108(.251 UUSE)** | SERVING | **UUSE 对照链** |
| 900100001/2/3 | .250 侧 OFFLINE | SERVING(1/2) | 遗留降级链，未使用 |

- 路由表 1 当前指向 **900100005**（切换命令见 §3）。
- 客户端/存储传输：`force_use_tcp = true`（客户端两处配置 + 存储 launcher/main），
  客户端 `max_connections = 8`（为高并发调的）。
- 存储 `sync_when_write = true`（已从 false 实验恢复）。

---

## 2. 快速自检（2 分钟）

```bash
# 链状态（.254）
export LD_LIBRARY_PATH=/opt/3fs/lib; CFG=/opt/3fs/etc/admin_cli.toml
TOKEN=$(cat /opt/3fs/etc/token.txt)
/opt/3fs/bin/admin_cli -cfg $CFG --config.user_info.token $TOKEN list-chains

# 客户端探针（.252/.253）
echo p > /3fs/uuse/test/p.txt && cat /3fs/uuse/test/p.txt && rm -f /3fs/uuse/test/p.txt

# .251 storage 是否活着
pgrep -a -f 'build/bin/storage_main'   # 应有进程；日志 /var/log/3fs/storage_main.log
grep -a 'uuse store loaded' /var/log/3fs/storage_main.log | tail -1
# 应显示：uuse store loaded target 101000100108 at /uuse-store/t101000100108
```

---

## 3. 常用操作（脚本都在 `temp_scripts/`，上传到对应 VM 的 /tmp）

**执行方式**（在 `E:\Code\HFS_BASE_SPDK\temp_scripts` 下）：

```powershell
node vm.mjs --vm host1 put <本地文件> <VM路径>   # host1=.250  host2=.251  host3=.252  host4=.253  host5=.254
node vm.mjs --vm host5 exec 'bash /tmp/xxx.sh'
```

连接信息见 `E:\Code\workspace.yaml`；临时脚本统一放 `temp_scripts/`。

| 操作 | 命令（在哪个 VM） |
|---|---|
| 重启 .251 手动 storage | `bash /tmp/uuse251_trace_restart.sh`（.251） |
| 切换路由表 1 | `bash /tmp/uuse254_route_chain.sh 900100004`（或 900100005）（.254） |
| 客户端 TCP/RDMA 切换 | `bash /tmp/uuse_set_transport.sh true|false`（.252/.253） |
| 客户端硬重启（清残留挂载） | `bash /tmp/uuse_fuse_restart.sh`（.252/.253） |
| 客户端连接池 | `bash /tmp/uuse_client_tune.sh 8`（.252/.253） |
| .251 UUSE store 复位 | `bash /tmp/uuse251_store_reset.sh`（.251，会格式化 UUSE 卷！） |
| 全集群重置 | `bash /tmp/uuse_cluster_reset.sh`（.254）→ 重建链 → **两台存储重启** |
| sync_when_write 切换 | `bash /tmp/uuse_storage_sync.sh true|false`（.250）/ `uuse251_storage_sync.sh`（.251） |
| 基准：读写 A/B 轮 | `bash /tmp/uuse_ab_round.sh <tag> c252 c253 <epoch>`（双客户端同步启动） |
| 基准：写 A/B | `bash /tmp/uuse_write_ab.sh <tag> c252 <epoch>` |
| 基准：读（新文件） | `bash /tmp/uuse_read2.sh <tag> c252 c253 <epoch>` |
| op 诊断 | `bash /tmp/uuse251_opdiag.sh` / `uuse251_burst_diag.sh`（.251） |

> **双客户端同步启动**：先算 `START=$(( $(date +%s) + 50 ))`，两个客户端都用同一个
> epoch（脚本会等到该时刻再开始）。

---

## 4. 已完成工作（按时间）

### M1–M3（详见设计文档）
- M1 chunk 形状探针 + `UuseChunkStore` PoC（slot+journal+COW+CRC+跨进程恢复）。
- M2 3FS 集成：`StorageTarget` 第三后端（`use_uuse_engine` + `uuse_conf`）；
  `storage_main` 重建；单链 E2E（md5 精确 + SIGKILL 恢复）。
- M3 RF2 双节点验证 + 正式 A/B（早期单客户端 TCP：写持平、读 16.5 vs 21 MiB/s）。
- **尾丢失根因与修复**：UUSE target 的 sync/resync 路径缺失 → storage SIGSEGV
  （`getAllMetadata` 等落 chunkStore_ 空指针）→ 修复 `apply-uuse-syncpaths.sh`；
  slot 池回收未接线（`punchHole` no-op）+ meta 回收滞后 → 复现尾丢失（cp 16MiB→15MiB）
  → 修复 `punchHole`/内联回收/journal 持久化 + slot 池按类扩容（`kClassFiles={8,8,24,8}`，
  6144 槽，1MiB 类 3072 槽）。80 轮 5.1GB 持续写 320/320 全过。
- op 成功日志（`apply-uuse-oplogs.sh`）：`uuse update ok` / `commit ok` / `read ok`。

### 本轮（2026-10-06/07）
- **性能矩阵**（顺序/随机 × 读/写 × 32/64，双客户端、TCP、交叉读）：
  UUSE 与原生全维度持平（±5-15%）。
- **高并发环境修复**：存储间转发 Soft-RoCE → TCP；客户端 `max_connections` 1→8；
  卡死会话/通道必须全集群重置 + 双存储重启才能清（单组件重启无效）。
- **异步优化（worker-direct，Shard-106）**：数据面从"单 I/O 线程 + 锁内 I/O"改为
  **per-thread client 池**（owner client 持文件、线程自建 client `submitRead/Write` +
  本线程 poll）；`update` 三阶段（锁内规划 → 锁外 I/O → 锁内 journal+发布）；
  submit 失败回落同步门面。SMOKE1/2_PASS；本地单线程 1MiB 读 4.5GB/s；
  3FS 级无回归。提交 `ff63002`。
- **sync_when_write=false 对照（否证 fsync 假设）**：写吞吐无提升
  （原生 103.2 vs 102.5；UUSE 96.9 vs 102.1）。提交 `a4f1e84`。

### Git 状态
- 最新提交：`a4f1e84`（本会话链：`54e130c`→`a672398`→`409b688`→`7c17c5a`→
  `b97f511`→`7509e87`→`ff63002`→`a4f1e84`）。
- 关键路径：`integrations/3fs/patches/`（一键安装 + syncpaths + oplogs）、
  `integrations/3fs/uuse/`（UuseChunkStore 源码 + smoke）、`doc/design/uuse_3fs_local_engine.md`。
- 注意：Windows 仓库目录仍是 `E:\Code\HFS_BASE_SPDK`（改名 UUSE 事项 pending）。

---

## 5. 待办 / 下一步（新会话建议按此顺序）

1. **写吞吐上限定位（最高优先级）**：~50 MiB/s/客户端 ≈ 50 ops/s（1MiB），
   32≈64 并发持平、单流即饱和、关 fsync 无变化 → 指向**更新流水线的 in-flight
   深度 × 单 op 延迟**。建议：
   - 查客户端 `updateChannelAllocator` channel 池大小（每条链 in-flight 上限）；
   - 查 chunk engine 提交批处理间隔（是否定时器批处理锁定单 op 延迟）；
   - 用单 op 计时（一次 1MiB 写 RTT）验证延迟归属。
2. **UUSE 读路径的 3FS 级验证**：客户端读路由只落 stock 侧（UUSE `read_ok=0`）。
   需要把 UUSE 副本设为 preferred/HEAD（`SetPreferredTargetOrder` /
   `RotateAsPreferredOrder` 等 admin 命令），再跑读矩阵并核对 `uuse read ok` 计数。
3. **UUSE 耐久性对齐**：UUSE journal append 无 per-op fsync（进程崩溃安全、掉电可能
   丢最近记录）；与 3FS `sync_when_write` 语义对齐方案待定。
4. **UUSE 配置调优**：`data_reactors = 1`（异步服务单分片）→ 可提到 4
   （`max_data_reactors=4`），压 UUSE 读路径时可能需要。
5. **清理遗留**：900100001/2/3 的离线 target（.250 `/storage/data1` 已用 68%）；
   以及本会话的测试目录 `/3fs/uuse/bm`、`bm2`（各 128 个 16MiB 文件）。
6. （可选）把性能矩阵/交接结论汇总进 `doc/performance/`。

---

## 6. 坑与注意事项（必读）

1. **UUSE 引擎整盘独占**：一个 storage 进程最多 **1 个 UUSE target**。两个 UUSE
   target 同时加载 → 第二个 `uuse store open failed` → **storage 启动中止**。
2. **卡死通道/会话**：kill 掉的压测会在存储侧留下锁死的更新通道（`Channel is
   locked` 风暴、链同步卡死）——只有**全集群重置（清 FDB + 重建 targets/chains）+
   两台存储重启**能清；单重启无效。
3. **集群重置后必须重启两台存储**，否则客户端报 `Remote I/O error`（routing 过期）。
4. **.251 storage 是手动进程**：VM 重启后需 `uuse251_trace_restart.sh` 拉起；
   `systemctl` 里的 storage 单元在 .251 上是停用状态。
5. **客户端配置有两份**：`hf3fs_fuse_main_launcher.toml` + `hf3fs_fuse_main.toml`
   （后者覆盖），改传输/连接池要改**两处**；改完硬重启（`uuse_fuse_restart.sh`）。
6. **UUSE store 格式化**（`uusectl format` / `uuse251_store_reset.sh`）会清空
   整个 UUSE 卷（该盘上所有 store）。
7. **凭据**：token 在 `/opt/3fs/etc/token.txt`，**不得**打印/入库。
8. 日志：`.251/.250` `/var/log/3fs/storage_main.log`（含 `uuse update/commit/read ok`
   op 日志，可用于对账）；客户端 `/var/log/3fs/hf3fs_fuse_main.log`。
9. 基准口径：**TCP、双客户端并发、交叉读（读对方文件）+ drop_caches**；
   顺序 1MiB / 随机 4KiB；每格 20s；读缓存会污染结果（顺序读 32 并发曾出现缓存假象）。

---

## 7. 环境上限（当前实验室口径，双客户端聚合）

| 维度 | 数值 | 备注 |
|---|---|---|
| 顺序写 | ~100 MiB/s | 32 并发即饱和（单流也接近） |
| 随机写 4K | ~300 IOPS | |
| 顺序读 1M | ~20 MiB/s | 冷读（交叉读+drop_caches） |
| 随机读 4K | ~700-750 IOPS | |
| UUSE 本地后端 | 单线程 1MiB 读 4.5 GB/s | 远高于 3FS 链路，不是瓶颈 |

**UUSE ≈ 原生（全维度持平）**；瓶颈在 3FS 更新流水线（见待办 1）。
