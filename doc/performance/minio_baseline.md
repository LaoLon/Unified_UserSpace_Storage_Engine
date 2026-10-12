# UUSE × MinIO 单节点性能基线（2026-10-12）

> 范围：**单存储节点（.250，1 盘）+ 单客户端（同机回环）**，同一块盘上 ext4 ↔ UUSE
> **交错 A/B**，warp 8s 腿 ×2 轮。
> 性质：**性能快照基线（baseline）**，不是 gate——gate 已由用户 2026-10-12 决定冻结，
> 归因、冻结依据与重开条件见 `doc/performance/minio_shard95_attribution.md` §12/§15。
> 配套：分布式/多客户端口径见 `minio/README.md` §2（2+2 拓扑与 4 节点集群）。

## 1. 环境与构建

| 项目 | 值 |
|---|---|
| VM | 6 vCPU / 7.4 GiB RAM / `HugePages_Total=1024`；内核 `7.0.0-38-generic` |
| 目标设备 | `VMware NVME_0002`，PCI `0000:13:00.0`（INTx，grub `pci=nomsi`）；内核态 `/dev/nvme2n1`；UUSE 侧 `Nvme0n1`（`uio_pci_generic`） |
| MinIO | fork 二进制 md5 `95f327a4…`；上游基线 `RELEASE.2025-10-15T17-29-55Z`（`9e49d5e7…`，Go 1.24.8）；补丁栈 `minio/patches/`（4 文件、39 锚点、可重放） |
| UUSE | core `b2a96a30…`、minio shim `b8962e6b…`（构建自提交 `b3e4866`）；SPDK `f0b63ed866f0e4d3c56e73bd4a98d0042956147a` |
| UUSE conf | `/etc/uuse-minio.conf`（md5 `82a921a6…`）：`env_driver=uio`、`hugepages=1024`、`metadata/devices=nvme0`、`metadata_block_size=4096`、`journal_mb=256`、`checkpoint_mb=512`、`extent_mb=1`、`prealloc_extents=64`、`max_extents_per_file=65536` |
| ext4 | `mke2fs 1.47.2`；`mkfs.ext4 -F -q -L perfb` + 默认挂载选项 |
| warp | `warp version (dev)`，md5 `70d10a1f…`（同一二进制用于两侧） |
| 证据 | VM `/tmp/bl_*` 原始 warp 日志与 MinIO 日志；脚本 `temp_scripts/bl250_seq.sh`（回环）、`bl250_one.sh`/`bl253_one.sh`（远端） |

## 2. 口径与命令

- 拓扑：单节点单盘（无 erasure 冗余）→ MinIO **ErasureSD 口径（1× 空间占用）**；
  客户端 = 同机 warp（`127.0.0.1:9100`）。
- 交错顺序（同盘、消除漂移）：**R1 ext4 → R1 UUSE → R2 ext4 → R2 UUSE**；
  每个变体开始前重建存储：ext4 腿 `mkfs.ext4`，UUSE 腿 `uusectl format --yes` + wipe，
  然后起 MinIO（`:9100`）。
- 每条腿：`warp put --duration 8s --concurrent 16 --obj.size 1MiB --noclear`
  → `warp get --list-existing --noclear --duration 8s --concurrent 16`
  →（仅 ext4）`sync; echo 3 > /proc/sys/vm/drop_caches` 后再 `get` 一轮（**冷缓存对照**）。
- 设备绑定切换脚本化：ext4 腿绑内核 nvme（`unbind → remove → rescan`），
  UUSE 腿绑 `uio_pci_generic`。

## 3. 结果（2 轮，全部腿 errors=0）

