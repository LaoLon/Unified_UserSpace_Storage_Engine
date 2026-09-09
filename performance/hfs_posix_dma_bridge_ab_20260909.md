# 普通 POSIX buffer 到 DMA/direct-async 原型验证

日期：2026-09-09
范围：普通 `malloc`/POSIX buffer 在 HFS 同步 `pread`/`pwrite` 路径上的 DMA staging 与
direct async 提交原型。

## 结论

本轮原型功能正确，但没有带来性能收益，默认不能打开：

- 普通 POSIX buffer 不能仅靠 `hfsRegisterBuffer` 变成 SPDK 可直接访问的物理 DMA buffer；
  当前 Linux/SPDK 约束下仍需要一次 buffer→DMA pool copy，读路径还需要一次 DMA pool→buffer copy。
- `HFS_POSIX_DIRECT_ASYNC=1` 会先取得 HFS DMA pool lease，再调用现有
  `hfsIoReadAtAsync`/`hfsIoWriteAtAsync`，最后用条件变量把同步 POSIX 调用等待到完成。
- 原有同步路径已经是“DMA pool + 一次 SPDK submit + 同步 completion”；新桥接路径在相同 copy
  之外又增加了异步对象、callback、条件变量和额外 try-lock，因此当前 qd32 NVMe 测试下降约
  15%～30%。

该实现保留为默认关闭的实验开关，用于验证路径和后续 profiling，不应作为当前性能优化发布。

## 实现边界

`src/hfs_io.c` 新增 `HFS_POSIX_DIRECT_ASYNC` 环境开关。仅当请求已经按 HFS unit 对齐、
调用方 buffer 不能直接通过 `spdk_vtophys` 使用、且开关为真时，才走该桥接路径。非对齐请求、
DMA-capable buffer、pool 不足或 direct async 提交失败时仍回退到原有路径。

重要的是，这不是零拷贝：

```text
普通 POSIX buffer
        │
        ├─ write: memcpy 到 HFS DMA lease ──> SPDK direct async
        └─ read : SPDK direct async ──> memcpy 回普通 POSIX buffer
```

真正的零拷贝仍要求调用方从 `hfsDmaAlloc`/SPDK DMA allocator 取得 buffer，并使用
`hfs_lib_async_pread`/`hfs_lib_async_pwrite`；该 API 已有 direct async 路径和 registered-DMA
验证，不应把普通 POSIX 内存伪装成 registered DMA。

## 正确性验证

VM 内执行：

```bash
HFS_POSIX_DIRECT_ASYNC=1 \
  bash scripts/db/io_fast_path_smoke.sh \
  build/lib/libhfs.so config/hfs_db_aio.json \
  /var/tmp/hfs-posix-direct-bridge-smoke.img \
  /var/tmp/hfs-posix-direct-bridge-smoke
```

结果：`REQUEST-LOCAL FAST IO OK`，所有请求错误为 0；`make -B app lib -j2` 通过。

## NVMe A/B

测试使用同一 VM、同一 `Nvme0n1`、同一 HFS 配置、FIO 3.42、`randrw` 50/50、256 MiB、
5 秒、HFS `numjobs=32`。关闭/打开桥接仅改变 `HFS_POSIX_DIRECT_ASYNC`，错误数均为 0。

| block size | bridge off IOPS | bridge on IOPS | 变化 | on p50 R/W (us) | on p99 R/W (us) |
|---:|---:|---:|---:|---:|---:|
| 4 KiB  | 61,226.51 | 52,011.40 | -15.0% | 501.760 / 528.384 | 1,433.600 / 1,957.888 |
| 8 KiB  | 47,530.29 | 40,616.08 | -14.5% | 518.144 / 552.960 | 3,653.632 / 4,145.152 |
| 16 KiB | 45,203.36 | 38,349.46 | -15.2% | 552.960 / 585.728 | 3,751.936 / 4,292.608 |
| 32 KiB | 46,732.05 | 32,843.26 | -29.7% | 749.568 / 806.912 | 2,703.360 / 3,522.560 |
| 64 KiB | 36,955.42 | 28,530.99 | -22.8% | 774.144 / 921.600 | 2,998.272 / 4,947.968 |

原始输出保留在 VM：
`/var/tmp/hfs-posix-direct-bridge-fio-20260909/{off,on}`。

## 根因与下一步

当前 POSIX API 是同步语义，调用线程提交一个请求后必须等待完成；把同一请求包装成
callback+condition variable 并不会形成类似原生 SPDK 的提交队列，反而增加调度层次。
因此后续优化按以下优先级进行：

1. 将需要高并发的调用方迁移到已有的 `hfs_lib_async_*`，由调用方显式使用 DMA buffer，
   并用批量 submit/completion 保持多个 outstanding request。
2. 如果必须保留 POSIX buffer，研究线程级可复用 DMA staging 和批量 flush，目标是摊薄
   pool acquire、callback 和 wait 开销；但该方案仍至少保留一次 copy，不能承诺零拷贝。
3. 只有在 profiling 证明 direct async 版本的 completion/锁开销被批量化后，才考虑扩大
   `HFS_POSIX_DIRECT_ASYNC` 的适用范围；在此之前保持默认关闭。
