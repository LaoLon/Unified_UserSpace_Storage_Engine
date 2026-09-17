# HFS 2.0 Shard-79/83/84 错误、超时、取消组合矩阵

日期：2026-09-18  
设备口径：单盘真实 NVMe `Nvme0n1`，PCI BDF `0000:0b:00.0`，配置
`config/hfs_nvme.conf`。测试前将设备临时绑定 SPDK，测试结束恢复内核 `nvme`
驱动；VM hugepages 为 1024。

## 结论

本次闭合的是可重复的顺序错误、前端超时、回调前取消、回调边界取消及其组合矩阵。
所有列出的组合均在真实 NVMe 上通过，且测试结束后设备驱动恢复、文件空间回收不变。
这不等价于真实掉电、控制器复位或硬件介质故障；Shard-79 的掉电等价验证、Shard-83
的性能放大及 Shard-84 的完整 stock RocksDB 兼容性仍按设计文档保持为独立边界。

## 覆盖矩阵

| Shard | 场景 | 注入/操作组合 | 期望结果 | 证据 |
|---|---|---|---|---|
| 79 | data flush 失败 | 写入 → `HFS_TEST_FAIL_FLUSH_DEVICE` → `datasync` → 清除注入重试 | `IoError`；重试成功；数据空间可回收 | `tests/file_api_test.cpp`，真实 NVMe |
| 79 | checkpoint flush 失败 | 扩展 logical range → `HFS_TEST_FAIL_CHECKPOINT_FLUSH` → `datasync` → 重试 | `IoError`；未发布错误 checkpoint；durable size 不越界 | `tests/file_api_test.cpp`，真实 NVMe |
| 79 | metadata commit 失败 | 写入 → `HFS_TEST_FAIL_METADATA_COMMIT` → `datasync` → 重试 | `IoError`；`logicalSize` 保持，`durableSize` 不前进；重试成功 | `tests/file_api_test.cpp`，真实 NVMe |
| 79 | clean flag 失败 | 释放 Busy client/file → `HFS_TEST_FAIL_CLEAN_FLAG` → `stop` → 重试 | `IoError`；清除注入后正常停止 | `tests/file_api_test.cpp`，真实 NVMe |
| 79 | journal/replay 损坏 | journal/checkpoint 结构损坏与双槽损坏 → mount/check | `Corrupt`/拒绝 mount；保留 fail-closed 现场 | `tests/format_mount_test.cpp`，真实 NVMe |
| 79 | 多盘副本错误 | 缺失 BDF、generation 落后、CRC 不一致 | 拒绝 mount，不降级、不静默迁移 | `run_hfs_dual_faults.py`，双 NVMe 真实回归 |
| 79/83 | DataPath 超时 | child I/O 延迟 25ms，配置 timeout 1ms | `TimedOut`；late completion 排空；submit=complete | `tests/data_path_device_test.cpp`，真实 NVMe |
| 83 | 核心同步取消 | stop token 预取消 write/read | `Cancelled`；write 不预留 extent，logical size 不变 | `tests/file_data_test.cpp`，真实 NVMe |
| 83 | ReadAsync 超时 | `IOOptions.timeout=1ms`，backend 延迟 25ms → `Poll` | callback 一次；`TimedOut`；late result 不改写 request | `rocksdb/hfs_filesystem_smoke.cpp`，真实 NVMe |
| 83 | 超时后取消 | 超时 callback 已发布 → `AbortIO` | 仍为 `TimedOut`，不被改写为 `Aborted` | `rocksdb/hfs_filesystem_smoke.cpp`，真实 NVMe |
| 83/84 | 取消抢先于超时 | timeout 1ms + backend 延迟 50ms → 立即 `AbortIO` → `Poll` | `Aborted`；callback 次数为 0；句柄可安全回收 | `rocksdb/hfs_filesystem_smoke.cpp`，真实 NVMe |
| 83 | 回调边界取消 | callback 已进入 → `AbortIO` → callback release | callback-visible status 保持 `OK`；结果完整 | `rocksdb/hfs_filesystem_smoke.cpp`，真实 NVMe |
| 83/84 | 多句柄取消 | 8 个 delayed async reads → 一次 `AbortIO` → `Poll` | 8 个均 `Aborted`；无 callback；无 UAF | `rocksdb/hfs_filesystem_smoke.cpp`，真实 NVMe |
| 84 | RocksDB flush 错误映射 | writable append → `HFS_TEST_FAIL_FLUSH_DEVICE` → `Sync` → retry | adapter 返回 `IOError`；清除注入后 Sync/Close 成功 | `rocksdb/hfs_filesystem_smoke.cpp`，真实 NVMe |
| 84 | 永久参数错误 | 各 output 指针为 null、invalid MultiRead/async 参数 | `InvalidArgument`；不解引用、不启动后台 I/O | `rocksdb/hfs_filesystem_smoke.cpp`，真实 NVMe |
| 84 | 错误映射与 namespace | 缺失路径、锁竞争、rename/delete、free-space reclaim | `NotFound`/`Busy` 正确区分；旧名消失；空间归还 | `rocksdb/hfs_filesystem_smoke.cpp`、`tests/file_api_test.cpp` |

## 执行方式与结果

```text
python E:\Code\test_script\hfs2_sync_vm.py
python E:\Code\test_script\run_error_cancel_matrix_vm.py
```

结果：`file_data_test: ok`、`data_path_device_test: ok`、`format_mount_test: ok`、
RocksDB filesystem smoke `exit_code=0`；最终 free-space reclaim 为
`initial=31398559744`、`allocated=31397511168`、`final=31398559744`。

