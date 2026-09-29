# HFS 2.0 MySQL 写 p99 归因（2026-09-29）

> 背景：同盘门禁（`hfs2_gate_remeasure_20260929.md`）HFS 写 p99 = native 的
> 117.6% / 128.7%，超过 §16.2 的 ≤110% 目标。本文定位增量来源。

## 1. 结论

- **约 20% 的 InnoDB 写走同步回退**：`async_fallbacks=21,016` /
  `async_writes=88,950`（19.1%）；
- 其中 **21,013 / 21,016 是单一文件（inode 32）的追加写**：每次
  `offset == logicalSize`（例：`offset=1,910,248 len=1,756`、
  `offset=9,996,628 len=1,756`），被核心异步写的固定覆盖检查
  （`offset + length > logicalSize` → `InvalidArgument`）拒绝；
- 回退后走 seam 的同步 `File::write` → 同步 DataPath，每 I/O 一次
  `spdk_thread_send_msg` 投递到单一 data reactor（与 bug #42 同机制），
  高并发下形成消息环长尾 → 写 p99；
- 屏障分段（186,370 barriers）：`barrier=93/4457 µs`、`device_flush=48/3264 µs`、
  `ckpt_freeze=25/107`、`ckpt_snapshot=48/133`、`meta_write=229/1819 µs`、
  checkpoints=102。单次屏障最坏 4.5 ms；客户端 p99（9.9 ms）主要由排队构成，
  HFS 相对 native 的增量来自回退写的同步路径。

## 2. 证据

- `sysbench oltp_write_only`（8 线程，trx=1）：HFS 2,606 TPS / p99 9.91 ms；
  `oltp_read_write` 2,029 / 9.22 ms；`oltp_read_only` 10,748 / 1.86 ms；
- seam 计数：`async_writes=88,950`、`async_fallbacks=21,016`、`async_reads=961`
  （读几乎全部命中 buffer pool）；
- 临时核心侧诊断（已回退）：被拒写的 `offset == logicalSize`（追加语义）、
  `len ∈ {1756, 3512, 5268, 7024, 8192}`（k×1756）、inode 32 占 21,013/21,016；
- 诊断回退后 `client.cpp` 与 `hfs_vfs.cc` 与 HEAD 一致，CTest 29/29；
  证据目录 `/home/hfs/mysql-p99-20260929`、`/home/hfs/mysql-fb-20260929`。

## 3. 修复方向（下一 shard，需要设计决策）

1. **异步追加写**：让异步路径支持 `offset == logicalSize` 的追加（提交前
   reserve + setSize），或 seam 侧 **write-ahead 扩展**（按 1 MiB 粒度预扩展，
   摊薄扩展成本），使 redo 追加写回到异步快路径；
2. 或 **同步路径去消息环**：seam 的同步回退直接走 DataPath 提交，
   避免单一 data reactor 的消息队列；
3. 验收：同盘 MySQL 写 p99 ≤110%、`async_fallbacks` 比例 <5%、CTest 与
   MySQL SIGKILL 崩溃矩阵不回退；若改变文件 size 语义需先更新设计（§20.4/§20.5）。

## 4. 边界

- 该回退是核心异步路径的**固定覆盖契约**（RocksDB `Allocate` 语义）与 InnoDB
  **追加写**语义的差异，不是 bug；
- inode 32 的具体文件（redo/undo/dblwr）未在核心侧暴露路径，按 offset/len 形态
  判断为 redo 类追加文件；如需精确归属可在 seam 侧按句柄记录路径（后续可选）。
