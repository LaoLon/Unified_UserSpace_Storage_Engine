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
- 5：已修复（Shard-71，单例析构 join 忙轮询线程导致挂起）。
- 6：已修复（Shard-71，旧世代 journal/checkpoint 被误读）。
- 7：已修复（Shard-71，ring 回绕陈旧槽位被误判 corruption）。
- 8：已修复（Shard-73，MPSC ring 满判定错误导致自旋）。
- 9：已修复（Shard-73，publish 读取消费者专属 tail_ 数据竞争）。
- 10：已修复（Shard-73，trace 全局计数多线程竞争）。
- 11：已修复（Shard-74，barrier 就绪条件错误导致永远不被准入）。
- 12：已修复（Shard-74，pending barrier 误挡更早提交的节点）。
- 13：已修复（Shard-75，extent 物理偏移单位不一致导致重复叠加）。
- 14：已修复（Shard-75，DmaArena 释放不回收预算且析构泄漏缓存块）。
- 15：已修复（Shard-76，Engine::stop 在打开 File 时销毁 namespace 导致悬空自锁死）。
- 16：已修复（Shard-77，DataPlane 零初始化合成映射逻辑偏移全部叠加在 0）。
- 17：已修复（Shard-77 边界收口，extent commit 失败可泄漏 allocator 状态）。
- 18：已修复（Shard-77 边界收口，extent/range 计算整数溢出未拒绝）。
- 19：已修复（Shard-81，FIO 异步完成路径落地，请求计数闭合）。
- 20：已修复（Shard-81，core-native async 替代 external worker pool）。
- 21：已修复（Shard-82，RocksDB LockFile 将 HFS 路径误当宿主机路径）。
- 22：已修复（Shard-82，RocksDB DB 路径尾斜杠未规范化）。
- 23：已修复（Shard-79，Journal::open ownership transfer 求值顺序不安全）。
- 24：已修复（Shard-79，checkpoint generation 平局时 mount 选择旧槽位）。
- 25：已修复（Shard-84，RocksDB File sync 使用 inode epoch 导致全局 durable epoch 回退）。
- 26：已修复（Shard-81，共享文件并发 preallocate 竞争丢 job）。
- 27：已修复（Shard-81，异步请求未保活 completion sink）。
- 28：已修复（Shard-81，完成丢失/错配导致挂起而非 fail-fast）。
- 29：已修复（Shard-81，child_submit/child_complete 计数语义不一致）。
- 30：已修复（Shard-81，FIO DDIR_SYNC/errno/get_file_size/fork 契约收口）。
- 31：已修复（Shard-81，后端 slot 耗尽被当终态数据错误）。
- 32：已修复（Shard-81，DmaArena release 尺寸与析构不一致）。
- 33：已修复（Shard-81，SpdkRuntime 停机竞态与无界排空）。
- 34：已修复（Shard-81，trace TSC 校准与 stop/producer 竞争）。
- 35：已修复（Shard-81，异步路径杂项收口）。
- 36：已修复（Shard-81，shard inode 槽位线性扫描导致每 I/O ~5ms 延迟）。

## Bug 记录

### 21. RocksDB LockFile 将 HFS namespace 路径误当宿主机路径

- **状态**：已修复
- **等级**：严重（RocksDB 无法打开注入的 HFS Env）
- **时间**：2026-09-17（Shard-82）
- **现象**：`hfs_db_bench` 在真实 NVMe 上创建 HFS Env 后，`DB::Open` 报
  `RocksDB lock is already held`。
- **排查过程**：锁路径 `/rocksdb-bench/LOCK` 属于 HFS namespace，不存在于宿主机；
  `open(2)` 失败后适配器统一返回了“已持有”错误。
- **根因**：RocksDB `FileSystem::LockFile` 需要宿主机可见的稳定路径，不能直接把
  HFS 绝对路径传给宿主机 syscall。
- **修复**：按 HFS DB 路径计算稳定 FNV-1a 名称，锁文件落在 `/var/tmp`，仍由
  `flock` 提供跨进程互斥。
- **验证结果**：真实 NVMe smoke 中 lock 阶段通过；后续 `DB::Open` 继续到 WAL
  `Sync`，并暴露当前 core 的 Shard-78 durability 未实现边界。

### 22. RocksDB DB 路径尾斜杠未规范化

- **状态**：已修复
- **等级**：一般（RocksDB Env 无法打开带尾斜杠的 DB 路径）
- **时间**：2026-09-17（Shard-82）
- **现象**：RocksDB 使用 `/rocksdb-bench/` 形式的 DB 路径时，HFS path parser 返回
  `Invalid argument: trailing slash is not allowed`。
- **根因**：RocksDB 的宿主路径约定允许 DB 目录尾斜杠，而 HFS namespace 要求除根
  路径外不带尾斜杠。
- **修复**：adapter 的统一路径转换去除尾斜杠，并保留 `/`。
- **验证结果**：真实 NVMe `hfs_db_bench fillseq` 和跨进程 `readrandom` 均通过。

