# HFS MySQL / RocksDB 可用化阶段开发说明书

> 基线：`main@3908170`，已完成 shard-01～25。
> 本文从 shard-26 继续，优先级高于旧文档中尚未实施的 shard-26 及以后计划；TiDB 延后到本阶段完成后重新编号。
> 目标是让 MySQL 和 RocksDB 在 HFS 上具备可重复、可重启、线程安全的基础生产语义，同时保持实现简洁并优先保护 I/O 性能。

## 1. 完成定义

本阶段完成后，才允许对外表述为“支持 MySQL 和 RocksDB”。必须同时满足：

1. 使用持久化 AIO/NVMe bdev；进程退出并重新启动后，HFS 能加载原 Blobstore，数据不丢失。
2. 一个 HFS 后端同一时刻只允许一个数据库进程挂载；客户端通过 socket/TCP 访问数据库，不直接挂载 HFS。
3. MySQL 完成初始化、启动、事务 CRUD、正常重启、异常退出恢复和并发短压测。
4. RocksDB 分进程完成 fill/read/overwrite，完成 WAL 恢复、并发读写和一致性检查。
5. 数据库线程不能并发轮询同一个 SPDK thread；fd、目录、cwd、inode 和打开文件生命周期不存在数据竞争。
6. `fsync`、`fdatasync`、目录 `fsync`、rename、unlink、truncate 的语义满足数据库恢复要求。
7. 基础 preload/fio 回归全绿；Malloc bdev 的既有单线程性能下降不超过 10%。

## 2. 当前阻塞点

### 2.1 后端不持久

- `config/hfs.json` 创建进程私有的 `Malloc0`。
- `hfs_mount.c` 硬编码 `Malloc0`，每次都调用 `spdk_bs_init()`。
- MySQL 初始化和服务启动是不同进程；RocksDB 当前三个 db_bench 用例也是不同进程。现状下后一个进程必然看到空文件系统。

### 2.2 SPDK thread 所有权错误

- `waiter()` 由调用方线程直接执行 `spdk_thread_poll(globalThread, ...)`。
- MySQL/RocksDB 有多个业务和后台线程，多个线程会并发 poll 同一个 SPDK thread，并操作同一个 channel。
- 调小超时只能缩短卡顿，不能修复线程所有权和消息丢失问题。

### 2.3 fd 与打开文件描述不满足 POSIX

- `hfs_is_hfs_fd()` 无锁读取 fd 表，而 open/close/dup 会并发改写。
- wrapper 先判断 fd、再进入库函数，存在 close/reuse 的 TOCTOU 窗口。
- dup 后 offset 应共享，当前是复制数值；同一 `HfsFile` 的 I/O scratch 字段也不能并发使用。
- unlink/rename 覆盖后，已打开 fd 应继续可用；数据 blob 应延迟到最后一个引用关闭后删除。

### 2.4 元数据与路径状态存在并发问题

- `fstat/stat` 取得 inode 指针后解锁，再从指针填充结果；inode 表压缩或 rename 会使指针失效。
- cwd 是进程级状态，当前没有同步保护；目录句柄注册表也没有并发保护。
- 每次元数据更新可能整表写回，数据库高频 fsync 下写放大明显。

### 2.5 热路径额外开销较大

- 每次 read/write 都分配并释放 DMA buffer。
- 非对齐写始终对覆盖范围执行完整 read-modify-write；对齐的大块中间区域也没有独立直写路径。
- `readv/writev` 先聚合到普通临时内存，产生额外分配和复制。

## 3. 简化后的目标架构

```text
MySQL / RocksDB threads
        │
        ├─ path router + fd registry（短锁、引用计数）
        ├─ metadata cache（rwlock、稳定快照、短事务）
        └─ request-local I/O context
                         │
                         ▼
              dedicated SPDK owner thread
              （唯一 poller、唯一 channel owner）
                         │
                         ▼
              persistent AIO / NVMe bdev
```

设计约束：

