# HFS 2.0 RocksDB 低队列深度读延迟优化方案

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

时间：2026-09-23  
状态：方案评审稿（未实施，待评审后按 AGENTS.md 先更新 `doc/design/hfs2.0.md` 再编码）  
正式设备：`Nvme0n1`，PCI BDF `0000:0b:00.0`  
配置：`config/hfs_nvme.conf`（当前工作区版本：`data_reactors=2`、`metadata_cpu=0`、
`data_cpus=1,2`、`frontend_cpus=3`、`[numa] polling=hybrid`、`stats=false`）

## 1. 背景与问题陈述

同一块 NVMe、同一 RocksDB `readrandom` workload（524,288 keys、16B key、1KB value、
4KiB block、32MiB block cache、无压缩、direct reads、8 worker、每轮 524,288 次 Get）
下，当前 HFS 默认共享 frontend 路径与 native ext4 direct 存在明显差距：

| Backend | Round1 | Round2 | Round3 | Average | p50 |
|---|---:|---:|---:|---:|---:|
| HFS/SPDK shared frontend（2 reactor） | 26,071.0 | 26,289.8 | 26,099.0 | **26,153.3 ops/s** | 304–312 µs |
| native ext4 direct | 60,675.2 | 59,500.7 | 60,088.2 | **60,088.0 ops/s** | 120–123 µs |

native / HFS ≈ **2.30x**，HFS 约为 native 的 **43.5%**。该口径下两侧设备读量与读字节
一致（HFS `bdev_data_read_ops=494,066` / `2,326,675,968 B`，native Linux
`nvme0n1` 约 494K 次 / 2.326 GB 每轮），**不存在字节级读放大**。

问题定位为：**低队列深度（QD=8，8 个 RocksDB worker 各 1 个同步 Get）下的端到端
延迟**，而非设备带宽或 HFS 读放大。高并发口径（FIO 8 jobs × iodepth=4）HFS 与
native 已接近（见 `performance-compare.md`），本方案不覆盖该口径的回归。

### 1.1 与交接文档的关系

Codex 交接文档的 26,153 / 60,088 数字与上述 v2 脚本结果一致，可复现。但本方案对
交接文档的以下判断做了修正（证据见 §2）：

1. **"worker-direct 没有收益" 不成立**：本方案实测同会话 A/B 为 +63.5%
   （stats=true），stats=false 保守口径也 +17.2%；旧负结果来自降级环境。
2. **copy-out / registered-DMA 优先级被高估**：实测 `read_copy_out_ns` 为
   **0.26 µs/read**，约占总延迟 0.08%，为延迟做 zero-copy 收益≈0。
3. **旧 stats 阶段值已过时**：交接文档的 "frontend queue wait 180 µs /
   completion wait 492 µs" 来自旧 1-reactor 环境；当前 2-reactor 实测为
   91.3 µs / 355.5 µs。
4. **per-reactor 不均衡不成立**：v2 采样 data reactor CPU 为 45.26% / 44.71%。

## 2. 本轮新增实测证据

以下测量均由 `E:\Code\test_script` 下脚本在 VM 上执行；设备每次测试后经
`setup.sh reset` 回到内核 `nvme`，HugePages 1024/1024 空闲。

### 2.1 同会话 stats=true 对照（2 reactor，worker CPU 4,5，frontend CPU 3）

| 指标 | shared frontend | worker-direct | 变化 |
|---|---:|---:|---:|
| ops/s | 22,759.7 | 37,208.6 | **+63.5%** |
| p50 | 341,826 ns | 193,672 ns | −43% |
| p95 | 619,612 ns | 405,050 ns | −35% |
| p99 | 945,170 ns | 596,714 ns | −37% |
| p99.9 | 1,832,352 ns | 2,019,879 ns | 略差 |
| frontend_read_ops | 494,066 | 0 | 完全绕过 |
| direct_worker_read/completion | 0 | 494,072 / 494,072 | 无 fallback |
| bdev read ops / bytes | 494,066 / 2,326,675,968 | 494,072 / 2,326,624,768 | 与 block miss 一致 |
| planner request=child=span bytes | 相等 | 相等 | 无读放大 |
| head/tail RMW | 0 / 0 | 0 / 0 | — |
| coalesced children | 1,036 | 977 | 可忽略 |
| queue_depth_sum / ops | 2.37（max 8） | 2.87（max 8） | — |
| copy-out | 0.26 µs/read | 0.26 µs/read | 非瓶颈 |
| worker CPU（每 worker） | ~5.5% | ~10% | direct 自己做提交 |

shared 路径的阶段分解（同一次运行）：

```text
frontend queue wait（worker enqueue → frontend submit）  91.3 µs/read  (27%)
frontend submit → completion                             235.7 µs/read (69%)
completion wait（worker CV 等待，即端到端等待）            355.5 µs/wait
copy-out                                                  0.26 µs/read  (≈0)
```

### 2.2 stats=false 的保守对照

| 组别 | Round1 | Round2 | Round3 | Average | p50 |
|---|---:|---:|---:|---:|---:|
| shared（交接文档 v2 脚本） | 26,071.0 | 26,289.8 | 26,099.0 | 26,153.3 | 304–312 µs |
| worker-direct（本轮新增） | 32,003.1 | 29,616.6 | 30,321.9 | **30,647.2** | 228–244 µs |

保守口径 **+17.2%**；direct/native = 51.0%（stats=true 口径为 61.9%）。
stats 开销对 shared 路径更大（其 per-read profiling 计数更多），因此 +63.5% 与
+17.2% 构成收益上下界，最终值必须由 W0 受控 A/B 确认。

### 2.3 worker-direct 退出挂死（复现）

| 运行 | 配置 | 现象 |
|---|---|---|
| direct，stats=true，1 轮 | 2 reactor / workers 4,5 | 读轮完成后进程 >15 min 不退出，累计 CPU 66 s |
| direct，stats=true，1 轮（复测） | 同上 | round 标记后进程仍存活，需外部 kill |
| direct，stats=false，3 轮 | 同上 | round 3 标记后进程仍存活，需外部 kill |

同一环境下 shared 运行可正常 `exit_code=0` 退出。该现象与已知的
`Engine::stop()` 与 RocksDB 后台线程退出锁竞争一致
（`doc/performance/rocksdb_readrandom_rootcause_20260922.md:730`、`:810`）。
在 W2 修复前，direct 取数必须"看到 round 标记后 kill"，且不得把"能退出"计入验收。

### 2.4 环境漂移（比较口径警告）

同一块 NVMe、同一 workload、无代码改动，native ext4 direct 在不同时段：

| 时间 | ops/s | p50 | 设备读 |
|---|---:|---:|---:|
| 2026-09-22 17:09（`same-nvme-direct-native`） | 21,964.5 | 359 µs | 494K/轮、2.326 GB |
| 2026-09-22 19:33（`two-data-v2-native`） | 60,088.0 | 122 µs | 494K/轮、2.326 GB |

本轮 fio raw 4K psync QD8 复测（当前环境）：

```text
CPU2,3: 39,628 IOPS, p50 187 µs
CPU4,5: 47,919 IOPS, p50 142 µs
CPU0,1: 50,146 IOPS, p50 134 µs
```

