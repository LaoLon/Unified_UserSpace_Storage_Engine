# HFS direct-DMA 异步并发矩阵

## 目的

验证注册 DMA buffer 的异步 positioned I/O 是否真正形成并发窗口，并观察并发深度从
1 到 32 时的吞吐、SPDK backend outstanding、运行时提交队列和 bounce-copy 情况。
本轮同时覆盖单 reactor 边界，用于验证完成路径和退出阶段的生命周期安全性。

## 测试口径

- VM 内 NVMe SPDK bdev：`Nvme0n1`
- HFS：`build/lib/libhfs.so` + `config/hfs_nvme.json`
- 请求：64 KiB、4 KiB 对齐、`hfsDmaAlloc()` 分配并 `hfsRegisterBuffer()` 注册
- 每个 depth 启动独立 Python 进程，执行 16 轮写入和 16 轮读取；读回校验首字节
- 深度：`1, 2, 4, 8, 16, 32`
- HFS data reactors：主矩阵 4；边界回归 1
- 统计：`io_direct_ops/io_copy_ops`、真实 `io_backend_outstanding`、
  `runtime_queue_depth_peak`、`async_submit/async_complete`
- 脚本：`scripts/db/direct_dma_async_matrix.sh`

## reactors=4 主矩阵

结果目录：`/var/tmp/hfs-direct-dma-matrix-20260909`。吞吐单位为 IOPS，时间单位为毫秒。

| depth | write IOPS | read IOPS | write ms | read ms | direct ops | copy ops | backend peak | queue peak | async submit/complete |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1  | 3,118.82  | 2,915.17  | 5.130 | 5.489 | 32   | 0 | 1  | 1  | 32/32 |
| 2  | 7,670.22  | 8,208.36  | 4.172 | 3.898 | 64   | 0 | 2  | 2  | 64/64 |
| 4  | 14,706.02 | 14,801.92 | 4.352 | 4.324 | 128  | 0 | 4  | 4  | 128/128 |
| 8  | 12,985.98 | 19,990.93 | 9.857 | 6.403 | 256  | 0 | 8  | 8  | 256/256 |
| 16 | 17,072.84 | 16,029.45 | 14.995 | 15.971 | 512  | 0 | 16 | 16 | 512/512 |
| 32 | 21,984.15 | 25,402.59 | 23.290 | 20.155 | 1,024 | 0 | 32 | 32 | 1024/1024 |

所有行 `write_errors=0/read_errors=0`，脚本最终输出 `DIRECT DMA ASYNC MATRIX OK`。
`backend_outstanding` 每行最终均回到 0，说明没有遗留后端请求；`io_copy_ops=0` 且
direct accounting 与 `2 * depth * rounds` 一致，说明这条 registered-DMA 路径没有触发
bounce copy。

## reactors=1 边界回归

首轮矩阵曾在 depth=2 退出阶段偶发 SIGSEGV。故障地址还原到
`spdk/lib/blob/blobstore.c:9291 (spdk_blob_close)`，不是 NVMe 数据校验失败，而是
异步完成与卸载之间的生命周期竞态。

修复后使用相同的 64 KiB/16 rounds/6 depths 连续执行 3 轮，全部通过。代表性首轮如下：

| depth | write IOPS | read IOPS | errors | direct/copy | backend peak | queue peak |
|---:|---:|---:|---:|---:|---:|---:|
| 1  | 9,214.71  | 10,558.13 | 0 | 32/0   | 1  | 1  |
| 2  | 9,399.14  | 14,755.01 | 0 | 64/0   | 2  | 2  |
| 4  | 16,275.21 | 17,815.97 | 0 | 128/0  | 4  | 4  |
| 8  | 22,994.45 | 22,647.28 | 0 | 256/0  | 8  | 8  |
| 16 | 22,349.97 | 25,213.54 | 0 | 512/0  | 16 | 16 |
| 32 | 17,674.59 | 22,154.19 | 0 | 1024/0 | 32 | 32 |

## 代码修复

`hfs_lib_async_direct_done()` 和 `hfs_lib_async_direct_finish()` 原先先调用
`hfsAsyncCompleteDirect()`，再释放 `HfsOpenFile`。前者会唤醒 waiter 并递减
`directInFlight`，因此 `hfsAsyncFini()` 可能在 open-file/blob 引用仍未退休时开始卸载
blobstore。现在改为：

1. 完成元数据发布；
2. 释放 `HfsOpenFile`；
3. 调用 `hfsAsyncCompleteDirect()` 发布用户可见完成；
4. 释放 direct context。

这样异步完成、in-flight 计数和 SPDK blob 生命周期使用同一个安全边界。

## 结论与后续

1. direct-DMA 异步路径已证明可以随 outstanding depth 增长，depth=32 时 backend
   outstanding 和提交队列均达到 32。
2. 注册 buffer 路径没有 bounce copy；当前剩余差距主要是 SPDK reactor/VM 调度、HFS
   元数据发布与 POSIX 生命周期维护成本，而不是用户 buffer 搬运。
3. 后续性能比较应固定 VM CPU 绑定、NVMe cache 状态和预热轮次；单次 IOPS 在不同 VM
   运行间波动明显，趋势比单点值更可靠。