- SPDK 对象只由专用线程创建、poll 和销毁。
- 不在 fd 表锁、元数据锁或 cwd 锁内等待 SPDK I/O。
- fd 表只保存引用计数对象指针；所有操作先 acquire，完成后 release。
- sequential read/write 只锁共享 offset；pread/pwrite 不锁 offset。
- I/O 回调只操作本次请求上下文，不复用 `HfsFile` scratch 字段。
- 持久介质默认只 load；格式化必须显式设置，禁止 load 失败后自动清盘。
- 不拦截通用 `syscall()` 变参入口；缺失能力按实际 trace 增加明确的 libc/API wrapper。

## 4. 开发与提交规则

1. 每个 shard 只解决一个主问题，代码改动原则上不超过 500 行。
2. 每片均执行：本地修改 → 同步 VM → 编译 → preload 回归 → 本片专项测试 → 更新 `doc/bug.md` → Git 提交。
3. C/C++ 名称遵循项目规范；所有函数、结构体和关键逻辑使用简洁英文 `/* */` 注释。
4. 不为“可能会用”提前实现 POSIX 接口；先用 strace/日志证明数据库确实调用，再补最小语义。
5. 新增模块按职责拆分，避免继续扩大现有 1000 行以上文件：
   - `hfs_runtime.[ch]`：SPDK owner thread 与完成通知。
   - `hfs_fd.[ch]`：fd registry、open-file description、引用计数。
   - `hfs_io.[ch]`：请求上下文、DMA buffer pool、数据 I/O。
   - `hfs_meta_txn.[ch]`：元数据事务与恢复。
6. 任何测试都必须检查真实退出码和数据结果，禁止使用管道或 `|| true` 掩盖数据库失败。

## 5. shard 计划

### shard-26：持久化 bdev 与安全 mount/load

目标：让不同进程顺序挂载同一个 HFS，并禁止意外格式化。

改动：

- 新增 `config/hfs_db_aio.json`，使用固定测试镜像上的 SPDK AIO bdev。
- 新增 `scripts/db/prepare_hfs_db_bdev.sh`，创建容量受控、对齐正确的测试镜像和运行目录。
- 增加运行时配置：`HFS_CONFIG_PATH`、`HFS_BDEV_NAME`、`HFS_FORMAT=1`。
- `HFS_FORMAT=1` 时调用 `spdk_bs_init()`；默认调用 `spdk_bs_load()`，未格式化则明确失败。
- 使用内核侧 lock file 独占后端，第二个挂载进程立即返回 `EBUSY`。
- 保留 Malloc0 配置用于无持久化的 fio 微基准。

验收：

- 进程 A 显式格式化、创建文件、写入并 fsync 后退出。
- 进程 B 默认 load，同一路径读取到完全一致的数据。
- 进程 C 在 A 持有后端时挂载，必须快速失败且不能破坏数据。
- 未设置 `HFS_FORMAT=1` 时，空镜像不得自动格式化。

提交：`shard-26: persistent bdev mount and explicit format`

---

### shard-27：SPDK 专用线程与高效完成通知

目标：所有数据库线程通过消息提交请求，只有一个专用线程 poll SPDK。

改动：

- 新增 `hfs_runtime.[ch]`，专用 pthread 内创建并拥有 SPDK thread、blobstore channel。
- 将完成标志改为请求级 completion；使用原子状态加 Linux futex 等待/唤醒，避免业务线程忙轮询。
- init/mount/unmount/file/meta 操作全部通过 `spdk_thread_send_msg()` 提交。
- 超时使用单调时钟和明确错误码；超时只标记请求失败，不释放仍可能被回调访问的上下文。
- fini 先停止接收请求，再排空队列、卸载 blobstore、退出 owner thread。

验收：

- 记录并断言所有 SPDK 回调都运行在 owner thread。
- 32 个应用线程循环 open/pread/pwrite/fsync/close，连续运行 60 秒无 hang、wrong-thread 或 UAF。
- 进程退出时间小于 3 秒，失败请求能返回稳定 errno。

提交：`shard-27: dedicated SPDK owner thread`

实现状态（2026-09-03）：已完成。VM 上 Make/CMake 双构建、preload smoke、持久化 AIO smoke，以及 32 线程持续 60 秒的 open/pread/pwrite/fsync/close 压测全部通过；未发现 wrong-thread、UAF 或退出 hang。

---