### 23. Journal::open ownership transfer 求值顺序不安全

- **状态**：已修复
- **等级**：严重（可能得到悬空或空的 journal device 指针）
- **时间**：2026-09-17（Shard-79）
- **现象**：`Journal::open(Engine&)` 在一个调用表达式中同时执行
  `std::move(device)` 与 `device.get()`。
- **根因**：C++ 函数实参求值顺序未定义，裸指针获取可能发生在 ownership move 之后。
- **修复**：在 move 前保存 `JournalDevice*`，再把 ownership 传入 `Journal::create`。
- **验证结果**：VM clean build、namespace/journal 回归和真实 NVMe RocksDB 多进程
  fillseq/readrandom/overwrite/readwhilewriting 均通过。

### 24. checkpoint generation 平局时 mount 选择旧槽位

- **状态**：已修复
- **等级**：严重（跨进程恢复可能回到旧 namespace snapshot）
- **时间**：2026-09-17（Shard-79）
- **现象**：RocksDB 进程内可见 `CURRENT`，进程重启后 mount 选择旧 checkpoint，
  `hfs_ldb checkconsistency` 报 `CURRENT` 不存在。
- **根因**：RocksDB 元数据 checkpoint 的 journal txid/sequence 都可能为 0，A/B
  选择逻辑以 txid 平局时恒选 A，忽略了 checkpoint header 的 generation。
- **修复**：读取并比较 checkpoint generation，按 generation、txid、sequence 的
  三元组选择最新有效槽位。
- **验证结果**：真实 NVMe canonical RocksDB benchmark、正常 reopen 和 SIGKILL 后
  `hfs_ldb checkconsistency` 均通过。

### 25. File sync 使用 inode epoch 导致全局 durable epoch 回退

- **状态**：已修复
- **等级**：严重（RocksDB 连续发布文件时 DB::Open 失败）
- **时间**：2026-09-17（Shard-84）
- **现象**：多个 RocksDB 文件连续 rename/sync 后返回 `durable epoch cannot move
  backwards`。
- **根因**：`File::sync/datasync` 将单 inode 的 logical epoch 传给 namespace-wide
  durability barrier；该值可能落后于已发布的全局 durable epoch。
- **修复**：同步入口统一提交 namespace 当前 global logical epoch；RocksDB
  `Close/Flush/RenameFile` 同时完成 durable metadata barrier。
- **验证结果**：真实 NVMe 四模式 benchmark、canonical 参数、正常跨进程 reopen、
  SIGKILL recovery 和 `hfs_ldb checkconsistency` 均通过。

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

### 5. SpdkRuntime 单例析构直接 join 忙轮询 reactor 导致挂起（已修复）

- **状态**：已修复
- **等级**：严重
- **时间**：2026-09-16（Shard-71）
- **现象**：测试断言失败后进程不退出，主线程在 `std::jthread::~jthread` 的 join 上挂起 16 分钟以上；metadata/data reactor 仍在 100% 忙轮询。
- **排查过程**：`gdb -p` 全线程回溯显示主线程位于 `Reactor::~Reactor` → `jthread::join`，而 reactor 线程仍在 `metadataMain`/`dataMain` poll 循环中；确认测试异常返回时 engine 未 stop，进程退出触发单例析构。
- **根因**：`Impl` 析构没有先请求 reactor 停止就 join，忙轮询线程永远不会退出。
- **修复**：`Impl::~Impl` 先对所有 reactor 置 stop 再做有界 join，然后清理。
- **验证结果**：测试失败路径可正常退出；CTest 9/9 通过。

### 6. 旧世代 journal/checkpoint 被误读（已修复）

- **状态**：已修复
- **等级**：严重
- **时间**：2026-09-16（Shard-71）
- **现象**：重新 format 后 `journal_device_test` 首个事务返回 txid 5 而不是 1；replay 应用了上一代 checkpoint 的状态。
- **排查过程**：format 只写 superblock，不清 journal/checkpoint 区域；旧 checkpoint 头部仍带有效 magic/CRC，open 时被选中，旧 journal 事务也被当作本轮事务。
- **根因**：journal/checkpoint 记录没有世代标识，format 也未失效旧内容。
- **修复**：slot 与 checkpoint 头部增加 volume UUID，世代不匹配视为陈旧内容；format 清零 journal 头部与两个 checkpoint 槽首块。
- **验证结果**：新镜像 `head=tail=last_txid=0`；journal 单测与真实设备测试通过。

### 7. ring 回绕后陈旧槽位被误判为 corruption（已修复）

- **状态**：已修复
- **等级**：一般
- **时间**：2026-09-16（Shard-71）
- **现象**：100 次追加（跨 ring 回绕）后 replay 在 sequence 300 报 `orphan record`。
- **排查过程**：回绕后该位置存的是同 UUID、更早事务的 RECORD 槽位；replay 只校验了 UUID，未校验槽位自带的 sequence。
- **根因**：同世代但序号不匹配的陈旧槽位与 corruption 无法区分。
- **修复**：replay 对 BEGIN/RECORD/COMMIT 槽校验 `sequence` 与期望位置一致，不一致按陈旧内容干净结束。
- **验证结果**：回绕场景 replay 完整且 `corruption` 为空。

