# HFS 2.0 MySQL/InnoDB 侵入式适配审计（os_file seam）

时间：2026-09-24  
状态：审计完成（未实施；实施前必须先更新 `doc/design/hfs2.0.md` §3.2/§11，见 AGENTS.md）  
审计对象：MySQL 8.0.46（`mysql-server` 最新 8.0 tag）`storage/innobase`  
源码位置（VM）：`/mnt/hfs-data/mysql-audit-20260924/mysql-server-mysql-8.0.46`  
原始证据（VM）：`/mnt/hfs-data/mysql-audit-20260924/audit/`  
本审计只读，不修改 HFS 或 MySQL 代码。

## 1. 背景与目标

HFS 2.0 设计明确排除 `MySQL/MariaDB/InnoDB native backend`（`hfs2.0.md:119`），
并永久删除 `LD_PRELOAD`/POSIX wrapper/FUSE/mmap（`:111-112`）。MySQL 没有文件系统插件
接口，InnoDB/Aria/MyISAM 直接调用 POSIX + libaio，因此"真 InnoDB on HFS"只有一条路径：
**侵入式替换 InnoDB 的 `os_file` 文件层**。替代方案 MyRocks（RocksDB 存储引擎）复用现有
RocksDB adapter，但缺失 FK/FTS/空间索引/在线 DDL/Group Replication/Clone 等能力。

本审计目标：列出 `storage/innobase` 内所有绕过 `os_file_*` 的文件 I/O 点，产出精确的
改造清单，用于判断移植工作量与风险。

## 2. 审计方法与范围

- 范围：`storage/innobase/**/*.{cc,h}` 全量。
- 方法：include 扫描 + 多模式 grep，再人工过滤成员函数名、注释、监控计数等假阳性。
  匹配模式：`pread/pwrite`、`fsync/fdatasync`、`fallocate/posix_fallocate`、
  `ftruncate/truncate`、`fcntl/flock`、`mmap/munmap`、`io_submit/io_setup/io_getevents`、
  `sync_file_range`、`lseek`、`fstat/stat`、`open/openat/creat`、`opendir/readdir/mkdir/
  rmdir/unlink/rename`、`access`、`dup`、`my_*` 文件 API、裸 `read/write`。
- 原始输出：`audit/includes.txt`、`audit/<pattern>.raw`、`audit/read_write.filtered`、
  `audit/audit_summary.txt`。

## 3. 总体结论

1. **I/O 高度收敛于 `os/os0file.cc`（7803 行）**：`open/read/write/pread/pwrite/fsync/
   fdatasync/fallocate/ftruncate/mkdir/readdir/rmdir/unlink/rename/fcntl(F_SETLK/F_SETFL)/
   lseek` 与 libaio（`io_submit/io_setup/io_getevents`）全部集中在该文件；`os_file_*`
   调用点共 941 处。
2. **无文件 mmap**：mmap 只出现在 `include/detail/ut/large_page_alloc-*.h` 与
   `page_alloc.h`（内存大页分配），与文件 I/O 无关。
3. **`os0file.cc` 之外没有裸 `open/read/write` 系统调用**：grep 到的 107 个 `open`、
   69 个 `read/write` 过滤结果全部是 C++ 方法名或注释。
4. **真实绕过点仅 7 处**（§4）：5 处属于数据路径（表空间分配 ×2、`fstat`、
   online DDL 日志、buffer pool dump），另 2 处为状态文件删除与 advisory 提示。
5. `ddl::pread/pwrite` 不是绕过点：实现已走
   `os_file_read_no_error_handling_int_fd`/`os_file_write_int_fd`（`ddl/ddl0ddl.cc:92,111`），
   仅函数体内的 `posix_fadvise` 需要 no-op。

## 4. 绕过 `os_file_*` 的真实文件 I/O 点