### shard-28：线程安全 fd registry 与 POSIX 打开文件描述

目标：修复 fd 判断、close/reuse、dup offset、并发 I/O 和 unlink-open 语义。

改动：

- 新增 `hfs_fd.[ch]`，用短时 rwlock 保护 fd slot；禁止持锁调用 libc、日志或 SPDK。
- 引入引用计数 `HfsOpenFile`：保存 blob、inode state、共享 offset、flags 和 I/O 锁。
- wrapper 使用一次 acquire/release 完成路由，删除“先 `hfs_is_hfs_fd`、后再次查表”的窗口。
- dup/dup2/dup3/F_DUPFD 共享同一个 `HfsOpenFile` 和 offset。
- close 先从表中摘除 slot，再释放内核占位 fd，最后递减对象引用。
- unlink 或 rename 覆盖只移除目录项；存在打开引用时加入 orphan，最后 close 才删除 blob。

验收：

- 多线程随机执行 open/dup/read/fstat/close/reopen，数据和 fd 类型始终正确。
- dup 后从任一 fd 读取都会推进共享 offset。
- unlink 后旧 fd 仍可读写，新 open 返回 ENOENT；最后 close 后 blob 被回收。
- 通过针对 fd 模块的 TSAN 单元测试；SPDK 集成测试用压力循环验证。

提交：`shard-28: concurrent fd and open-file lifecycle`

实现状态（2026-09-03）：核心实现已完成。新增 fd registry 和引用计数的
open-file description，dup/dup2/dup3/F_DUPFD 共享 offset；close/reuse 使用
先摘 slot、后释放 placeholder 的顺序；unlink-open 与 rename 覆盖采用 orphan
blob 延迟回收；fcntl 状态查询只在命令需要时读取可变参数。VM 上
`fd_lifecycle_smoke.sh` 已通过共享 offset、fd 复用、
unlink 后旧 fd 读写/fstat、rename 覆盖、同路径 rename 和 8 个并发描述符的
定位读写；Make/CMake 双构建、preload smoke、持久化 smoke，以及 16 线程
10 秒 owner-thread stress 均通过。独立 TSAN 单元测试尚未建立，保留到并发
测试基础设施阶段；本阶段不宣称 MySQL/RocksDB 已支持。

---

### shard-29：元数据并发模型与稳定快照

目标：让路径解析、stat、cwd、目录枚举和 inode 生命周期在线程间一致。

改动：

- 元数据表使用 rwlock：lookup/stat 持读锁，create/unlink/rename/truncate 持写锁。
- API 不向锁外返回可失效的 inode 指针；返回 inode id 或结构体快照。
- 文件大小使用稳定 inode state 更新，pread/fstat 不再依赖可移动数组元素地址。
- cwd 改为受锁保护的进程级状态；实现 HFS 目录 fd 的 fchdir。
- 目录枚举创建稳定快照并增加句柄锁；实现 fdopendir，移除无锁全局目录数组扫描。
- 修正 O_CREAT|O_EXCL、O_APPEND、O_TRUNC、rename 覆盖和目录 errno。

验收：

- 16 线程并发 rename/stat/open/readdir，结果只允许成功或符合 POSIX 的瞬时 ENOENT。
- RocksDB CURRENT/MANIFEST 的 size 与内容在并发读取中保持一致。
- MySQL 相对路径、openat(dirfd) 和目录 fsync 压力测试连续 10 轮通过。

提交：`shard-29: thread-safe metadata and path state`

实现状态（2026-09-04）：已完成。元数据访问拆为读写锁，目录枚举使用句柄
锁和稳定快照；元数据格式增加版本头、持久化 `nextInodeId`，并兼容旧的
`[count][inodes]` 布局，删除/压缩 inode 表不再复用 ID。preload 增加受锁保护
的 HFS cwd、`fchdir`、`fdopendir`、并发目录注册表和相对 `openat` 路径；补齐
`O_CREAT|O_EXCL`、`O_APPEND`、`O_TRUNC`、`O_DIRECTORY` 及目录 errno 语义。
新增 `scripts/db/metadata_path_concurrency_smoke.sh`。VM 上 Make/CMake 双构建、
元数据路径并发（16 线程 × 40 轮）、fd 生命周期、跨进程持久化，以及 16 线程
10 秒 owner-thread stress 全部通过。MySQL/RocksDB 真实数据库验收仍保留到
shard-33～35，当前阶段不宣称已支持。

