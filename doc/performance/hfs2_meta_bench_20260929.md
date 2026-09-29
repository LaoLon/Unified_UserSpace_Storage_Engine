# HFS 2.0 metadata microbenchmark（§17.1）与 checkpoint/replay 修复（2026-09-29）

> 设计依据：`doc/design/hfs2.0.md` §17.1（metadata ≥20,000 ops/s、1M active inode
> mount ≤10 s、replay ≤2 s）。工具：`tools/hfs_meta_bench.cpp`（CMake target
> `hfs_meta_bench`，仅用 public Engine/Client/File API）。

## 1. 工具口径

- 参数：`--config`、`--dir`、`--inodes`、`--stat-ops`、`--rename-ops`、
  `--commit-ops`、`--sync-every`、`--format`、`--mount-only`；
- 阶段：create（`open(Create|Truncate)` + 立即释放）→ stat → rename →
  commit（`write 4 KiB` + `fsync`，durable 屏障口径）→ stop 后重挂载计时；
- checkpoint 冻结/编码占比来自 `HFS_DIAG_SYNC_STATS=1` 的 `sync phases` 行；
- 基准配置：`max_namespace_bytes = 2 GiB`（生产 `config/hfs_nvme.conf` 保持
  1 GiB）；1M 目标含 root、`/meta-bench`、anchor 三个 inode。

## 2. 结果（VM，真实 NVMe，`VMware NVME_0002`）

| 规模 | create ops/s | stat ops/s | rename ops/s | commit ops/s | mount s | remount s |
|---|---:|---:|---:|---:|---:|---:|
| 200k inode | 856,729 | 2,599,868 | 1,112,775 | 1,097.5 | 0.028 | 0.503 |
| **1M inode（设计目标）** | **437,758** | 2,334,884 | 837,667 | 941.8 | 0.0285 | **1.103** |

- 设计 §17.1 对照：metadata 目标 ≥20,000 ops/s → **437,758**（22×）；mount ≤10 s →
  0.0285 s（干净卷）/ 1.103 s（checkpoint 装载 + journal replay）；replay ≤2 s ✓；
- 1M inode 的 checkpoint（快照约 110 MB）：`ckpt_freeze` 42.6 ms avg / 80.3 ms max，
  `ckpt_snapshot` 106.9 ms avg / 195.5 ms max，`meta_write` 25.8 ms avg；
  checkpoint barrier max 302 ms（每 100k create 触发一次）；
- P2-2a 冻结/编码拆分在 1M 规模的实际占比：约 60 ms 编码已移出 namespace 锁
  （freeze 42.6 ms vs 总 106.9 ms），为 P2-2(b) 的收益上限提供了基准。

## 3. 期间发现并修复的两个 bug（`doc/bug.md` #43/#44）

1. **#43 checkpoint 控制写批上限**：`JournalDevice::writeBatch` 把整个快照切成
   1 MiB chunk 放进一个 `controlWriteBatch`，超过
   `kMaxControlBatchWrites(8)` 后返回 `control write batch is too large`，
   大 namespace 的 sync/checkpoint 全部失败。修复：`controlWriteBatch` 内部按
   8 chunk 分批顺序提交（批间崩溃由 checkpoint CRC/generation 拒绝旧槽位）。
2. **#44 replay 每事务 before-image**：`Journal::replay` 对每个事务保存全量快照
   作为回滚镜像（被拒记录立即 `return Corrupt`，回滚是死代码），200k inode 的
   重挂载因此达 **78.8 s**（O(T×N)）。修复：replay 不再捕获 before-image。
   重挂载 **78.8 s → 0.503 s**（200k），1M 为 1.103 s。

## 4. 验证与边界

- CTest 29/29（含 `journal_test`/`journal_device_test`/`format_mount_test`）；
- 崩溃矩阵（MySQL SIGKILL）与本次修改的 checkpoint 路径无交互，历史证据保持
  `corruption=none`；
- commit 阶段是 `fsync` 屏障口径（约 940–1100 ops/s，含 checkpoint 增长），
  不代表纯 metadata 事务上限；
- 工具早退（client 未 close）时的析构崩溃记为 bug #45（待处理）；正常路径不受影响；
- 后续：把 (b) 增量 checkpoint 的收益目标与本节 1M 数据对齐（dirty 集应显著小于
  110 MB 快照）。