### 8. MPSC ring 满判定错误导致 reserve 永久自旋（已修复）

- **状态**：已修复
- **等级**：严重
- **时间**：2026-09-16（Shard-73）
- **现象**：`data_plane_test` 在 depth=8 的 ring 写满后再次 `reserve()` 不返回，主线程 100% CPU 自旋 5 分钟以上。
- **排查过程**：gdb attach 显示卡在 `ShardRing::reserve` 的 `yield` 分支；推导槽位 sequence 与 head 的关系发现满判定用了 `sequence + depth <= head`，而正确条件是比较 `sequence < head`（已发布未消费的槽位 sequence = 上一圈位置+1，恒小于当前 head）。
- **根因**：满判定条件写错，已满时落入“其它生产者正在发布”的自旋分支。
- **修复**：`sequence < head` 返回 Busy；`sequence == head` 才可预约；`sequence > head` 防御性 yield。
- **验证结果**：ring 满立即 Busy，`data_plane_test` 通过。

### 9. publish 读取消费者专属 tail_ 数据竞争（已修复）

- **状态**：已修复
- **等级**：一般
- **时间**：2026-09-16（Shard-73）
- **现象**：TSAN 报 `shard_ring.cpp` `drain` 与生产者线程的数据竞争。
- **排查过程**：`publish` 为计算 high watermark 读取 `tail_`，而 `tail_` 只由消费者 `drain` 修改。
- **根因**：违反 ring 的单写者约定。
- **修复**：新增原子 `inFlight_`（publish 加、drain 减），watermark 由它推导，生产者不再触碰 `tail_`。
- **验证结果**：TSAN 干净。

### 10. trace 全局计数多线程竞争（已修复）

- **状态**：已修复
- **等级**：一般
- **时间**：2026-09-16（Shard-73）
- **现象**：TSAN 报 `trace::event` 内 `g_sequence/g_recorded` 竞争。
- **排查过程**：多生产者测试开启 trace 后，普通 `uint64_t` 全局变量被并发读写。
- **根因**：trace 骨架计数未原子化。
- **修复**：改为 `std::atomic<uint64_t>`（relaxed），采样判定使用取回后的序号。
- **验证结果**：TSAN 干净，trace 事件仍正常计数。

### 11. barrier 就绪条件错误导致永远不被准入（已修复）

- **状态**：已修复
- **等级**：严重（truncate/size/sync 等 barrier 会永久排队）
- **时间**：2026-09-16（Shard-74）
- **现象**：`scheduler_test` 场景 5 中，活跃读完成后 barrier 仍未准入，后续写也被挡住。
- **排查过程**：`canAdmit` 对 barrier 要求 `treeRoot_ == nullptr && fifoHead_ == nullptr`，但 barrier 本身就在 FIFO 头，条件永远为假。
- **根因**：把“没有更早的等待者”写成了“等待队列为空”。
- **修复**：改为 `treeRoot_ == nullptr && (fifoHead_ == nullptr || fifoHead_ == &node)`。
- **验证结果**：barrier 在树清空且位于队首时准入，场景 5/6 与压力模拟通过。

### 12. pending barrier 误挡更早提交的节点（已修复）

- **状态**：已修复
- **等级**：严重（会造成调度死锁）
- **时间**：2026-09-16（Shard-74）
- **现象**：32 客户端压力模拟在 `active` 为空且仍有 pending 时卡住：队首节点因其后有排队 barrier 而无法准入。
- **排查过程**：`canAdmit` 中只要存在 pendingBarrier 就拒绝所有其它节点，忽略了 barrier 之前已提交的节点语义上应优先完成。
- **根因**：barrier 的“禁止越过”范围应只覆盖 sequence 更大的后来者。
- **修复**：改为 `pendingBarrier_->sequence < node.sequence` 时才拒绝。
- **验证结果**：压力模拟不再死锁，`pendingCount=0`、全部 4096 op 完成。

### 13. extent 物理偏移单位不一致导致物理地址重复叠加

- **状态**：已修复
- **等级**：严重（跨 extent/非对齐 I/O 会落到错误物理位置）
- **时间**：2026-09-17（Shard-75）
- **现象**：75A 代码自检发现：`SyntheticExtentMap::lookup` 裁剪时按字节调整了 `physicalBlock`，而 `IoPlanner` 又把该字段当块号乘 `blockSize` 再加字节内偏移，物理地址被叠加两次且单位混乱。
- **排查过程**：对照 `extent_map.cpp` 裁剪公式与 `io_planner.cpp` 物理地址推导，确认同一字段在两处语义不一致；合成 fixture 的跨 extent 场景会立即暴露。
- **根因**：`ExtentView` 物理字段同时承担“块号”和“裁剪后物理位置”两种角色，接口单位未冻结。
- **修复**：字段改为字节口径 `physicalByte`（lookup 裁剪只加字节差），planner 使用字节偏移后换算为元数据块；`DataPath` 再按 `metadataBlockSize/deviceBlockSize` 换算为设备扇区提交。
- **验证结果**：`io_planner_test` 跨 extent/非对齐断言通过；`data_path_device_test` 非对齐 RMW 写后回读与设备内容一致。