结论：设备/CPU 环境存在跨会话漂移；**任何跨会话数字（包括旧 "HFS 80%" 与
handover 的 "43.5%"）不得放入同一张对比表**。W0 必须固化同会话交错协议。

## 3. 根因分析（代码级）

### 3.1 共享 frontend 路径的固定软件开销（第一根因）

每次读经过：

```text
RocksDB worker
  → AsyncReadDispatcher::enqueue          rocksdb/hfs_filesystem.cpp:472
  → frontend CV 唤醒 / run loop           rocksdb/hfs_filesystem.cpp:574
  → processSubmissions → Client::submitRead → ShardRing::publish
                                          rocksdb/hfs_filesystem.cpp:627
  → data reactor pump（planning/DMA/submit）
                                          src/core/async_data_service.cpp:484
  → SPDK bdev → completion queue push      src/core/client.cpp:180
  → AsyncReadDispatcher::wake → frontend processCompletions
                                          rocksdb/hfs_filesystem.cpp:765
  → publishCallback → worker CV 唤醒       rocksdb/hfs_filesystem.cpp:832
```

8 个 worker 共用 1 个 frontend 线程，每读 3 次跨线程唤醒 + 1 次串行提交。
实测 frontend queue wait 91.3 µs/read 即该串行/唤醒成本。

### 3.2 data reactor 拾取延迟

data reactor 采用 hybrid polling，空闲时最多 `sleep_for(100 µs)`
（`src/runtime/spdk_runtime.cpp:801-818`）；`ShardRing::publish` 只维护
`armed_` 标志，**没有任何唤醒 reactor 的机制**（`src/core/shard_ring.cpp:56-78`）。
新请求到达空闲 reactor 时最坏要等一个 sleep 周期。这是 direct 路径 p50
（194 µs）仍高于 native（122 µs）的主要残余项。

### 3.3 worker-direct 生命周期与设计契约冲突

- 实现：`rocksdb/hfs_filesystem.cpp:1035-1063`（提交）、`:323-342`
  （`DirectWorkerEntry` 析构 `client->close().wait()`）、`:885-898`
  （`HfsContext::~HfsContext` 顺序：dispatcher → client → engine）。
- 设计契约：`doc/design/hfs2.0.md:314-322` 明确禁止 RocksDB worker 线程直接调用
  `Client::submitRead/poll/AbortIO/close`，并要求单一 frontend owner。
  direct 实验与现行设计冲突，必须先改设计再实现。
- 挂死未定位到具体锁环：可能在 `Engine::stop()` 等待 metadata user / outstanding，
  也可能在 `DirectWorkerEntry`/`Client::close()` 与 reactor 停止的顺序上。

### 3.4 已排除项（不作为优化目标）

| 候选 | 证据 | 结论 |
|---|---|---|
| 字节读放大 | request=child=span=bdev bytes，RMW=0 | 无 |
| bounce buffer / copy-out | 0.26 µs/read | 非瓶颈 |
| per-reactor 不均 | 45.26% / 44.71% | 均衡 |
| worker/数据 reactor CPU 饱和 | worker 5.5–10%，reactor ~45% | 未饱和 |
| RocksDB block cache 口径 | 两侧每轮 miss ≈ 494K | 同口径 |
| NVMe 带宽 | 每轮 2.326 GB / 设备读量一致 | 非带宽问题 |

## 4. 优化目标与验收口径

### 4.1 目标（stats=false，W0 受控协议）

1. worker-direct 相对 shared **≥ +30%**（以受控 A/B 为准；预期区间 +20%~+60%）。
2. HFS 读吞吐 **≥ 40,000 ops/s**（native 60K 的 67%）。
3. p50 **≤ 200 µs**、p99 **≤ 700 µs**（direct）。
4. 进程在完整 workload（含 compaction）后能正常退出，退出码 0。
5. 正确性不回退：每轮 `hits=524288/524288`，无 checksum/IO 错误。

### 4.2 非目标

- 不追求超过 native 60K；不承诺 zero-copy；不改 metadata/journal/extent allocator；
  不重写 reactor 架构；不改变 FIO 前端路径的既有契约。

## 5. 优化方案

工作项按依赖顺序排列：W0 → W1 → W2 →（W3 备选）→ W4 条件触发。

### W0 测量协议与基线固化（前置，无代码改动）

**目的**：消除环境漂移，把 ±3x 的跨会话噪声压到 <5%。

**实施**：
1. 新增统一脚本 `E:\Code\test_script\run_read_latency_ab_20260923.py`：
   - 内核态阶段：native ext4 direct ×3 轮（会话开始）；
   - SPDK 阶段：一次 format，同一 DB，shared/direct 交错（shared 3 轮、direct 3 轮，
     推荐 S/D/S/D/… 逐轮交错）；
   - 内核态阶段：native ext4 direct ×3 轮（会话结束，检测漂移）。
2. 固定参数：`--keys=524288 --threads=8 --key-size=16 --value-size=1024
   --cache-size=33554432 --block-size=4096 --direct-reads=1 --compression=none
   --worker-cpus=4,5`；shared/direct 均用 `taskset -c 0-5`；`stats=false`。
3. 记录：ops/s、p50/p95/p99、设备读 ops/bytes、CPU map、进程退出码。
4. direct 在 W2 修复前使用 `stdbuf -oL` + round 标记后 kill 的取数方式，报告需注明。

**验收**：native 首尾两组偏差 <5%；shared 组内偏差 <5%。  
**产物**：`/mnt/hfs-data/read-latency-ab-<date>.log` 与结论表。

### W1 data reactor 拾取延迟消除（低风险，先做）

**依据**：§3.2；预计 p50 下降 30–80 µs，shared/direct 均受益。

**步骤**：
1. **配置 A/B（零代码）**：`[numa] polling = active` vs `hybrid`，shared/direct 各 3 轮。
   `polling=active` 让 reactor 满载轮询，消除 100 µs 睡眠，但占用整个 reactor 核。
2. 若确认收益，实现 **per-reactor doorbell**：
   - 每个 data reactor 持有一个 `eventfd`（或等价的 futex 等待）；
   - 空闲路径把 `sleep_for` 改为等待 eventfd（有界超时，保留 hybrid 语义）；
   - `ShardRing::publish` 在 `armed_` 由 false→true 时（仅 doorbell 翻转）写 reactor 的
     eventfd；reaper 侧在 `clearDoorbell()` 后重查非空以闭合竞态（沿用现有
     `armDoorbell` 协议，`src/core/shard_ring.cpp:104-114`）。
   - 该唤醒只影响延迟，不改变正确性；唤醒合并/丢失由有界超时兜底（与 bug #37 的
     "wake 只是提示" 原则一致）。
3. 补单测：`io_planner_test`/`spdk_async_backend_test` 之外，新增 doorbell 竞态单测
   （publish 与 idle-wait 并发、超时兜底、stop 期间无泄漏）。

**验收**：受控 A/B 下 p50 下降 ≥30 µs，且 `polling=hybrid+doorbell` 与
`polling=active` 的延迟差 <10 µs；reactor 空闲 CPU 不高于当前 hybrid。  
**回退**：doorbell 若引入竞态或复杂度，则保留 `polling=active` 作为性能口径配置项。