| # | 位置 | 调用 | 用途 | 处理 |
|---|---|---|---|---|
| 1 | `fil/fil0fil.cc:5496` | `posix_fallocate(file.m_file, 0, sz)` | 创建临时表空间 | 走新 `os_file_allocate` → `File::allocate` |
| 2 | `fil/fil0fil.cc:6680` | `posix_fallocate(file->handle.m_file, node_start, len)` | 表空间扩展 | 同上；失败已有 `os_file_set_size` 回退 |
| 3 | `fsp/fsp0file.cc:181` | `fstat(m_handle.m_file, &m_file_info)` | `Datafile::init_file_info` | 改 `os_file_get_size`/`os_file_get_status`，合成 `struct stat` |
| 4 | `row/row0log.cc:2811,3565` | `ftruncate(index->online_log->file.get(), 0)` | online DDL 日志回收 | 改 `os_file_truncate`/`File::truncate` |
| 5 | `buf/buf0dump.cc:249-555` | `fopen/fprintf/fclose` + `unlink`/`rename` | `ib_buffer_pool` dump/load（datadir 内，`srv_buf_dump_filename`） | 改 `os_file_*`；或一期关 `innodb_buffer_pool_dump_at_shutdown/load_at_startup` |
| 6 | `srv/srv0start.cc:3055` | `unlink(srv_monitor_file_name)` | 关机删 `innodb_status` 文件 | 走 `os_file_delete` 或留宿主路径 |
| 7 | `ddl/ddl0ddl.cc:105,123` | `posix_fadvise(fd, ...)` | DDL 合并文件缓存提示 | advisory，no-op（同文件 :41-43 已有平台宏先例） |

**确认无需迁移（宿主语义，应留在 ext4）**：

- `handler/ha_innodb.cc:2524 dup()` + `:2538 my_close`：`innobase_mysql_tmpfile` 使用
  `mysql_tmpfile`（`tmpdir` 宿主路径）。
- `handler/ha_innodb.cc:1038-1053 my_access/my_stat`：`innodb_alter_tmp_dir` 选项校验。
- `clone/clone0api.cc:83 std::rename`、`:2210 my_mkdir`：clone 状态文件/schema 目录，
  一期直接禁用 clone。

**已排除的假阳性**：`sync/sync0arr.cc:880`、`include/srv0srv.h`、`srv/srv0mon.cc`、
`log/log0write.cc`、`log/log0chkp.cc`、`include/log0types.h` 的 fsync 命中均为注释、
`Log_file_handle::fsync()` 成员声明/调用或监控计数；`read_write.filtered` 全部为成员
函数与注释；`access`、`dup`（除上表）均为注释/宿主路径。

## 5. os_file seam 与 HFS 映射

### 5.1 seam 函数分组（均位于 `os/os0file.cc`）

