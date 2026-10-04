# HFS 2.0 RocksDB kFSBuffer 读路径设计评审（Shard-111，2026-09-29）

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

> 前置：`doc/review/hfs2_zero_copy_frontend_review_20260929.md` §3.3（P2-1 RocksDB
> 结论：`memory_allocator` 不覆盖 direct 读，唯一路径是 kFSBuffer）。
> 结论：**采纳 kFSBuffer + 非 direct 读 + arena `fs_scratch`**；adapter 内实现，
> 核心与 public ABI 不变；预取路径保持 RocksDB scratch（bounce）。

## 1. 源码事实（RocksDB 9.11.2）

- `BlockFetcher::ReadBlock`：`if (file_->use_direct_io())` → 读入 RocksDB 自分配的
  `AlignedBuf`；`else if (use_fs_scratch_)` → `MultiRead`，`scratch == nullptr`，
  FS 通过 `fs_scratch`（`FSAllocationPtr = unique_ptr<void, function<void(void*)>>`）
  交出 buffer 所有权，`result` 指向实际数据；
- `use_fs_scratch_` 仅在 `CheckFSFeatureSupport(fs, FSSupportedOps::kFSBuffer)` 为真
  时置位；
- `GetBlockContents()` 对 `use_fs_scratch_` 的无压缩块执行
  `CopyBufferToHeapBuf()`（`memory_allocator` 的 `heap_buf_`）→ **RocksDB 内部
  仍有一次 memcpy**，这是 BlockContents 所有权决定的，FS 无法消除；
- `FilePrefetchBuffer` 使用 RocksDB 自带 `scratch`（`req.scratch =
  buf->buffer_.BufferStart()`）→ 预取/迭代器读继续走 bounce，不在本项范围；
- `FSReadRequest::scratch` 为输入提示，FS "can ignore this and allocate its own
  buffer"。

## 2. 契约（加入 `hfs2.0.md` §11.2）

1. `HfsFileSystem::SupportedOps` 声明 `kAsyncIO | kFSBuffer`；
2. `HfsRandomAccessFile::use_direct_io()` 返回 false（顺序文件保持 true，
   其调用方自带 scratch）；
3. `MultiRead`/`ReadAsync` 在 `scratch == nullptr && len != 0` 时：
   - **先按 512 对齐 span**（RocksDB 只在 `use_direct_io()` 时自行对齐；kFSBuffer
     路径下 FS 收到的是未对齐请求，必须自行对齐，否则 HFS planner 会生成
     head/tail RMW 子请求）；
   - 对齐后的 span ≤1 MiB 时优先 `Engine::acquireDmaBuffer`（class 池），写
     `fs_scratch`（deleter 捕获 engine + size，调用 `releaseDmaBuffer`），
     `scratch` 指向该 buffer 以复用现有提交路径；读完后 `result` 指向
     “调用方原始偏移”并截断到原始长度；
   - >1 MiB（预取 readahead）或 arena 不可用时回落 `new char[len]`
     （deleter `delete[]`），设备路径自动 bounce，功能不回退；
4. 读完成后 RocksDB 持有 `fs_scratch` 直到 BlockContents 拷贝完成，随后调用
   deleter 归还 arena；
5. 设备读 0 拷贝（设备 → arena），RocksDB 侧一次内部 memcpy 保留；
6. core `SpdkRuntime::isDmaCapable` 要求 **dword 对齐**（SPDK PRP 要求）：
   非对齐候选一律 bounce。该修复独立于本项，消除了“odd buffer 直接提交设备
   导致 `not dword aligned` 失败”的潜在缺陷。

## 3. 收益与边界

- 收益 = 去掉 HFS arena bounce lease（全局 mutex + 有序索引）+ copy-out
  （arena→RocksDB `AlignedBuf`）；大工作集（设备读占比高）时可见；
- 边界：
  - 预取路径仍 bounce；
  - 块压缩时 `GetBlockContents()` 走 `compressed_buf_` 路径，同样一次内部拷贝；
  - `use_direct_io()==false` 使 WAL/日志等顺序读不再要求对齐 scratch，行为不变
    （这些路径本就不走 BlockFetcher）；
  - arena 预算：每请求 ≤ block_size+trailer，class 池复用；`dma_arena_mb` 需覆盖
    并发在途读（512 MiB 默认远超需求）。

## 4. 验收

- `hfs_db_bench readrandom`（stats=true）出现 `read_direct_user_children > 0`、
  `read_copy_out_bytes` 下降；
- 同会话 pre/post 二进制 A/B：大工作集 readrandom 吞吐不回退（目标：相对 native
  的差距收窄，记录实际值）；
- RocksDB 校验和（`verify_checksums`）与 `hfsctl check corruption=none` 通过；
- 预取/压缩口径单独声明为 bounce。

**实际结果（2026-09-29，见 `doc/performance/hfs2_rocksdb_kfsbuffer_20260929.md`）**：

- 三轮迭代（v1 无对齐 −42% → v2 适配器对齐 −3.2% → **v3 对齐 + ≤1 MiB arena
  上限：post/pre ops = 1.0016，p50 0.982，p99 1.016**）；
- 最终：`read_direct_user_children=170,098`、`read_bounce_children=0`、
  `read_copy_out_bytes=0`，设备读字节 +0.01%，SPDK 对齐错误 0；
- CTest 29/29。

## 5. 风险

| 风险 | 缓解 |
|---|---|
| `fs_scratch` 生命周期与结果 slice 不一致 | RocksDB 持有到 BlockContents 拷贝后（源码核对）；错误路径 `ReleaseFileSystemProvidedBuffer` 也会释放 |
| arena 预算耗尽 | 回落 heap buffer（bounce），不失败 |
| engine 已停止后 deleter 触发 | `releaseDmaBuffer` 在 arena 不存在时为 no-op；arena 析构兜底 |
| `use_direct_io()==false` 影响其他读路径 | 只改随机读文件；顺序文件与写文件不变；CTest/RocksDB L4 回归 |