### W2 worker-direct 产品化与退出挂死修复（主收益）

**依据**：§2.1、§2.3、§3.3。

**步骤**：

1. **挂死根因定位（先于实现）**
   - 复现后抓取全线程 `wchan`/`stack`（含 `Engine::stop()`、`HfsContext` 析构、
     `DirectWorkerEntry` 析构、metadata reactor、RocksDB 后台线程）；
   - 确认锁环属于以下哪一类并记录：
     a. `Engine::stop()` 等待 outstanding/metadata user，而 user 持有方在等
        RocksDB 后台线程退出；
     b. worker 线程退出时 `DirectWorkerEntry` 析构的 `client->close().wait()`
        与 reactor 停止互相等待；
     c. completion 丢失导致 `AsyncReadState` 析构永久等待 `terminal`。
2. **设计文档更新（编码前）**
   - 改写 `doc/design/hfs2.0.md` §5.3：以"worker-owned frontend channel"替代绝对
     禁止条款，明确：
     * 每个 frontend 线程最多一个 Client；Client 的 completion queue、credit、
       request slot、open File 全部线程私有；
     * 共享对象仅限 Engine / AsyncDataService / DmaArena；
     * 关闭顺序：停止新提交 → 排空 outstanding → close Client → 停止 dispatcher →
       engine stop；任何一层失败必须 fail-closed，不得半量 fallback；
     * direct 建立失败（如 Client 创建失败、文件打开失败）时回落到 shared dispatcher，
       回落必须可观测（计数器 + 日志）。
   - §17 增补已知风险：direct 退出挂死（修复前不得用于正式验收）、资源上限
     （线程数 × maxOutstanding × 队列容量）、fork 规则。
   - 明确配置入口（建议新增 `[rocksdb] worker_direct_reads = auto|off|on`，正式值
     由 W0 数据决定；实验期保留 `HFS_ROCKSDB_DIRECT_WORKER_READS`）。
3. **实现**
   - 修复生命周期（按 1 的结论）；`DirectWorkerEntry` 析构改为有界等待 + 错误上报；
   - 修正 direct 路径 p99.9 尾延迟（当前 2.0 ms vs native 0.6 ms）：确认是否来自
     eventfd 唤醒/worker 调度/reactor 拾取，必要时对 direct client 使用专用
     wait 策略（`ClientOptions.completionWait`）；
   - 保持 `frontend_read_ops=0`、`direct_worker_read_ops == direct_worker_completion_ops
     == bdev ops` 作为"未回落"的判定条件。
4. **测试**
   - 新增生命周期单测：worker 线程退出时仍有 outstanding；engine stop 时仍有
     worker Client；反复 create/destroy worker Client；direct 建链失败回落；
   - 现有 `file_api_test`、`data_path_device_test`、`io_planner_test`、
     `spdk_async_backend_test`、CTest 全量；
   - 真实 NVMe：受控 A/B（W0 脚本）+ 完整 workload（含 compaction）正常退出。

**验收**：W0 受控 A/B direct ≥ shared × 1.30；p50 ≤ 200 µs、p99 ≤ 700 µs；
连续 3 次完整运行退出码 0；CTest 全绿。  
**停止条件**：2 个工作日内无法安全定位/修复挂死，或受控收益 <15% → 放弃 direct，
保留 W1，转 W3。  
**回退**：保留 shared 默认路径；direct 降级为实验开关并记录为已知边界。

### W3 shared 路径微优化（备选，仅当 W2 被否或收益不足）

1. worker enqueue 后先做有界自旋（建议 ≤20 µs）再阻塞，削减 91.3 µs 队列等待；
2. `processTimeouts` 仅在存在非空 deadline 时遍历 `active_`，避免每轮锁所有
   state mutex（`rocksdb/hfs_filesystem.cpp:803-819`）；
3. 放开 `HFS_ROCKSDB_COMPLETION_BATCH` 上限（当前 clamp 到 64，
   `rocksdb/hfs_filesystem.cpp:358-367`），A/B 64/128/256；
4. 评估把 `config.client_depth` 接入 RocksDB adapter 的 `ClientOptions`
   （当前 adapter 使用默认 `maxOutstanding=32`，配置未生效，`hfs_filesystem.cpp:1767`）。

**验收**：shared 提升 ≥5%，p99 不劣化。

### W4 registered-DMA / zero-copy（条件触发，默认不做）

仅当 W1–W3 后 data reactor CPU 成为瓶颈（>80%）时再评估；延迟收益按实测
copy-out 0.26 µs/read 判断为≈0。任何实现必须先定义 RocksDB buffer 的
registration/ownership 契约，否则维持 bounce+copy-out。

## 6. 设计文档与配置变更清单

| 文档/文件 | 变更 | 时机 |
|---|---|---|
| `doc/design/hfs2.0.md` §5.3 | worker-owned frontend channel 契约、关闭顺序、回落规则 | W2 编码前 |
| `doc/design/hfs2.0.md` §17 | direct 挂死风险、资源上限、fork/生命周期边界 | W2 编码前 |
| `doc/design/hfs2.0.md` §6.2/§6.3 | data reactor idle 等待与 doorbell 唤醒语义 | W1 编码前 |
| `config/hfs_nvme.conf` | `polling` 正式值（W1 结论）；direct 配置项（W2 结论） | W1/W2 验收后 |
| `doc/bug.md` | 新增 direct 退出挂死条目（修复后标注验证结果） | W2 定位后 |
| 本文件 | 回填 W0–W3 实测结果与最终结论 | 每个里程碑 |

## 7. 测试与验证矩阵

| 层级 | 项目 | 通过标准 |
|---|---|---|
| 单测 | doorbell 竞态、direct 生命周期、回落路径 | 新增用例全过 |
| 集成 | `io_planner_test`、`spdk_async_backend_test`、`file_api_test`、`data_path_device_test` | CTest 全绿 |
| 真实设备 A/B | W0 脚本，native 首尾 + shared/direct 交错 | 目标见 §4.1 |
| 正确性 | 每轮 `hits=524288`、无 checksum/IO 错误、`hfs_ldb checkconsistency` | 通过 |
| 退出 | 完整 workload（含 compaction）进程退出码 0，无 kill | 通过 |
| 回归 | FIO smoke（source-matched fio 3.42）、crash/recovery 矩阵 | 不回退 |
| 观测 | direct 计数器、frontend/direct 分流计数、doorbell 唤醒/超时计数 | 可诊断 |

## 8. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| direct 挂死根因在 metadata/engine 生命周期 | 修复面扩大 | 先抓栈定位；若超期则停止条件生效，转 W3 |
| 设计契约变更影响其他 frontend | FIO 回归 | 变更限定在 RocksDB adapter 与通用 Client 生命周期，不改 FIO 事件队列语义 |
| doorbell 引入丢唤醒/竞态 | 正确性 | 沿用 bug #37 原则：唤醒只是提示，有界超时兜底；补并发单测 |
| 每 worker Client 资源增长 | 内存/句柄 | 上限 = 线程数；maxOutstanding 与队列容量按配置校验；超限回落 shared |
| 环境漂移掩盖收益 | 错误结论 | W0 协议：同会话交错 + 首尾 native 校准；偏差 >5% 重测 |
| stats=true 放大 direct 优势 | 高估收益 | 验收一律以 stats=false 为准，stats 数据只用于归因 |