| 组 | 函数 | HFS 映射 |
|---|---|---|
| 生命周期 | `os_file_create_func`、`os_file_create_simple_no_error_handling_func`、`os_file_create_tmpfile`、`os_file_close_func`、`os_file_close_no_error_handling` | `Client::open`/`close` + handle 表 |
| 数据 | `os_file_read_func`、`os_file_write_func`、`os_file_pread`、`os_file_pwrite`、`os_file_read_page`、`os_file_write_page`、`os_file_read_first_page`、`os_file_read_string`、`os_file_write_zeros`、`SyncFileIO::execute`（`:2045`） | `File::read/write`（coroutine wait）；`SyncFileIO::execute` 是模拟 AIO 的同步入口 |
| 同步 | `os_file_fsync_posix`（`:2822`，fsync/fdatasync）、`os_file_flush_func`、`os_parent_dir_fsync_posix`（`:2887`，open+fsync+close 目录） | `File::sync`（目录同步 = namespace durable epoch） |
| 尺寸 | `os_file_get_size`（`:3439`）、`os_file_set_size`（`:5375`）、`os_file_set_size_fast`（`:5343`，`fallocate ZERO_RANGE` 在 `:5352`）、`os_file_truncate`（`:3561`）、`os_file_set_eof`（`:3579`） | `File::stat`、`File::allocate` + `File::truncate` |
| 空间 | `os_file_punch_hole_posix`（`:2063`）、`os_get_free_space`（`:5778`，statvfs） | punch hole 不支持（fail closed）；free space → `Engine::freeDataBytes` |
| 目录/路径 | `os_file_create_directory`（`:3045`，mkdir 调用 `:3046`）、`os_file_scan_directory`（opendir/readdir 调用 `:3073`）、`os_file_delete_dir`（rmdir `:3091`）、`os_file_delete_func`、`os_file_delete_if_exists_func`、`os_file_rename_func`（rename 调用 `:3405`）、`os_file_create_subdirs_if_needed`、`os_file_get_parent_dir` | `Client::open(Create\|Directory)` + mkdir-p、`Client::getChildren`、`Client::remove`、`Client::rename` |
| 状态 | `os_file_status_posix`、`os_file_exists`、`os_file_get_status`、`os_file_can_delete` | **需新增 path `stat`**；exists 用 open 探测 |
| 锁/拷贝 | `os_file_lock`（`:1502`，`fcntl F_SETLK`）、`os_file_copy_func` | 宿主锁文件（RocksDB adapter 先例）或 HFS 进程锁；copy = 读写循环 |
| O_DIRECT | `os_file_set_nocache`（`:5296`，`fcntl F_SETFL O_DIRECT`） | no-op（HFS 天然 direct） |
| AIO | `AIO::linux_dispatch`（`:2543`）、`AIO::linux_create_io_ctx`（`:2572`）、`os_aio_linux_handler`（`:2524`）、`os_aio_simulated_handler`（`:7394`）、`os_aio_func`（`:6926`） | v1 `innodb_use_native_aio=OFF`；模拟 AIO 收敛到 `SyncFileIO::execute` |

### 5.2 关键语义差异

- `os_file_set_size`/`set_size_fast`：HFS `File::allocate` 是"预留 + 清零"，强于
  `posix_fallocate`；`File::truncate` 负责收缩。
- `os_file_fsync_posix`：InnoDB 每次 commit fsync redo；HFS `File::sync` 是
  namespace 级 durable epoch 且 `sync==datasync`（`src/core/client.cpp:1137`），
  功能更强但**没有 data-only/per-file barrier**，性能是头号风险（需组提交合并验证）。
- `os_parent_dir_fsync_posix`：rename/unlink 后目录 fsync → HFS `File::sync`（目录句柄）。
- `os_file_get_status`：InnoDB 需要 `st_size/st_mode`；HFS 无权限模型，需合成 mode。
- `os_file_punch_hole`：用于页压缩（`FALLOC_FL_PUNCH_HOLE|KEEP_SIZE`），HFS 不支持，
  必须禁用压缩并让该调用 fail closed。
- `os_file_lock`：InnoDB 单实例锁；HFS 已有进程级独占，可用宿主锁文件实现。

## 6. 改造清单

### 6.1 新增 InnoDB os_file backend（HFS VFS）

1. **handle 替换 fd**：`os_file_t`/`pfs_os_file_t` 由 fd 改为 handle 索引，映射到
   `hfs::File`；需要覆盖 `OS_FILE_CLOSED`、PFS 包装与 `os_fd_t`（DDL 临时文件）。
2. **路径路由**：`srv_data_home`/datadir/Fil_path 前缀 → HFS namespace；`tmpdir`、
   错误日志、binlog、socket、`innodb_alter_tmp_dir` 保持 POSIX。未命中 HFS 前缀时
   走原生实现，保证混合运行。
3. **同步门面**：InnoDB 几十个后台线程（page cleaner、log writer、AIO handler、purge、
   DDL）调用同步 `os_file_read/write`；每线程一个 `hfs::Client`（复用 RocksDB
   worker-direct registry 生命周期）或共享 dispatcher 阻塞门面。
