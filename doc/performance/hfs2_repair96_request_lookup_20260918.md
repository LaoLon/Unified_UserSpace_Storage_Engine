# HFS 2.0 Repair-96：请求查找与完成回收完整性

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

日期：2026-09-18
状态：已实现并通过 VM Release 验证

## 修复范围

Repair-96 收敛请求完成路径的两个完整性问题：

- 使用固定容量、无 tombstone 的开放寻址表保存 `requestId -> slot` 映射；删除使用 backward-shift，避免长时间运行后探测链退化。
- 完成事件只有在 `requestId`、slot token 和 generation 全部匹配时才释放 slot 与 credit。未知、重复或不匹配的完成事件不会消耗并发额度，并会记录 completion overflow/corruption 状态。

请求表容量按最大 outstanding 请求数的两倍向上取整为 2 的幂，正常容量下不依赖线性扫描；线性扫描仍作为损坏或极端插入失败时的安全兜底。

## 验证

在 VM 内使用 GCC 13.3、Release 配置和 FIO 3.42 构建了以下目标：

```text
fio-hfs
hfs_trace_dump
async_completion_queue_test
fio_completion_hub_test
request_lookup_test
```

测试结果：

```text
async_completion_queue_test: ok (eventfd=yes)
fio_completion_hub_test: ok
request_lookup_test: ok
```

`request_lookup_test` 覆盖重复插入、完整查找、未知删除、删除后的重新插入，以及 10,000 轮碰撞/删除/重插入压力循环。

本修复证明请求映射和完成回收的完整性，不宣称已经改善最终 NVMe 吞吐或尾延迟；性能结论仍以最终 raw/HFS 同口径 gate 为准。