## 9. 里程碑与顺序

1. **M1（W0+W1 配置 A/B）**：协议脚本 + `polling=active` A/B 数据；决定是否实现 doorbell。
2. **M2（W2 定位 + 设计更新）**：挂死根因报告、§5.3/§17 更新、direct 生命周期修复。
3. **M3（W2 实现 + 验收）**：受控 A/B 达标、退出正常、CTest/回归全绿。
4. **M4（W3/W4 决策）**：若 M3 未达 §4.1 目标，按序启用 W3；W4 仅在 reactor CPU 饱和时触发。

## 10. 开放问题

1. direct 挂死的锁环具体位置（需栈证据，M2 解决）。
2. direct p99.9 ≈2 ms 尾延迟的来源（reactor 拾取 / eventfd / worker 调度）。
3. `config.client_depth` 是否应接入 RocksDB adapter（当前未生效）。
4. `polling=active` 在正式性能口径下是否可接受（占用 reactor 整核）。
5. 每 worker Client 的 open File 句柄是否随 worker 线程数线性增长，是否需要共享文件表。

## 11. 执行记录（2026-09-23）

脚本：`E:\Code\test_script\run_read_latency_ab_20260923.py`（W0/W1 受控 A/B）、
`run_fill_vs_skipfill_probe_20260923.py`（fill 变量隔离）、
`capture_direct_teardown_hang_20260923.py`（挂死栈证据）。
协议：同一 SPDK 会话、同一 DB、shared/direct 逐轮交错、native 会话首尾各 3 轮、
`stats=false`、workers CPU4/5、`taskset -c 0-5`。direct 运行在 round 标记后由脚本 kill。

### 11.1 W0 受控 A/B 结果

| 组别 | 运行 | ops/s | p50 | p95 | p99 |
|---|---|---:|---:|---:|---:|
| native（会话开始） | native_begin | 70,544.6 | 109.4 µs | 194.9 µs | 278.0 µs |
| shared hybrid | S2 / S3 / S4 | 28,097.8 / 28,373.7 / 28,431.0 | 290 / 280 / 279 µs | ~458 µs | ~556 µs |
| direct hybrid | D1 / D2 / D3 / D4 | 70,291.7 / 70,288.3 / 69,932.9 / 69,631.9 | 110 / 110 / 109 / 110 µs | ~193 µs | ~265 µs |
| shared active | SA1 / SA2 | 37,571.7 / 37,617.2 | 210 / 210 µs | ~357 µs | ~437 µs |
| direct active | DA1 / DA2 | 70,323.2 / 70,690.3 | 110 / 110 µs | ~190 µs | ~257 µs |
| native（会话结束） | native_end | 70,175.6 | 108.8 µs | 196.0 µs | 281.6 µs |

结论：

1. **worker-direct 达到 native 同量级（70.0K vs 70.4K，p50 110 µs vs 109 µs）**，
   相对 shared hybrid 为 **+147%**；W2 的目标（≥40K、p50 ≤200 µs）在 stats=false
   口径下已超过。
2. **`polling=active` 使 shared 从 ~28.3K 提升到 37.6K（+33%）**，p50 从 ~283 µs
   降到 210 µs；direct 无变化（已到设备延迟地板）。W1 的拾取延迟假设成立。
3. direct 各轮 `hits=524288/524288`、`block_cache_miss≈494K`，与 native 一致；
   早前 stats=true 运行已确认 `direct_worker_read_ops == completion_ops == bdev ops
   == 494,072`、`frontend_read_ops=0`，本轮 stats=false 无计数但行为一致。

### 11.2 fill 变量隔离

| 运行 | 模式 | fill | ops/s | p50 |
|---|---|---|---:|---:|
| D_fill | direct | 同进程 fill+compact+1 轮 | 74,830.8 | 101.9 µs |
| D_skip1/2/3 | direct | 复用 DB，1 轮 | 74,344.7 / 73,050.1 / 71,980.1 | ~102–106 µs |
| S_skip1/2/3 | shared | 复用 DB，1 轮 | 27,687.8 / 27,665.8 / 27,700.2 | ~296–302 µs |

结论：fill-in-process 不会压低 direct 吞吐；2026-09-22 测得的 direct 30–37K
在当前环境不可复现，且同一会话内 shared 仅变化 ~8%。**决策只采用同会话交错数据**；
旧 direct 数值作为未解释的历史偏差记录在案，不再用于结论。

### 11.3 异常：shared 首轮偶发挂死（S1）

S1（shared hybrid、复用刚 fill 的 DB、1 轮）在 240 s 窗口内未完成 round，被脚本
kill；随后 S2/S3/S4 及 S_skip1/2/3 共 6 次 shared 运行均正常（20 s 完成）。
当前判定为**偶发丢唤醒/挂死**（bug #37 类），与 direct 无关，纳入 W2 稳定性范围；
在复现前不阻塞 W0/W1 结论。

### 11.4 W2 挂死根因（gdb 栈证据）

复现：direct + fill + 1 轮，round 完成后进程不退出；gdb 全线程栈：

```text
主线程 67341: exit() → __run_exit_handlers
              → rocksdb::ThreadPoolImpl::Impl::JoinThreads
              → std::thread::join(LWP 67355)   [等待后台线程退出]

后台线程 67355 "rocksdb:high":
  __call_tls_dtors
  → ~unordered_map<HfsContext*, unique_ptr<DirectWorkerEntry>>   (thread_local)
  → DirectWorkerEntry 析构 → 最后一个 shared_ptr<HfsContext>
  → ~HfsContext → dispatcher stop / client close
  → Engine::stop().wait() → updateCleanShutdown
  → SpdkRuntime::deviceBlockSize() → pthread_mutex_lock  [阻塞]
```

根因：`DirectWorkerEntry`（`rocksdb/hfs_filesystem.cpp:323-342`）以 `shared_ptr`
持有 `HfsContext`，把 HFS context/engine 的生命周期绑到了 RocksDB 全局线程池线程的
TLS 析构上；进程退出时主线程等后台线程 join，而后台线程在 TLS 析构里执行
`Engine::stop()`，形成环形等待。shared 模式没有 per-thread Client，因此不触发。

修复方向（W2 编码前先更新设计文档 §5.3/§17）：

1. `DirectWorkerEntry` 改为非拥有引用（`HfsContext*` + registry 失效标记），
   不得用 `shared_ptr` 延长 context/engine 生命周期；
2. adapter 在 `HfsContext` 内维护 direct channel registry；context 析构（主线程、
   DB 关闭之后）先 retire 全部 channel（在 engine 存活时销毁 client/file/lease），
   再 stop dispatcher、stop engine；
3. 拥有线程退出时自行 close 自己的 channel；被 context 抢先 retire 的 TLS 析构
   必须成为 no-op；
4. registry 关闭后新 direct 建链失败必须回落 shared，并计数/日志。

### 11.5 下一步（更新后的顺序）