### 14. DmaArena 释放不回收预算且析构泄漏缓存块

- **状态**：已修复
- **等级**：一般
- **时间**：2026-09-17（Shard-75）
- **现象**：`io_planner_test` 期望释放后 `bytes == 0` 断言失败；ASAN 报 `memAlloc` 共 8192 字节泄漏。
- **排查过程**：检查 `release()` 只把块放回 free list，`bytes_` 从不回减；`DmaArena` 没有析构释放缓存块，进程退出时 free list 内存未归还。
- **根因**：`bytes_` 统计语义未定义（在途用量还是保留预算），且缺少资源析构。
- **修复**：明确 `bytes_` 为 arena 保留预算（背压 cap 的作用对象），`release()` 不回减；新增析构函数通过 `deallocator` 释放全部缓存块。
- **验证结果**：测试改为 retained 语义断言后通过；ASAN 无泄漏；CTest 13/13。

### 15. Engine::stop 在打开 File 时销毁 namespace 导致悬空自锁死

- **状态**：已修复
- **等级**：严重（use-after-free + 永久挂起）
- **时间**：2026-09-17（Shard-76）
- **现象**：`file_api_test` 在 mount 后创建 Client/File，调用 `engine->stop()` 后进程永久挂起；gdb 回溯主线程停在 `Namespace::close` 的 mutex 上（main → File::Impl::~Impl → Namespace::close），无其他线程持锁。
- **排查过程**：locals 显示测试已执行到最后销毁 File 句柄的阶段；`Engine::stop` 已 `namespaceState.reset()`，而 `ClientState` 保存的 namespace 裸指针未失效，`close()` 实际在已释放内存的 mutex 上等待。
- **根因**：`stop()` 未检查活跃 client/open file 就销毁 namespace；前端状态保存裸指针，违反 hfs2.0.md §14.3“活跃 client/operation 未 drain 时 stop() 返回 Busy”。
- **修复**：引入 `MetadataLease`（Client 及其存活 File 持有）：存活期间计数 `Engine::metadataUsers`，`stop()` 计数非零返回 Busy；`file_api_test` 增加 Busy 停止契约断言。
- **验证结果**：`file_api_test` 通过；CTest 15/15、ASAN/TSAN 干净。

### 16. DataPlane 零初始化合成映射逻辑偏移全部叠加在 0

- **状态**：已修复
- **等级**：严重（allocate 全部失败）
- **时间**：2026-09-17（Shard-77）
- **现象**：`file_data_test` 首次 `File::allocate` 返回 NotFound（logical range is not fully allocated）。
- **排查过程**：`DataPlane::zeroExtents` 为运行中的每个物理 extent 都调用 `map.add(0, extentBytes, ...)`，多个逻辑区间全部落在 0 导致重叠；planner 处理完第一段后 cursor 已前移，第二段 `logicalOffset=0 != cursor` 触发 NotFound。
- **根因**：合成 extent map 的逻辑偏移没有按 `i * extentBytes` 展开。
- **修复**：逻辑偏移改为 `i * extentBytes`，写入偏移同步对应。
- **验证结果**：`file_data_test` 全部场景通过；CTest 17/17、ASAN/TSAN 干净。

### 17. extent commit 失败后 allocator 与 inode map 不一致

- **状态**：已修复
- **等级**：严重
- **时间**：2026-09-17（Shard-77 边界收口）
- **现象**：同一 inode 存在重叠的 outstanding reservation 时，后一个 commit
  可能在 extent map 发布失败，但其 allocator extent 已进入 COMMITTED，形成空间泄漏。
- **根因**：`commitExtents` 逐个修改 allocator，最后才更新 inode map，缺少完整预校验和补偿。
- **修复**：先在候选 extent map 上验证全集合与所有 reservation 状态，再修改 allocator；
  中途失败释放已经提交的 runs，map 仅在全部成功后发布；`releaseExtents` 只接受 RESERVED runs。
- **验证结果**：新增重叠 reservation 回归；`allocator_test`、`namespace_test` 通过。

### 18. extent/range 算术溢出未 fail closed

- **状态**：已修复
- **等级**：严重
- **时间**：2026-09-17（Shard-77 边界收口）
- **现象**：极大 offset/length 在 namespace reservation、extent lookup 或 planner 中相加回绕，
  可能产生错误逻辑范围。
- **根因**：直接使用 `offset + length` 与 `size + extentBytes - 1`，没有统一溢出检查。
- **修复**：reservation、lookup、planner 入口拒绝溢出；extent 向上取整改用除法和余数，
  truncate 同样避免加法回绕。
- **验证结果**：新增极大范围 reservation 回归；Linux/CMake 结构测试通过。