---

### shard-30：可恢复的元数据短事务

目标：保证 create/unlink/rename/size 更新在异常退出后可恢复，并降低 fsync 写放大。

改动：

- 元数据格式升级，加入 generation、checksum 和固定大小 intent record。
- 兼容 shard-29 的 v2 版本头和更早的 `[count][inodes]` 布局；首次修改时升级为 v3，不静默重新格式化。
- 每次命名空间事务只记录受影响 inode 的 after-image：先持久化 intent，再写脏页，最后清除 intent。
- mount 时校验 checksum；发现有效 intent 时幂等重放，禁止静默重新格式化。
- inode 表分页，仅写脏页；普通数据写只更新内存 size，fsync/close 按需提交。
- `fsync(file)` 顺序：数据 I/O 完成 → blob resize metadata sync → bdev flush → inode size/mtime 事务提交。
- `fsync(directory)` 提交该目录相关的 create/unlink/rename；`fdatasync` 不刷新无关时间字段。

验收：

- 在事务的每个持久化步骤注入崩溃，重挂载后只能看到事务前或事务后的完整状态。
- rename 后 CURRENT 文件永远是完整旧值或完整新值。
- 10000 次无新增修改的 fsync 不产生 inode 全表写回；日志/计数器能证明写入页数。

提交：`shard-30: recoverable metadata transactions`

实现状态（2026-09-04）：核心实现已完成。元数据升级为固定 v3 header + generation/checksum +
固定大小 intent record；create/unlink/rename/size 事务按 intent → stable image →
clear intent 顺序提交，mount 时校验并幂等重放。兼容 v2/legacy 元数据并在首次写入时迁移。
`fsync` 先等待 blob 数据和 metadata sync，再提交独立可写 bdev descriptor 的设备 flush，
最后提交 inode size/mtime。新增 `scripts/db/metadata_transaction_smoke.sh`，覆盖 create、
source-last rename 覆盖、size after-image 的三个崩溃边界，并校验恢复后的数据内容；VM 上
Make 构建、事务恢复、元数据路径并发和持久化 smoke 全部通过。

当前实现为保持事务路径简单，stable image 仍按完整元数据 blob 写入；“仅写脏页”、写放大
计数和 10000 次 fsync 性能验收移至 shard-31 的 request-local/page 优化，不在本 shard
提前宣称完成。

---

### shard-31：请求级 I/O 与热路径优化

目标：在正确并发模型上减少分配、复制和不必要的读改写。

改动：

- 新增 `hfs_io.[ch]`；每次 I/O 使用独立上下文，移除 `HfsFile` 中共享的 readBuffer/writeBuffer/ioOffset/finished。
- 建立有限大小的 DMA buffer pool，热路径不做 malloc/free；池耗尽时等待或返回可诊断错误，禁止无限增长。
- 对齐读写直接提交；非对齐写仅对首尾 io unit 做 RMW，中间完整 io unit 直接写。
- pread/pwrite 可并发；sequential read/write 只在取得并更新共享 offset 时加短锁。
- readv/writev 按 iovec 分段复制到 DMA buffer，避免先聚合成一个普通大 buffer。
- 支持 KEEP_SIZE；PUNCH_HOLE 对齐部分走 unmap，边缘写零；不支持的 flag 返回 EOPNOTSUPP。

验收：

- 热身后 4K/16K/128K 对齐 I/O 的每请求动态分配次数为 0。
- 非对齐写不会破坏相邻字节；稀疏区域读取为零。
- Malloc fio 4K 与 128K 基线相对 `3908170` 下降不超过 10%，并记录 p50/p99。

提交：`shard-31: request-local fast IO path`

