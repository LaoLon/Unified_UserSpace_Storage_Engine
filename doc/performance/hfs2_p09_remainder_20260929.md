# HFS 2.0 P0-9 剩余小项实施与验收（2026-09-29）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

> 台账：`doc/review/hfs2_architecture_optimization_review_20260928.md` §3 P0-9、
> §14.2。本轮完成 ②（`File::read/write` 每次重建 `extentMapFor`）与
> ③（direct 路径 `isBufferDmaCapable` 双调用）；①（sink/handle intrusive ref）
> 仍需 profile 证据，未做。

## 1. 改动内容

### 1.1 ② 同步路径复用 per-inode 快照

`src/core/client.cpp`：

- 抽出 `File::Impl::cachedSnapshot()`：按 per-inode data version 命中缓存并记账；
- `asyncSnapshot()` 改为先查缓存再走原有 stat/extents 重试构建；
- 新增 `buildSnapshot()`（不走缓存、直接构建，保持同步路径“必定成功”语义）与
  `syncSnapshot()`（缓存优先，churn 超时回落直接构建）；
- `File::read` 用 `syncSnapshot()` 的 `stat.logicalSize` 与 `extentMap`，不再每次
  `stat + extentMapFor`；
- `File::write` 预写尺寸用 `cachedSnapshot()` 命中（miss 时仅 `stat`，不建图），
  提交后用 `syncSnapshot()` 取图；
- `File::append` 复用快照取 append 偏移。

正确性依据：所有改变 size/extent 的路径（`commitExtents`、`setSize`、
`truncate`、`freeInode`、checkpoint restore、replay Alloc/SetSize）都递增
per-inode data version；`releaseExtents` 只回收未发布的预留，不递增。

`tests/snapshot_cache_device_test.cpp` 期望值随同步写路径参与缓存而更新
（hits=6 builds=4，含注释说明每次递增来源）。

### 1.2 ③ direct 子请求缓存 DMA 探针结果

- `AsyncDataBackend::Request` 新增 `dmaVerified`；
- `AsyncDataService::acquireLeases` 已在 direct 判定时执行
  `backend->isBufferDmaCapable()`，结果写入 `ChildState::directVerified`；
- `backendRequest()` 仅对“已验证且非 lease”的 direct 子请求置 `dmaVerified`；
- `SpdkAsyncDataBackend::submit/pump` 对 `dmaVerified` 跳过第二次
  `spdk_vtophys`；lease/RMW 子请求仍由 backend 校验，行为不变。

## 2. 功能验收

- `ctest --test-dir build-verify -j1`：**29/29 通过**（含
  `snapshot_cache_device_test: hits=6 builds=4 ok`、`data_path_device_test`、
  `async_data_service_test`、`spdk_async_backend_test`）。

## 3. 同会话 A/B（pre/post 同树库，交替 3 轮，`runtime=10s`）

构建两个 `libhfs_core.so`（同一天、同树、仅本轮改动差异），共用同一
`hfsctl`/`fio-hfs.so`，每轮先 `hfsctl format`，交替顺序运行：

| 口径 | pre 中位 IOPS | post 中位 IOPS | post/pre | p99 读/写 |
|---|---:|---:|---:|---:|
| `hfs_async=0` 16j QD8 4K randrw 50/50 | 46043.2 | 46086.3 | 1.0009 | 1.0000 / 1.0000 |
| `hfs_async=1` 1j32 4K randrw 50/50 | 103927.1 | 104116.6 | 1.0018 | 1.0000 / 1.0144 |

- 两个口径都在设备饱和区（同步 46k、异步 104k IOPS），去掉的 CPU 工作不是瓶颈；
- 结果在噪声内，**无回退**；p99 写 +1.4% 为单个直方图档位差；
- closure 全部闭合、`error=0`。

## 4. 结论

- ②③ 属于“消除每请求冗余”的小项：功能与回归通过，吞吐无可见收益；
- 真正的收益场景是**高并发下 namespace 锁竞争**与**高 IOPS 下的 CPU 占比**，
  当前 FIO 形状未暴露；如后续在 8j4/MySQL 场景出现锁竞争证据，可再复测；
- ① 保持未做，等待 profile 证据（intrusive in-flight sink ref 需要关闭/提前
  释放用例）。