1. 更新 `doc/design/hfs2.0.md` §5.3/§17（worker-owned frontend channel 契约与风险）；
2. 实现 W2 生命周期修复 + 单测 + 真实 NVMe 受控 A/B（stats=false、交错、含 compaction、
   正常退出）；
3. W1 doorbell 降为 shared 回落路径的可选优化（`polling=active` 已提供 +33%，
   doorbell 目标是在不烧核的前提下接近该值）；
4. 复查 S1 偶发挂死（压力重复 + 栈证据），与 W2 一并验收。

### 11.6 W2 修复与验证（2026-09-23）

设计文档已先行更新：`doc/design/hfs2.0.md` §5.3 增加 worker-owned frontend channel
契约（非拥有引用、registry、主线程 retire、回落规则），§17.2 增加对应风险项。

代码（`rocksdb/hfs_filesystem.cpp`）：

1. `DirectWorkerEntry` 不再持有 `shared_ptr<HfsContext>`，改为非拥有 `HfsContext*`；
   条目由 thread-local 与 `HfsContext` registry 共同以 `shared_ptr` 持有，避免
   teardown 访问已释放条目。
2. `HfsContext::~HfsContext` 先 `retireAllDirectEntries()`（关闭 registry、在 engine
   存活时释放 client/open File/lease），再停 dispatcher、停 engine。
3. 拥有线程退出时自行 close channel；被 context 抢先 retire 的 TLS 析构为 no-op。
4. registry 关闭后建链失败回落共享 dispatcher；active 非空时 retire 记录错误并
   失败关闭（不无界等待）；completion 轮询遇 retired channel 时以 Aborted 结束等待。

验证结果：

| 项目 | 结果 |
|---|---|
| 全量构建 | `build_rc=0` |
| CTest（设备已绑定） | **26/26 passed** |
| direct 退出 | D1/D2/D3 与 D4（3 轮）全部 `exit_clean`，teardown=0 s；不再挂死 |
| direct stats=true 计数 | `direct_worker_read_ops=494,072 == completion_ops == bdev ops`，`frontend_read_ops=0`，bytes `2,326,622,720`，`hits=524,288/524,288`，69,328.6 ops/s |
| direct 性能 | D1 70,001.0 / D2 69,692.8 / D3 70,305.2 / D4 70,512.6 ops/s，p50 ~110 µs |
| shared 性能 | S_fill 28,591.4 / S2 27,859.4 ops/s，p50 ~280–292 µs |

遗留：shared 偶发挂死在本轮再次出现一次（S1，120 s 未完成 round；同一脚本内
S2 正常）。该问题已在 §11.7 定位并修复。

### 11.7 shared 偶发挂死根因与修复（2026-09-23）

复现：20 次 shared 重复中在第 1 次 `fill` 阶段挂死（120 s 未完成 round）；gdb 栈：

```text
frontend dispatcher (LWP 70870):
  processCompletions → state->condition.notify_all()
  → ___pthread_cond_broadcast → __condvar_quiesce_and_switch_g1
  → futex_wait(cond+0x10, expected=3)          [等待 G1 waiter 释放引用]

worker (LWP 70910):
  AsyncReadDispatcher::poll → condition.wait
  → futex_wait(cond+0x28 = __g_signals[0])     [等待 G1 信号，永不返回]

data reactor 0/1: nanosleep（hybrid idle）
main: std::thread::join(worker)
```

根因：`AsyncReadState`（内含 `std::condition_variable`）由 worker 在 `poll` 返回后
立即 `delete`，而 frontend 线程此时可能仍在执行 `notify_all()`（`processCompletions`
在 `publishCallback` 之后还会 `ioHandle.reset()`、置 `terminal`、再次 `notify_all()`）。
worker 可在 broadcast 尚未完成内部记账时释放 state，破坏 glibc condvar 的 group 状态，
后续 broadcast 永久等待已失效的 G1 引用。该竞态与 direct 无关，shared 与 direct 的
`processCompletions`/`processDirectCompletions` 都存在，只是 direct 的销毁与 notify
在同一线程，触发窗口更小。

修复（`rocksdb/hfs_filesystem.cpp`）：

1. 新增 `AsyncReadDispatcher::retireHandle()` + `drainRetired()`：shared 路径的
   `deleteAsyncReadHandle` 不再直接 `delete`，而是把 state 放入 dispatcher 的
   deferred retire 列表；dispatcher 在每轮 run loop 末尾、确认 state 已不在
   `active_` 且 `terminal=true` 后才释放。
2. direct 路径的 state 仍由 worker 线程同步释放（notify 与销毁同线程，无跨线程竞态）。
3. 非 terminal 的 state 只记录错误并泄漏，不阻塞 dispatcher。

验证：

| 项目 | 结果 |
|---|---|
| 全量构建 | `build_rc=0` |
| CTest（设备已绑定） | **26/26 passed** |
| shared 重复 | **20/20 正常完成**（此前 ~2/18 挂死），p50/吞吐无回退（26.8–27.7K ops/s） |
| direct 复查 | d1 71,304.2 / d2 71,102.2 ops/s，round 10 s、teardown 0 s，干净退出 |

设计文档 §5.3 已同步补充该生命周期规则（per-request 等待状态只能由 dispatcher
线程释放，worker 通过 deferred retire 交还 handle）。

### 11.8 统一 CPU 布局对比与占用（2026-09-23）

统一口径：两侧进程都限制在 cpuset `0-5`；RocksDB worker 都固定在 CPU4,5；HFS 为
metadata CPU0、data reactor CPU1,2、frontend CPU3；native 侧把 `0000:0b:00.0` 的
16 个 MSI-X IRQ 固定到 CPU0-3（与 HFS 的非 worker CPU 范围一致），测试后恢复原
affinity。每 1 s 采样 `/proc/<pid>/task/*/stat` 与 `/proc/stat`（归一化到墙钟）。

性能（同一会话，1 轮 524,288 次 Get）：

| 组别 | ops/s | p50 | p99 | worker 均值 CPU |
|---|---:|---:|---:|---:|
| native ext4 direct | 68,737.0 | 109.8 µs | 289.9 µs | 18.1%/worker |
| HFS shared | 27,106.4 | 310.1 µs | 570.6 µs | 4.2%/worker |
| HFS direct | 69,589.8 | 108.2 µs | 284.1 µs | 11.9%/worker |

每 CPU 平均占用（含 fill/compact 阶段，仅作方向性归因）：

| CPU | 角色 | native | HFS shared | HFS direct |
|---|---|---:|---:|---:|
| 0 | HFS metadata reactor | 2.4% | **95.4%** | **89.5%** |
| 1 | HFS data reactor 0 | 1.2% | 34.7% | **87.0%** |
| 2 | HFS data reactor 1 | 0.7% | 33.4% | **86.3%** |
| 3 | HFS frontend dispatcher | 0.6% | 27.3% | 1.4% |
| 4 | RocksDB worker | 22.7% | 25.7% | 41.5% |
| 5 | RocksDB worker | 20.6% | 17.9% | 41.6% |
| 6/7 | 系统/后台 | ~0.8% | ~3% | ~3.2% |

结论：

1. **统一布局下 HFS direct 与 native 性能等价**（69.6K vs 68.7K，p50 108 vs 110 µs），
   worker CPU 反而更低（11.9% vs 18.1%）。
