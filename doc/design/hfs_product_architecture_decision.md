# HFS 产品架构决策

> 状态：当前架构决策（POSIX-only），2026-09-10 生效。此前 async/registered-DMA
> 实现和测试结果保留在历史文档中，不再代表当前产品接口。

## 决策

当前继续定位为“单数据库进程的 SPDK 加速库”，不在本阶段启动 daemon + shared-memory/ring
文件系统架构。

产品形态为：

- `libhfs` 提供完整 POSIX 语义的 preload 路径、请求级 pipeline、按 reactor/channel 的数据路径和持久化元数据。
- `LD_PRELOAD` 提供 MySQL/MariaDB-compatible server 与 RocksDB 的兼容层。
- 一个 HFS 进程独占一个 bdev；数据库客户端通过数据库自身的 socket/TCP 接口访问数据。
- 真实 NVMe 与持久化 AIO 是同一单进程模型的后端验证矩阵，不改变进程边界。

同步 POSIX syscall 是唯一对外支持的 HFS I/O 入口。内部仍可使用 DMA pool、
`spdk_vtophys()` direct 检测、SPDK batch 和 pipeline，但这些实现细节不形成用户 ABI。

## 证据

| 能力 | 当前证据 | 结论 |
| --- | --- | --- |
| POSIX 兼容路径 | fd 生命周期、目录路径、stat/statfs、时间戳、`*at`、fadvise 回归通过 | preload 适合作为兼容层 |
| 内部数据面 | 请求级 batch、per-reactor DMA pool、range reservation、direct/vector 尝试通过 | 作为 POSIX 路径内部优化，不暴露 async ABI |
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

- 支持：单数据库进程、单 HFS mount、持久化 AIO/NVMe、同步 POSIX/preload。
- 不支持：多个 HFS 进程同时挂载同一 bdev、mmap 数据面、native libaio/io_uring ABI、
  未完成验证的通用多进程文件系统语义。
- direct/vector 仅在 POSIX syscall 内部确认 buffer 可由 SPDK 地址转换并满足对齐约束时启用；
  普通 buffer 使用受控 DMA bounce 路径。

## 后续阶段入口

下一阶段应继续完成真实 NVMe 上的 POSIX QD/线程/reactor 矩阵，并将 raw SPDK 与 sync
preload 分开报告。若业务明确要求多进程共享挂载或应用级异步 ABI，再另开 daemon/SDK 阶段，先冻结 IPC、
权限、故障恢复和元数据一致性协议，不能把 daemon 作为当前 shard-48 的隐式扩展。

当前性能验证按项目记忆使用 VM 内真实 NVMe 配置 `config/hfs_nvme.json` 和
`HFS_BDEV_NAME=Nvme0n1`；功能 smoke 可显式选择持久化 Aio 镜像。