| 轮 | 变体 | PUT (MiB/s) | GET 热 (MiB/s) | GET 冷（drop_caches）(MiB/s) |
|---|---|---:|---:|---:|
| R1 | ext4 | 826.93 | 2889.25 | 2880.78 |
| R1 | UUSE | 840.49 | 1910.08 | — |
| R2 | ext4 | 811.76 | 3357.19 | 3390.23 |
| R2 | UUSE | 933.34 | 2171.75 | — |
| — | **UUSE / ext4** | **101.6% / 115.0%** | **66.1% / 64.7%** | — |

对照（冻结的正式 A/B，不同轮次/口径）：PUT 103.3/103.7%、GET p99 0.73–0.95x、
混合口径 GET 71.4/70.3%、GET-only 口径 62.1%、shim 元数据缓存净收益 +4.6%
（`minio_shard95_attribution.md` §9/§12）。**本次复测与冻结结论一致。**

## 4. 解读

1. **写路径：UUSE 与原生持平或更优（101.6%–115.0%）**。ext4 享有页缓存/回写合并
   红利，UUSE 直写仍不落下风；barrier 结构性修复（shape change 可回放）后，
   写路径无遗留差距。
2. **读路径：UUSE 为原生的 64.7%–66.1%**，且 **ext4 冷/热 GET 几乎相同**
   （2880.8 vs 2889.3；3390.2 vs 3357.2）——在这个约 7 GiB 数据集 ≈ 7.2 GiB 内存
   的负载里，**页缓存对吞吐贡献≈0**，读受设备吞吐限制。差距来自
   **内核 NVMe 多队列 vs SPDK 单 qpair 的读并发深度**（归因 §13），以及每对象
   ~2 次元数据小读（ext4 命中页缓存 vs UUSE 落设备）——后者已由 shim 元数据缓存
   消除约一半（小读 −40%、GET +4.6%）。
3. 错误面：全部 8 条腿 `errors=0`。

## 5. 未纳入本基线的测量（边界与后续）

- **远端客户端腿（.253 → .250）未纳入**：采集期间 .250 出现**宿主侧退化**——
  内核 workqueue 抢占（`mpt_fault_reset_work [mptbase]`、`vmballoon_work` 连续
  "hogged CPU"，multipathd 短时 ~110% CPU，load 4.1），远端 PUT 在第 1 秒后停摆
  （460 MiB/s → 1.5 MiB/s 中位，p50 ~6.6s）；而**同一实例的回环 c16 PUT 仍有
  911.89 MiB/s（p50 17.2ms）**，设备本体 `dd` 1.9 GB/s、单次 fsync 0.00s 正常
  → 归因宿主/VM 状态，不在 MinIO/UUSE 路径。**待宿主侧重启 .250 后**用
  `bl250_one.sh` + `bl253_one.sh` 重测并补入。
- 2 客户端 + 2 存储拓扑（负扩展观察：聚合 PUT ≈111.6、GET ≈368.6 MiB/s）：
  `minio/README.md` §2。
- ext4 + `MINIO_API_ODIRECT=on`（dio）对照实验：未做（计划见会话记录）。
- 4 节点分布式 A/B gate：未按口径测量（性能口径已冻结）。

## 6. 边界与复现

- 结果只与**同 VM、同盘、同构建**比较；换 VM 或宿主后不直接计算回退，另建基线。
- 单盘 ErasureSD 口径（1× 空间、无冗余），不含网络/分布式/多客户端路径。
- UUSE **无宿主页缓存**为设计不变量；shim 元数据缓存默认开启
  （`UUSE_MINIO_META_CACHE=0` 关闭；命中/失效计数经 `UUSE_MINIO_SHIM_DIAG=1` 输出）。
- 复现（VM 内）：
  - 回环 A/B（本文口径）：`bash /root/bl250_seq.sh loop`
  - 远端客户端（两机顺序执行）：`bash /root/bl250_one.sh <ext4|uuse> <round>`，
    随后 `.253` 上 `bash /root/bl253_one.sh <ext4|uuse> <round>`
  - 恢复四节点集群：四台 `minio-wipe` → `bash /root/uuse-cluster-up.sh`
