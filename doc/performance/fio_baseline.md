# HFS 2.0 FIO 三层性能基线（ext4+libaio / SPDK raw / HFS）

日期：2026-09-27 会话（UTC 16:21:40–16:35:33，本地 2026-09-28 00:21–00:35）。
范围：同一块 NVMe 控制器上 ext4+libaio、SPDK raw bdev 与 HFS 的 `randrw` 50/50
三方矩阵，4K/16K/64K × 总队列深度 QD32/64，每格 3 次取中位数；HFS 与 SPDK raw
使用相同总队列深度。本文件只描述本会话结果；不同会话或 VM 的绝对 IOPS 不直接比较回退。

相关矩阵：RocksDB 见 `doc/performance/rocksdb_baseline.md`，MySQL 见
`doc/performance/mysql_baseline.md`；三者共用本会话设备与构建。

## 1. 环境与构建

| 项目 | 值 |
|---|---|
| VM | 6 vCPU、约 7.2 GiB RAM、`HugePages_Total=1024` |
| 目标设备 | `VMware NVME_0002`，PCI `0000:13:00.0`；内核为 `/dev/nvme0n1`，测试期间绑定 `uio_pci_generic`，SPDK bdev `Nvme0n1` |
| HFS 源码 | Windows 工作区 `HEAD 8dc2175` 同步到 VM 后 Release 重建（`/root/code/build-verify`）；同步后 CTest `26/26` 通过 |
| SPDK | `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`（VM 内 Git checkout） |
| FIO | 3.42，源码匹配的 `/home/hfs/fio/fio`；SPDK 插件 `spdk_bdev` |
| 二进制指纹 | `fio-hfs.so` `823be5a3…`、`hfs_nvme.conf` `89e63677…`、`spdk_bdev` `1dbf2cbe…` |
| HFS 配置 | `config/hfs_nvme.conf`：`metadata_cpus=0`、`data_reactors=1`、`data_cpus=1`、`frontend_cpus=3`、`client_depth=32`、`trace=false`、`stats=false` |
| 证据目录 | VM `/home/hfs/fio-three-way-20260927-matrix`（201 个文件） |

## 2. 口径与命令

- 负载：`--rw=randrw --rwmixread=50 --direct=1 --time_based=1 --runtime=10`，
  文件/区域 256 MiB，计时前先以 direct 写填满 256 MiB（raw 1 MiB、HFS 64 KiB
  顺序写）。
- 进程统一 `taskset -c 0-5`，`--group_reporting=1`，`--numjobs` 与 `--iodepth`
  按层设置；JSON 的 IOPS 为该层读+写合计速率，取 3 次中位数。
- ext4：`mkfs.ext4` + `mount -o noatime`，`--ioengine=libaio`，1 job × QD。
- SPDK raw：`--ioengine=spdk_bdev`（`LD_PRELOAD=spdk_bdev`），1 job × QD32/64。
- HFS：`--ioengine=external:fio-hfs.so`、`--hfs_async=1`、`--hfs_client_depth=32`；
  QD32 = 1 job × 32，QD64 = 2 jobs × 32，即与 raw 相同的总未完成请求数。
- 每个计时轮前 HFS 执行一次 `hfsctl format --yes`；raw/HFS 每轮时序为
  预填充 → 计时 10 s。

复现命令见第 6 节。

## 3. IOPS 矩阵（3 次中位数）

| I/O | 总 QD | ext4+libaio | SPDK raw | HFS | HFS/raw | ext4/raw |
|---|---:|---:|---:|---:|---:|---:|
| 4K | 32 | 11546 | 95986 | 91081 | **94.9%** | 12.0% |
| 4K | 64 | 14058 | 94092 | 96534 | **102.6%** | 14.9% |
| 16K | 32 | 47737 | 77568 | 76091 | **98.1%** | 61.5% |
| 16K | 64 | 6723 | 77837 | 78186 | **100.4%** | 8.6% |
| 64K | 32 | 18356 | 45402 | 44152 | **97.2%** | 40.4% |
| 64K | 64 | 21412 | 47311 | 46042 | **97.3%** | 45.3% |

## 4. p99 延迟矩阵（读/写，µs，3 次中位数）