### 19. FIO 外部引擎文件布局未形成实际 I/O

- **状态**：已修复
- **等级**：严重
- **时间**：2026-09-17（Shard-80A）
- **现象**：真实 NVMe 上 FIO 引擎可加载、HFS 可 format/mount/stop，但最小 job 未产生 I/O 统计。
- **排查过程**：初版因缺少 invalidate 回调报 Bad file descriptor；补齐后又发现零长度布局和 numjobs 进程锁问题，现已修复；另修复 FIO worker clone 丢失 `hfs_config` 的问题；仍需继续闭合 FIO DMA 与完整请求矩阵契约。
- **根因**：FIO 虚拟文件引擎不能依赖 host fd 的默认文件布局路径，当前 size/open 回调组合尚未满足 FIO 的实际请求准入条件。
- **修复**：已补齐无 fd invalidate、get_file_size、open 时 preallocate，以及进程级 `hfs_config` callback；功能矩阵前保持 Shard-80 未完成。
- **验证结果**：编译通过；无 `HFS_CONFIG` 环境变量时，真实 NVMe bind→format→mount→cleanup 下 `.fio` 内 `hfs_config`、`numjobs=2`、4K write 完成 4096B、errors=0；高 QD 功能矩阵与 DMA all-or-nothing 门禁仍未通过。
- **2026-09-18 修复**：移除 external worker pool，`queue()` 在 FIO 提交线程同步执行 HFS core I/O 并返回 `FIO_Q_COMPLETED`，避免跨线程 SPDK reactor/`io_handle` 生命周期问题；真实 NVMe 4K randrw、4 jobs、3 秒 smoke 完成约 19 MiB，errors=0。正式 32-job 性能 gate 仍需在该修复上重跑。
- **2026-09-18 收口**：异步完成路径（`hfs_async=1`，见 #27-#31）落地后，32 job 正式 gate 的
  `accepted=terminal=published`、`queued=reaped`、`outstanding=0` 全部闭合；
  `--verify=crc32c --verify_fatal=1 --end_fsync=1` 在 async 与 sync 回退均 `err=0`。
  性能比值仍未达标，属独立性能问题，不由本 Bug 跟踪。

### 20. FIO 高深度外部线程提交触发 SPDK 消息队列崩溃
- **状态**：已修复
- **等级**：严重
- **时间**：2026-09-17（Shard-80A）
- **现象**：真实 NVMe 上，临时将 FIO `queue()` 请求交给外部 `std::thread` 后，
  `iodepth=8` 的 read/write 在稳定路径清理或下一组请求期间触发 SIGSEGV/SIGABRT；
  回溯落在 SPDK `msg_queue_run_batch`，或随后表现为 glibc heap corruption。
- **复现条件**：`fio-hfs.so`、`.conf`、`Nvme0n1`、`numjobs=1/2`、`iodepth=8`；
  固定 worker 数量仍可复现，故不是“每个请求创建线程”单一问题。
- **当前边界**：FIO 请求暂保持在提交线程同步执行；真实 NVMe 4K `numjobs=2`
  smoke 通过，高 QD 异步 queue、DMA all-or-nothing 和完整矩阵不得宣告通过。
- **下一步**：在 core 内建立明确的前端提交/完成生命周期（包括 reactor 归属、
  `io_handle` 保活和停止顺序），再恢复 FIO 三态 queue；不以普通 buffer 或降低 QD
  替代异步验收。
- **2026-09-18 收口**：core-native 异步路径已实现（`AsyncDataService`/`SpdkAsyncDataBackend`/
  `AsyncCompletionQueue`，FIO `queue()` 返回 `FIO_Q_QUEUED`，`getevents/event` 消费
  token），不再使用任何外部 `std::thread` 调 core。真实 NVMe 32 job gate 的
  `accepted=terminal=published`、`child_submit=child_complete`、`queued=reaped`、
  `outstanding=0`、`token_mismatch=0` 全部闭合；`#27-#31` 修复了该路径的
  生命周期、fail-fast、计数、FIO 契约和后端 credit 缺陷。

### 26. 共享文件并发 preallocate 竞争导致 31/32 FIO job 被静默丢弃

- **状态**：已修复
- **等级**：严重（并发 workload 未实际运行）
- **时间**：2026-09-18（Shard-81）
- **现象**：正式 gate 中 32 个 job 只有 1 个产生 I/O，HFS/raw=0.000019；插桩探针显示
  4 个 job 中 3 个 `open_alloc_failed`、`queued=0` 且 fio `err=0`。
- **排查过程**：在 VM 上给 `fioHfsOpen` 增加临时 trace，确认 loser 的
  `File::allocate(0,size)` 返回错误；对照 `Namespace::reserveExtents` 与
  `InodeExtentMap::addExtents`，确认 loser 与 winner 为同一逻辑区间预留了不同物理
  extent，后 commit 者撞 `AlreadyExists`。