实现状态（2026-09-04）：核心实现已完成并在 VM 上验证。新增 `hfs_io.[ch]`，使用固定大小
DMA pool 和请求级上下文；对齐 I/O 直接提交，非对齐写只对首尾 unit 做 RMW；pread/pwrite
和 readv/writev 不再使用 `HfsFile` 共享 scratch buffer，positioned I/O 可并发。`KEEP_SIZE`
和 `PUNCH_HOLE|KEEP_SIZE` 已实现，punch 的对齐范围在 unmap 后补 zeroes/固定池写零，避免
后端 unmap 成功但读回旧数据。fio 精确挂载点、根目录 `mkdir` 语义及 fork 子进程边界已收口：
runtime 记录 owner PID，fork child 不提交 SPDK message、不识别继承的 HFS fd，基准显式使用
`thread=1`。

VM 回归通过：Make 强制重编译、CMake Release、请求级 fast I/O（含向量 I/O、稀疏间隙、
KEEP_SIZE、PUNCH 和 8 路 positioned write）、元数据事务恢复、元数据路径并发、fd 生命周期、
持久化 bdev。fio 使用 malloc bdev、O_DIRECT、iodepth=32、10s，结果如下（延迟单位 ns）：

| 场景 | read IOPS | write IOPS | read BW | write BW | read p50/p99 | write p50/p99 |
|---|---:|---:|---:|---:|---:|---:|
| randrw 4K | 6839.52 | 6806.22 | 28.01 MB/s | 27.88 MB/s | 58112 / 120320 | 58112 / 119296 |
| seq 128K | 5772.32 | 5750.12 | 756.59 MB/s | 753.68 MB/s | 73216 / 142336 | 73216 / 142336 |

阶段口径仍为：fio 使用单进程 `thread=1`，不宣称通用 fork 后共享 HFS 状态；MySQL/RocksDB
真实数据库验收仍留在 shard-33～35，当前只能表述为“兼容性开发中”。

---

### shard-32：数据库 POSIX 契约收口

目标：根据 MySQL/RocksDB 的真实调用补齐最小 POSIX 能力，同时拆小拦截层。

改动：

- 将 `hfs_preload.c` 按 path/fd/dir 三类拆分，共享 resolver 与路由内部头文件。
- 修复 open/fcntl 变参读取：只在命令或 flags 需要参数时读取 va_arg。
- 补齐数据库 trace 确认需要的接口，预计包括：statx、fstatat(dirfd)、unlinkat(dirfd)、renameat(dirfd)、mkdirat、statvfs/statfs、fadvise、chmod/chown/utimensat 系列。
- inode 格式保存 mode/uid/gid 和必要时间字段；权限检查保持最小且一致。
- mmap 到 HFS fd 明确返回 ENODEV；RocksDB 测试关闭 mmap。native libaio/io_uring 不在本阶段伪装支持。
- 增加 `scripts/db/posix_contract_test.sh`，覆盖数据库依赖的成功路径和 errno。

验收：

- 使用 strace 对 MySQL 初始化/启动和 RocksDB fill/read 做调用审计；HFS fd 不得落入内核 `/dev/null` 数据路径。
- 契约测试连续 10 次全绿，现有 preload smoke/fio 无回归。
- 对未支持调用给出准确 errno，数据库能明确降级或测试直接失败，禁止静默成功。

提交：`shard-32: database POSIX contract`

实现状态（2026-09-04）：核心 POSIX 契约已完成。元数据升级为 v4，保存 mode/uid/gid、
atime/mtime/ctime，并兼容 v3、v2 与 legacy 布局；创建 inode 的 mode 已贯通到 `open`/`mkdir`。
preload 已补齐 HFS 目录 fd 的 `openat`/`fstatat`/`unlinkat`/`renameat`/`mkdirat` 路由，
`statx`、`statfs`/`statvfs`（含 64 位 glibc ABI）、chmod/chown/utimensat/futimens、
fadvise 和明确的 `mmap -> ENODEV` 语义；`fcntl` 只在命令确实需要时读取变参。新增
`scripts/db/posix_contract_test.sh` 覆盖成功路径、错误码、属性、容量、空路径和 mmap 契约。

VM 回归通过：`make -B app lib`、CMake Release、POSIX 契约连续 10 次、preload smoke、
fd 生命周期、元数据事务、元数据路径并发、持久化 bdev、请求级 I/O、16 线程 10 秒压力和
fio。最近一次 fio（4K randrw / 128K seq，iodepth=32，10s）分别为 6285/6258 与
4954/4922 read/write IOPS，p50 为 59136/75264 ns，p99 为 185344/244736 ns。