4. **实现范围**：§5.1 表中除 AIO 外的全部函数；`SyncFileIO::execute` 改为 HFS
   read/write。
5. **文件布局建议**（MySQL fork 内）：`storage/innobase/os/hfs_vfs.{h,cc}` 承载
   backend 与 handle 表，`os0file.cc` 仅保留路由与错误映射改动，降低 rebase 冲突。

### 6.2 绕过点修复

按 §4 表 #1–#7 逐条替换；#5 可一期用参数规避，但必须在设计文档记录为临时边界。

### 6.3 AIO

- v1：`innodb_use_native_aio=OFF`，模拟 AIO 线程走同步 HFS 路径；验证
  `os_aio_simulated_handler` 的全部 I/O 都经过 seam。
- 可选二期：把 `AIO::linux_dispatch`/`os_aio_linux_handler` 映射到
  `Client::submitRead/Write`，保留 native AIO 语义（高成本，非目标）。

### 6.4 服务器层边界（不在本次审计范围，但必须配置）

- `--log-bin`、`--log-error`、`--slow-query-log-file`、`--tmpdir`、socket、pid 用绝对
  宿主路径；`--initialize`/bootstrap 必须与运行期串行（HFS 单进程独占 bdev）。
- 系统表/数据字典在 MySQL 8.0 已是 InnoDB，随 datadir 进 HFS；MyISAM/Aria 表禁止创建。

### 6.5 禁用项

native AIO、punch hole/页压缩、clone、MyISAM/Aria、`--daemonize`（fork →
`ForkedProcess`）、`--innodb-directories` 指向宿主、多实例共享 datadir。

### 6.6 HFS 侧 API 缺口

| 缺口 | 用途 |
|---|---|
| `Client::stat(path)`（path 级 stat） | `os_file_get_status/exists` |
| 递归 mkdir-p | datadir/#innodb_redo/#innodb_temp 建目录 |
| statvfs 等价 | `os_get_free_space`（可用 `Engine::freeDataBytes`） |
| 锁契约（或明确宿主锁文件） | `os_file_lock` |
| data-only `datasync` | redo per-commit fsync 性能 |
| punch hole（可选） | 页压缩；一期不做 |

## 7. 与 RocksDB frontend 的调用流程差异

| 维度 | RocksDB | MySQL/InnoDB |
|---|---|---|
| 抽象层 | 正式 `Env`/`FileSystem` C++ 接口，HFS"替换接口" | 无 FS 接口，直调 POSIX，HFS"替换实现"（侵入） |
| 调用形态 | 对象化、异步 `ReadAsync`+`io_handle`+callback、`Poll`/`AbortIO` | 同步 `os_file_read/write` + fd；异步另走 libaio |
| 调用者 | worker + 少量后台线程 | page cleaner、log writer、AIO handler、purge、DDL 等几十个后台线程 |
| 写路径 | adapter 可缓冲（≤1 MiB），`Flush` no-op，`Sync` 才 durable | 无缓冲直写；双写缓冲、redo 独立线程组、`O_DIRECT` |
| 持久化 | `Sync/Fsync` → `File::sync`，同 epoch 合并 | `os_file_fsync`（fsync/fdatasync）每 commit + rename/unlink 后目录 fsync |
| 目录操作 | `CreateDir/GetChildren/DeleteDir/RenameFile/LinkFile/LockFile` | `os_file_create_directory/scan_directory/delete/rename` + 少量绕过 |
| 生命周期 | 进程内单 Env，已有 worker-direct registry | 单进程多线程但启动/停机顺序复杂，需同类 registry |

调用链对比：