2. HFS 的额外 CPU 主要是**常驻成本**：metadata reactor 在读-only workload 下仍约
   0.9–1.0 核（busy-poll）；direct 模式下两个 data reactor 各约 87%（轮询 + 搬运），
   frontend 空闲。合计 HFS direct ≈ 3.5 核，native ≈ 0.5 核。
3. shared 路径 CPU 更低（≈2.3 核）但吞吐只有 direct 的 39%；若要压缩 HFS 常驻 CPU，
   需让 metadata reactor 支持空闲休眠、或 data reactor 用 doorbell + hybrid 取代
   busy-poll；两者都需要 runtime 变更和新的延迟/CPU 权衡验收。

### 11.9 W1 doorbell 实现与验证（2026-09-23）

实现（设计文档 §6.3 已先行更新）：

1. `ShardRing::publish` 返回是否由本次 publish 触发 `empty→non-empty` 的 doorbell；
2. `AsyncDataBackend` 增加 `wake()`，`SpdkAsyncDataBackend::wake()` 调
   `SpdkRuntime::wakeDataReactor(shard)`；
3. `AsyncDataService::submit` 仅在 doorbell 翻转时调用 `backend->wake()`，避免每请求
   syscall；
4. 每个 data reactor 持有 nonblocking `eventfd` + `idleWait` 原子标记；空闲路径
   （hybrid 深层睡眠与 interrupt）改为：清 eventfd → 置 idleWait → 重查 poller →
   `ppoll` 有界等待；生产者只在 `idleWait=true` 时写 eventfd，超时仍是兜底。

验证（同会话，1 轮 524,288 Get，workers CPU4/5）：

| 组别 | ops/s | p50 | p99 |
|---|---:|---:|---:|
| shared hybrid（doorbell 前） | 27.1–28.4K | 280–314 µs | ~556 µs |
| **shared hybrid + doorbell** | **34.4–35.0K** | **219–221 µs** | ~461 µs |
| shared `polling=active` | 37.5–38.0K | 208–212 µs | ~432 µs |
| HFS direct（doorbell 前后） | 69.6–72.9K | 105–108 µs | ~257 µs |

每 CPU 占用对比（shared，1 轮）：

| CPU | 角色 | hybrid + doorbell | active |
|---|---|---:|---:|
| 0 | metadata reactor | 94.0% | 98.9% |
| 1 | data reactor 0 | 44.5% | 98.3% |
| 2 | data reactor 1 | 58.5% | 98.4% |
| 3 | frontend | 42.4% | 42.5% |
| 4/5 | workers | 31.3 / 23.0% | 36.3 / 29.1% |

结论：doorbell 用约一半的 reactor CPU 拿到 active 约 92% 的吞吐（34.98K vs 38.03K），
shared hybrid 从 27K 提升到 34.5K（+27%）、p50 310→220 µs；direct 不受影响。
CTest 26/26，全部运行干净退出。剩余常驻 CPU 是 metadata reactor（~94%，busy-poll），
不在 W1 范围，后续如需压缩需单独设计空闲休眠。

### 11.10 metadata reactor 空闲休眠（2026-09-23）

实现（设计文档 §6.3 已先行更新）：

1. metadata reactor 复用 eventfd + `idleWait` 协议：只在无 SPDK 事件且
   `controlInFlight == 0` 时进入 `ppoll` 有界等待（hybrid 1 ms、interrupt 100 µs、
   active 不休眠）；
2. `controlIoDone` 释放 in-flight credit；`submitControlIo` 成功提交后加 credit，
   保证在途 journal/checkpoint/format I/O 期间不休眠；
3. 所有线程向 metadata reactor `spdk_thread_send_msg` 后写它的 eventfd（control 提交、
   `postToDataReactor`、legacy data submit 同时补了目标 data reactor 的唤醒）；
4. Reactor 析构兜底关闭 eventfd，覆盖 mount 失败等早退路径。

验证：

| 指标 | 修复前 | 修复后 |
|---|---:|---:|
| metadata 线程 CPU（读-only round） | ~95%（busy-poll） | **~1%**（pidstat） |
| CPU0 组成（读-only round） | metadata 占满 | %usr 0、%sys 0、%soft 16.8%（网络 softirq，环境背景）、idle 83% |
| shared 读吞吐 | 34.4–35.0K | **35,276.5 ops/s**，p50 217 µs |
| direct 读吞吐 | 70.9–72.9K | 73,958.1 ops/s，p50 109 µs |
| fill（写路径 control I/O） | ~5 s | 4 s |
| CTest | 26/26 | 26/26 |

结论：metadata reactor 常驻 ~1 核的 busy-poll 已消除，读/写路径吞吐与延迟无回退；
CPU0 剩余的 softirq 来自 VM 网络（SSH/agent），与 HFS 无关。

### 11.11 bio / dio / shared / direct 四组对比（CPU 1-3 配置，2026-09-23）

配置调整：HFS 占用 CPU1-3（`data_reactors=1`、`metadata_cpu=1`、`data_cpus=2`、
`frontend_cpus=3`），测试 worker 固定 CPU4,5，进程 cpuset `1-5`；native NVMe IRQ
固定 CPU1-3（测后恢复）。选择 1 reactor 是因为 parser 禁止 frontend 与 metadata/data
重叠，2 reactor 需要 4 个 CPU。

同一会话，1 轮 524,288 Get，`stats=false`：

| 组别 | ops/s | p50 | p99 | 设备读/轮 |
|---|---:|---:|---:|---:|
| RocksDB bio（ext4 buffered，drop cache）r1 | 222,369.4 | 5.0 µs | 319.6 µs | 88,506 / 550 MB |
| RocksDB bio r2/r3（page cache 热） | 409,854.9 / 411,252.8 | 4.6 µs | ~11 µs | 0 |
| RocksDB dio（ext4 direct） | 72,038.7 / 71,582.8 / 71,204.3 | ~107 µs | ~280 µs | 494K / 2.326 GB |
| HFS shared（1 reactor） | 32,464.9 / 32,270.6 / 32,374.4 | ~248 µs | ~500 µs | SPDK 计数（stats off） |
| HFS direct（1 reactor） | 72,368.4 / 71,863.2 / 71,990.5 | ~112 µs | ~245 µs | SPDK 计数（stats off） |
| native dio 漂移复查 | 70,051.2 | 109 µs | 285 µs | 494K / 2.326 GB |

结论：

1. **bio 不是设备口径**：r1 只读 550 MB（page cache 吸收其余），r2/r3 设备读为 0，
   410K ops/s 全部来自 page cache；不能与 dio/HFS 同表排名，只能作为"内存足够时
   的上限参考"。
2. **dio 与 HFS direct 等价**：72.0K vs 72.1K，p50 107 vs 112 µs，设备读量同为
   2.326 GB/轮；1 reactor 下 direct 仍保持 2 reactor 水平（~72K）。
3. **HFS shared 1 reactor ≈ 32.4K**（2 reactor 为 34.4–35.3K，-6%），p50 248 µs；
   doorbell 收益仍在。
4. 1 reactor 后 CPU0 完全留给系统；HFS 占 1-3，测试占 4-5，满足本次统一布局要求。