数据库审计结果：已用 strace 跑过现有 MariaDB 初始化和 RocksDB fill/read smoke；现有脚本
分别因 MariaDB helper 的跨进程初始化、默认进程私有 `Malloc0`/挂载失败而未完成端到端流程，
不能据此标记数据库 supported。审计未发现 `/hfs/...` 上真实 `read`/`write`/`pread64`/
`pwrite64`/`mmap` 内核数据调用，HFS 数据路径没有误落到 `/dev/null`；完整持久化、多进程
初始化、WAL/事务/恢复验收继续由 shard-33～35 完成。

---

### shard-33：MySQL 完整验收

目标：真实 MySQL 完成初始化、事务、重启与恢复；MariaDB 保留为兼容回归。

改动：

- 重写 `scripts/db/mysql_smoke.sh`，使用持久化 bdev，严格保存每一步退出码和日志。
- 第一次进程显式格式化并执行 initialize；第二次及以后只 load。
- 客户端不设置 LD_PRELOAD，只通过 socket/TCP 连接服务进程。
- 配置使用 POSIX 同步 I/O；若当前版本提供 native AIO 开关则关闭。native libaio/io_uring 适配留给后续独立阶段。
- 新增 CRUD、事务 rollback/commit、大字段、临时表、DDL、正常重启和 SIGKILL 恢复用例。
- 新增 4/16 线程短时 sysbench，用于发现锁竞争和退出问题。

验收：

- initialize → start → SQL 校验 → clean shutdown → restart → SQL 再校验全部通过。
- 写入已提交事务后 SIGKILL，重新启动能恢复且表检查通过；未提交事务不可见。
- sysbench 4/16 线程各运行 60 秒，无 hang、崩溃、I/O error、表损坏和 HFS wrong-thread 日志。
- 连续完整执行 3 轮全绿后，才标记 MySQL supported。

提交：`shard-33: MySQL persistent and recovery validation`

实现状态（2026-09-04）：MariaDB 端到端验收已完成。新增的 smoke 使用持久化 Aio bdev
和独立运行目录，初始化阶段通过 bootstrap wrapper 将 `mariadb-install-db` 的真实
`mariadbd` 放入单个 HFS 进程；服务阶段每次重新挂载同一镜像，显式关闭 native AIO，
使用 POSIX `fsync`。preload 已补齐 MariaDB 实际使用的 `open64/openat64`、fortify
open、`readlink`、`realpath`、64 位 stat ABI，以及绝对路径和 HFS 目录 fd 的 `*at`
路由；HFS owner/log 线程屏蔽 MariaDB 使用的终止信号，保留宿主进程的信号管理线程。

VM 验收通过：系统表初始化、数据库/表/索引创建、CRUD、事务提交与回滚、LONGTEXT、
临时表、正常重启、SIGKILL 后 committed/uncommitted 数据恢复，以及 4/16 并发客户端
负载。当前 VM 未安装 sysbench，因此并发验收使用 `mysqlslap` 4/16、每档 3 轮；完整
脚本输出 `MYSQL PERSISTENT RECOVERY OK`。这只完成 MariaDB/MySQL 兼容性验收，RocksDB
仍待 shard-34，性能汇总和两种数据库连续三轮验收仍待 shard-35，阶段总口径继续保持
“兼容性开发中”。

---

### shard-34：RocksDB 完整验收

目标：RocksDB 跨进程、并发和异常恢复稳定通过。

改动：

- 重写 `scripts/db/rocksdb_smoke.sh`，首个 fill 进程格式化，后续进程只 load 同一后端。
- 固定关闭 mmap，WAL 保持开启；分别测试普通 I/O和 direct I/O 配置。
- 覆盖 fillseq、readrandom、overwrite、readwhilewriting、flush/compact。
- 在持续写入时 SIGKILL，重启后执行 WAL recovery 与 `ldb checkconsistency`。
- 脚本解析完整退出码和结果，不能用 grep/head 的成功状态代替 db_bench 状态。

