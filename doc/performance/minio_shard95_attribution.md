# UUSE × MinIO 写路径性能归因（Shard-9.5，2026-10-12）

> 依据：`doc/design/uuse.md` §16.5（tracepoint 与归因流程）、
> `doc/design/uuse_minio_frontend.md` §7.4（≥95% gate）、
> `minio/README.md` §2（9.5 行）。本轮结论全部来自真实 `Nvme0n1`
> （`0000:13:00.0`，INTx）单节点 UUSE + warp 负载，trace 数据由
> `temp_scripts/minio_shard95_trace_analyze.py` 离线解析。

## 1. 方法与口径

| 项 | 值 |
|---|---|
| 归因运行 | .250 单节点、`/etc/uuse-minio.conf`（trace=true 副本 `/root/uuse-attrib.conf`）、trace_sample=1、trace_dir=/root/uuse-trace |
| 负载 | `warp put --duration 10s --concurrent 16 --obj.size 1MiB`（本地回环，errors=0） |
| 核心点位 | `FSYNC_BEGIN/END`、`JOURNAL_APPEND/FLUSH`、`CHKPT_BEGIN/END/BYTES/FREEZE`、`META_LOCK_WAIT`、`BDEV_SUBMIT/COMPLETE`、`COPY_IN/OUT`、`EXTENT_PLAN`、`SNAP_BUILD` |
| 追加点位（append-only） | `SYNC_PHASE=60`（arg0=phase、arg1=epoch、arg2=ns、arg3=附加量）、`SYNC_BATCH=61`（arg0=服务的 sync 数、arg1=target epoch、arg2=staged 记录数、arg3=checkpoint 类型 0/1/2）、`CHKPT_FREEZE.arg3=freeze_ns` |
| shim 侧 | `UUSE_MINIO_SHIM_DIAG=1` 每 5s 输出每类 C ABI 调用的 calls/avg/max/锁等待 |

## 2. 归因结论（优化前，2026-10-12 首轮）

单对象（1 MiB）PUT 在 MinIO 中 = 2 次 `File::sync`（part Fdatasync +
xl.meta O_DSYNC close-sync）。trace 显示：

1. **durability barrier 是写路径唯一的串行化点**：barrier（leader）
   p50 3.9 ms / p99 11.9 ms / max 16.7 ms，barrier 线程占用率 ≈ 0.79–0.94
   （10 s 内 1787–2130 次 full checkpoint，每次 barrier 均值 ≈ 4.4 ms）；
   `join_wait` 合计 49–68 s / 10 s → 平均 5–7 个 PUT 线程在 barrier 队列上等待。
2. **barrier 内部（leader 均值）**：
   | 分段 | 均值 | 说明 |
   |---|---|---|
   | `snapshot`（freeze+encode） | 1.5–2.0 ms | 全量 namespace 快照，payload 随对象数线性增长 |
   | `ckpt_write`（checkpoint 记录+superblock） | 1.7–1.9 ms | 单次 writeBatch（≤1 MiB range × 3）+ flush |
   | `window`（组提交合并窗） | 0.34 ms | `max_group_commit_wait_us=200` |
   | `flush`（设备 flush） | 0.2 ms | 覆盖率检查后多数真实下发 |
   | `staged_take/prepare/publish/require_full` | ≈0 | 非瓶颈 |
3. **checkpoint 全部为 full（delta=0）**：MinIO 每个 PUT 的
   create/rename 都是 shape change，`checkpointRequired()` 使每次 barrier
   走全量快照；快照 payload 中位数 1.3 MB（尾部 3.3 MB），约 141 B/slot，
   与 namespace 规模线性增长。checkpoint 写带宽 ≈ payload/S ≈ 300–430 MB/s，
   与 400–560 MB/s 的数据写争用同一设备。
4. **数据路径写放大（次要但显著）**：`BDEV_SUBMIT` 直方图显示每个
   512 B xl.meta 写入都伴随一次 **2047 block（≈1 MiB）的 extent 尾部
   zero-fill 写**（`(flag,blocks)=(1,2047)` 计 8804 次/10 s ≈ 2.1/对象），
   另有 1 block RMW 读写。即 1 MiB 对象在设备上产生 ≈3 MiB 写
   （payload 1 MiB + 2 × xl.meta extent zero-fill），
   shim 侧 `pwrite` 平均 5.0 ms、并发 6.5。
5. **组提交窗口不是瓶颈**：窗口 200 µs→2000 µs 无收益（此前实验），
   归因确认 barrier 为“速率保持型”单服务者，吞吐由
   16 并发 PUT 的时延决定，扩大窗口只增时延。

## 3. 本轮优化（纯加法，语义不变）

