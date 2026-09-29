# HFS 2.0 metadata footprint 证据（P1-5 子项闭合，2026-09-29）

> 设计依据：`hfs2.0.md` §17.1（metadata memory ≤1 KiB/active inode，不含实际
> name bytes 与 DMA arena）。目的：判断 P1-5 的两个子项（hot/cold split、
> `kMaxChildren` 4+ 有界溢出块）是否有实现价值。

## 1. 结论

- **namespace footprint = 297 B/active inode**（1M inode 实测）：设计预算
  ≤1 KiB/inode 的 **3.4× 余量** → **hot/cold split 不需要**；
- **request-state 池 = 16,232 B × `maxOperations`(4096) ≈ 66.5 MB**：有界固定成本，
  约为 7.4 GiB VM RAM 的 0.9% → **`kMaxChildren` 4+ 有界溢出块不需要**；
- 两项加入回归守卫：`namespace_test` 的 1M inode footprint 断言（<1 KiB/inode）
  与 async service 启动日志
  `request_state_bytes=16232 operations=4096 children=64`。

## 2. 方法

- **namespace**：`tests/namespace_test.cpp` 新增 footprint 段——在结构测试
  （无 SPDK/journal）中创建 1M 个文件（`/d/f%07u`），读取
  `/proc/self/status` 的 `VmRSS` 前后差值 / 1M；含目录项与短名（8 字符）开销，
  不含 DMA arena；
- **request-state**：`AsyncDataService` 初始化时记录 `sizeof(RequestState)`
  （固定 `children[kMaxChildren=64]` + `ExtentSnapshotMap` 主导）× 服务容量
  （`maxOperations` = `operation_pool` 4096）。

## 3. 证据

```
namespace footprint: inodes=1000000 rss_delta_kb=290048 per_inode_bytes=297
hfs: async data service: request_state_bytes=16232 operations=4096 children=64
```

- `namespace_test` 断言 `per_inode_bytes < 1024`（设计预算），回归即失败；
- CTest 29/29（套件时长 2.3 s → 4.1 s，footprint 段约 1.5 s）。

## 4. 边界

- 精确拆分"不含 name bytes"未做（名字内联在 map 节点中）；297 B 已包含短名开销，
  仍远低于预算；
- 实际进程内存还包含 DMA arena（≤512 MiB，§8.2/§8.3 单独预算）与 journal ring
  （256 MiB，§10 单独预算），不在本口径内；
- 若未来 inode/目录索引结构显著变大，`namespace_test` 的断言会先失败，再评估
  hot/cold split。