| I/O | 总 QD | ext4 读/写 | raw 读/写 | HFS 读/写 | HFS/raw 读 | HFS/raw 写 |
|---|---:|---:|---:|---:|---:|---:|
| 4K | 32 | 1253/1270 | 766/766 | 733/733 | 95.7% | 95.7% |
| 4K | 64 | 1401/1417 | 1384/1401 | 1221/1221 | 88.2% | 87.1% |
| 16K | 32 | 1401/1384 | 897/905 | 840/840 | 93.6% | 92.8% |
| 16K | 64 | 2376/2376 | 1581/1597 | 1417/1417 | 89.6% | 88.7% |
| 64K | 32 | 3228/3195 | 1384/1401 | 1237/1253 | 89.4% | 89.5% |
| 64K | 64 | 4686/4686 | 2441/2441 | 2212/2277 | 90.6% | 93.3% |

## 5. 轮次离散度与闭合证据

每格 3 次原始 IOPS（第 1/2/3 次，最大/最小）：

| 层 | 4K QD32 | 4K QD64 | 16K QD32 | 16K QD64 | 64K QD32 | 64K QD64 | 最大离散 |
|---|---:|---:|---:|---:|---:|---:|---:|
| ext4 | 12466/11546/9503 | 14058/13525/19772 | 47737/49237/41696 | 7483/5967/6723 | 16271/18356/19586 | 21412/21711/5546 | 3.91× |
| raw | 95705/98234/95986 | 97176/93817/94092 | 77568/80583/77136 | 77837/78010/77303 | 44917/45732/45402 | 47685/47311/45705 | 1.04× |
| HFS | 91081/92117/90541 | 97040/95968/96534 | 76091/77499/74653 | 77187/79201/78186 | 44152/44066/46377 | 46042/46776/42518 | 1.10× |

- 54 次计时运行（3 层 × 6 格 × 3 次）`error=0`；36 次预填充全部成功。
- HFS 18 个计时轮 + 18 个预填充轮 closure 全部闭合：
  `queued=reaped`、`outstanding=0`、`token_mismatch=0`、`busy=0`、
  `completed_error=0`、`prealloc_failure=0`，计时轮 `peak=32/64`、预填充 `peak=4`。
- raw 与 HFS 每格 3 轮的离散度 ≤1.10×；ext4+libaio 侧仍离散（最大 3.91×）。

## 6. 观察、边界与复现

观察：

- 在本口径（单文件 `randrw` 50/50、256 MiB、10 s）下，HFS/raw 为
  **94.9%–102.6%**，p99 为 raw 的 **87.1%–95.7%**，即本形状内 HFS 延迟不超过 raw。
- 该结果不能替代 `doc/design/hfs2.0.md` §16.2 的 8j4 正式 gate：上一会话的同机
  8j4 中位比值为 0.764，本会话未复测 8j4；本矩阵只说明 QD32/64 全深度随机
  混合下 HFS 与 raw 接近。
- ext4+libaio 层保持已知的离散特性（部分格 runtime 超出 10 s，为 30–40 s），
  其数值只作为宿主文件系统参考层，不参与 HFS/raw 判定。
- HFS QD64 格在 JSON 中 `job_runtime=20000 ms` 是两个并发 10 s job 的累计
  报告值，实际墙钟约 11 s；raw 侧为单 job。报告使用 JSON 的 IOPS 速率字段，
  两种报告方式对 IOPS 无影响。
- 与 2026-09-27 newshape 会话相比，本会话 raw 4K QD32 中位由 76034 升至
  95986（+26%），HFS 同步上升；属设备/会话漂移，不做跨会话回退结论。

边界：

- 测试使用独占测试盘，运行期间盘上无其他负载；设备在会话结束后恢复内核
  `nvme` 驱动。
- HFS 侧 `hfs_client_depth=32` 为配置默认值；QD64 格通过第二队列把总深度提到
  64，未修改 core 参数。

复现：

```bash
# VM 内（HFS 源码目录）
ALLOW_FORMAT=1 RUNS=3 RUNTIME_SECONDS=10 \
  RESULT=/home/hfs/fio-three-way-<date> \
  bash fio/run_three_way_matrix_vm.sh
```

脚本 `fio/run_three_way_matrix_vm.sh` 会校验序列号与 PCI BDF、按层绑定驱动、
交替运行三层并输出 JSON/stderr 证据。

## 7. 证据

- `/home/hfs/fio-three-way-20260927-matrix/`：54 个计时 JSON、36 个预填充 JSON、
  对应 stderr/format 日志、`environment.txt`、`spdk-nvme.json`。
- `/home/hfs/fio-three-way-20260927-matrix.metrics.tsv`：本报告使用的
  IOPS/p99/errors/runtime 汇总。
- `/home/hfs/fio-three-way-20260927-matrix.run.log`：脚本完成日志
  （`matrix cleanup: stage=complete exit=0`）。