- **根因**：`reserveExtents` 只以已提交 extent 判断占位，同 inode 在途 reservation
  不可见，因此并发 allocate 非可串行化；adapter 又在每个 job 的 `open_file` 重复
  执行整文件 preallocate。`fioHfsOpen` 只返回错误码不设置 `errno`，fio 以 err=0
  静默丢弃这些 job。
- **修复**：新增进程级 single-flight（`fio/preallocation_single_flight.*`），owner
  唯一执行 allocate、waiter 复用同一结果；owner 通过新增的 `File::allocatedBytes()`
  校验 committed extent 覆盖（不再硬编码 `coverageComplete`）；open 失败统一设置
  `errno`。
- **验证结果**：VM Release 主机/设备测试通过；真实 NVMe 32 job gate 请求计数闭合；
  4 job 探针 owner=1/waiter=3、无失败；`hfs_async=0/1` verify+end_fsync 均 `err=0`。

### 27. accepted 异步请求未保活 completion sink

- **状态**：已修复
- **等级**：严重（reactor 可 publish 到已释放的 ClientState）
- **时间**：2026-09-18（Shard-81）
- **现象**：代码审查发现 `RequestState::sink` 为裸指针，`Operation` 的
  `CompletionSink` 明确 non-owning；若前端先释放 `IoHandle` 再销毁 Client/File，
  reactor 完成回调会写入已释放的 `ClientState`。
- **根因**：accepted 请求只在 frontend handle 上持有 sink 引用，operation 本身没有
  保活；设计 §8.1/§13.3 要求 operation 持 `CompletionSinkRef` 直到 publish。
- **修复**：`AsyncDataService::submit` 增加必填 `std::shared_ptr<void> sinkOwner`，
  `RequestState` 持有到 terminal publish 之后；`Client` 传入自身 state；`~Client`
  在存在在途 outstanding/handle/open file 时记录错误。
- **验证结果**：新增 `OwnedSink` 生命周期用例（前端中途释放引用，publish 仍到达、
  weak 在完成后才过期）；32 job gate 无异常。

### 28. 完成丢失/错配导致 credit 泄漏与 cleanup 永久挂起

- **状态**：已修复
- **等级**：严重（挂起 + 计数失真）
- **时间**：2026-09-18（Shard-81）
- **现象**：代码审查发现 completion queue push 失败或 token 不匹配时只置
  `completionOverflow` 就返回：credit/slot 不释放、`fioHfsCleanup` 的 drain 条件
  永不满足；`completionPublished_` 在 publish 失败后仍自增。
- **根因**：完成路径缺少 fail-fast 契约；`AsyncDataSink::publish` 无法把投递失败
  反馈给 core。
- **修复**：`publish` 改为返回 bool，core 分别统计
  `completionPublished/completionDropped`；`Client::poll` 在 overflow 时立即返回
  `Corrupt`；FIO `drainAsync` 遇到致命 poll/协议错误立即中止，并增加 30s 硬截止。
- **验证结果**：`RejectingSink` 用例断言 `published==0/dropped==1`；
  `async_completion_queue_test`/`fio_completion_hub_test` 通过；真实 NVMe 清理
  不再挂起。

### 29. child_submit/child_complete 计数语义不一致

- **状态**：已修复
- **等级**：一般（closure 证据失真）
- **时间**：2026-09-18（Shard-81）
- **现象**：`childSubmits_` 按 RMW phase 提交计数，`childCompletes_` 按 child 终结
  计数；非对齐 I/O 必然 submit>complete，gate 的 `child_submit == child_complete`
  只对 aligned direct 负载成立。
- **根因**：两个计数器统计对象不同（phase vs child）。
- **修复**：`ChildState::counted` 保证每个 child 只计一次 submit；completes 只统计
  已提交 child；未提交即被拒的 child 计入新增 `childSubmitRejected`；Engine
  closure 日志输出 `dropped/child_rejected`。
- **验证结果**：RMW 用例断言 `childSubmits==childCompletes==1`、`rejected==0`；
  32 job gate 中 `child_submit=child_complete`。

### 30. FIO DDIR_SYNC/errno/get_file_size/fork 契约收口

- **状态**：已修复
- **等级**：严重（`--verify`/`end_fsync` 必然失败；open 失败静默）
- **时间**：2026-09-18（Shard-81）
- **现象**：`queue()` 对 `DDIR_SYNC` 返回 `EOPNOTSUPP`，fio `do_verify`/
  `end_fsync`/`fsync_on_close` 无法通过；open/preallocate 失败不设置 `errno`，
  fio 以 err=0 丢 job；`get_file_size` 在 `size=0` 时伪造 4096；fork 模式
  `numjobs>1` 静默失败。
- **根因**：adapter 未映射 sync 语义、未遵守 fio errno 契约、伪造文件尺寸。
- **修复**：新增 `fio/fio_status_errno.hpp`（映射+设置 errno）；`DDIR_SYNC` 映射到
  `File::sync()`（async 与 sync 路径）；`get_file_size` 优先 open+stat 真实大小，
  否则返回请求尺寸（0 保持 0，由 fio 报缺 size）；fork 模式在 init 明确拒绝并提示
  `thread=1`。
