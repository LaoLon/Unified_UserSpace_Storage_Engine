# HFS 2.0 Repair-94：Trace 请求关联与归因完整性

> 注：项目已于 2026-10-04 由 HFS 更名为 UUSE；本文为历史记录，正文与证据路径保留 HFS 时期名称。

日期：2026-09-18
状态：已实现并通过 VM Release 验证

## 修复内容

Repair-94 将 trace 从“按点位计数”扩展为可离线关联的请求时间线：

- 新增 append-only 的 request shard、request bdev 和 request parent 点位；旧
  `BdevSubmit/BdevComplete`、`ShardBackendSubmit/Complete` 的参数保持不变。
- `requestId` 和 `childIndex` 从异步 shard 请求传入 SPDK actor-local I/O，bdev submit/
  complete 可以按 `(requestId, childIndex)` 配对；frontend `FeSubmit/FeReap` 和 parent
  complete 可以按 `requestId` 配对。
- trace format 升级为 v2。manifest 记录 TSC 频率校准、wall-clock 起点、每个 source
  的 OS thread id，以及 `recorded/dropped/closed` 状态。
- 高速点按 request id 成对采样；`trace_sample > 1` 时不会因为 submit 和 complete
  落在不同线程而独立抽样，避免离线延迟样本失配。
- `hfs_trace_dump` 会校验 record 的 source id、format version、manifest closed 状态和
  `recorded - dropped == file records`；校验失败返回非零，并输出 request、parent、
  shard、bdev 四段的 p50/p99。

## VM 验证

环境为 VM GCC 13.3、Release、FIO 3.42、SPDK runtime。构建目标：

```text
fio-hfs
hfs_trace_dump
trace_test
async_completion_queue_test
fio_completion_hub_test
request_lookup_test
```

结果：

```text
trace_test: ok
async_completion_queue_test: ok (eventfd=yes)
fio_completion_hub_test: ok
request_lookup_test: ok
```

构建过程只出现已有的 journal 编译器告警和 FIO 头文件聚合初始化告警，没有
Repair-94 编译错误；Release FIO 编译命令未包含 `-O0`。

本修复增强 trace 证据和离线归因能力，不改变 trace-off 路径，也不宣称最终 NVMe
吞吐或 p99 gate 已通过。