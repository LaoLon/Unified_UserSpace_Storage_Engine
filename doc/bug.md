# HFS 2.0 Bug 记录

本文只记录 HFS 2.0 当前实现发现的 Bug。HFS 1.x 和旧 shard 的 Bug 历史已从工作区
清理；如需复盘，只能从 Git 历史查询。

Bug 等级：**致命 > 严重 > 一般**。

每条记录必须包含：现象、排查过程、根因、修复、验证结果和当前状态。

## 状态汇总

- 1：已修复（Shard-68，段错误）。
- 2：已修复（Shard-69，bdev attach 被静默跳过）。
- 3：已修复（Shard-69，reactor 退出时 channel 残留）。
- 4：已修复（Shard-70，控制 I/O 实参求值顺序）。

## Bug 记录

### 1. `.conf` schema 表使用 `std::initializer_list` 导致静态存储悬空

- **状态**：已修复
- **等级**：致命
- **时间**：2026-09-16（Shard-68）
- **现象**：`config_parser_test`、`engine_lifecycle_test` 启动解析阶段段错误；Make/CMake 构建成功，`task_test` 正常。
- **排查过程**：用 ASAN/UBSAN 直接编译测试，报 `stack-use-after-return`；栈回溯定位到 `src/config/parser.cpp` 的 `keyAllowed()`，损坏地址位于静态初始化帧。
- **根因**：`SectionSpec::keys` 使用 `std::initializer_list<const char *>` 保存静态表键名；`kSections` 静态初始化结束后，initializer_list 指向的临时数组已销毁，读取即悬空。
- **修复**：`SectionSpec::keys` 改为 `std::vector<const char *>`，键名在静态初始化时拷贝为自有存储。
- **验证结果**：Make/CMake 构建、3 个单元测试（parser 14 用例、engine 生命周期、Task/Result）、ASAN+UBSAN 全部通过；`hfsctl --config=config/hfs_nvme.conf validate` 输出正常。

### 2. `spdk_subsystem_load_config` 静默跳过 bdev attach（已修复）

- **状态**：已修复
- **等级**：严重
- **时间**：2026-09-16（Shard-69）
- **现象**：SPDK env 初始化成功、配置加载回调 `rc=0`，但 `spdk_bdev_get_by_name("Nvme0n1")` 始终为 NULL；日志中没有任何 RPC 错误。
- **排查过程**：阅读 `lib/init/json_config.c`，确认该函数按 RPC 状态掩码过滤方法；`bdev_nvme_attach_controller` 注册为 `SPDK_RPC_RUNTIME`，而自建 thread 模式下 RPC 状态停留在 `SPDK_RPC_STARTUP`，方法被静默跳过（`stop_on_error=false` 时连失败都不返回）。
- **根因**：事件框架会在 `framework_start_init` 后调用 `spdk_rpc_set_state(SPDK_RPC_RUNTIME)`；直接使用 `spdk_subsystem_init` 的应用必须自行切换。
- **修复**：`spdk_subsystem_init` 完成后调用 `spdk_rpc_set_state(SPDK_RPC_RUNTIME)`，并改用 `stop_on_error=true`。
- **验证结果**：bdev 正常出现，`spdk_runtime_test` 通过。

### 3. data reactor 退出时仍未释放 bdev channel（已修复）

- **状态**：已修复
- **等级**：一般
- **时间**：2026-09-16（Shard-69）
- **现象**：`spdk_thread_destroy("hfs2-data0/1")` 报 `still has channel for io_device Nvme0n1/bdev_Nvme0n1/...`，并出现 `active_poller bdev_nvme_poll still registered` 警告；功能测试仍能通过。
- **排查过程**：检查 `lib/thread/thread.c` 发现 `spdk_put_io_channel` 在 ref 归零时通过 `spdk_thread_send_msg` 延迟销毁；我们未 poll 线程就 destroy，消息没有执行。
- **根因**：channel 销毁是线程消息，不是同步释放。
- **修复**：释放 channel 后对所属 reactor 线程做有界 poll 排空（`drainThreadMessages`），metadata reactor 关闭 descriptor 后同样处理。
- **验证结果**：退出日志无 channel/poller 报错，`channelCount()` 停止后为 0。

### 4. 控制 I/O 实参求值顺序导致 moved-from shared_ptr 解引用（已修复）

- **状态**：已修复
- **等级**：严重
- **时间**：2026-09-16（Shard-70）
- **现象**：`format_mount_test` 首次控制路径写入时，metadata reactor 在 `controlSubmitTrampoline` 中 SIGSEGV。
- **排查过程**：日志确认消息 ctx 与 shared_ptr 入队时都有效；崩溃位于投递执行阶段。核对 C++ 规则后确认 `submitControlIo(io->device, std::move(io))` 的实参求值顺序未定义，GCC 先执行 move，再对已清空的 shared_ptr 取 `device`。
- **根因**：同一条调用表达式中既解引用又从同一 shared_ptr 移出。
- **修复**：移出前先取出 `device` 到局部变量，禁止在实参里混合解引用与 move。
- **验证结果**：Make/CMake/CTest 7/7、hfsctl `format --yes`/`info` 全部通过。

## 记录模板

### N. 简短标题

- **状态**：待处理 / 处理中 / 已修复
- **等级**：致命 / 严重 / 一般
- **时间**：YYYY-MM-DD
- **现象**：
- **排查过程**：
- **根因**：
- **修复**：
- **验证结果**：