- **验证结果**：`fio_status_errno_test` 通过；真实 NVMe
  `--verify=crc32c --verify_fatal=1 --end_fsync=1` async/sync 均 `err=0`；缺 size
  新文件报错、已有文件 `create_serialize=0` 省略 size 读取 `err=0`；fork 探针 rc=1
  且输出提示。

### 31. 后端 slot 耗尽被当终态数据错误

- **状态**：已修复
- **等级**：严重（压力下合法请求失败）
- **时间**：2026-09-18（Shard-81）
- **现象**：`SpdkAsyncDataBackend::acquireSlot()` 失败直接返回
  `Rejected/ENOMEM`，service 把 Rejected 转成 child 终态错误；多 child/RMW 场景
  在集中 shard 负载下会误报数据错误。
- **根因**：设计要求的 `WaitingForCredit` io-wait 路径未实现。
- **修复**：backend 增加有界 pending 队列，无 slot 时返回 `WaitingForCredit` 入队；
  `AsyncDataBackend::pump()` 由 service 在所属 reactor 每轮调用，slot 释放后按
  FIFO 补投；仅 pending 也满时才 Rejected。
- **验证结果**：service 级 WaitingForCredit 用例通过；32 job gate
  `child_rejected=0`、`child_submit=child_complete`。

### 32. DmaArena release 尺寸与析构不一致

- **状态**：已修复
- **等级**：一般（静默泄漏/释放仍在使用的 DMA 内存）
- **时间**：2026-09-18（Shard-81）
- **现象**：`release` 要求 span 尺寸与登记一致，调用者传申请尺寸即静默泄漏；
  析构释放了在途 large 分配却泄漏在途 class 分配。
- **根因**：release 以调用者尺寸推导 class，析构未覆盖全部自有分配。
- **修复**：release 按指针查表并使用登记尺寸；foreign 指针安全忽略；析构释放
  class 缓存块、在途 class 与在途 large 全部分配。
- **验证结果**：`io_planner_test` 计数分配器用例 alloc==free==2 且不同尺寸 release
  生效。

### 33. SpdkRuntime 停机竞态与无界排空

- **状态**：已修复
- **等级**：严重（潜在 UAF/挂起）
- **时间**：2026-09-18（Shard-81）
- **现象**：`stop()` 在 join 完成后才清 `activeFlag`，期间提交路径只检查
  `activeFlag`，可向正在销毁的 reactor 线程 `send_msg`；reactor 排空在 stranded
  `queue_io_wait` 上无上界自旋。
- **根因**：停机不是提交栅栏；排空缺少截止时间。
- **修复**：`controlIo`/`postToDataReactor`/`submitDataRead/Write`/
  `setDataReactorPoller` 同时检查 `stoppingFlag`；`dataMain` 停止后最多排空 5 秒，
  超时记录错误退出。
- **验证结果**：`spdk_runtime_test` 断言 stop 后 `postToDataReactor` 失败；设备
  测试通过。

### 34. trace TSC 校准与 stop/producer 竞争

- **状态**：已修复
- **等级**：一般（归因数据失真/测量失败）
- **时间**：2026-09-18（Shard-81）
- **现象**：TSC 只在单线程单点校准且不检查 invariant TSC；`stop()` 与通过
  `enabled` 检查的 producer 存在竞争，`recorded - dropped` 可能与落盘记录不一致；
  channel 元数据在 claim 后写入存在数据竞争。
- **根因**：校准假设跨 CPU TSC 可比；stop 未等待在途 producer；claim 发布顺序不严。
- **修复**：CPUID 检查 invariant TSC，双采样并拒绝 >5% 漂移，不满足时回退
  `steady_clock` ns（`tsc_hz=1e9`）；`event()` 使用 `ActiveProducerGuard`，`stop()`
  有界等待在途 producer 后再写 manifest/关文件；channel 元数据先写后用 release
  发布 claim。
- **验证结果**：`trace_test` 新增并发 stop/producer 用例，落盘记录数 == recorded、
  `dropped==0`；原有 manifest format v2 断言通过。

### 35. 异步路径杂项收口

- **状态**：已修复
- **等级**：一般
- **时间**：2026-09-18（Shard-81）
- **现象/根因/修复**：
  - Zero（hole）child 也申请 bounce lease，稀疏读在 arena 紧张时误报 NoMemory
    → `acquireLeases` 跳过 Zero child；
  - `hfs_async=0` 回退每 I/O `new` bounce → 改用 engine DMA arena + RAII guard；
  - `OperationPool::create` 在 `noexcept` 中抛异常 `new` → 改 `new (std::nothrow)`；
  - `CompletionNotifier::notify` 遇 EINTR 丢唤醒 → 重试写 eventfd；
  - `ClientOptions` 提供外部 wait strategy 但缺少 wake hook 时静默全超时 →
    `initializeAsync` 拒绝该组合；
  - `FioCompletionHub::release/shutdown` 在 waiter 未退出时清除 inUse 造成复用
    竞态 → notify 后有界等待 `waiterCount()==0` 再交接；
  - `fioTimeout` 对超大 timespec 返回 `microseconds::max()`，与 steady_clock 相加
    可能溢出 → 夹到 24h。
