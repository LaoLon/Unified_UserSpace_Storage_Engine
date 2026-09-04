# Workspace Instructions

## Codex && opencode 共用项目记忆

- `doc/bug.md` 是当前 Bug 记录和已知限制的优先参考；发现或修复 Bug 时立即维护它。
- `doc/design/fio_baseline_notes.md` 保存已完成的 FIO 基线数据和测试口径。
- `doc/shard/mysql_rocksdb_stage_development.md` 是 shard-26 起的当前执行计划，优先于旧分片文档中尚未实施的 shard-26 及以后内容。
- `doc/项目总结.md` 是早期项目总结，`doc/shard/libhfs_shard_development.md` 是原始分片计划；使用前必须与 Git 历史和当前源码交叉核对，不能把其中的旧目录结构、旧容量或“待实现”描述直接当作现状。
- 需要了解当前进展时，先查看 `git log --stat` 和 `git status`，再以当前源码、测试脚本和最新文档为准。
- shard-31 实现提交为 `9a5de90`，已在 `main`：完成请求级 I/O、固定 DMA pool、对齐直写/非对齐首尾 RMW、并发 positioned I/O、readv/writev、KEEP_SIZE/PUNCH_HOLE，以及 owner PID fork 防护；后续继续按 `doc/shard/mysql_rocksdb_stage_development.md` 推进 MySQL/RocksDB 兼容性。
- shard-28 本轮已新增 `src/hfs_fd.[ch]`：fd slot 使用短 rwlock，`HfsOpenFile` 用引用计数、共享 offset 和 I/O 锁承载 open-file description；dup/dup2/dup3/F_DUPFD 共享描述，close 先摘 slot 后释放 placeholder；unlink/rename 覆盖通过 orphan 延迟回收 blob，旧 fd 仍可 read/pread/lseek/fstat/ftruncate。
- shard-28 验证口径：VM 上 `make -B app lib`、CMake Release、preload smoke、persistent smoke、fd lifecycle smoke，以及 16 线程 10 秒 runtime stress 全部通过。专项测试覆盖 fd 复用、共享 offset、`F_DUPFD_CLOEXEC`、dup2、无参 `F_GETFL`、unlink-open、rename 覆盖/同路径 rename 和并发 positioned I/O。独立 TSAN 单测尚未建立；在 shard-33/34/35 完成前不得称 MySQL/RocksDB 已支持。
- shard-28 排查经验：构建共享库必须强制重链，版本脚本只能给 `fcntl64` 保留 `GLIBC_2.28` 版本，不能把所有 wrapper 放到自定义版本；rename 压缩 inode 数组后必须按 id 重新解析指针；同路径 rename 不得进入目标 blob 回收。
- shard-29 已完成：`hfs_lib.c` 使用元数据读写锁和 append 串行锁，读路径返回 inode 快照，目录句柄使用独立游标锁；元数据头保存 magic/version/`nextInodeId`，兼容旧 `[count][inodes]` 布局并禁止 inode ID 复用；preload 增加受锁保护的 HFS cwd、`fchdir`、`fdopendir`、目录注册表锁、相对 `openat`、`O_EXCL`/`O_APPEND`/`O_TRUNC`/`O_DIRECTORY` 语义。新增 `scripts/db/metadata_path_concurrency_smoke.sh`。VM 上 Make/CMake、元数据路径并发、fd 生命周期、持久化 smoke 和 16 线程 10 秒 stress 全部通过；当前仍不得称 MySQL/RocksDB 已支持，真实数据库验收留在 shard-33～35。
- shard-30 已完成核心可恢复元数据短事务：v3 metadata header 增加 generation/checksum，固定大小 intent record 保存最多 4 个 inode after-image；create/unlink/rename/size 按 intent → stable image → clear intent 顺序提交，mount 校验并幂等重放，兼容 v2/legacy 布局并在首次修改时迁移。`fsync` 先同步 blob 数据/metadata，再通过独立可写 bdev descriptor 执行 `spdk_bdev_flush`，最后提交 dirty inode size/mtime；source-last rename 覆盖恢复已加入 `scripts/db/metadata_transaction_smoke.sh`。VM 上 Make、CMake Release、三个崩溃边界、路径并发和持久化 smoke 全部通过。stable image 目前仍整 blob 写入，脏页/写放大优化留给 shard-31；MySQL/RocksDB 真实数据库验收仍未完成。
- shard-31 已完成：`src/hfs_io.[ch]` 用 16 个固定 DMA buffer 承载请求级读写，移除 `HfsFile` 共享 scratch 状态；对齐 I/O 走直接 chunk，非对齐首尾只做 unit RMW，pread/pwrite 与 readv/writev 保持调用方 buffer 语义。`PUNCH_HOLE` 在 unmap 后补 zeroes/池化写零，修复后端 unmap 成功但不保证读零的问题；运行时记录 owner PID，fork child 不再进入 HFS 路由或 SPDK queue。新增 `scripts/db/io_fast_path_smoke.sh` 覆盖向量 I/O、稀疏读、KEEP_SIZE、对齐/非对齐 punch、8 路 positioned write 和 fork child 防护。VM 上 Make、CMake Release、元数据事务/路径并发、fd 生命周期、持久化 bdev 和 fio 全部通过；fio 结果已记录在阶段文档与 `doc/design/fio_baseline_notes.md`，MySQL/RocksDB 真实数据库验收仍未完成。
- shard-32 已完成核心 POSIX 契约收口：元数据升级 v4 并兼容 v3/v2/legacy，inode 保存 mode/uid/gid 与 atime/mtime/ctime；preload 补齐目录 fd 的 `*at` 路由、statx/statfs/statvfs（含 64 位 ABI）、chmod/chown/utimensat/futimens、fadvise，并对 HFS fd 的 mmap 返回 ENODEV；修复 open mode 传递、AT_FDCWD 路径物化和 Python statvfs64 符号。新增 `scripts/db/posix_contract_test.sh`，VM 上契约连续 10 次、Make/CMake、既有 smoke、持久化、16 线程 10 秒压力和 fio 全部通过；strace 审计确认没有 `/hfs` 数据调用落入内核 `/dev/null` 路径。旧 MySQL/RocksDB smoke 仍受进程私有 Malloc0/跨进程生命周期限制，真实数据库端到端验收留在 shard-33～35，不得称已支持。
- shard-33 已完成 MariaDB/MySQL 端到端验收：`scripts/db/mysql_smoke.sh` 使用持久化 Aio bdev、bootstrap wrapper 和隔离 process group，覆盖系统表初始化、CRUD/DDL、事务提交回滚、LONGTEXT、临时表、正常重启、SIGKILL 恢复和 4/16 并发客户端负载；VM 输出 `MYSQL PERSISTENT RECOVERY OK`。preload 补齐 MariaDB 使用的 64 位/fortify open、readlink/realpath、绝对路径和目录 fd 路由；HFS owner/log 线程屏蔽宿主终止信号，避免数据库信号线程被 SPDK 后台线程抢收。当前 sysbench 未安装，使用 mysqlslap fallback；RocksDB 仍待 shard-34，阶段整体仍不得称 MySQL/RocksDB 已支持。
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

1. 使用 `E:\Code\ssh\pool.py`、`E:\Code\ssh\sync_to_vm.py` 连接虚拟机和上传本地文件
2. 虚拟机环境信息见 `E:\Code\workspace.yaml`
3. 每次先编辑本地文件，然后同步到虚拟机
4. 每次回复先称呼用户为 **Huizareth**
5. 不用管本地的环境依赖，只管理虚拟机的就行
6. 在 `doc/bug.md` 维护 bug 记录：每次发现 bug 必须记录（现象、排查过程、根因、修复），bug 分级：致命、严重、一般；解决后立即写入该文件（含排查过程细节，便于复盘）
7. 写代码必须写英文注释：简洁即可（一句话说明函数/结构体/关键逻辑的用途），复杂函数可适当增多（分步骤解释）；注释用 `/* */` 风格，放在函数/结构体声明上方