| # | 改动 | 位置 | 效果 |
|---|---|---|---|
| O1 | 写路径 zero-fill 只覆盖**写后文件大小以内**的范围；`truncate` 增长时对已映射区间补零，保持“文件大小内未写字节读为 0”不变量 | `src/core/client.cpp` | 512 B xl.meta 写不再触发 1 MiB extent 尾部 zero-fill；设备写放大 ≈3×→≈1× |
| O2 | checkpoint freeze 快路径：`InodeExtentMap::appendTo`（去 per-inode 分配）、目录名 arena、view reserve、按 hash 桶遍历目录项 | `src/metadata/{inode_map,namespace}.{hpp,cpp}` | freeze+encode 均值 ≈2.0→≈1.5 ms（同 payload 下约 −40%） |
| O3 | 快照编码两遍法（计数+写入）＋ raw cursor，取代 per-field `vector::insert` | `src/metadata/namespace.cpp` | 编码 memcpy 化，且计数遍保证长度与写入严格一致 |
| O4 | 控制路径 DMA 由 `spdk_dma_zmalloc` 改 `spdk_dma_malloc`（载荷立即被 memcpy 覆盖） | `src/runtime/spdk_runtime.cpp` | 去掉每次 checkpoint 的 1 MiB 级零填充 |
| O5 | trace 追加 `SYNC_PHASE`/`SYNC_BATCH`、`CHKPT_FREEZE.arg3` | `src/common/trace.hpp`、`src/core/engine.cpp`、`src/storage/journal.cpp` | 归因点位（append-only，旧事件布局不变） |

## 4. A/B 复测（同盘 ext4↔UUSE，INTx，warp 本地回环 QD16 1 MiB，2×2 轮）

| 指标 | ext4 r1/r2 | UUSE r1/r2 | 比值 |
|---|---|---|---|
| PUT 吞吐 | 860.2 / 849.8 MiB/s | 511.3 / 544.7 MiB/s | **59.4% / 64.1%** |
| PUT p50 | 17.9 / 18.1 ms | 33.3 / 32.5 ms | 1.86x / 1.80x |
| PUT p99 | 32.7 / 32.6 ms | 111.6 / 59.6 ms | 3.4x / 1.83x（r1 有长尾） |
| GET 吞吐 | 2366.7 / 2369.1 MiB/s | 1957.1 / 1966.2 MiB/s | **82.7% / 83.0%** |
| GET p99 | 19.9 / 20.0 ms | 14.8 / 14.3 ms | **0.74x / 0.72x（优于 ext4）** |
| errors | 0 | 0 | — |

对比优化前（9.5 首轮）：PUT 47.7%/47.1% → **59.4%/64.1%**（相对 +31%），
GET 78%→83%，GET p99 保持优于 ext4。**gate 仍未达标**（PUT ≈62%），
9.5 保持开放。

## 5. 正确性验证（本轮改动出口）

- 单节点功能矩阵 **21/21**（尺寸 0/1/4K/64K/1M/5M/5M+1/12M、multipart、
  range、versioning、bigdir 1000、copy、remove、stat）；
- 崩溃矩阵：写入循环中 `kill -9` → 重启 → **ACK 对象 149/149 完整、
  sweep 60/60 无半成品**；`uusectl check corruption=none`；
- CTest **29/29**（含 `namespace_test` 快照往返、`journal_test`
  checkpoint 往返、`data_path_device_test`）；
- 四节点集群恢复后跨节点读校验 60/60（.250 写、.251/.253 读）。

## 6. 剩余差距与下一步（未实施）

barrier 的 `snapshot` + `ckpt_write` 都随 namespace 规模 O(N) 增长，
是 PUT gate 的决定性差距。候选方向（按优先级）：

1. **增量 checkpoint / shape change 可回放**：把 create/remove/rename/link
   作为可回放记录（journal slot 已支持变长 payload，每个记录独占一个
   4 KiB slot，可携带 name），使 shape change 不再强制 full base；
   预期 barrier S 从 ≈3.4 ms 降至 <1 ms；
2. 若维持 full base：快照 v3 稀疏编码（跳过空 slot）＋ payload 压缩；
3. 数据路径：把 extent 尾部 zero-fill 改为读时惰性置零（需扩展
   extent 初始化语义），进一步压缩元数据写放大。

## 7. 环境观察（待后续复现）

单节点崩溃验证后的某次 mount 曾报
`stored geometry does not match device 'Nvme0n1'`（此前同一卷多次
mount/check 均正常），随后 `uusectl format` 打印的几何与卷内记录完全一致
（`blocks=5242880 data_start=198656 data_blocks=5044224`），重新 format 后
wipe/集群恢复全部正常；用“format→mount→kill -9→check×2→mount”复现失败。
已记录：疑与 lifecycle（`updateCleanShutdown`）与 journal 两套 superblock
写入者各自交替 A/B slot 的时序有关，与本轮改动无关（本轮改动不触碰
superblock 几何字段），留待专项排查。