### 11.12 写路径对比：bio / dio / HFS（2026-09-23）

规格与 11.11 读测试一致：524,288 keys（`--num=65536 × 8 threads`）、16B key、
1KB value、32MB block cache、`compression=none`、WAL 开、sync 关；worker 固定 CPU4,5，
进程 cpuset `1-5`；HFS 为 `metadata_cpus=1 / data_reactors=1 / data_cpus=2 /
frontend_cpus=3`。工具为 `hfs_db_bench`（新增 `--env=native`、`--worker_cpus`、
`--compression`）。

| 组别 | fill ops/s | 逻辑 MB/s | fill 秒 | 进程 wall 秒 | 设备写 |
|---|---:|---:|---:|---:|---:|
| native bio（buffered） | 199,239 / 205,388 | 197.6 / 203.7 | 2.63 / 2.55 | 4.6 / 4.6 | 1.84–2.32 GB |
| native dio（direct flush/compaction） | 213,500 / 211,643 | 211.8 / 209.9 | 2.46 / 2.48 | 10.0 / 10.2 | ~2.12 GB |
| HFS（`use_direct_io_for_flush_and_compaction=1`） | 241,145 | 239.2 | 2.17 | 3.7 | SPDK（内核计数 0） |
| HFS（同上 flag=0） | 239,306 | 237.3 | 2.19 | 3.8 | SPDK（内核计数 0） |

口径说明：

1. fill 阶段只计 Put；wall 包含 `Flush + CompactRange + close`，HFS 还包含 close 时的
   持久化 sync。native bio 的 wall 偏小是因为脏页在进程退出后才回写，不等于设备写完成；
   dio 为同步 O_DIRECT，wall 更接近真实设备时间。
2. HFS 写路径无 bio/dio 之分（adapter 写回缓冲 + SPDK bdev），该 flag 对 HFS 无效果
   （两轮差 0.8%）。
3. 本节结果：HFS 写 Put 吞吐最高（+13% vs native dio、+18~21% vs native bio），
   端到端 wall 也最短（3.7 s，且包含持久化）；上一轮 bio2 的 16 MB/s 抖动来自
   两次 run 共用 page cache 造成的回写限速，已用"每次 mkfs + drop caches"消除。
4. 写测试数字部分受 memtable/CPU 限制，端到端 wall time 是更可靠的设备口径。

### 11.13 RocksDB 1GB cache 测试矩阵（2026-09-23）

参数：524,288 keys（`--num=524288/threads`）、16B key、1KB value、4KiB block、
**block cache = 1 GiB**、compression=none、WAL 开、sync 关；worker 固定 CPU4,5、
cpuset `1-5`；HFS `metadata_cpus=1 / data_reactors=1 / data_cpus=2 / frontend_cpus=3`。
native 读测试前 `drop_caches`，读测试 3 轮（r1 冷、r2/r3 热）。工具为扩展后的
`hfs_db_bench`（新增 `--use_direct_reads/--rounds/--fresh/--block_size/readseq/fillrandom`）。

写（Put 阶段 ops/s；wall 含 Flush+CompactRange）：

| workload | threads | bio | dio | HFS |
|---|---:|---:|---:|---:|
| fillseq | 1 | 296,544 | 287,862 | **341,275** |
| fillseq | 2 | 211,254 | 217,852 | **235,677** |
| fillseq | 4 | 188,314 | 205,101 | **208,404** |
| fillseq | 8 | 201,133 | 202,693 | **237,440** |
| fillrandom | 1 | 204,773 | 201,804 | **220,819** |
| fillrandom | 2 | 176,590 | 202,110 | **209,027** |
| fillrandom | 4 | 190,732 | 184,869 | 179,323 |
| fillrandom | 8 | 171,307 | 162,018 | **189,376** |

读 r1（冷，drop_caches 后）：

| workload | threads | bio | dio | HFS |
|---|---:|---:|---:|---:|
| readseq | 1 | 90,923 | **91,178** | 83,279 |
| readseq | 2 | **176,618** | 175,412 | 173,083 |
| readseq | 4 | **296,218** | 288,890 | 290,614 |
| readseq | 8 | 376,141 | 376,802 | **432,654** |
| readrandom | 1 | **112,416** | 79,100 | 75,145 |
| readrandom | 2 | **210,462** | 157,126 | 163,887 |
| readrandom | 4 | **313,902** | 250,716 | 263,718 |
| readrandom | 8 | **405,628** | 325,155 | 360,818 |

读 r2/r3（RocksDB block cache 命中，取平均）：

| workload | threads | bio | dio | HFS |
|---|---:|---:|---:|---:|
| readseq | 1 | 1,167,266 | 1,130,671 | 1,161,806 |
| readseq | 2 | 2,236,367 | 2,219,350 | 2,181,903 |
| readseq | 4 | 2,200,877 | 2,257,656 | 2,203,320 |
| readseq | 8 | 2,273,889 | 2,159,794 | 2,265,806 |
| readrandom | 1 | 717,435 | 695,840 | 678,297 |
| readrandom | 2 | 1,396,514 | 1,365,500 | 1,342,981 |
| readrandom | 4 | 1,425,574 | 1,338,738 | 1,371,435 |
| readrandom | 8 | 1,402,486 | 1,322,220 | 1,353,295 |

结论：

1. **1GB cache 命中后三后端无差别**：r2/r3 全部由 RocksDB block cache 提供，
   readseq 0.44–0.86 µs/op、readrandom 0.70–1.49 µs/op，bio/dio/HFS 差异 <5–8%，
   即 warm 读性能与底层文件系统无关。
2. 冷读 r1 才体现文件系统差异：随机读 bio 最好（page cache 预读/合并，t8 406K），
   HFS 在 t2–t8 超过 dio（361K vs 325K @t8），顺序读 t8 HFS 最高（433K）。
3. 写路径 HFS 最好：fillseq 全部并发第一（208–341K），fillrandom 除 t4 外第一
   （189–221K）；native bio/dio 相当。
4. 异常：bio fillrandom t8 的 wall 34.6 s 是 page cache 回写限速，Put 计时不受影响；
   与 §11.12 的 bio2 抖动同因。
5. 并发扩展：读吞吐随线程数近似线性到 t4–t8（受 worker 只有 CPU4,5 限制）；
   写吞吐在 t1 最高、t2–t8 因 WAL/memtable 竞争略降。

### 11.14 高并发矩阵：HFS CPU 0-3（2 reactors）vs bio/dio（2026-09-23）

配置：HFS `metadata_cpus=0 / data_reactors=2 / data_cpus=1,2 / frontend_cpus=3`，
worker 固定 CPU4,5，cpuset `0-5`；native IRQ 固定 CPU0-3；1GB block cache、
524,288 keys、compression=none；读 3 轮（r1 冷、r2/r3 热）；并发 8/16/32/64。

写（Put ops/s）：

| workload | threads | bio | dio | HFS |
|---|---:|---:|---:|---:|
| fillseq | 8 | 208,715 | 207,989 | **231,339** |
| fillseq | 16 | 181,336 | 184,450 | **219,266** |
| fillseq | 32 | 182,581 | 172,073 | **192,885** |
| fillseq | 64 | 157,824 | 158,269 | **163,856** |
| fillrandom | 8 | 163,924 | 169,535 | **203,569** |
| fillrandom | 16 | 151,399 | 152,963 | **183,162** |
| fillrandom | 32 | 142,654 | 145,860 | **166,027** |
| fillrandom | 64 | 132,462 | 127,615 | **149,944** |

