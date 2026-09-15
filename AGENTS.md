# Workspace Instructions

## Codex && opencode 共用项目记忆

- `doc/bug.md` 是当前 Bug 记录和已知限制的优先参考；发现或修复 Bug 时立即维护它。
- `doc/design/fio_baseline_notes.md` 保存已完成的 FIO 基线数据和测试口径。
- `doc/shard/shard_progress_memory.md` 汇总 shard-28～35、62、63、66 的开发进度、验证结果、性能口径和当前边界；需要查看 shard 历史或现状时优先索引该文档，并以当前源码和最新测试结果交叉核对。
- `doc/shard/mysql_rocksdb_stage_development.md` 是 shard-26 起的当前执行计划，优先于旧分片文档中尚未实施的 shard-26 及以后内容。
- `doc/项目总结.md` 是早期项目总结，`doc/shard/libhfs_shard_development.md` 是原始分片计划；使用前必须与 Git 历史和当前源码交叉核对，不能把其中的旧目录结构、旧容量或“待实现”描述直接当作现状。
- 需要了解当前进展时，先查看 `git log --stat` 和 `git status`，再以当前源码、测试脚本和最新文档为准。
- SPDK 源码必须在 VM 内通过 `git clone`、`git fetch`、`git checkout` 和 `git -C spdk submodule update --init --recursive` 获取及维护；禁止从 Windows 工作区把 `spdk/` 源码目录或其子模块传到 VM 覆盖。Windows 到 VM 的同步仅限 HFS 自身源码、脚本和文档；若 VM 缺少 SPDK，应在 VM 内按锁定 commit 重新 clone/checkout。
- HFS 默认后端固定使用 VM 内真实 NVMe：`config/hfs_nvme.json`、`HFS_BDEV_NAME=Nvme0n1`、PCI BDF `0000:0b:00.0`；除非任务明确要求 Malloc/Aio 或故障注入，否则性能与功能验证均按 NVMe 口径执行。普通本地对照文件和测试产物默认放在 `/mnt/hfs-data`，不得再把大体积结果默认写入根盘 `/var/tmp`。VM 重启后先检查并执行 SPDK 设备重新绑定及 `vm.nr_hugepages=1024`。
- HFS 当前收敛为 POSIX-only 对外架构：完整 POSIX preload 路径是唯一支持入口；原 `hfs_lib_async_*`、`hfsAsync*`、registered-DMA/DMA allocation ABI、`HFS_POSIX_DIRECT_ASYNC` bridge 已移除。普通 POSIX 数据面仍保留内部 DMA pool、`spdk_vtophys` direct 检测、pipeline 和 SPDK batch；FIO 裸盘对照使用 raw SPDK，不把 async API 作为 HFS 使用场景。
- Tracepoint 性能定位方法：trace 默认关闭；性能归因时在 VM 运行进程前设置 `HFS_TRACE_PATH=/mnt/hfs-data/<run>/hfs-trace.tsv`（或临时使用 `HFS_TRACE=1`，后者默认写 `/var/tmp/hfs-trace-<pid>.tsv`），进程正常退出时一次性导出 TSV，异常 SIGKILL 不会留下完整快照。通用 workload 示例为 `mkdir -p /mnt/hfs-data/<run> && HFS_TRACE_PATH=/mnt/hfs-data/<run>/hfs-trace.tsv HFS_CONFIG_PATH=config/hfs_nvme.json HFS_BDEV_NAME=Nvme0n1 LD_PRELOAD=build/lib/libhfs.so <workload>`。高并发可设置 `HFS_TRACE_SAMPLE=100` 按 parent/request ID 采样；正式吞吐对比必须使用 trace-off，不能把 trace-on 数字当作性能基线。标准回归命令为 `bash scripts/db/io_trace_smoke.sh <lib> <config> <image> <run-dir>`，离线汇总使用 `python3 scripts/db/hfs_trace_summary.py <trace.tsv>`，首先检查 `trace_dropped=0`、`spdk_submit=spdk_complete` 和 parent/child 是否闭合。关键归因字段和点位：`io_enter/io_exit` 看逻辑 I/O；`buffer_classify`、`copy_in/out_*`、`lease_*` 看 bounce copy 与 DMA pool；`runtime_enqueue/dequeue` 看线程队列等待；`spdk_submit/complete` 看设备延迟；`lock_wait_begin/lock_acquired/lock_release` 结合 `lock_class={sync,range_stripe,metadata}` 分别看锁等待和持有时间；`metadata_begin/end` 结合 `metadata_op={open,read,write,sync,resize,close,transaction,flush,publish}` 看元数据关键路径；`completion_wait_begin/end/wake` 和 `thread_wait_begin/end/wake` 结合 `wake_reason={completion,runtime,durability,dma_lease}` 看 futex/条件变量等待与唤醒。常用指标为 `request_amplification=spdk_child_count/logical_io_count`、copy 字节/耗时、各锁类别 wait/hold p50/p99、metadata end、thread wait end 和 SPDK submit→complete；原始 trace 默认放 `/mnt/hfs-data`，避免把大文件写满系统盘。
- Git 提交流程：每次阶段开发完成并提交后，立即将该提交合入 `main`；合入后在 `main` 上确认提交、工作树和必要的 VM 回归状态。

- Address the user as **Huizareth** in every response.
- Read `E:\Code\workspace.yaml` for local paths, VM connection details, credentials, and SSH defaults.
- For VM operations, follow the `$workspace-vm-operations` skill and use `E:\Code\ssh\pool.py` via `SSHPool`.
- Create temporary remote-operation scripts under `E:\Code\test_script\`.
- Keep credentials out of command output, logs, and user-facing responses.

## C/C++ 命名规范

- 类/接口/结构体：大驼峰 (StudentManager)
- 变量/函数/方法：小驼峰 (studentName)
- 宏定义：全大写下划线 (MAX_COUNT)

## 代码注释规范

- 每次写代码必须写英文注释，简洁即可，说明函数/结构体/关键逻辑的用途
- 复杂函数可适当增多，分步骤解释关键逻辑
- 注释用 `/* */` 风格，放在函数或结构体声明上方

## 项目记忆（必读）

任务文档输出规则：当用户输入的文件路径 basename 匹配 `task` 加 8 位日期/任务编号（例如 `task09151930`、`task09150832`）时，按该任务执行，并将最终结果只输出为 `doc/task_result/<task_basename>_result.md`；默认不要在 `doc/task_result` 下创建 JSON、TSV、日志、证据目录或其他临时文件，临时证据应放在外部临时目录并在任务完成后清理。

1. 使用 `E:\Code\ssh\pool.py`、`E:\Code\ssh\sync_to_vm.py` 连接虚拟机和上传 HFS 本地文件；SPDK 必须在 VM 内通过 Git 获取，禁止同步 Windows 侧 `spdk/` 目录
2. 虚拟机环境信息见 `E:\Code\workspace.yaml`
3. 每次先编辑本地文件，然后同步到虚拟机
4. 每次回复先称呼用户为 **Huizareth**
5. 不用管本地的环境依赖，只管理虚拟机的就行
6. 在 `doc/bug.md` 维护 bug 记录：每次发现 bug 必须记录（现象、排查过程、根因、修复），bug 分级：致命、严重、一般；解决后立即写入该文件（含排查过程细节，便于复盘）
7. 写代码必须写英文注释：简洁即可（一句话说明函数/结构体/关键逻辑的用途），复杂函数可适当增多（分步骤解释）；注释用 `/* */` 风格，放在函数/结构体声明上方