验收：

- fill 与 read 位于不同进程，读取命中率和 key 数正确。
- 8 个后台/业务线程的 readwhilewriting 运行 60 秒，无 CURRENT/MANIFEST/WAL 错误。
- SIGKILL 后数据库可重新打开，一致性检查通过。
- 连续完整执行 3 轮全绿后，才标记 RocksDB supported。

提交：`shard-34: RocksDB persistent and recovery validation`

实现状态（2026-09-05）：RocksDB 端到端验收已完成。重写后的 smoke 使用持久化 Aio bdev
和独立运行目录，首个 fill 进程格式化，后续进程跨进程重开同一镜像；WAL 保持开启，mmap
关闭，并通过 `use_direct_reads` 与 `use_direct_io_for_flush_and_compaction` 覆盖 direct
I/O 路径。preload 补齐 RocksDB 实际绑定的 GLIBC_2.2.5 `read`/`__read`、`fread`/
`fread_unlocked` 及 stdio 状态接口；`fclose` 先摘除 HFS fd slot，再关闭 libc 占位 fd，避免
数据库流关闭后留下不可复用的 fd 描述。

VM 上 `make -B app lib`、CMake Release 均通过。RocksDB 完整流程连续 3 轮通过，每轮包含
20,000 条 fillseq、跨进程 readrandom、direct I/O 重开、overwrite、flush/compact、8 线程
60 秒 readwhilewriting、SIGKILL 后 WAL recovery 以及 `ldb checkconsistency`，每轮输出
`ROCKSDB PERSISTENT RECOVERY OK`，总验收输出 `SHARD34_ACCEPTANCE_OK`。RocksDB 的阶段验收
已完成，但性能汇总和两种数据库的最终支持口径仍留给 shard-35；在 shard-35 完成前仍统一
称为“兼容性开发中”。

---

### shard-35：性能回归与阶段收口

目标：确认兼容性修复没有把 HFS 重新串行化，并形成可复现报告。

改动：

- 新增数据库 benchmark 汇总脚本，分别保存环境、配置、吞吐、p50/p99、CPU 和错误计数。
- Malloc bdev 仅用于和旧 HFS 微基准比较；持久 AIO/NVMe 数据只和相同后端、相同配置比较。
- 对 fd lock、metadata lock、DMA pool、SPDK queue 增加低成本统计，默认关闭详细日志。
- 更新 README、fio 基线、数据库支持矩阵和 `doc/bug.md`。
- 删除调试打点、重复 wrapper 和不再使用的兼容字段；公共函数保持单一职责。

性能门槛：

- Malloc fio 单线程 4K/128K 相对 `3908170` 不回退超过 10%。
- 4 线程数据库负载相对 1 线程应有可观察提升；若 owner thread 已饱和，必须用队列深度和 CPU 数据证明，而不是增加全局锁。
- 稳态对齐 I/O 不发生动态内存分配；ERROR/WARN 日志为 0。
- MySQL 与 RocksDB 验收脚本各连续 3 轮全绿。

提交：`shard-35: database support performance closeout`

## 6. 明确不在本阶段实现

- 多个独立进程同时直接挂载并访问同一个 Blobstore。
- 通用 fork/exec 共享 HFS 状态。
- 完整 mmap 文件映射语义。
- native libaio/io_uring 的异步完成队列仿真。
- 硬链接、符号链接、完整 ACL/xattr 和通用 Linux 文件系统兼容。

这些能力不能通过返回假成功来绕过。MySQL/RocksDB 若实际依赖其中某项，应停止对应 shard、记录证据，并新增独立设计，而不是继续扩大通用 wrapper。

## 7. 最终支持口径

阶段完成后的准确表述应是：

> HFS 支持单数据库进程在持久化 SPDK AIO/NVMe bdev 上运行 MySQL 和 RocksDB；支持多线程 POSIX 同步 I/O、WAL/事务恢复、正常重启和异常退出恢复。数据库客户端通过 socket/TCP 访问。暂不支持多个 HFS 进程共享挂载、文件 mmap 和 native libaio/io_uring。

在 shard-33、34、35 未全部通过前，统一表述为“兼容性开发中”，不能写“已支持”。
