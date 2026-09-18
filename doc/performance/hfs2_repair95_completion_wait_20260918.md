# Repair-95：统一异步 completion wait 路径

日期：2026-09-18

## 修复内容

此前 FIO completion hub 在 `fio_hfs.cpp` 中自行重复了 core 的轮询、最终扫描和
deadline 逻辑，导致 FIO 与原生 `Client::poll()` 的等待语义分叉。现在：

- `Client::poll()` 统一执行 drain、adaptive spin、ring rearm/rescan 和 timeout；
- `ClientOptions` 只允许 frontend 注入 `prepare/epoch/waitSince` 三个 wait callback；
- FIO endpoint 只负责 futex/epoch 唤醒，不再调用多次零超时 `Client::poll()`；
- adaptive spin 每轮检查 deadline，不会因固定 spin budget 延长 FIO timeout；
- futex word 改为对齐的普通 `uint32_t`，通过 C++20 `atomic_ref` 进行原子访问，避免
  把 `std::atomic<uint32_t>` 对象直接 reinterpret 成 futex 地址。

## 验证

VM 验证命令使用 Release 构建，目标包括：

```text
fio-hfs
hfs_trace_dump
async_completion_queue_test
fio_completion_hub_test
```

结果：构建成功，`async_completion_queue_test: ok`，`fio_completion_hub_test: ok`。
新增测试覆盖外部 wait strategy 的 producer wake、最终扫描和 completion drain。

## 边界

该修复统一等待和唤醒语义，不宣称已经解决 shared-file 单 shard 拓扑或所有性能
差距；这些问题分别由 Repair-97 和最终 Repair-98 gate 处理。
