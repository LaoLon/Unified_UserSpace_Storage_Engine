# HFS 产品架构决策

> 状态：阶段性决策，基于 `main@ac56166`、SPDK `f0b63ed866f0e4d3c56e73bd4a98d0042956147a`。

## 决策

当前继续定位为“单数据库进程的 SPDK 加速库”，不在本阶段启动 daemon + shared-memory/ring
文件系统架构。

产品形态为：

- `libhfs` 提供异步 libhfs I/O、请求级 pipeline、按 reactor/channel 的数据路径和持久化元数据。
- `LD_PRELOAD` 提供 MySQL/MariaDB-compatible server 与 RocksDB 的兼容层。
- 一个 HFS 进程独占一个 bdev；数据库客户端通过数据库自身的 socket/TCP 接口访问数据。
- 真实 NVMe 与持久化 AIO 是同一单进程模型的后端验证矩阵，不改变进程边界。

## 证据

| 能力 | 当前证据 | 结论 |
| --- | --- | --- |
| POSIX 兼容路径 | fd 生命周期、目录路径、stat/statfs、时间戳、`*at`、fadvise 回归通过 | preload 适合作为兼容层 |
| 异步数据面 | async pipeline、请求级 batch、DMA size class、direct/vector 计数通过 | libhfs 适合作为性能核心 |
| 并发扩展 | 多 data reactor/channel、4 reactor 多文件写入、owner poller 非阻塞完成通过 | 单进程内可继续扩大并行度 |
| 数据库 | MariaDB/MySQL 初始化、CRUD/DDL、事务、重启、SIGKILL；RocksDB WAL、flush/compact、恢复和一致性检查通过 | 单进程数据库场景可支持 |
| 持久化 | Aio bdev 跨进程重开、事务恢复和设备 flush 回归通过 | 持久化后端路径可验收 |

## 暂不选择 daemon 的原因

daemon 方案需要一次性定义跨进程协议，而当前收益主要来自单进程内的 owner/channel、fd
生命周期和数据请求调度。引入 daemon 会新增以下尚未验证的协议面：

- 多进程 mount 的租约、权限和故障转移；
- shared-memory/ring 的 ABI、版本升级和请求取消；
- daemon 崩溃后的未完成请求、DMA buffer 和锁回收；
- 跨进程 metadata epoch、fsync 顺序和数据库进程隔离；
- 一个 bdev 被多个客户端访问时的限流、优先级和安全边界。

这些问题不能通过继续增加 preload wrapper 解决，应该作为独立架构阶段设计和验收。

## 支持边界

当前产品边界保持明确：

- 支持：单数据库进程、单 HFS mount、持久化 AIO/NVMe、同步 POSIX/preload、async libhfs。
- 不支持：多个 HFS 进程同时挂载同一 bdev、mmap 数据面、native libaio/io_uring ABI、
  未完成验证的通用多进程文件系统语义。
- direct/vector 仅在 buffer 通过 SPDK 地址转换并满足对齐约束时启用，否则回退到 DMA
  bounce 路径。

## 后续阶段入口

下一阶段应先完成真实 NVMe 上的 QD/线程/reactor 矩阵，并将 raw SPDK、sync preload 和
async libhfs 分开报告。若业务明确要求多进程共享挂载，再另开 daemon 阶段，先冻结 IPC、
权限、故障恢复和元数据一致性协议，不能把 daemon 作为当前 shard-48 的隐式扩展。

当前 VM 可见一块未挂载的 VMware NVMe 设备（约 30 GiB），但它是原始块设备，尚未被指定为
可擦除的 HFS 测试盘。本阶段不对该设备执行 format、写入或 fio，以免破坏宿主数据；真实 NVMe
结论需要用户明确提供可覆盖设备后再纳入验收。