读 r1（冷，drop_caches 后）：

| workload | threads | bio | dio | HFS |
|---|---:|---:|---:|---:|
| readseq | 8 | 368,650 | 366,609 | **449,243** |
| readseq | 16 | 376,366 | 383,359 | **613,753** |
| readseq | 32 | 381,075 | 374,593 | **609,891** |
| readseq | 64 | 367,575 | 371,342 | **614,267** |
| readrandom | 8 | **404,108** | 324,965 | 360,904 |
| readrandom | 16 | 417,528 | 335,780 | **433,901** |
| readrandom | 32 | 420,380 | 332,348 | **459,260** |
| readrandom | 64 | 407,017 | 324,078 | **476,745** |

读 r2/r3（block cache 命中）：readseq ≈2.1–2.3M、readrandom ≈1.3–1.4M，
bio/dio/HFS 三个后端无差别（t64 因 64 线程压 2 个 worker CPU 略降）。

结论：

1. 2 reactor + worker-direct 让 HFS 在高并发冷读上明显领先：顺序读 t16–t64
   ≈610K ops/s（bio/dio ≈375K），随机读 t16–t64 433–477K（bio 404–420K、
   dio 325–336K）；t8 时随机读仍是 bio 略优。
2. 写路径 HFS 在全部并发档位领先（fillseq 164–231K、fillrandom 150–204K）。
3. 1 vs 2 reactor 同布局对照（1R control：仅改 `data_reactors=1`/`data_cpus=1`，
   其余 CPU 布局相同，t8/16/32/64）：写 ±4% 以内；冷顺序读 1R 持平或更好
   （t32 666K vs 610K）；冷随机读 2R 仅 +2~4%；热读相同。**2 reactor 在本
   workload 没有可测吞吐提升，收益只是 actor CPU headroom**。§11.14 初稿中
   "2R 在 t16 以上有扩展性收益"是拿 2R 高并发对比 1R t8，属错误对照，已作废。
4. 异常：bio fillrandom t8 与 dio readseq t8 的 wall 偏大（33.1s/32.1s）是
   page-cache 回写/`drop_caches` 前的 `sync` 等待，基准计时不受影响。

### 11.15 单 reactor 上限探测（2026-09-23）

工具：`spdk_nvme_perf`（裸设备上限）、`fio-hfs.so`（HFS 深队列，job 不额外绑核，
cpuset 0-5，reactor CPU1）；HFS 1 reactor（metadata 0 / data 1 / frontend 3）。

裸设备（spdk_nvme_perf，CPU1）：

| workload | QD | IOPS | MiB/s | avg lat |
|---|---:|---:|---:|---:|
| 4K randread | 32 | 111,371 | 435 | 287 µs |
| 4K randread | 128 | 111,990 | 437 | 1,143 µs |
| 4K randread | 256 | 128,051 | 500 | 1,999 µs |
| 64K seq read | 32 | 61,038 | 3,815 | 524 µs |

HFS 单盘 1 reactor（fio-hfs，512MB 文件）：

| workload | 并发 | IOPS | MB/s | p99 | reactor CPU1 |
|---|---|---:|---:|---:|---:|
| 4K randread | 8×32 (QD256) | 112,672 | 462 | 3.1 ms | 91% |
| 4K randread | 16×32 (QD512) | 93,688 | 384 | 5.7 ms | 83% |
| 4K randread | 32×32 (QD1024) | 67,150 | 275 | 10.9 ms | 65% |
| 64K seq read | 8×16 (QD128) | 56,158 | 3,680 | 3.0 ms | 91% |

双盘（nvme0+nvme1，1 reactor，512MB 文件）：QD256 109,204 IOPS（reactor 91%）、
QD512 103,466、64K 顺序 3,617 MB/s —— 与单盘无实质差异。

结论：

1. **4K 随机：单 reactor 与设备上限重合**。HFS QD256 = 112.7K，裸设备 QD128/256 =
   112.0K/128.1K；reactor 91% 说明已接近其处理上限，但设备同期也在上限，
   无法区分谁先到。QD 继续加深只增加排队（p99 恶化、IOPS 反而下降）。
2. **64K 顺序：设备带宽先到**（HFS 3.68 vs 裸 3.82 GB/s = 96%）。
3. **双盘没有翻倍**（109K vs 113K），但该结论受下面异常影响，不单独作为 reactor
   上限证据。
4. **发现异常（待查）**：同一 HFS 文件从 512MB 换成 4GB 后，4K 随机读跌到
   43–56K、reactor 仅 47–48%（单盘复现、连跑两次不变），512MB 同会话恢复
   90.8–112.6K。裸设备对整个 4GB 设备仍能 113K，说明不是 reactor 饱和，疑似
   虚拟盘 thin-provision/host-side 效应；在查清前，单 reactor 硬上限只能标注为
   "≈112–128K 4K 随机 ops/s（与设备同级）"，不能断言 reactor 是唯一瓶颈。

## 附录 A：数据来源

| 数据 | 来源 |
|---|---|
| shared stats=true 22,759.7 / p50 342 µs | `run_hfs_profile_v2_20260922.py`，`/mnt/hfs-data` 输出 |
| direct stats=true 37,208.6 / p50 194 µs | `run_hfs_direct_v2_final_20260922.py` |
| direct stats=false 3 轮 30,647 | `run_hfs_direct_v2_perf_20260922.py` |
| shared stats=false 26,153 / native 60,088 | `run_same_nvme_two_data_cpu_compare_v2_20260922.py` |
| 旧同盘 native 21,964 / shared 17,704 / direct p99 9.15 ms | `run_same_nvme_direct_compare_20260922.py`、`run_same_nvme_hfs_shared_20260922.py` 对应 VM 日志 |
| 环境拓扑 / fio 每 CPU 延迟 | `probe_perf_analysis_20260922.py`、`probe_native_latency_per_cpu_20260922.py` |
| 挂死复现 | §2.3 三个运行记录 |

## 附录 B：复现命令（当前工作区）

```bash
# 环境探针（拓扑 / 旧日志 / fio 每 CPU）
python E:/Code/test_script/probe_perf_analysis_20260922.py
python E:/Code/test_script/probe_old_native_logs_20260922.py
python E:/Code/test_script/probe_native_latency_per_cpu_20260922.py

# shared stats=true profile
python E:/Code/test_script/run_hfs_profile_v2_20260922.py

# direct stats=true（挂死复现，取数后需外部 kill）
python E:/Code/test_script/run_hfs_direct_v2_final_20260922.py

# direct stats=false 3 轮
python E:/Code/test_script/run_hfs_direct_v2_perf_20260922.py

# 2 reactor shared/native 基线（交接文档来源）
python E:/Code/test_script/run_same_nvme_two_data_cpu_compare_v2_20260922.py
```

注意：以上脚本均为临时实验脚本；W0 的正式协议脚本需重新整理为可复用入口，并在
任务完成后按约定清理临时证据文件。