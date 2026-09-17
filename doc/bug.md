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

### 19. FIO 外部引擎文件布局未形成实际 I/O（处理中）

- **状态**：处理中
- **等级**：严重
- **时间**：2026-09-17（Shard-80A）
- **现象**：真实 NVMe 上 FIO 引擎可加载、HFS 可 format/mount/stop，但最小 job 未产生 I/O 统计。
- **排查过程**：初版因缺少 invalidate 回调报 Bad file descriptor；补齐后生命周期正常，仍需继续闭合 FIO `get_file_size`、open/layout 与请求调度契约。
- **根因**：FIO 虚拟文件引擎不能依赖 host fd 的默认文件布局路径，当前 size/open 回调组合尚未满足 FIO 的实际请求准入条件。
- **修复**：已补齐无 fd invalidate、get_file_size 和 open 时 preallocate 的骨架；功能矩阵前保持 Shard-80 未完成。
- **验证结果**：编译通过；真实 NVMe bind→format→mount→cleanup 下 4K read/write 各 1 I/O、errors=0；高 QD 功能矩阵与 DMA all-or-nothing 门禁仍未通过。

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