```text
RocksDB:  DB::Get → FileSystem::NewRandomAccessFile → ReadAsync(adapter)
          → Client::submitRead → ShardRing → data reactor → SPDK bdev
          → completion → callback → worker

InnoDB:   buf_read_page → fil_io → os_aio
          → native AIO: io_submit → os_aio_linux_handler
          → simulated AIO: os_aio_simulated_handler → SyncFileIO::execute
          → fd pread/pwrite（seam）
          commit: log_write_up_to → os_file_fsync（seam）
```

## 8. 风险与验证

### 8.1 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| `File::sync` 无 data-only barrier | redo per-commit 延迟/吞吐 | 先测组提交；必要时新增 HFS data-only sync（需设计变更） |
| `punch_hole` 缺失 | 页压缩不可用 | 禁用压缩；fail closed |
| `struct stat` 合成字段 | 第三方代码依赖 st_dev/st_ino | 只保证 size/mode/mtime 有效 |
| 绕过点遗漏 | datadir 文件落宿主或失败 | 本审计清单 + 运行期路径审计（strace 校验） |
| fork/初始化顺序 | bootstrap 失败 | install/initialize 与 server 串行；禁 `--daemonize` |
| 上游 rebase/维护 | 安全补丁成本 | 限定改动在 seam + 7 个绕过点；记录 patch 清单 |
| GPLv2 fork | 对外分发须开源 | 内部使用可接受；对外需合规评审 |

### 8.2 验证矩阵

| 层级 | 项目 | 通过标准 |
|---|---|---|
| 单元 | seam 函数级测试（handle、路径路由、错误映射） | 全过 |
| 功能 | `mysqld --initialize`、启动/停机、SQL CRUD、DDL（含 online） | 无错误 |
| 正确性 | kill -9 崩溃矩阵、双写 torn page、redo 恢复、`hfsctl check` | 状态守恒 |
| 性能 | mysqlslap 读/写、`innodb_flush_log_at_trx_commit=1` 组提交、sysbench | 对标 HFS 1.x 参考（读≈native、写 40–62%） |
| 回归 | FIO/RocksDB frontend 不受影响（HFS core 无改动时） | 不回退 |
| 边界 | punch hole/压缩/clone/MyISAM/daemonize 明确拒绝 | fail closed |

### 8.3 停止条件

沿用 `hfs2.0.md` §17.3；追加：崩溃恢复失败、双写/redo 不一致、datadir 文件落到宿主、
`os_file_fsync` 语义无法保证。

## 9. 证据与复现

- VM 源码：`/mnt/hfs-data/mysql-audit-20260924/mysql-server-mysql-8.0.46`
  （tarball 462 MB，来自 `codeload.github.com/mysql/mysql-server/tar.gz/refs/tags/mysql-8.0.46`）。
- VM 原始输出：`/mnt/hfs-data/mysql-audit-20260924/audit/`
  （`includes.txt`、`pread_pwrite.raw`、`fsync.raw`、`fallocate.raw`、`ftruncate.raw`、
  `dirent.raw`、`libaio.raw`、`open.raw`、`read_write.filtered`、`audit_summary.txt`）。
- Windows 临时脚本：`E:\Code\test_script\mysql_innodb_syscall_audit.sh`、
  `mysql_innodb_audit_probe_20260924.py`、`mysql_innodb_audit_fetch_20260924.py`、
  `vm_run_20260924.py`、`vm_run_file_20260924.py`、`vm_upload_20260924.py`。

## 10. 下一步

1. 决策：MyRocks（A）还是 InnoDB seam 移植（B）；本审计只服务 B。
2. 若选 B：先更新 `hfs2.0.md` §3.2/§11（放开 MySQL/InnoDB 范围、定义新 frontend
   契约与禁用项），再实现。
3. 第一步实现建议：先做 seam + 绕过点的最小闭环（单实例 `--initialize` + 启停 +
   kill -9 恢复），再补 AIO/性能。
4. 完成后回填本文件的实现结果与偏差记录。
