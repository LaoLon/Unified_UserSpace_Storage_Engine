# HFS 2.0 RocksDB kFSBuffer 读路径（Shard-111）实施与验收（2026-09-29）

> 设计：`doc/design/hfs2.0.md` §11.2；评审：`doc/review/hfs2_rocksdb_kfsbuffer_review_20260929.md`。
> 实现：`rocksdb/hfs_filesystem.cpp`（kFSBuffer + 对齐 arena `fs_scratch`）、
> `src/runtime/spdk_runtime.cpp`（direct DMA 对齐校验）。

## 1. 机制

1. `HfsFileSystem::SupportedOps` 声明 `kAsyncIO | kFSBuffer`；
2. `HfsRandomAccessFile::use_direct_io()` 返回 false → `BlockFetcher` 走
   `MultiRead` 的 `fs_scratch` 分支；预取缓冲（`FilePrefetchBuffer::
   FSBufferDirectRead`）同样走 `fs_scratch`；
3. adapter 在 `scratch == nullptr` 时按 512（`GetRequiredBufferAlignment`）
   将请求对齐到块边界，从 arena 分配对齐 span 的 buffer（≤1 MiB，走 class 池），
   读完后 `result` 指向“调用方原始偏移”处并截断到原始长度；>1 MiB（预取
   readahead）回落 heap buffer，由 core bounce；
4. arena 不可用时回落 `new char[]`（功能不回退，设备路径自动 bounce）；
5. core `SpdkRuntime::isDmaCapable` 增加 **dword 对齐**校验（SPDK PRP 要求），
   非对齐 span 一律 bounce——修复了“odd buffer 直接提交设备导致
   `nvme_pcie_prp_list_append: not dword aligned` 失败”的潜在缺陷。

## 2. 三轮迭代（同会话 pre/post，交替 2 轮，stats=true）

口径：`hfs_db_bench`/observer readrandom，200000 keys × 1 KiB，8 线程，
32 MiB block cache，compression=none，每轮先 format 再 fill+compact+readrandom。

| 迭代 | post/pre ops | p50 | p99 | direct | bounce | copy-out bytes | 说明 |
|---|---:|---:|---:|---:|---:|---:|---|
| v1：仅 kFSBuffer，无对齐 | 0.5806 | 1.747 | 1.591 | 83,563 | 424,481 | 405,695,394 | 未对齐预取请求触发 head/tail RMW 各约 169k |
| v2：适配器对齐，无大小上限 | 0.9676 | 0.9925 | 1.1498 | 170,092 | 0 | 0 | 大预取走 arena 大块（zeroed `spdk_dma_zmalloc/free`）拉低 p99 |
| **v3：对齐 + ≤1 MiB arena 上限（最终）** | **1.0016** | **0.9820** | **1.0159** | **170,098** | **0** | **0** | 设备读字节 +0.01%，RMW=0 |

v3 明细（中位）：

| 侧 | ops/s | p50 ns | p99 ns | direct | bounce | copy-out | 设备读字节 |
|---|---:|---:|---:|---:|---:|---:|---:|
| pre | 80,621.2 | 106,572 | 246,324 | 0 | 170,097 | 800,774,656 | 800,760,832 |
| post | 80,747.9 | 104,654 | 250,252 | 170,098 | 0 | 0 | 800,851,456 |

- SPDK 对齐错误：0；每轮 `closure` 闭合、`error=0`；
- CTest 29/29（含 `data_path_device_test`、`snapshot_cache_device_test`）。

## 3. 结论与边界

- RocksDB 读路径实现**设备级零拷贝**（设备 → arena 直通），copy-out 归零；
  吞吐与 pre 持平（消除的 memcpy 与 arena 分配/对齐开销相抵），p50 −1.8%；
- RocksDB 仍有一次 `fs_scratch → heap_buf_` 内部拷贝（`BlockContents` 所有权），
  这是 kFSBuffer 契约下的上限；
- >1 MiB 预取 readahead 与预取 buffer 复用仍走 bounce，基准口径必须声明；
- core 对齐校验是独立于本项的**正确性修复**：任何非 dword 对齐的 direct
  候选都会被降级为 bounce，不再把失败留给设备提交；
- 后续如需进一步收益，可评估 arena 大块池化或预取路径的 fs_scratch 复用
  （需单独设计与测量）。