- **验证结果**：`async_data_service_test`（hole lease）、`io_planner_test`、
  `fio_completion_hub_test`（release 等待 waiter）、`trace_test`、
  `async_completion_queue_test` 全部通过；真实 NVMe 探针与 verify smoke 无回归。

### 36. shard inode 槽位线性扫描导致每 I/O ~5ms 延迟

- **状态**：已修复
- **等级**：严重（性能 gate 结构性失败）
- **时间**：2026-09-18（Shard-81 性能复测定位）
- **现象**：修复 #26-#35 后，正式口径 32 job gate 的 HFS 只有约 9k IOPS（raw 65k，
  比值 13.7%）；单线程单 4K I/O 的 clat 也约 4.8ms（raw 约 0.48ms）。
- **排查过程**：用 trace v2 分段定位（新增 `ActorPickup`/`PlanReady` 事件与 dump 的
  `ring_wait/plan_exec/admit_exec` 段）：请求在 `PlanReady` 后到 `RequestShardSubmit`
  前停留 5-7ms，且该窗口内 reactor 线程没有任何事件；对照 `prepare` 路径，确认停顿
  发生在 `findInode`。`findInode` 对 `maxInodesPerShard`（默认 500k）做线性扫描，
  而 `reclaimInodeSlot` 在每次请求完成且调度器清空后回收槽位，导致下一个请求重新
  全表扫描；扫描 500k 个 `InodeSlot`（工作集约 100MB）约 5ms。
- **根因**：inode 槽位查找为 O(maxInodesPerShard)，且槽位频繁回收使缓存命中失效。
- **修复**：`ShardState` 增加 `RequestLookupTable`（fileId→slot 固定容量开放寻址、
  backward-shift 删除）与 O(1) free 索引栈；`findInode` 改为哈希查找/插入，
  `reclaimInodeSlot` 改为哈希删除并把索引还栈（`async_data_service.cpp`）。
- **验证结果**：新增 2000 请求 × 262144 槽位计时回归（新实现 <2s，旧实现约 2s+ 超界）；
  正式 5-run gate：HFS 中位 `59091 IOPS`、raw 中位 `66935 IOPS`、`HFS/raw=88.28%`、
  errors=0、`accepted=terminal=published`、`child_submit=child_complete`、
  `outstanding=0`、`token_mismatch=0`；p99 为 HFS `1433.6us` vs raw `1253.4us`
  （1.144x），仍略高于 1.10x 门禁。证据目录
  `/mnt/hfs-data/hfs2-postfix-gate-20260918`。

### 37. fio cleanup 忽略 Engine::stop 失败导致 data reactor UAF

- **状态**：已修复
- **等级**：致命（停机期 SIGSEGV）
- **时间**：2026-09-18（Shard-81 优化复测发现）
- **现象**：优化后拓扑矩阵中 `official 8j×4`、`pin2 32j×1`、`pin2 8j×4` 三类
  用例偶发 SIGSEGV（rc=-11，无 JSON 输出，约 3/7 概率）；gdb 栈显示 data reactor
  线程在 `ShardRing::drain`（`ShardActor::runOnce` ← `AsyncDataService::pump`）
  崩溃，且崩溃前已打印 `fio-hfs closure` 但没有 `hfs: engine stopped`。
- **排查过程**：对比成功/失败日志顺序，确认失败运行中 `processEngine->stop().wait()`
  已返回但引擎未执行 mounted 停机分支；`releaseProcessUser()` 不检查返回值即
  `processEngine.reset()`，把仍在轮询的 data reactor 连同 `asyncService`/`ShardRing`
  一起释放。复现时在 stop 失败分支加日志，捕获到原因字符串
  `async data operations are still outstanding`（`Engine::stop` 的 `outstanding()!=0`
  检查，属并发 cleanup 的瞬态窗口）。
- **根因**：fio cleanup 把 stop 失败当成功继续释放引擎，违反“停机栅栏”语义；
  瞬态 Busy 无人重试。
- **修复**：`releaseProcessUser()` 对 `Engine::stop()` 做有界重试（200 × 10ms），
  仅在成功后才 `reset()`；失败时保留引擎并打印原因（`fio/fio_hfs.cpp`）。
- **验证结果**：复现矩阵 7/7 通过（`off_8j4`×4、`pin2_32j1`×3，rc=0、JSON 完整、
  `hfs: engine stopped` 存在）；正式 5-run gate2 中 10 次 HFS 运行仅 2 次出现
  `engine stop retried after: async data operations are still outstanding`，
  均在有界重试内成功停机，无崩溃。证据目录
  `/mnt/hfs-data/hfs2-teardown-check-20260918`、`/mnt/hfs-data/hfs2-postfix-gate2-20260918`。

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
