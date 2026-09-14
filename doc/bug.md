# HFS 项目 Bug 记录

Bug 等级：**致命 > 严重 > 一般**。

每条记录：现象 → 排查过程 → 根因 → 修复。排查过程中的思路和关键证据一并记录，方便复盘。

---

## 状态汇总（截至 2026-09-09）

### 已修复

1–5、8–10、10b、11–43、45–47、51–52、57、60–62、67–72、76、78。包括已由后续 shard 修复的历史记录；条目中的原始“遗留”描述保留用于复盘，当前状态以本汇总和条目状态字段为准。

### 未修复

6、7、44、48–50。

- 6：SPDK io channel 释放的 `wrong_thread` 日志仍存在，当前仅接受该 cosmetic 问题。
- 7：fork/exec 子进程共享 HFS 运行时仍是架构限制。
- 44：MariaDB 正常退出时 `ib_buffer_pool.incomplete` 创建失败，待进一步定位路径语义和 teardown 时序。
- 48：多条 SPDK 异步回调只唤醒 waiter、不传播 bserrno，失败可能被报告为成功。
- 49：元数据事务失败不回滚内存 after-image，创建失败路径还可能遗留孤儿 blob/占位 fd。
- 50：fsync 在 inode 元数据事务之前执行设备 flush，事务写入后没有持久化屏障，掉电语义不完整。

---

## 致命级

### 1. spdk_env_init 单线程 futex 自锁挂起（init 死锁）

- **状态**：已修复
- **时间**：2026-09-01（shard-15 验证后，shard-16 开发期间）
- **等级**：致命
- **现象**：`spdk-hfs` 主程序与所有 LD_PRELOAD 进程（touch/cat/sh）在 `spdk_env_init` 处挂起。strace 显示单线程 `futex(0x..., FUTEX_WAIT_PRIVATE, 2, NULL)`（锁值 2=被持有，自锁）。重启 VM 无效，`spdk_lspci`（同样 DPDK 库）正常。

#### 排查过程（约 2 小时，多轮排除）

1. **怀疑环境残留**：清理 `/var/tmp/spdk*`、`/run/dpdk`、`/var/run/dpdk`、`/dev/hugepages/*`、残留进程（`pkill -9` tar/log/spdk-hfs）→ 仍挂。
2. **怀疑 DPDK 共享状态**：`SPDK_FILE_PREFIX=hfs_t1` 绕过损坏的共享配置 → 仍挂。
3. **怀疑代码回归**：`git stash` 回到已验证的 shard-15 提交 → 仍挂。一度误判为"系统环境问题"。
4. **用户重启 VM** → 仍挂。**排除环境残留**。
5. **最小复现**：写 `/tmp/env_test.c`（仅 `spdk_env_opts_init + spdk_env_init + fini`，链接 libspdk_env_dpdk + DPDK 全库）→ **成功（T=0）**！证明 `spdk_env_init` 本身正常，问题在 spdk-hfs 二进制的进程上下文中。
6. **拦截 pthread_mutex_lock 打栈**（`LD_PRELOAD=/tmp/libml.so`，检测 `*(int*)mutex != 0`）→ 捕获：`close 拦截器 → pthread_mutex_lock`，锁 word=1（同一线程已持有）→ **自锁确认**。
7. **读代码定位**：`hfs_lib_init`（持 `g_libLock`）→ `hfs_log_init` → `hfsLogTrimTail()` → `close(fd)` → **close 拦截器** → `hfs_is_hfs_fd()` → **再次 lock `g_libLock`** → 单线程自锁。

#### 根因

`hfs_is_hfs_fd` 是 close 拦截器的快速路径，内部却加 `g_libLock`。当 `hfs_lib_init` 已持有该锁、期间任何 close（如 `hfsLogTrimTail` 关闭日志文件）被拦截时，同一线程重复加锁 → futex 自锁。

**为什么 15:01 前不挂**：日志文件 `/var/log/hfs/run/hfs_base_spdk.log` 首次运行前不存在 → `hfsLogTrimTail` 的 `open()` 失败提前返回 → 无 close。文件一旦创建，此后每次运行必挂——**与 VM 重启无关，解释了所有"重启无效"的现象**。

#### 修复

`hfs_is_hfs_fd` 去掉锁（fd 表固定数组、单写者、快速路径），见 `src/hfs_lib.c`。同时补充注释说明原因。

#### 经验

- **拦截器内部的快速路径（is_xxx 检查）绝不能加锁**：它可能在库初始化持锁期间被调用。
- 排查"挂起"类问题：先做最小复现（剥离业务代码）→ 缩小到进程上下文 → 用 LD_PRELOAD 拦截器打印调用栈（无 gdb 环境）。
- 状态相关 bug（首次 OK、此后必挂）优先怀疑"文件/资源一旦创建就改变路径"的代码。

---

### 2. 双 umount 崩溃（已卸载 blobstore 再次 unload）

- **状态**：已修复
- **时间**：2026-09-01（死锁修复后首次完整运行）
- **等级**：致命
- **现象**：`spdk-hfs` 主程序 umount 时 `spdk_put_io_channel called from wrong thread` 报错后 segfault（`ip == segfault 地址`，跳转到含 `_HFS_INIT_DONE=1` 字符串的堆区——函数指针损坏）。
- **根因**：`main` 结尾调 `hfsUmount(fs)`，随后 `hfs_lib_fini()`（dtor 也调用）再调 `hfsUmount(g_fs)`——**同一 fs 双 umount**：第二次对已卸载的 blobstore 再 `spdk_bs_unload` → 内部状态已销毁 → 跳坏指针。偶发（app20=0 因内存未重用幸运通过，app21=139）。
- **修复**：`hfsSpdkBsUnloadCb` 回调中置 `fs->blobstore = NULL`；`hfsDoUmount` 幂等（blobstore 为空时直接完成）。见 `src/hfs_mount.c`。
- **经验**：异步资源释放要"回调里置空句柄 + 幂等"，防止重复释放。

---

## 严重级

### 3. hfs fd 与内核真实 fd 编号冲突（数据写丢/读错）

- **状态**：已修复
- **时间**：2026-09-01（shard-16 验证）
- **等级**：严重
- **现象**：python 场景 `os.write(fd, b'hello world')` 返回 11，随后 `os.read(fd, 100)` 返回 `b''`；`os.fstat` 返回 15152（无关数据）。主程序（无并发 fd）却一切正常。
- **排查**：strace 确认 write/read 都走了拦截（无对应 syscall）；write 的 DBG 显示 `size=11` 正常更新；read 的 DBG 不打印（EOF 分支）。**关键线索**：`SIZE_AFTER_OPEN: 15152`——fstat 透传内核返回了 fd 3 上**别的真实文件**的大小。
- **根因**：hfs fd 表占用编号 3，但**内核 fd 3 是空闲的**。hfs 打开后，进程内其他 real open（python 内部文件）拿到内核 fd 3 → 与 hfs fd 3 撞号 → 后续 close(3) 被拦截成 hfs close（关错句柄）、read(3) 透传读到别的文件。malloc bdev 项目里主程序 fd 少不冲突，python 等复杂进程必现。
- **修复**：`hfs_lib_open` 里用 `open("/dev/null", O_RDONLY)` **占坑真实内核 fd**，hfs fd 编号 == 内核占位 fd；`hfs_lib_close` 清表后释放占位。冲突消除（python FD: 3 → 191）。
- **经验**：LD_PRELOAD 拦截层的"假 fd"必须与内核 fd 一一对应（占坑），否则多 fd 进程必崩。

---

## 一般级

### 4. lseek64/pread64/pwrite64 未拦截（python 等 64 位程序 lseek 失效）

- **状态**：已修复
- **时间**：2026-09-01
- **等级**：一般（数据通路断裂但无崩溃）
- **现象**：python `os.lseek(fd, 6, SEEK_SET)` 后 `os.read` 返回空。strace 显示 `lseek(191, 0, SEEK_SET)` 系统调用（走了 real，落到 /dev/null 占位）。
- **根因**：64 位构建的程序用 `lseek64` 符号（最终也是 lseek 系统调用），只拦截了 `lseek`。
- **修复**：补 `lseek64/pread64/pwrite64` 拦截，转发到同一 hfs_lib 函数。同理 open64/openat64（dash/busybox 用）。
- **经验**：LD_PRELOAD 拦截要同时覆盖 64 位变体（open64/openat64/lseek64/pread64/pwrite64）。

### 5. dup/dup2/dup3 未拦截（sh 重定向数据断裂 + 引用计数缺失） 


- **时间**：2026-09-01
- **等级**：一般（sh 的 `>` 重定向失效；曾升级为严重——dup 复制后 close 旧 fd 释放共享 file，导致已关闭 blob 上写 I/O → abort `spinlock destroyed`）
- **状态**：已修复
- **现象**：`sh -c 'echo hello > /hfs/t1'` 报 `I/O error`；或（补 dup 拦截后）`spdk_for_each_channel: could not find io_device` + abort。
- **根因**：
  1. dash 重定向用 `dup2(3, 1)`：未拦截时 fd 1 内核侧变成 /dev/null，echo 写进 /dev/null（数据断裂）；write 后 hfs 表里 fd 1 无记录 → 拦截层行为不一致。
  2. dup 拦截后：fdTable[1] 与 fdTable[3] 共享同一 HfsFile，close(3) 释放 file（blob 已关），fd 1 仍指向 → 写已关闭 blob → SPDK abort。
- **修复**：拦截 dup/dup2/dup3（`real_dup` 占位内核 + `hfs_lib_fd_copy` 复制表项）；**HfsFile 加 refCount**：open=1、fd_copy++、close--（归零才 hfsClose+free）。见 `src/hfs_lib.c`、`src/hfs_preload.c`。
- **经验**：任何 fd 复制接口（dup 族）都要同步 hfs 表；共享句柄必须引用计数。

### 6. spdk_bs_free_io_channel wrong_thread 警告（cosmetic）

- **状态**：未修复
- **时间**：2026-09-01
- **等级**：一般（Release/NDEBUG 下仅打印 ERRLOG，assert 不生效；功能正常）
- **现象**：umount 时 `spdk_put_io_channel(ch) called from wrong thread global:1 (should be :...<乱码>)`。
- **根因**：malloc bdev 的 io channel 归属 SPDK 内部线程（bdev 创建线程），我们在 globalThread 上下文释放；`spdk_bs_free_io_channel` 的线程检查失败（Release 下 assert 被 NDEBUG 禁用，仅日志）。**注意：不能去掉 free**——`spdk_bs_unload` 需要 channel 释放完成，否则 unload 永远等待（见此前"去掉 free 后所有进程 dtor 挂起"的教训）。
- **处理**：保留 free（功能必须），警告接受为已知问题；channel 释放线程问题留待接入真实 bdev/事件框架时解决。

---

### 7. fork+exec 子进程无法访问 HFS（架构限制，一般级）

- **状态**：未修复
- **时间**：2026-09-02（shard-17 验收时暴露）
- **等级**：一般（已知架构限制，非缺陷）
- **现象**：`LD_PRELOAD=... sh -c 'mkdir -p /hfs/d; dd if=/dev/zero of=/hfs/d/f ...; rm /hfs/d/f; rmdir /hfs/d'` 中 mkdir/dd/rm/rmdir 全部静默失败（exit 0 是 echo 掩盖）。同进程 python 场景（mkdir→open→write→fsync→rename→unlink→rmdir）全链路通过。
- **根因**：dash 的 mkdir/dd/rm/rmdir 是外部命令 → fork+exec 子进程 → 子进程继承 `_HFS_INIT_DONE` 走 pass-through（透传内核）→ 内核里 /hfs 不存在 → 失败。而 shard-16 的 `echo > /hfs/t1` 成功是因为 echo 是**内建**（sh 主进程执行重定向）。malloc bdev 是进程私有内存，跨进程本就无法共享数据；即使子进程重新 init 也各自独立。
- **处理**：shard-17 拦截实现正确，验证改用单进程场景；跨进程一致性需换可共享 bdev 或 SPDK primary/secondary 多进程支持（列入后续规划，FIO/数据库冒烟将使用单进程或专用 runner）。
- **经验**：验收 shell 命令要注意内建 vs 外部命令（fork+exec）；LD_PRELOAD 文件系统在"进程私有存储"架构下天然只对单进程场景可靠。

### 8. fio 相关三个问题（一般级，shard-22 修复）

- **状态**：已修复
- **时间**：2026-09-02（shard-21/22）
- **等级**：一般（fio 配置/环境层面）
- **问题 A：libaio io_submit 对 HFS fd 报 EBADF**
  - 现象：fio libaio 引擎 `io commit error=Bad file descriptor`。
  - 根因：io_setup/io_submit 走系统调用，绕过 read/write 拦截；iocb 的 fd 是内核 /dev/null 占位 fd，内核 AIO 不支持 → EBADF。
  - 处理：fio 改用 psync 引擎（pread/pwrite 走拦截，同步执行）。libaio 的 io_submit 拦截（异步转同步）留作后续增强。
- **问题 B：malloc bdev 容量不足（ENOSPC）**
  - 现象：blob resize 报 `Unfreeze failed, rc=-28`（ENOSPC），fio 64MB 文件写不进去。
  - 根因：config/hfs.json bdev 仅 32768×512=16MB。
  - 处理：num_blocks 32768 → 524288（256MB）。
- **问题 C：create_on_open 读空文件报 EIO（full resid）**
  - 现象：fio randread/randrw 报 `err=5, func=full resid`；单独 write job 正常。
  - 根因：create_on_open=1 时 fio 不预分配文件，随机读命中未写区域 → hfs pread 返回 0 → fio resid 检查判 EIO。
  - 处理：去掉 create_on_open（fio setup 阶段 ftruncate 预分配 64MB，hfs_lib_ftruncate 增长 blob）；配置改为混合 rw job（读写同一文件）。
- **验证**：randrw err=0（read 12.9K/write 12.9K IOPS，各 52.9MB/s）；seq err=0（9.8K IOPS，1.28GB/s）。
- **经验**：fio 配置项会改变 setup 行为（create_on_open 跳过预分配）；读路径 EIO 优先怀疑"读未写区域返回 0"。

### 9. MariaDB 冒烟暴露的拦截层问题（shard-24 修复，一般级）

- **状态**：已修复
- **时间**：2026-09-02
- **等级**：一般（数据库场景暴露的拦截缺口）
- **问题 A：目录 fd 不支持（O_DIRECTORY 打开报 EISDIR）**
  - 现象：mariadbd 检查 datadir 时 `File '/hfs/mysql/' not found (Errcode: 21)`。
  - 根因：hfs_lib_open 对目录 inode 直接报 EISDIR；mariadbd 需要只读打开目录（检查 + fsync）。
  - 修复：目录只读打开分配目录 fd（file=NULL、inodeId 有效）；hfs_is_hfs_fd 改为按 inodeId 判定；close/fsync/fstat/fcntl 兼容 file=NULL；数据操作（read/write 等）返回 EISDIR。
- **问题 B：fcntl 文件锁缺失**
  - 现象：InnoDB `Unable to lock ./ibdata1 error: 22`（EINVAL）。
  - 根因：F_SETLK/F_SETLKW/F_GETLK 未处理返回 EINVAL。
  - 修复：F_SETLK/LKW 返回 0（接受锁），F_GETLK 置空 flock。
- **问题 C：相对路径 + chdir 支持缺失**
  - 现象：mariadbd chdir(datadir) 后报 `Can't change dir (Errcode: 2/20)`；相对路径文件全部透传内核失败。
  - 修复：拦截 chdir/fchdir（managed 目录校验 + 记录 cwd）；hfs_view_path 把相对路径按 cwd 拼接再判断前缀；所有 path 型拦截改用 hfs_view_path。
- **问题 D：hfs_view_path TLS 单 buffer 双路径覆盖**
  - 现象：python rename 相对路径报 ENOENT（源存在）。
  - 根因：rename(old,new) 两次调用 hfs_view_path 用同一 TLS buffer，new 覆盖 old。
  - 修复：新增 hfs_view_path_buf（调用方 buffer），rename/renameat/renameat2 用两个独立 buffer。
- **问题 E：posix_fallocate / fallocate64 绕过拦截**
  - 现象：InnoDB `preallocating ./ibdata1 failed error 9`（EBADF）。
  - 根因：libc posix_fallocate 直接发 fallocate 系统调用；mariadbd 绑定 fallocate64 符号——都绕过 fallocate 拦截，内核 fallocate 对 /dev/null 占位 fd 报 EBADF。
  - 修复：拦截 posix_fallocate（返回 errno 语义）与 fallocate64。
- **问题 F：readlink 透传导致 aria 误判**
  - 现象：Aria 表创建后 `Can't find file global_priv.MAI (errno: 2)`。
  - 根因：aria 用 readlink 检查控制/表文件，透传内核视角 ENOENT（HFS 文件内核不可见），aria 误判文件缺失。
  - 修复：拦截 readlink/readlinkat（managed 路径：存在返回 EINVAL，缺失 ENOENT）。
- **问题 G：新拦截函数 real_ 指针懒 resolve 缺失导致 ctor 前崩溃**
  - 现象：加 readlink 拦截后 mariadbd/DPDK 启动段错误（连续 3 次），主程序与 python 正常。
  - 根因：依赖库 ctor（DPDK）在程序 ctor 前调 readlink，real_readlink 未 resolve → NULL 调用。
  - 修复：readlink/readlinkat 懒 resolve（同 opendir 系列）。
- **验证**：主程序 APP=0、冒烟 SMOKE=0、fio err=0（randrw 12.6K IOPS / seq 9.4K IOPS）全绿。
- **遗留**：MariaDB bootstrap 主体成功（InnoDB 完整初始化）但退出阶段多线程卡（信号线程 110 秒）；Aria 表文件 1017 在无 readlink 拦截时复现；bootstrap 卡住需后续多线程/退出路径修复。记录不阻塞 shard-25（RocksDB 单进程为主）。

### 10. RocksDB 冒烟暴露的问题（shard-25，一般级）

- **状态**：已修复
- **时间**：2026-09-02
- **等级**：一般
- **问题 A：sync_file_range 探测触发断言崩溃**
  - 现象：db_bench 启动断言 `IsSyncFileRangeSupported: Assertion !(ret == -1 && errno != ENOSYS)`。
  - 根因：RocksDB 用零范围探测 sync_file_range 支持；未拦截，内核对我们 /dev/null 占位 fd 返回 EBADF/EINVAL（非 ENOSYS）触发断言。
  - 修复：拦截 sync_file_range，HFS fd 返回 ENOSYS（RocksDB 降级 fsync）。
- **问题 B：readv/writev/preadv2 绕过拦截**
  - 现象：RocksDB 的 CURRENT 校验失败（"does not end with newline"）——读返回 0。
  - 根因：RocksDB 部分读路径用 readv/preadv2，未拦截 → 内核 /dev/null 占位 fd 返回 0。
  - 修复：拦截 readv/writev/preadv/pwritev/preadv2/pwritev2（临时 buffer 聚合/分散），及 glibc 内部 __read/__write（fopen/fread 路径）。
  - **遗留（补充排查结论）**：gdb/strace/打点三重确认 CURRENT 读完全未经过任何 read 系列符号（read/pread/preadv/preadv2/__read 全无触发），
    且 open 显示 CURRENT size=16——判断 RocksDB 的 ReadFileToString 走 GetFileSize(fstat)+0 字节读路径（多线程场景下 fd 表项
    时序/GetFileSize 透传），属于多线程 fd 表状态问题，留待 shard-29（per-file 锁/多线程模型）解决。
- **验证**：主程序/冒烟/fio 回归全绿（fio randrw 11K IOPS、seq 7.9K IOPS）。

### 10b. MariaDB/RocksDB 修复跟进（一般级）

- **状态**：已修复
- **时间**：2026-09-02（"先修复"轮）
- **问题 A：waiter 超时过大导致退出卡 ~110 秒**
  - 现象：MariaDB bootstrap abort 后关闭卡（"Signal handler thread did not exit in a timely manner"，110 秒后才响应 SIGTERM）。
  - 根因：WAITER_MAX_TIME=20 亿次 poll 循环（约 110 秒），SPDK 消息丢失/饥饿时超时极慢，进程退出被拖死。
  - 修复：WAITER_MAX_TIME 20 亿 → 500 万（约 0.5-2 秒快速失败）；实测 Signal handler 提示从大量 → 1 次。
- **问题 B：openat(dirfd) 目录 fd 支持缺失**
  - 现象：Aria/InnoDB 用 openat(目录 fd, 相对名) 打开表文件（global_priv.MAI 等），dirfd 非 AT_FDCWD 被透传内核失败（1017）。
  - 修复：目录 fd 记录 dirPath（hfs_handle 新字段）；openat/openat64 对 hfs 目录 fd 走 hfs_lib_openat（拼接 dirPath 解析）。
  - 遗留：1017 仍现（Aria 建表后的 GetFileSize/头部读取同样走透传路径，与 RocksDB CURRENT 同根因——多线程 fd 表时序）。
- **验证**：主程序/冒烟/fio 回归全绿（randrw 12.5K IOPS / seq 9.6K IOPS）。

### 11. 持久化后端阶段暴露的问题（shard-26 修复）

- **时间**：2026-09-03
- **等级**：严重（后端数据无法跨进程复用，数据库场景不可用）
- **状态**：已修复
- **现象**：每个进程都把后端固定为 `Malloc0`，mount 总是调用 `spdk_bs_init`；前一个进程写入的数据无法被后一个进程加载。配置文件读取失败也可能被忽略，继续执行后续 mount。
- **排查过程**：对 mount、SPDK subsystem 配置加载和 preload 初始化链路逐段检查；将 bdev 改为 AIO 文件后，先执行格式化进程，再执行默认加载进程，确认第二个进程能读到第一进程写入的内容；并启动两个持有者进程验证同一后端的互斥锁。
- **根因**：bdev 名称硬编码；没有区分“首次格式化”和“已有 blobstore 加载”；`hfsEnvSetup` 只等待 JSON 回调完成，没有检查回调 rc；多个进程没有主机侧 backend ownership 保护。
- **修复**：增加 `HFS_CONFIG_PATH`、`HFS_BDEV_NAME`、`HFS_FORMAT=1`；默认走 `spdk_bs_load`，显式格式化才走 `spdk_bs_init`；传播 JSON 配置失败；按 backend 创建 `flock` 锁并在 mount 失败/退出时释放。新增 `config/hfs_db_aio.json` 和 `scripts/db/persistent_smoke.sh`。
- **验证**：VM 上 `make app`、`make lib`、`make run`、CMake Release 构建、preload smoke 和持久化 smoke 全部通过。

### 12. Windows 同步导致 shell 脚本执行权限丢失（shard-26 修复）

- **时间**：2026-09-03
- **等级**：一般（测试/部署脚本无法直接启动）
- **状态**：已修复
- **现象**：持久化 smoke 在 VM 上通过源码同步后执行准备脚本时报 `Permission denied`；tar 从 Windows 工作树传输时没有保留 Unix executable mode。
- **排查过程**：源码编译和 preload smoke 已通过，失败只发生在 `persistent_smoke.sh` 调用镜像准备脚本的阶段；检查远端脚本 mode 后确认是执行权限，而非路径或依赖问题。
- **根因**：Windows 工作树不可靠地保存/传递 POSIX 脚本执行位。
- **修复**：脚本内部改为显式 `bash prepare_hfs_db_bdev.sh` 调用，避免依赖同步后的 mode。

### 13. recreate 镜像未清除旧 blobstore 元数据（shard-26 修复）

- **时间**：2026-09-03
- **等级**：一般（测试隔离失效，可能掩盖空盘加载保护问题）
- **状态**：已修复
- **现象**：持久化 smoke 的“空白 backend 必须拒绝加载”检查意外成功；检查发现测试复用了上一次运行已经格式化的镜像。
- **排查过程**：确认 `HFS_RECREATE=1` 已传递，继续检查准备脚本；发现脚本只 `truncate` 已存在文件。truncate 不会清除文件前部已有的 SPDK blobstore 元数据。
- **根因**：把调整文件大小误当成重新创建镜像，导致跨次运行的测试状态泄漏。
- **修复**：仅在显式 `HFS_RECREATE=1` 时删除已有镜像，再创建新的稀疏文件；默认仍拒绝覆盖已有镜像。

### 14. 空盘保护测试只检查应用退出码导致误判（shard-26 修复）

- **时间**：2026-09-03
- **等级**：一般（测试无法证明 preload 初始化失败）
- **状态**：已修复
- **现象**：空镜像上的 `python3 -c 'pass'` 返回 0，测试误报“未格式化 backend 被加载”。
- **排查过程**：检查日志确认 HFS 初始化实际失败；进一步发现 preload 初始化失败后会让应用回退到透传，Python 本身没有执行失败，所以退出码不能作为挂载结果。
- **根因**：测试把应用进程退出状态误当成 HFS 挂载状态，未执行 HFS 路径操作。
- **修复**：改为空盘上执行 `/hfs/unformatted.txt` 的 `open(O_CREAT)`；若 HFS 未挂载则返回错误，若错误自动挂载则测试会成功并触发失败分支。

### 15. CMake run 目标的嵌套 shell 引号破坏执行（shard-26 修复）

- **时间**：2026-09-03
- **等级**：一般（CMake 自测目标不可执行）
- **状态**：已修复
- **现象**：CMake `run` 目标执行时，`/bin/sh` 将 hugepage 检查中的命令拆开，出现 `cat ... not found`、`syntax error` 和目标失败。
- **排查过程**：Make 构建和应用自测正常，失败仅在 CMake custom target；对比生成器命令后确认 `bash -c` 与 CMake/Make 的多层参数解析发生冲突。
- **根因**：把复杂的条件 shell 逻辑嵌入 CMake `COMMAND`，同时经过 CMake、Make 和 `/bin/sh` 三层解析，引用边界不稳定。
- **修复**：CMake `run` 改用 `${CMAKE_COMMAND} -E env HFS_FORMAT=1 <target>`，只保留目标执行和显式格式化；VM 上 CMake `run` 已通过。

### 16. owner-thread 压测脚本错误判断 fsync 返回值并泄漏 fd（shard-27 修复）

- **时间**：2026-09-03
- **等级**：一般（测试脚本缺陷；会把有效实现误报失败并拖慢进程退出）
- **状态**：已修复
- **现象**：32 线程压力测试初始化阶段在 `assert os.fsync(fd) == 0` 处失败；随后未关闭的 HFS fd 使卸载日志报告 `Blobstore still has open blobs`，外层 `timeout` 等待 75 秒后退出。
- **排查过程**：根据 Python traceback 定位到压力测试 heredoc 第 17 行；单独运行同样的持久化写入并打印返回值，确认 `os.pwrite` 成功且 Python `os.fsync` 成功时返回 `None`，不是整数 0。进一步检查卸载错误，确认初始化断言异常绕过了 `os.close`。
- **根因**：把 Python `os.fsync` 的成功语义误写成 C 风格整数断言；测试初始化和循环中的 fd 缺少异常安全的 `finally` 清理。
- **修复**：改为直接调用 `os.fsync`，并在初始化和 worker 循环中用 `try/finally` 确保 fd 关闭；重新执行压力测试验证卸载不再残留 open blob。

### 17. SPDK env opts_size 未初始化导致启动告警（shard-27 修复）

- **时间**：2026-09-03
- **等级**：一般（当前 SPDK 会自动回退到最小结构长度，但新增字段可能被忽略）
- **状态**：已修复
- **现象**：每次启动都输出 `Invalid opts->opts_size 0 too small, please set opts_size correctly`，但环境仍然能够初始化。
- **排查过程**：检查 owner thread 的 SPDK 初始化代码和 SPDK `spdk_env_opts_init` 实现，发现该初始化函数会保留调用方传入的 `opts_size`；调用方对零初始化的结构直接调用它，导致大小字段仍为 0。SPDK 随后只在内部按最小兼容长度继续处理。
- **根因**：未按 SPDK ABI 约定在 `spdk_env_opts_init` 前设置 `opts.opts_size = sizeof(opts)`。
- **修复**：用带 `.opts_size` 的结构初始化 owner thread 的环境选项，再调用 `spdk_env_opts_init`；重新构建和回归确认启动告警消失。

### 18. 默认 Make 目标未更新共享库导致专项测试误用旧代码（shard-28 修复）

- **时间**：2026-09-03
- **等级**：一般（验证结果不可信，可能掩盖新实现问题）
- **状态**：已修复
- **现象**：fd 生命周期专项测试在首次运行时仍表现为旧的共享偏移行为；检查发现 `build/lib/libhfs.so` 的时间戳早于本轮源码，而默认 `make` 只构建 `app`。
- **排查过程**：对比 `build/bin/spdk-hfs` 与 `build/lib/libhfs.so` 的构建时间，随后执行 `make -B app lib`；强制重建共享库后，测试路径和新 fd registry 行为一致。
- **根因**：Makefile 的默认目标没有依赖 `lib`，仅执行 `make` 或 `make app lib` 时可能因时间戳判断跳过共享库链接。
- **修复**：VM 构建脚本改为 `make -B -j$(nproc) app lib`，每轮专项测试前强制生成 app 和 preload 库。

### 19. 共享库版本脚本覆盖 glibc 符号导致 preload 静默失效（shard-28 修复）

- **时间**：2026-09-03
- **等级**：严重（HFS 路径可能落到宿主文件系统，数据库数据隔离失效）
- **状态**：已修复
- **现象**：为解决 Python 的 `fcntl64@GLIBC_2.28` 解析，初版版本脚本把所有导出符号放入 `HFS_1.0`；Python 的其他版本化 libc 调用未命中 HFS wrapper，`/hfs/fd-lifecycle` 被创建到了宿主文件系统。
- **排查过程**：检查 `nm -D` 的符号版本并清理宿主测试目录；用 SPDK mount 日志和宿主目录状态交叉确认，问题只在全量版本脚本存在时出现。
- **根因**：版本脚本改变了未版本化 wrapper 的符号匹配，导致动态链接器优先选择应用所需的其他版本符号。
- **修复**：版本脚本只为 `fcntl64` 导出 `GLIBC_2.28`，其余 HFS wrapper 保持未版本化；重新构建后，专项脚本只在 HFS 元数据中创建测试路径。

### 20. unlink 后旧 fd 读取 inode 表失败（shard-28 修复）

- **时间**：2026-09-03
- **等级**：严重（违反 POSIX unlink-open 语义，WAL/临时文件可能读不到已打开数据）
- **状态**：已修复
- **现象**：文件写入并 unlink 后，旧 fd 的 `pread` 返回 `ENOENT`，但对应数据 blob 仍然存在。
- **排查过程**：确认 unlink 已从 inode 表删除目录项；继续检查读路径，发现 read/pread/lseek/fstat 都重新按 inode id 查表，因此无法访问已摘链的 inode。
- **根因**：打开文件描述只保存 blob handle，没有保存 orphan 文件的大小和时间属性；目录项删除后所有 fd 操作仍依赖可失效的 inode 表项。
- **修复**：新增 `HfsOpenFile` orphan 状态和引用计数，保存 size/ctime/mtime；旧 fd 的 read/pread/lseek/fstat/ftruncate/write 走 orphan 元数据，最后一个 open-file description 释放后才删除 blob。

### 21. rename 覆盖触发 inode 数组压缩后的悬空指针（shard-28 修复）

- **时间**：2026-09-03
- **等级**：严重（可能修改错误 inode 或造成 rename 数据损坏）
- **状态**：已修复
- **现象**：rename 覆盖目标时，删除目标 inode 会用数组末元素填补空洞；随后继续使用删除前保存的 source/parent 指针，覆盖场景可能更新错误元素。
- **排查过程**：构造 destination 先创建、source 后创建的顺序，使 source 位于数组末端；审查 `hfsMetaRename` 的 compact 分支，确认删除目标后旧指针不再稳定。
- **根因**：inode 表使用紧凑数组，删除操作会移动元素；函数跨越删除边界持有元素指针。
- **修复**：在压缩前保存 source 和 destination parent 的 inode id，压缩后按 id 重新查找，再更新 parent/name；缺失时返回 `EIO`。

### 22. rename 同一路径被误判为覆盖并删除自身 blob（shard-28 修复）

- **时间**：2026-09-03
- **等级**：严重（幂等 rename 可能导致文件数据不可恢复）
- **状态**：已修复
- **现象**：元数据层对 `rename(path, path)` 正确返回 no-op，但外层先把目标当作待替换文件；成功返回后可能标记并删除该文件自己的 blob。
- **排查过程**：对比 `hfsMetaRename` 的同路径早退与 `hfs_lib_rename` 的覆盖回收逻辑，确认两层判断不一致。
- **根因**：外层只检查目标是否为普通文件，没有比较 source 和 destination 是否指向同一 inode。
- **修复**：rename 回收目标前解析 source inode，并仅当 source/destination inode 不同才建立 orphan 回收记录；fd 生命周期专项测试加入同路径 rename 数据保持断言。

### 23. fcntl 无第三参数时拦截器仍读取 va_arg（shard-28 修复）

- **时间**：2026-09-03
- **等级**：一般（数据库常见 fd 状态探测存在未定义行为，极端情况下可能崩溃）
- **状态**：已修复
- **现象**：`fcntl(F_GETFL)` 和 `fcntl(F_GETFD)` 的调用约定不带第三参数，但 preload wrapper 无条件读取可变参数。
- **排查过程**：对 fd 复制专项测试加入无参 `F_GETFL`，检查 wrapper 的两条 `fcntl`/`fcntl64` 路径，确认状态查询在 C ABI 层读取了不存在的参数。
- **根因**：把所有 fcntl 命令按“带参数”处理，未按命令区分可选第三参数。
- **修复**：新增命令判断，仅对需要参数的命令执行 `va_arg`；无参状态查询使用默认值，并验证 HFS fd 的 `F_GETFL` 返回打开 flags。

### 24. O_CREAT|O_EXCL 在本次创建后被重复判定为 EEXIST（shard-29 修复）

- **时间**：2026-09-04
- **等级**：严重（数据库初始化创建文件会全部失败）
- **状态**：已修复
- **现象**：元数据并发 smoke 首次创建 `relative.dat` 时返回 `EEXIST`。
- **排查过程**：确认镜像为本轮重新格式化，且 inode 在 open 前不存在；逐行检查 `hfs_lib_open` 后发现 `O_EXCL` 判断位于创建分支之后，对新建 inode 和原有 inode 使用了同一条件。
- **根因**：没有记录 inode 是否由当前 open 调用创建，导致成功创建后再次触发“排他创建遇到已存在文件”的错误路径。
- **修复**：增加 `created` 状态，仅对 open 开始前已存在的 inode 返回 `EEXIST`；保留创建成功路径的正常返回。

### 25. 紧凑 inode 表按数量分配 ID 导致并发删除后 ID 复用（shard-29 修复）

- **时间**：2026-09-04
- **等级**：致命（旧 fd 可能解析到新文件，rename/stat 结果错误并破坏数据生命周期）
- **状态**：已修复
- **现象**：16 线程创建、rename、stat、删除 smoke 中出现 `ENOENT`、size 断言失败和 SPDK 删除 open snapshot 告警。
- **排查过程**：确认元数据读写锁已覆盖 resolver 和 mutator；将失败缩小到创建/删除循环后，检查 inode 表的删除压缩逻辑，发现尾部 inode 被前移后，新 inode 仍以 `inodeCount + 1` 取 ID，可能与存活 inode 或旧 open fd 的 inode ID 重合。
- **根因**：把紧凑数组下标数量误当作全局稳定 inode ID，删除元素后没有单调递增且持久化的 ID 分配器。
- **修复**：元数据头新增 `nextInodeId`，创建时单调分配并持久化；加载旧版 `[count][inodes]` 布局时按最大现存 ID 初始化，保存时自动升级为新布局。

### 26. fsync 未刷新数据 bdev 导致突退出后读到零（shard-30 修复）

- **时间**：2026-09-04
- **等级**：严重（数据库已提交数据可能在进程突退出后丢失）
- **状态**：已修复
- **现象**：元数据 intent 故障注入测试中，`fsync` 返回成功且 inode size 已恢复为 4，但重挂载后读取文件内容为四个零字节。
- **排查过程**：逐步确认 create intent 和 size after-image 均能在三个崩溃边界后重放；重挂载时 `fstat` size 正确，只有数据内容错误。检查 `hfs_lib_fsync` 发现其只调用元数据整表保存，没有对数据 blob 执行 `spdk_blob_sync_md`，也没有提交底层 AIO bdev flush。故障进程直接 `_exit` 时，数据仍可能停留在 bdev/page cache。
- **根因**：把“元数据持久化完成”错误当成“文件数据已持久化”，缺少 POSIX `fsync` 所需的数据同步和设备 flush 顺序。
- **修复**：新增 `hfsSync`，先同步文件 blob metadata，再通过独立可写 bdev descriptor 的 `spdk_bdev_flush` 刷新底层设备；`hfs_lib_fsync` 在 metadata intent 提交前等待该同步完成。故障注入测试增加数据内容校验，修复后必须同时恢复 size 和 payload。

### 27. rename 覆盖源 inode 位于数组末尾时 intent 下标失效（shard-30 修复）

- **时间**：2026-09-04
- **等级**：严重（崩溃恢复可能生成错误 inode 表，导致文件丢失或名称错配）
- **状态**：已修复
- **现象**：目标文件先创建、源文件后创建时执行覆盖 rename；若在 intent 已落盘、稳定镜像尚未写入时退出，重挂载后的目标项可能为空或仍指向旧 inode。
- **排查过程**：构造目标 inode 位于较小下标、源 inode 位于数组末尾的场景，审查压缩流程发现删除目标时末尾源 inode 被移动到目标下标；原实现仍把删除前的源下标写入 intent，`hfsMetaBuildIntent` 会忽略已超出新 count 的旧下标，导致 step-1 恢复缺少源 after-image。
- **根因**：紧凑 inode 数组的下标在删除/压缩后不稳定，rename 在压缩前保存的 source index 被错误地当作事务 after-image 下标。
- **修复**：rename 压缩并重新按稳定 inode ID 定位后，使用源 inode 和被填充目标槽的最终下标构造 intent；新增 source-last 覆盖 rename 的三个崩溃边界回归。

### 28. 请求级写路径把 resize 成功当成失败（shard-31 修复）

- **时间**：2026-09-04
- **等级**：严重（所有首次写入均失败，数据库无法初始化数据文件）
- **状态**：已修复
- **现象**：shard-31 首次 `pwrite` 返回 `EAGAIN`，请求级 I/O smoke 在创建文件后立即失败。
- **排查过程**：远端构建成功，但 resize 回调和数据 I/O 回调均没有错误日志；检查写路径发现 `hfsResizeTo` 成功返回 `0` 后被 `if (!hfsResizeTo(...))` 误判为失败，函数直接返回，调用方继承了旧的 `errno=EAGAIN`。
- **根因**：将“成功返回 0、失败返回 -1”的 C API 误写成了布尔成功判断，导致成功分支提前退出。
- **修复**：改为显式判断 `hfsResizeTo(...) != 0`，并用请求级 fast I/O smoke 覆盖首次写入、非对齐 RMW、向量 I/O 和并发写入。

### 29. Python 大文件向量入口绕过 preload wrapper（shard-31 修复）

- **时间**：2026-09-04
- **等级**：严重（Python/RocksDB 的 `pwritev` 可能对占位 fd 直接返回 `EBADF`）
- **状态**：已修复
- **现象**：请求级 smoke 的标量 `pwrite` 成功，但 `os.pwritev` 返回 `EBADF`；`strace` 显示调用的是 `pwritev2` 系统调用。
- **排查过程**：确认 `pwritev2` 的 `GLIBC_2.26` 版本符号已经导出后，继续检查 Python 可执行文件的未定义符号，发现其依赖的是 `pwritev64v2@GLIBC_2.26`，而 preload 库没有提供该 glibc large-file 别名。
- **根因**：只实现了普通 `pwritev2/preadv2` 名称，没有覆盖 64 位 ABI 别名，Python 直接落回 libc 并对 `/dev/null` 占位 fd 发起系统调用。
- **修复**：新增并导出 `pwritev64v2/preadv64v2`，统一转入 HFS vector I/O；回归脚本加入 Python `pwritev/preadv` 检查。

### 30. unmap 成功返回但未清零数据（shard-31 修复）

- **时间**：2026-09-04
- **等级**：严重（`PUNCH_HOLE` 读回旧数据，可能泄露已删除内容并违反 POSIX 语义）
- **状态**：已修复
- **现象**：`fallocate(PUNCH_HOLE|KEEP_SIZE)` 返回成功，但读回被 punch 的范围仍为原始数据。
- **排查过程**：确认边缘范围的读改写路径正常后，检查 SPDK blobstore 的 unmap 实现；发现底层 bdev 不支持 unmap 时 blobstore 会直接返回成功，并且 blobstore 文档明确不依赖 unmap 的清零效果。Aio 测试配置未启用 fallocate，因此该路径实际是成功空操作。
- **根因**：把 `spdk_blob_io_unmap` 的成功回调错误地当成了“读到零”的保证，忽略了 blobstore/bdev 的 unmap 语义差异。
- **修复**：对齐范围仍先提交 unmap 以释放底层空间；随后提交 `WRITE_ZEROES`，若后端不支持则使用固定 DMA 池分块写零，保证读语义确定且不引入无限分配。

### 31. fio 创建虚拟挂载点时根路径解析错误（shard-31 修复）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：一般（fio 无法创建 `/hfs` 父目录，性能回归无法执行）
- **现象**：fio randrw 在准备阶段报告 `failed to create dir (/hfs): Invalid argument`，而 `/hfs/fio/...` 的普通文件 I/O 路径本身尚未执行。
- **排查过程**：单独运行 fio 并保留 stderr，确认失败发生在 `mkdir("/hfs")`；先修正默认前缀 `/hfs/` 的精确路径转换，使其返回 `/`，随后发现元数据层仍把根目录创建请求返回为 `EINVAL`。
- **根因**：虚拟挂载前缀带尾部 `/` 时，精确前缀路径和前缀下的根路径没有统一转换为 HFS 根 inode；根 inode 已存在时也没有返回 POSIX 约定的 `EEXIST`。
- **修复**：`hfs_inner_path` 对精确挂载点及单独尾斜杠路径统一返回 `/`，`hfs_lib_mkdir("/")` 返回 `EEXIST`，fio 的目录准备和后续 I/O 恢复正常。

### 32. fio fork 子进程继承 SPDK 运行时上下文（shard-31 修复）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：严重（子进程可能崩溃或卡死，数据库基准无法稳定运行）
- **现象**：fio 使用默认 process job 模式时，在创建/打开 HFS 文件期间收到 SIGSEGV；同一配置增加 `thread=1` 后稳定通过。
- **排查过程**：gdb 显示崩溃位于 `hfsDoOpenById`，父进程 SPDK owner 线程正在消费子进程地址空间中的请求上下文；上下文中的 HFS 对象字段已被无关数据覆盖。对比 fio 的 fork 启动方式和 HFS 的进程内 SPDK message queue 后，确认问题只出现在 fork 后继续复用已初始化运行时的场景。
- **根因**：fork 只复制用户态地址空间，不复制可继续工作的 SPDK 线程/消息队列状态；子进程提交的栈或堆指针可能被父进程 owner 线程按相同虚拟地址解释，造成跨进程指针访问和锁状态失效。
- **修复**：运行时记录 owner PID，fork 子进程禁止提交 SPDK 消息、识别继承的 HFS fd 或重新初始化/析构 inherited HFS 状态；路径解析在进入继承锁前直接转为 passthrough。fio 基线显式使用 `thread=1`，避免把不安全的 fork job 模式当成 HFS 并发能力；fork 子进程 smoke 覆盖“不崩溃且不静默创建 HFS 文件”。

### 33. 非对齐 PUNCH 在单 unit 范围内错误清零前缀（shard-31 修复）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：严重（部分数据可能被错误清零，违反 `PUNCH_HOLE` 范围语义）
- **现象**：打孔范围完全位于一个非对齐 unit 时，请求前的同一 unit 前缀也可能被清零。
- **排查过程**：补充 offset=17、length=23 的单 unit 打孔用例后，逐步检查首尾边缘处理和对齐中间范围处理，发现首边缘处理完成后 `alignedEnd` 可能小于当前游标。
- **根因**：无论是否实际执行 aligned unmap，都无条件把游标重置为 `alignedEnd`；当没有中间完整 unit 时，游标从请求结束位置回退到了 unit 起点。
- **修复**：仅在成功执行 aligned unmap 后更新游标；无中间完整 unit 时保留首边缘后的游标，再处理尾边缘。请求级 smoke 加入单 unit 非对齐 PUNCH 的前后边界校验。

### 34. open 创建 mode 被丢弃导致 stat 权限不符合调用方请求（shard-32 修复）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：严重（数据库创建的数据文件权限不符合 POSIX 契约，可能导致后续访问失败或权限配置失效）
- **现象**：POSIX 契约测试用 `openat(..., 0640)` 创建文件后，`stat` 返回权限 `0644`。
- **排查过程**：确认 `hfs_preload.c` 已正确读取 open 的变参后，检查 `hfs_lib_open` 到 `hfsMetaCreateFile` 的调用链，发现 mode 没有继续传入；元数据创建逻辑固定使用默认文件 mode。
- **根因**：创建接口没有把调用方的 mode 传入 inode，导致 mode 参数在 preload 层之后被静默丢弃。
- **修复**：扩展 `hfsMetaCreateFile`/`hfsMetaMkdir` 接口，创建 inode 时保存 `mode & 07777`；stat/fstat 统一从 inode 属性返回。契约测试加入自定义文件和目录 mode 校验。

### 35. AT_FDCWD 路径转换成功但未写入目标缓冲（shard-32 修复）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：严重（statx/fstatat 等路径接口对合法 HFS 路径返回 `ENOENT`，数据库能力探测会误判文件系统状态）
- **现象**：`statx(AT_FDCWD, "/hfs/...", ...)` 路由已进入 HFS，但因后续使用未初始化的 path buffer 返回 `ENOENT`。
- **排查过程**：契约测试已通过普通 `stat` 和目录 fd 相对 `fstatat`，仅绝对 HFS `statx` 失败；检查 `hfs_view_at_path` 发现 AT_FDCWD 分支调用了返回 inner path 的 helper，却没有把结果复制到调用者 buffer。
- **根因**：路径判定与路径物化复用了不同的存储语义，返回指针指向 TLS/输入字符串，而调用方统一假设结果在 pathBuf 中。
- **修复**：AT_FDCWD 分支统一通过 `hfs_view_path` 获取结果并复制到 pathBuf，同时保留长度检查；statx/fstatat 回归覆盖绝对和 fd-relative 两种路径。

### 36. Python statvfs64 符号未被 preload 拦截（shard-32 修复）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：一般（Python/数据库探测 HFS 容量时落入内核路径并得到 `ENOENT`）
- **现象**：ctypes 直接调用 `statvfs` 成功，但 `os.statvfs("/hfs/...")` 仍访问真实内核路径并返回 `ENOENT`。
- **排查过程**：对失败进程做 strace，并检查 Python 可执行文件动态符号，确认其依赖 `statvfs64@GLIBC_2.2.5` 与 `fstatvfs64@GLIBC_2.2.5`，而 preload 只提供普通 `statvfs/fstatvfs`。
- **根因**：只覆盖了源码层面的普通 API 名称，没有覆盖运行时实际绑定的 large-file ABI 及对应 glibc 版本符号。
- **修复**：加入 statfs64/fstatfs64/statvfs64/fstatvfs64 wrapper，映射到统一 HFS 实现，并在 version script 导出 GLIBC_2.2.5 版本；契约测试加入 Python `os.statvfs` 路径。

### 37. 旧数据库 smoke 使用进程私有 Malloc0，无法作为端到端验收（shard-32 记录）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：严重（若误把旧 smoke 的失败/成功当成数据库支持结论，会掩盖跨进程数据路径缺口）
- **现象**：strace 审计中，旧 `mysql_smoke.sh` 的 `mariadb-install-db` 无法在 HFS datadir 创建 InnoDB/Aria 文件；旧 `rocksdb_smoke.sh` 的 `db_bench` 返回 `No such device`。两者均未完成数据库端到端流程。
- **排查过程**：保留完整输出后确认 MariaDB helper 会 exec 新的 `mariadbd` 进程，而脚本只设置默认 `Malloc0` 和 `HFS_AUTO_MKDIRS`；RocksDB 同样使用默认进程私有 `Malloc0`。HFS 日志显示 RocksDB 进程初始化/挂载失败，随后路径 wrapper 按设计返回 `ENODEV`。strace 未发现对 `/hfs/...` 的真实 `read/write/pread64/pwrite64/mmap` 内核数据调用，仅看到参数打印和程序启动参数。
- **根因**：旧脚本的注释假定“exec 后仍可复用进程内 malloc bdev”，与当前 owner-PID/fork 防护和 SPDK 进程模型不一致；脚本也没有使用 Aio 持久后端及独立运行目录。
- **修复**：本阶段将其明确标记为非验收脚本，保留审计证据；真实 MySQL 多进程初始化/启动和 RocksDB fill/read 迁移到 shard-33～35，必须使用持久 Aio 后端、正确的进程生命周期和不依赖 `/dev/null` 的数据路径后重新验收。

### 38. 依赖库构造阶段调用 preload wrapper 触发空指针跳转（shard-33 已修复）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：严重（任何链接 libselinux 的辅助程序在 LD_PRELOAD 下都可能在启动阶段崩溃）
- **现象**：MariaDB 初始化失败；gdb/strace 显示 `/usr/bin/mkdir` 在进入 main 前收到 SIGSEGV。libselinux 构造函数依次调用了 `statfs64` 和 `access`，崩溃地址为 NULL。
- **排查过程**：直接对 `mkdir --version` 做 gdb 回溯，确认调用栈位于 dynamic loader → libselinux 初始化；反汇编确认 `statfs64@plt` 已绑定到 libhfs wrapper，而 wrapper 的真实函数指针仍为 NULL。修复 `statfs64` 的惰性解析后继续定位，发现 libselinux 随后的 `access@plt` 仍会跳转到未初始化的 `real_access`。
- **根因**：共享库依赖的构造函数可能早于 libhfs 自身构造函数执行；仅在 `hfs_preload_ctor` 中一次性解析真实 libc 符号，无法覆盖构造阶段提前进入 wrapper 的路径。
- **修复**：为 `statfs64/fstatfs64/statvfs64/fstatvfs64/access` 增加惰性解析和非 64 位 ABI 回退；所有解析失败路径返回 `ENOSYS`，不再直接调用 NULL。加入无 HFS 路径的外部 helper 启动回归，修复后再继续 MariaDB 验收。

### 39. HFS 后台线程抢收 MariaDB 的进程终止信号（shard-33 修复）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：严重（正常停止和 SIGKILL 恢复可能卡住或留下未清理的数据库进程）
- **现象**：MariaDB 的信号管理线程没有按预期收到 `SIGTERM`；信号被 HFS SPDK owner/log
  线程接收，服务停在 `unireg_abort`/`spdk_thread_poll`，smoke 的清理阶段超时。
- **排查过程**：用 `/proc/<pid>/task/*/status` 检查各线程 signal mask，再用 `strace`
  追踪进程组信号投递，确认 HFS 新建线程继承了可接收 `SIGHUP/SIGQUIT/SIGALRM/SIGTERM/
  SIGTSTP` 的 mask；MariaDB 的 `rt_sigtimedwait` 线程因此没有拿到目标信号。
- **根因**：preload 进程内启动的 SPDK 和日志线程没有遵守宿主程序的信号线程模型。
- **修复**：runtime owner 在线程入口、log writer 在线程入口屏蔽上述宿主管理信号，并在
  runtime 初始化失败/退出时恢复原 mask；验证信号由 MariaDB 管理线程接收，完整 smoke
  的正常停止、重启和异常恢复通过。

### 40. SIGKILL 后 MariaDB 子进程保留 HFS 后端锁（shard-33 修复）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：严重（下一次挂载可能得到 `ENODEV`，恢复验收被残留进程污染）
- **现象**：只 kill `mariadbd` 的单个 PID 后，bootstrap/helper 或后台子进程仍持有 HFS
  lock；下一次启动偶发挂载失败，或 smoke 外层清理阶段一直等待。
- **排查过程**：对比正常重启和 SIGKILL 流程，检查进程树、lock 文件持有者和 HFS owner
  线程，确认 MariaDB 的进程组中存在未随主 PID 退出的 helper/线程相关进程。
- **根因**：测试脚本只保存并杀主进程 PID，没有隔离和回收整个数据库进程组。
- **修复**：bootstrap 和 server 均使用独立 session/process group；失败清理和 SIGKILL
  恢复使用负 PGID 回收整个进程组，并保留 PID 等待。随后持久化 SIGKILL recovery smoke
  在同一镜像上稳定通过。

### 41. MariaDB `openat64` 绝对路径被错误按 dirfd 解释（shard-33 修复）

- **状态**：已修复
- **时间**：2026-09-04
- **等级**：严重（系统表存在但初始化仍报告文件不存在或无法打开）
- **现象**：MariaDB 使用 `openat64`/fortify open 访问 `/hfs/mysql/...` 时，传入的 dirfd
  不是 `AT_FDCWD`；preload 没有按 POSIX 规则忽略 dirfd，导致已存在的 HFS 文件得到
  `ENOENT`/`ENODEV`。
- **排查过程**：对 `global_priv.MAI` 的 `lstat64`、`readlink`、`realpath` 和最终 open
  调用做 native syscall 对照，发现路径为绝对路径且 dirfd 为 -1；HFS stat 成功而
  `openat64` 没有进入 HFS 路由。
- **根因**：`hfs_view_at_path` 和 openat wrapper 只把 `AT_FDCWD` 当作绝对路径入口，
  忽略了 `openat(2)` 对绝对路径“dirfd 无效且必须被忽略”的规定。
- **修复**：绝对路径优先物化到 HFS 内部路径，再处理 `AT_FDCWD` 和 HFS 目录 fd 相对路径；
  同步覆盖 `openat/openat64/__openat*_2/readlinkat`。MariaDB 初始化和持久化重启回归通过。

### 42. RocksDB 版本化 stdio ABI 绕过 HFS，以及 `fclose` 残留 fd slot（shard-34 修复）

- **状态**：已修复
- **时间**：2026-09-05
- **等级**：严重（数据库元数据读取会误落到占位 fd，或因 fd slot 残留导致后续打开失败）
- **现象**：RocksDB 在 preload 下读取 `CURRENT` 时报告缺少换行，随后可能出现
  `No such device`、manifest 打开失败或 `EBUSY`；HFS 介质上的 `CURRENT` 实际内容正确。
- **排查过程**：先对比 HFS 介质上的文件内容与 RocksDB 进程读取结果，再用 strace 和动态
  符号绑定检查调用链，确认 `libstdc++` 使用 `read@GLIBC_2.2.5`，RocksDB 使用
  `fread_unlocked@GLIBC_2.2.5`，它们绕过了原先只有未版本化导出的 wrapper。补齐版本化
  wrapper 后，RocksDB 已能读取文件，但 libc 内部关闭 stdio 流不经过公开 `close` wrapper，
  HFS fd slot 仍占用同一个占位 fd，导致后续打开返回 `EBUSY`。
- **根因**：preload 没有覆盖数据库实际绑定的 glibc 版本化 `read`/stdio ABI；同时把 HFS
  fd 交给 libc `fclose` 时没有先摘除 HFS fd slot。
- **修复**：在 version script 中导出 GLIBC_2.2.5 版本的 `read`、`__read`、`fread`、
  `fread_unlocked`、`feof`、`ferror`、`clearerr` 和 `fclose`；为 HFS stdio 流增加轻量 EOF/
  error 状态，读路径直接路由 `hfs_lib_read`；新增 `hfs_lib_release_fd`，由 `fclose` 先摘
  slot 并释放 open-file description，再调用 libc 关闭占位 fd。
- **验证**：Make/CMake 强制构建通过；RocksDB 20,000 条数据、8 线程 60 秒 WAL 并发、
  普通/direct I/O、覆盖写、flush/compact、SIGKILL/WAL recovery 和 `ldb checkconsistency`
  连续 3 轮通过，输出 `SHARD34_ACCEPTANCE_OK`。

### 43. shard-35 临时回归执行器直接执行非可执行脚本（测试工具修复）

- **状态**：已修复
- **时间**：2026-09-05
- **等级**：一般（临时回归执行器误报多项项目测试失败）
- **现象**：回归执行器对权限为 0644 的 shell 脚本直接执行，远端返回 exit code 126，
  看起来像 preload、持久化和契约测试全部失败。
- **排查过程**：检查每个失败日志均为 `Permission denied`，而同批次直接通过 `bash`
  调用的 request-local fast I/O 测试正常；检查脚本权限确认仓库脚本并未标记 executable。
- **根因**：临时执行器把脚本路径当作可执行文件调用，没有显式交给 bash；这是测试编排
  错误，不是 HFS 运行时或脚本逻辑错误。
- **修复**：执行器统一改为 `bash <script> <args>`，随后重新执行完整回归并按各脚本
  的真实返回码判定结果。

### 44. MariaDB HFS Hook 正常退出时 buffer pool 临时文件创建失败（待修复，一般级）

- **状态**：未修复
- **时间**：2026-09-05
- **等级**：一般（不影响本轮 SQL 结果和主数据恢复，但会丢失 InnoDB buffer pool dump，
  可能影响下一次启动的预热状态）
- **现象**：原生 ext4 与 HFS Hook 对比压测均完成，HFS 组在正常 shutdown 阶段额外出现
  `InnoDB: Cannot open '/hfs/mysql/ib_buffer_pool.incomplete' for writing: No such file or directory`；
  随后仍输出 `Shutdown complete`。原生组没有该错误。
- **排查过程**：对比两组独立 MariaDB 10.11.14 日志；HFS 组的错误只出现在 InnoDB
  `Starting shutdown`/`Dumping buffer pool(s)` 之后，前面的 12 个 mysqlslap case 全部返回 0，
  `benchdb.t` 行数保持 10,000，未发现 I/O error、崩溃或表损坏。错误前可见多次 HFS blob
  close 完成日志，说明问题位于退出阶段的 HFS 路径/生命周期边界，而非压测 SQL 主路径。
- **初步根因**：InnoDB 退出线程创建隐藏临时文件 `ib_buffer_pool.incomplete` 时，HFS
  Hook 的目录视图或 runtime 退出时序未能保持 `/hfs/mysql` 可创建状态；当前证据尚不足以
  区分 open 路径语义缺口和 runtime teardown 竞态。
- **后续**：用 syscall 级 trace 对该次 `openat/openat64`、目录 inode 状态和 HFS runtime
  teardown 顺序做最小复现；修复前不将 buffer pool dump 视为 HFS MariaDB 退出阶段完全兼容。
- **真实 NVMe 复现（2026-09-08）**：在 SPDK 直连 `0000:0b:00.0` 的 raw NVMe
  bdev 上重复出现 `Signal handler thread did not exit in a timely manner`；系统表已提交、
  服务可启动且 12 个 mysqlslap workload 全部返回，但正常 `mysqladmin shutdown` 仍可能
  长时间等待。为完成性能采集，测试器在检测到该警告后回收 bootstrap/server 进程组；
  SIGKILL 会留下 `/var/tmp/spdk_pci_lock_<BDF>`，下次挂载前必须确认无 SPDK peer 后清理，
  否则 HFS 路由表现为 `ENODEV`。这属于退出时序/测试清理缺陷，不影响本轮 SQL 结果，
  但当前不能宣称 direct-NVMe 下 MariaDB 正常退出完全兼容。

### 45. truncate 缩短后再扩展读回历史数据（shard-36 修复，严重级）

- **状态**：已修复
- **时间**：2026-09-05（架构与性能审查）
- **等级**：严重（违反 POSIX 零填充语义，可能向调用方暴露已截断的历史内容）
- **现象**：VM 最小复现先写入 4096 个 `A`，执行 `ftruncate(fd, 0)`，再执行
  `ftruncate(fd, 4096)`；随后 `pread` 读回的 4096 字节仍全部为 `A`，而不是零。探针输出
  `TRUNCATE_REGROW_ZERO=0 old_byte_count=4096`。
- **排查过程**：检查 `hfs_lib_ftruncate`，缩短路径只调用 `hfsMetaSetSize` 修改 inode size；
  数据 blob 不缩容、不打孔，也不清理新 EOF 所在 unit 的尾部。再次扩展时调用
  `hfsResizeTo`，但 blob 已经拥有足够 cluster，因此直接成功返回，旧 payload 重新进入可读范围。
- **根因**：inode 逻辑长度和 blob 已分配/有效数据范围没有独立建模；truncate shrink 没有建立
  “截断后区域必须读零”的持久语义。
- **修复**：新增 `hfsIoZeroRange`，shrink 先清零 `[newEOF, oldEOF)`（保留非对齐 unit 前缀），
  再通过 `hfsResizeExact` 释放完整后继 cluster；grow 在发布新 EOF 前清零
  `[oldEOF, newEOF)`，即使 blob 已有容量也不会重新暴露旧 payload。`O_TRUNC` 复用相同流程，
  先完成数据清理再提交 inode size。
- **验证**：新增 `scripts/db/posix_data_contract_test.sh`，覆盖 shrink→grow、O_TRUNC→grow、
  非对齐 EOF、跨 1 MiB cluster、首次格式化和第二进程重挂载；VM 上均通过。

### 46. 覆盖写不更新 mtime（shard-36 修复，一般级）

- **状态**：已修复
- **时间**：2026-09-05（架构与性能审查）
- **等级**：一般（文件内容已变化但 stat/fstat 时间戳保持旧值）
- **现象**：VM 探针把文件 mtime 设置为 1 秒，随后在 offset 0 覆盖写 1 字节并 fsync；
  `fstat` 前后均为 `1000000000ns`，输出 `OVERWRITE_MTIME_ADVANCED=0`。
- **排查过程**：检查 `hfs_lib_write`、`hfs_lib_pwrite` 和 `hfsPublishWriteEnd`，三处都只在
  `writeEnd > inode.size` 时更新 size/mtime。对文件现有范围内的覆盖写没有任何 mtime 更新。
- **根因**：把“更新写入时间”错误绑定到“文件增长”，缺少独立的 content-dirty/mtime-dirty
  状态。
- **修复**：统一 write、pwrite、writev 和 pwritev 的成功收尾路径；任意成功内容写均更新
  inode 与 open-file description 的 mtime/ctime，size 仅在 write end 超过 EOF 时增长，并将
  metadata 标记为待提交。
- **验证**：数据契约脚本将 mtime 设为 1 秒后执行同长度 `pwrite` 和 `fsync`，断言
  `st_mtime_ns` 严格前进；VM 通过。

### 47. statfs/statvfs 容量混用且长期陈旧（shard-36 修复，一般级）

- **状态**：已修复
- **时间**：2026-09-05（架构与性能审查）
- **等级**：一般（容量监控、数据库预分配判断和 ENOSPC 预警不可信）
- **现象**：默认 1.5GiB Malloc0 配置下，VM 探针在创建一个文件后得到
  `f_frsize=512, f_blocks=1530, f_bfree=1529`，折算总容量仅 783,360 字节。
- **排查过程**：初始化时 `g_fs->numCluster` 保存 blobstore 总 data cluster 数，
  `freeClusterCount` 保存当时空闲 cluster；但每次文件 resize 回调又把 `numCluster` 覆盖为
  当前文件的 cluster 数。`hfs_fill_statvfs` 随后把 `numCluster + freeClusterCount` 当成总块数，
  同时把 cluster 计数配上 `unitSize`（512B）作为 block size，且 free 值从 mount 后不再刷新。
- **根因**：同一字段承担“文件已分配 cluster”和“文件系统总 cluster”两个含义；容量单位和
  刷新策略没有统一。
- **修复**：删除创建文件时“占满全部 free cluster”的错误预分配；`Hfs` 拆分为固定的
  `totalDataClusters`、`clusterSize` 与原子 `freeClusterCount`。`statfs/statvfs` 在 SPDK
  owner thread 刷新空闲 cluster，统一以 blobstore cluster size 填充 `f_bsize/f_frsize`，不再
  把单个 blob 的分配量当作文件系统容量。
- **验证**：数据契约脚本断言总容量大于 512 MiB、`f_blocks` 在分配/删除前后不变、写入两个
  cluster 后 `f_bfree` 下降、unlink 后 `f_bfree` 回升；VM 通过。

### 48. SPDK 异步操作错误被 waiter 当作成功（执行中，严重级）

- **状态**：执行中；请求私有错误传播已完成，真实 error-bdev 完整矩阵待补
- **时间**：2026-09-05（架构与性能审查）
- **等级**：严重（底层 I/O/元数据失败可能向数据库返回成功，破坏错误处理与持久化保证）
- **现象**：静态审查发现 `hfsMetaOpCb` 忽略 `bserrno`，只完成共享 completion；因此
  `hfsMetaBlobWrite` 的 open/resize/write/sync/close 任一步只要回调到达，函数最终即可返回 0。
  `hfsSpdkBsOpenByIdCb`、blob close/delete 回调也只记录日志，没有让 `hfsOpenById`、
  `hfsClose`、`hfsDeleteBlobById` 返回失败。
- **排查过程**：逐一对照所有 `waiter` 调用、回调参数和 `operationRc` 字段。`hfsIoRequest`、
  resize 和 fsync 路径已使用 request-local rc；元数据和部分 blob 生命周期路径仍复用
  `Hfs::completion`/`HfsFile::completion`，但没有一致保存错误码。
- **根因**：completion 只表达“回调已发生”，没有统一携带操作结果；部分旧 callback 在请求级
  rc 模型引入后未迁移。
- **当前进展**：shard-37 增加 `HfsRequestResult`（operation、request id、submit rc、
  completion rc、`bserrno`、完成 unit 数），并迁移 blob 生命周期、data I/O、resize、metadata
  I/O、mount/unmount 和容量采样。callback 在唤醒 waiter 前写入该对象；POSIX 层统一将 SPDK rc
  映射为 errno。`HFS_TEST_FAIL_SUBMIT` 覆盖提交拒绝，`HFS_TEST_FAIL_COMPLETE` 在 owner thread
  调用对应 callback 的 EIO 分支，覆盖 blob open/sync/close、read/write/resize、metadata
  open/write 和 statvfs 容量采样；VM 回归通过。新增 `error_bdev_injection_test.sh` 以 AIO 上的
  `EE_Aio0` error vbdev 注入一次真实 bdev flush failure，验证 `fsync` 返回 `EIO`，而已写数据仍可读。
- **剩余工作**：AIO blobstore 的 `spdk_blob_io_unmap` 不会下发底层 bdev UNMAP，故 error vbdev 无法
  观察该逻辑操作；需在支持 unmap 的后端或专用 SPDK 测试环境完成 unmap/write-zeroes、blob delete 和
  unload 的真实 completion 矩阵，并核对 request 统计闭合。

### 49. 元数据提交失败不回滚内存状态并可能泄露资源（执行中，严重级）

- **状态**：执行中；metadata undo、已提交删除的重挂载回收与 close 延迟回收已完成
- **时间**：2026-09-05（架构与性能审查）
- **等级**：严重（API 返回失败后，同进程仍可能看见未持久化的新命名空间或错误 size）
- **现象**：静态审查发现 mkdir/create/remove/rename/set-size 均先原地修改 `fs->inodes` 和
  `inodeCount`，再调用 `hfsMetaCommit`；commit 失败分支只释放临时 buffer，不恢复旧数组。
  create-file 在 data blob 创建成功后，若 inode 表已满或 commit 失败，也没有删除新 blob。
  `hfs_lib_open` 在占用 `/dev/null` placeholder 后若 `HfsFile` 分配失败，同样没有关闭该 fd。
- **排查过程**：沿每个 metadata mutator 的失败出口检查对象所有权和内存 after-image，确认
  没有 undo image、staging table 或统一 cleanup 标签；再与 intent record 的磁盘恢复逻辑
  对照，磁盘事务并不能修复仍在运行进程的内存状态。
- **根因**：把“先改内存、后持久化”的 happy-path 实现误当成完整短事务，缺少提交前 staging
  和失败时的资源补偿。
- **当前进展**：metadata commit 保留稳定镜像，失败时恢复运行中 inode 表；若 intent 已发布或
  无法恢复，则置 `metaRecoveryRequired` 并拒绝后续可写操作，强制重新挂载恢复。create 的 data
  blob 与 format 的 super-blob 都有失败 cleanup；final close 失败会返回原始 errno、关闭内核
  placeholder，并保留 open description/orphan 在 teardown 前重试。挂载在加载并重放元数据后于 SPDK
  owner thread 遍历 blobstore，收集除 metadata blob 和所有现存 FILE inode blob 以外的 blob，再逐个
  删除并刷新空闲 cluster；这样已提交 unlink/rename 的后续 delete 失败会在下一次挂载可重复回收，删除
  再失败只记录并留待下次挂载重试。注入脚本连续 1,000 次 metadata-write 失败后验证目录项、空闲
  cluster 和重挂载状态均回到基线，并新增已提交 unlink 的 blob-delete 失败、跨进程重挂载后容量恢复断言。
  对 `PUNCH_HOLE`，若 accelerated `WRITE_ZEROES` callback 失败，会改走 DMA 分块写零；只有可观察的
  hole 内容最终为零才返回成功，专项注入已验证该 fallback。
- **剩余工作**：用真实 error-bdev 验证 unmap 后 DMA fallback 也失败时的 PUNCH_HOLE 行为，并覆盖
  intent/stable/clear 的真实设备 completion 错误。

### 50. fsync 的 inode 元数据写入位于最后一次设备 flush 之后（执行中，严重级）

- **状态**：执行中；双 barrier 顺序已实现，掉电模型与 group commit 待完成
- **时间**：2026-09-05（架构与性能审查）
- **等级**：严重（带易失写缓存的 AIO/NVMe 后端上，fsync 返回后 inode size/mtime 仍可能掉电丢失）
- **现象**：代码顺序为 `hfsSync(file)` 执行 blob metadata sync 和 bdev flush，随后
  `hfsMetaCommitInode` 才把 HFS inode intent/stable/clear 三个镜像写入 metadata blob；提交后
  没有第二次 bdev flush。现有 crash smoke 使用进程 `_exit`/SIGKILL，不能模拟设备缓存掉电。
- **排查过程**：对照 `hfs_lib_fsync`、`hfsSync`、`hfsMetaCommit` 和本仓库 SPDK bdev API 注释；
  SPDK 明确说明带 volatile cache 的设备在 flush 完成前不保证持久，而 HFS inode payload 在该
  flush 完成后才提交。
- **根因**：当前顺序只保证数据先于 inode 更新提交到队列，没有给 inode 事务阶段建立持久化
  barrier；intent 的三个阶段也没有用 flush/FUA 隔开，恢复协议只在进程崩溃/page-cache 场景
  经过验证。
- **当前进展**：`hfs_lib_fsync` 现在固定执行 data blob sync + 首次 bdev flush →
  `hfsMetaCommitInode` → data blob sync + 第二次 bdev flush。第二个 flush 覆盖刚提交的 metadata
  blob intent/stable/clear 写入；任何阶段失败都向 `fsync` 返回错误。注入回归新增 metadata-write
  失败断言：数据仍可读、首次 `fsync` 返回 `EIO`，清除故障后的下一次 `fsync` 完成 metadata 提交与
  post-commit barrier。`HFS_DURABILITY_TRACE_PATH` 默认关闭；启用时记录 data barrier、metadata
  commit 和 post-commit barrier 的序号、inode 与结果。`durability_barrier_test.sh` 已在 VM 断言
  `fsync` 和 `fdatasync` 的成功路径偏序；`io_error_injection_test.sh` 额外注入 post-commit
  bdev flush failure，断言 `fsync` 返回 `EIO`、数据仍可见且后续 barrier retry 可成功。
  `durability_crash_recovery_test.sh` 用测试专用 hook 在首个 barrier、metadata transaction、最终
  barrier 后分别终止进程；新进程确认前两个未应答操作只恢复为完整旧镜像或完整新镜像，且最终
  barrier 后的 inode size 与 payload 一同可恢复。该测试仅验证进程崩溃协议，未将 VM/AIO 文件缓存
  误称为物理掉电模拟。
  2026-09-06 VM 上强制 Make/CMake、核心注入回归、MariaDB 完整恢复流程与 RocksDB 跨进程/WAL/
  一致性检查均通过。
- **剩余工作**：重新定义 power-fail 模型和 barrier：data/blob mapping 落盘 → flush；intent 落盘
  → flush；stable image/log commit 落盘 → flush；clear 可延迟批量完成。实现 group commit 减少
  flush 数，并用 SPDK error bdev、可控 write-cache/掉电模拟验证 fsync/fdatasync/目录 fsync。

### 51. shard-37 metadata 格式化清理回调缺少日志头（已修复，一般级）

- **状态**：已修复
- **时间**：2026-09-05（shard-37 开发）
- **等级**：一般（新增错误路径无法链接，阻塞共享库与测试二进制构建）
- **现象**：首次 VM 强制执行 `make -B app lib` 时，链接器报告
  `undefined reference to HFS_LOG_ERROR`，定位到 metadata super-blob format 失败后的 cleanup callback。
- **排查过程**：编译阶段没有报语法错误，但 `hfs_meta.c` 对日志宏的调用被编译器警告为隐式
  声明；链接阶段将其视为外部函数引用。对比同文件的 include 列表确认缺少 `hfs_log.h`。
- **根因**：新增故障清理回调复用了项目日志宏，却没有同步引入宏定义所在的头文件。
- **修复**：在 `src/hfs_meta.c` 显式包含 `hfs_log.h`；重新执行 VM 强制 Make/CMake 构建与回归。

### 52. fsync 的 metadata 失败回滚了已成功 pwrite 的运行时 inode（已修复，严重级）

- **状态**：已修复；VM 定向与全量回归均通过
- **时间**：2026-09-06（shard-37/38 衔接开发）
- **等级**：严重（用户写入已返回成功，却会在同一进程被逻辑 EOF 隐藏）
- **现象**：先使一个文件的 `fsync` 在 blob sync 阶段失败，再成功 `pwrite(fd, "meta", 4, 0)`；注入
  `metadata-write` completion failure 后 `fsync` 正确返回 `EIO`，但紧随其后的 `pread(fd, 4, 0)` 返回
  空串而非 `meta`。data blob 中的内容仍存在，却因 inode size 被回滚为 0 而不可见。
- **排查过程**：`pwrite` 已将运行时 inode size 置为 4 并标记 `metaDirty`，首次 data sync 成功；故障发生在
  `hfsMetaCommit` 写入 pending intent 之前。随后该函数的通用失败处理无条件通过 stable image 恢复整个
  inode 表，把刚刚成功写入但尚未持久化的 size 覆盖成旧的 0。`io_error_injection_test.sh` 的 metadata
  failure 回归稳定复现该行为。
- **根因**：通用 metadata transaction rollback 没有区分“调用尚未成功的 namespace mutation”和“已向用户
  报告成功、仅由 fsync checkpoint 持久化的 data mutation”；两者错误地共享内存回滚策略。
- **修复**：为 `hfsMetaCommit` 增加运行时 mutation 保留标志。仅 `hfsMetaCommitInode` 在 intent 尚未发布时
  保留 dirty inode 和 `metaDirty`，使数据继续对当前进程可见并允许后续 `fsync` 重试；mkdir/create/unlink/
  rename/setattr 等未成功对外提交的 namespace mutation 仍恢复 stable image。若 intent 已发布，仍设置
  `metaRecoveryRequired` 并拒绝继续写入，以恢复协议为准。
- **验证**：`scripts/db/io_error_injection_test.sh` 覆盖 metadata-write 注入后的可见性与故障清除后的
  `fsync` 重试；2026-09-06 VM 强制 Make/CMake Release、POSIX 数据契约、metadata transaction、fd
  lifecycle、fast I/O、错误注入和真实 error-bdev flush 回归全部通过。

### 53. CMake 链接遗漏 lz4 系统库（已修复，一般级）

- **状态**：已修复
- **时间**：2026-09-06（shard-37/38 VM 验证）
- **等级**：一般（不影响 Makefile 构建，但 CMake Release 目标无法链接）
- **现象**：VM 上 `make -B app lib` 成功，而 `cmake --build build/cmake-release --parallel 4`
  在链接 `spdk-hfs` 时报告 `libspdk_accel.a(accel_sw.o)` 对
  `LZ4_createStream`、`LZ4_compress_fast_continue` 等符号的 `undefined reference`。
- **排查过程**：对比 Makefile 与 CMake 的最终链接命令。Makefile 命令包含 `-llz4`，CMake 的
  `HFS_SYS_LIBS` 只列出 `numa/dl/rt/uuid/ssl/crypto/m` 及 FUSE/AIO 库，没有 lz4；SPDK
  `accel_sw` 静态库因此无法解析 LZ4 符号。
- **根因**：两套构建系统的系统库清单没有保持一致，新增/启用 SPDK software accel 后 CMake
  路径漏同步 lz4 依赖。
- **修复**：将 `lz4` 加入 `CMakeLists.txt` 的 `HFS_SYS_LIBS`，保持其位于 SPDK/DPDK 静态库
  之后，供链接器解析静态库依赖。
- **验证**：2026-09-06 在 VM 上重新执行 `make -B app lib`、CMake Release、核心可靠性回归、
  MariaDB/MySQL 持久化恢复、RocksDB WAL/一致性恢复和数据库性能基准，全部通过；CMake
  Release 最终链接命令已包含 `-llz4`。

### 54. scaling smoke 的 stats-off 样本可继承外部统计环境（已修复，一般级）

- **状态**：已修复
- **时间**：2026-09-06（shard-39 收尾与 shard-40 基线）
- **等级**：一般（会污染统计开销对照，但不改变 HFS 数据正确性）
- **现象**：`scripts/db/hfs_scaling_benchmark.sh` 的 stats-off 运行只设置了
  `HFS_STATS_DISABLED=1`，而 HFS 初始化实际依据 `HFS_STATS_PATH`/`HFS_STATS` 判断是否启用；
  若调用环境预先导出了任一变量，所谓 off 样本仍会开启统计，导致 overhead 结果失真。
- **排查过程**：对照脚本的 `run_fio` 环境数组与 `hfsStatsInit` 的环境解析逻辑，发现
  `HFS_STATS_DISABLED` 并不是库支持的控制变量；在继承 `HFS_STATS_PATH` 的环境中可复现。
- **根因**：基准脚本没有清理继承的统计开关，把脚本内部标签误当成库侧禁用语义。
- **修复**：stats-off 分支显式通过 `env -u HFS_STATS_PATH -u HFS_STATS -u HFS_STATS_TIMING`
  清理所有统计开关；stats-on 仍使用本轮生成的独立报告路径。
- **验证**：脚本 `bash -n`、VM Make/CMake Release 和 fd/I/O/POSIX/runtime 回归均通过；
  stats-off/on 配对仍可输出可复现的独立样本。

### 55. metadata scaling 初版把数据容量耗尽误报为元数据失败（已修复，一般级）

- **状态**：已修复；基准脚本已改为元数据目录对象，并使用可配置的大容量稀疏镜像
- **时间**：2026-09-06（shard-42 元数据索引开发）
- **等级**：一般（污染扩展性结论，不改变 HFS 数据一致性）
- **现象**：初版 `metadata_scaling_benchmark.sh` 在 1 GiB 镜像中创建普通文件；每个文件仍会
  分配一个 data blob。约 1,022 个对象后 `open(O_CREAT)` 返回 `ENOMEM`，后续 open/stat/rename/
  unlink 被连锁计入 errors，导致 10K 样本被误判为元数据索引失败。
- **排查过程**：将失败位置缩小到第 1,022 个 create，检查 HFS bdev cluster 使用后确认是每个
  文件的 data blob 耗尽 1 GiB 后端容量，而不是 inode/dentry 索引冲突；改用目录对象后同一
  workload 在 10K inode 下 errors=0，SPDK submit/complete 仍闭合。
- **根因**：基准意图测量 namespace metadata，却同时把固定 1 GiB 的 data-cluster 容量纳入
  工作集；脚本没有把对象类型和镜像容量作为参数。
- **修复**：脚本默认创建目录并用 `O_DIRECTORY`/`rmdir` 覆盖 open/unlink 语义，保留
  `HFS_METADATA_OBJECT_TYPE=file` 作为显式选项；镜像大小通过 `HFS_METADATA_IMAGE_SIZE` 配置，
  默认 128 GiB sparse image。报告中明确记录对象类型和容量口径。
- **验证**：VM 上 1K/10K 目录样本均 errors=0；1K/10K 的 open/stat p99 保持在约 7/8 微秒，
  create/readdir/rename/unlink 的放大仍被单独记录为 v4 整镜像提交问题，不再与后端 ENOSPC 混淆。

### 56. v5 WAL 对齐 padding 被误当作逻辑 payload（已修复，严重级）

- **状态**：已修复；离线格式、WAL recovery、Make/CMake 构建均通过
- **时间**：2026-09-06（shard-43 v5 格式基础）
- **等级**：严重（所有非 4 字节对齐的 inode WAL 记录无法恢复，可能阻断提交事务重放）
- **现象**：v5 `SET_INODE` 记录的逻辑 payload 为 212 字节，WAL 记录头要求 8 字节对齐，编码后
  实际记录长度为 256 字节。恢复解析器直接将 216 字节（包含 4 字节零 padding）传给 inode
  应用函数，因长度必须等于 212 而拒绝整个事务；提交 marker 也存在同样的 4 字节对齐问题。
- **排查过程**：`scripts/db/metadata_v5_format_test.sh` 的 checkpoint round-trip 通过，但加入
  一条 SET_INODE + COMMIT 后 replay 失败并返回 `EIO`。对比 `v5EncodeRecord` 的对齐逻辑和
  `v5WalRecord` 的 `length - header` 语义，定位到解析层没有区分物理记录长度与逻辑 payload 长度。
- **根因**：格式设计只保存物理 record length，未额外保存逻辑 payload length；实现却按未对齐
  payload 长度做严格相等校验。
- **修复**：恢复端按记录类型声明逻辑长度（SET_INODE=4+208、COMMIT=4），要求尾部 padding
  不超过 7 字节且全部为零；checksum 仍覆盖物理 payload（含 padding），从而同时保证对齐和完整性。
- **验证**：VM 上 v5 格式测试覆盖 committed WAL、缺 commit WAL、页 checksum 损坏和 superblock
  A 回退；随后强制 Make、CMake Release 均通过。

### 57. shard-41 数据 I/O 每请求单独通知 owner queue（已修复，一般级）

- **状态**：已修复；同步语义保持不变，单 owner 的并行上限仍列为后续架构工作
- **时间**：2026-09-06（shard-41 性能修复迭代）
- **等级**：一般（吞吐扩展受限，不改变数据正确性）
- **现象**：4 KiB randrw、psync、iodepth=1 的 1/2/4/8/16 线程矩阵中，所有数据 I/O
  都通过一次独立的 `spdk_thread_send_msg` 和一次同步 completion；queue peak 随线程数升高，
  4 线程只有约 1.9 倍单线程吞吐。
- **排查过程**：shard-40 已移除 fd/runtime 全局热锁；shard-41 的统计显示 IO/metadata
  contention 上升但 owner queue、逐请求 completion 仍是固定串行点。对照 `HfsRuntimeSubmit`
  和 `hfsIoSubmit` 确认，数据面没有复用 metadata 的异步提交机制，无法把同一 burst 的请求
  合并到一次 owner 通知中。
- **根因**：同步 POSIX API 需要等待每个 SPDK completion，但提交通知粒度仍是每请求一次；
  在高并发下重复进入 SPDK message queue，放大 owner 线程的通知/唤醒开销。
- **修复**：新增 `HfsRuntimeBatchItem` 和 owner-side batch dispatcher；`hfsIoSubmit` 通过
  `hfsRequestWaitBatch` 链接调用者栈上的 request，多个并发请求共用一次 owner 通知后逐项提交
  SPDK I/O。batch dispatcher 只在显式 timing 模式记录每项 queue/owner 时间；`HFS_RUNTIME_BATCH=0`
  可回退到原始路径用于 A/B。请求仍在 completion 返回前保持栈生命周期，不改变 channel 归属。
- **验证**：VM 上 `make -B app lib`、CMake Release、`io_fast_path_smoke.sh`、8 线程 5 秒
  `runtime_stress.sh` 全部通过，错误数为 0；5 秒 A/B 在 4 线程为 24.35k（batch on）对
  24.73k（batch off），收益未宣称为绝对吞吐提升。单 owner 队列的结构性上限仍需 shard-45
  异步 API 和 shard-46 多 reactor/channel 解决。

### 58. 错误注入测试仍假设 metadata blob 每次操作都会 open（已修复，一般级）

- **状态**：已修复；测试改为注入实际存在的 metadata read-modify-write 请求
- **时间**：2026-09-06（shard-44 脏范围 checkpoint 基础）
- **等级**：一般（误报回归失败，不改变 HFS 数据正确性）
- **现象**：`io_error_injection_test.sh` 最后一项对 `mkdir` 注入 `metadata-open`，但 shard-42
  已将 metadata blob handle 提升为 mount 常驻句柄，正常元数据路径不再执行 open 请求，测试
  因此报告“injected request returned success: metadata-open”。
- **排查过程**：对照 `hfsMetaAcquireBlob` 的 resident 分支与错误注入操作表，确认该分支只
  绑定常驻 handle，不调用 `hfsRequestWait`；本轮范围写入在非 unit 对齐边界会真实执行
  `metadata-read` 的 RMW 请求。
- **根因**：测试契约落后于 shard-42 的常驻 metadata handle 优化，注入点不再属于可达路径。
- **修复**：将测试注入点改为 `metadata-read`，覆盖范围 checkpoint 的 RMW setup failure，
  同时保留失败后目录项不落盘的断言。
- **验证**：重新运行 `io_error_injection_test.sh` 后，提交失败、恢复、错误 bdev 和 metadata
  RMW 注入均应分别报告 EIO；常驻 handle 不再被错误地要求执行 open。

### 59. fsync 单文件提交会清除其他 inode 的脏状态（已修复，严重级）

- **状态**：已修复；数据 inode 以 ID 记录并在一个受限 metadata epoch 中合并提交
- **时间**：2026-09-06（shard-44 group commit 基础）
- **等级**：严重（跨文件 fsync 顺序可能导致已成功写入的数据在重启后丢失）
- **现象**：进程先写文件 B，再对文件 A 调用 `fsync(A)`。旧实现只把 A 传给
  `hfsMetaCommitInode`，但提交成功后直接把全局 `metaDirty` 清零；随后 `fsync(B)`
  看到“没有脏元数据”而跳过，B 的 size/mtime 仍只存在内存中，进程异常退出或重启后丢失。
- **排查过程**：审查 `hfs_lib_fsync`、`hfsPublishWriteEnd` 和 `hfsMetaCommit` 的状态流，
  确认 dirty 标记只有一个全局布尔值，而 v4 header checksum 却覆盖完整 inode 表；若只写
  一个 inode，还会让 header checksum 与磁盘上的其他脏 inode 不匹配。
- **根因**：没有按 inode 记录 dirty 集合，也没有把同步调用与一次 durable metadata epoch
  绑定；单 inode 提交错误地承担了全局 dirty 清理职责。
- **修复**：新增 mount 内的 dirty inode ID 集合和 `metaDurableEpoch`。每次成功数据写入
  登记 inode；`fsync` 把显式 inode 与其他 dirty inode 合并，在 v4 intent 的四个 after-image
  槽位内一次提交并只在该 epoch 成功后清空集合。超过四个槽位时走已有完整镜像保存路径，
  避免发布未受 intent 保护的 header checksum；inode 不存在（orphan）时仍可提交其他脏 inode。
  同时新增 `metadata_sync_count` 统计，区分 metadata blob sync 与设备 flush。
- **验证**：新增跨文件 fsync/reopen smoke，要求只 fsync A 后 B 仍可恢复；并复跑 metadata
  checkpoint、事务、路径并发、错误注入、崩溃恢复、fast I/O 与 runtime stress。

### 60. 同步 SPDK 请求误调用异步回调导致并发段错误（已修复，严重级）

- **状态**：已修复；VM 元数据并发、崩溃恢复、错误注入、快速 I/O、runtime stress、async、
  MariaDB/RocksDB 持久化回归均通过
- **时间**：2026-09-06（shard-45 public direct path）
- **等级**：严重（并发 preload/恢复 workload 可能在 SPDK owner thread 跳转到非法地址）
- **现象**：16 线程 metadata/path smoke 和 durability recovery 偶发或稳定以 SIGSEGV 结束；
  gdb 显示崩溃位于 SPDK completion 的 callback dispatch，回调参数指向栈上的同步
  `HfsIoRequest`，而 `hfsIoDone` 从该对象读取了一个非空、位于 worker stack 的伪函数指针。
- **排查过程**：单线程路径通过，16 线程路径复现；回溯确认崩溃发生在 `bs_request_set_complete`
  调用 `hfsIoDone` 后，非法跳转紧跟在 `request->asyncDone` 检查处。新增 public direct child
  path 后，`HfsIoRequest` 扩展了异步 completion 字段，但同步栈请求只有“字段应为 NULL”的
  隐含约定，缺少可验证的请求类型判别。对同步 I/O、async child 和 SPDK owner completion
  逐项比对后确定这是 callback metadata 误解释，而不是 blob 数据或 metadata checksum 错误。
- **根因**：同步与异步 child 共用 `HfsIoRequest` 和 `hfsIoDone`，completion 端只判断函数指针
  是否非 NULL；同步请求没有显式异步标识，栈对象在并发/复用场景下出现非零扩展字段时会把
  任意值当作 callback 调用，导致 owner thread 跳坏地址。
- **修复**：增加 `HFS_IO_ASYNC_MAGIC` 请求标识。只有 async child 在提交前写入 magic 且
  `asyncDone` 非 NULL 时，`hfsIoDone` 才交付父状态回调；所有同步 request 保持 callback
  字段不可达。该保护不改变同步等待、DMA lease 或 direct public request 生命周期。
- **验证**：VM 强制 Make/CMake Release；`metadata_transaction_smoke`、16 线程
  `metadata_path_concurrency_smoke`、error injection、durability crash recovery、fast I/O、
  8 线程 runtime stress、async pipeline、persistent smoke 和 RocksDB WAL/recovery 均返回 0，
  async `spdk_submit=81` 与 `spdk_complete=81` 闭合，`async_submit=20` 与
  `async_complete=20` 闭合，`io_pipeline_peak=4`。

### 61. 多 reactor direct write 完成与 metadata 操作发生锁反转（已修复，严重级）

- **状态**：已修复；direct write 的 metadata 发布改为 owner poller 非阻塞重试，并回原 data reactor 完成
- **时间**：2026-09-07（shard-46 多 reactor 专项）
- **等级**：严重（并发 open/create 与 direct write 会稳定超时，旧路径可能进一步 SIGSEGV）
- **现象**：设置 `HFS_DATA_REACTORS=2` 后，首个 4 MiB aligned direct write 可以完成，但并发创建下一个文件时 `open()` 超时；早期实现还出现 segfault。gdb 显示 SPDK owner 线程停在 `hfs_lib_async_direct_done -> hfsPublishWriteEnd -> pthread_rwlock_wrlock(g_metaLock)`，调用线程持有 `g_metaLock` 并在 `hfsMetaCreateFile -> hfsRequestWait` 等待 owner completion。
- **排查过程**：先确认数据 reactor completion 已到达，再检查 owner、async worker 和 Python 主线程栈；发现 direct completion 在 owner 回调中同步获取全局 metadata 写锁，而前台 open/create 持有同一锁等待异步 metadata I/O，形成“owner 等锁、调用者等 owner”的闭环。解除 `hfs_lib_open` 在 `hfsOpenById` 周围的锁只是降低一条路径的风险，不能覆盖 create/metadata commit，因此继续用 owner poller 复现并验证完整闭环。
- **根因**：跨 reactor completion 直接执行阻塞式 metadata 发布，且 metadata API 在持锁期间等待 SPDK completion；SPDK owner 被锁等待阻塞后无法处理释放该锁所需的 completion。
- **修复**：新增 `hfsTryPublishWriteEnd` 和 `hfs_direct_publish` owner poller；锁忙时只注册 1 ms 周期重试，不在 owner 线程睡眠或阻塞。发布成功后再向原 data reactor 投递 public request completion，保持回调线程归属和 open-file 引用生命周期。
- **验证**：reactor=2/4 多文件 direct aligned write、open/create 并发、async pipeline、metadata transaction/path、durability、error injection、fast path、runtime stress、MariaDB persistent 和 RocksDB WAL/recovery 全部通过。

### 62. 大范围 range lock 把不同文件错误串行化（已修复，严重级）

- **状态**：已修复；大范围操作使用 per-inode `rangeGate`，小范围操作继续使用有序全局 hash bucket
- **时间**：2026-09-07（shard-46 多 reactor 专项）
- **等级**：严重（4 MiB truncate/write 会覆盖全部 256 个 bucket，导致不同 inode 的 I/O 长时间互相阻塞）
- **现象**：多文件测试在文件 0 完成 direct write 后，主线程对文件 1/2 执行 `ftruncate(4 MiB)` 时卡住；gdb 显示主线程与 async worker 都在 `hfsIoRangeLock` 的同一全局 `rangeLocks[]` 上等待，尽管 blob/inode 不同。
- **排查过程**：审查 range bucket 计算发现 4 MiB / 4 KiB = 1024 个 unit，遍历过程会收集全部 256 个哈希 bucket；因此一次大范围操作等价于 mount 级排他锁。确认 data reactor/channel 分配不是阻塞根因后，将大范围路径与 per-inode runtime 对照测试。
- **根因**：固定数量的全局哈希锁只适合小范围区间；当请求覆盖的 unit 足够多时，哈希集合饱和为全表，跨 inode 产生不必要的锁冲突，并与同步 truncate 等待形成队头阻塞。
- **修复**：为 `HfsInodeRuntime` 增加 `rangeGate`。当请求覆盖至少四分之一 bucket 时仅获取该 inode 的 gate 写锁，不再抓取全局 bucket；小范围请求先获取 gate 读锁，再按 bucket 编号升序加锁，释放时反序释放，保留交叉区间的死锁顺序保证。
- **验证**：reactor=2（16 文件，64/64 submit-complete）与 reactor=4（32 文件，128/128，3 个 data reactor 均有计数）通过，既有 fast path、crash、stress 和数据库回归无回退。

### 63. HFS 数据面仍明显慢于原生 SPDK direct NVMe（第一轮已修复，仍有残余优化项，严重级）

- **状态**：第一轮修复已完成；普通 bounce I/O 已接入 data reactor，写入元数据发布已延迟合并；DMA direct path、range lock 和 64 KiB 尾延迟仍需后续优化
- **时间**：2026-09-08（direct-NVMe 读写分离专项）
- **等级**：严重（不影响数据正确性，但限制读写吞吐和尾延迟）
- **现象**：在同一块 `/dev/nvme0n1` 上，关闭重型 timing 统计后做等口径对照，4 KiB
  QD1 的 HFS 读/写分别为 17.1k/15.8k IOPS，原生 SPDK 分别为 24.9k/22.6k，
  HFS 低约 30%；QD32 的 HFS 读/写为 74.8k/32.7k，原生 SPDK 为 122.1k/87.9k，
  HFS 低约 39%/63%。64 KiB QD32 时 HFS 读/写仅 11.2k/10.3k，原生 SPDK 为
  44.7k/35.7k，HFS p99 延迟约 79–98 ms，而原生 SPDK 约 1.8–2.0 ms。所有样本
  errors=0。
- **排查过程**：
  1. 旧阶段脚本存在口径差异：HFS 使用 `psync`，原生 SPDK 使用单 job
     `iodepth=32`。改为 HFS `numjobs=1/32` 对齐 SPDK `iodepth=1/32` 后，差距仍稳定存在。
  2. 低开销 HFS 统计显示 `io_direct_ops=0`，而 `io_copy_ops` 与 I/O 操作数完全相等，
     说明普通 FIO 用户缓冲区没有通过 `spdk_vtophys`，每个请求都走 DMA pool 和一次
     用户缓冲区拷贝；原生 SPDK FIO 使用 DMA 缓冲区并直接异步提交。
  3. `hfsIoReadAt`/`hfsIoWriteAt` 每个请求都获取 inode sync/range 锁，经 owner/batch
     队列提交后同步等待 completion；这与原生 SPDK 的异步 outstanding queue 不同。
  4. `hfsPublishWriteEnd` 在每次成功写入后获取全局 metadata 写锁、更新时间戳并标记
     dirty；统计中 `meta_write_lock` 基本等于 `io_write_ops`，即使文件容量已经预分配也
     重复执行。
  5. 64 KiB 请求覆盖多个 unit；固定 64 KiB DMA class 的目标 buffer 数只有 8 个，且
     每个请求持有多个 range bucket，QD32 时形成明显排队和尾延迟放大。
- **根因**：HFS 数据面是“普通用户缓冲区 bounce copy + POSIX 锁/元数据语义 + owner
  同步等待”的组合路径；原生 SPDK 是“DMA buffer + 异步队列 + 无文件级元数据/范围锁”。
  因此当前差距主要来自 HFS 上层路径和并发模型，而不是 NVMe 设备或 SPDK bdev 本身。
- **本轮修复**：
  1. `hfsIoSubmit` 及 read/write pipeline 的 bounce request 统一选择
     `hfsRuntimeDataReactorFor(blobId)` 对应的 channel；data reactor 新增 batch dispatcher，
     避免逐请求 `spdk_thread_send_msg` 破坏原有批处理。
  2. 为 inode runtime 增加 logical EOF 与 pending mtime/ctime 原子状态；不扩展文件的
     write 只记录延迟元数据，不再每次获取全局 metadata 写锁；`fsync` 批量 drain/commit，
     `stat/fstat` 在提交前叠加 pending 时间戳，扩展写、truncate 仍走锁保护路径。
- **本轮验证**：
  1. `HFS_DATA_REACTORS=4` 的多 reactor accounting 为 `192/192`，32 线程 runtime stress、
     fast I/O、POSIX data contract、metadata recovery 全部通过。
  2. 4 KiB NVMe 对照（stats-off）：HFS QD32 read/write/randrw 为
     `110.4k/37.0k/40.2k IOPS`，原生 SPDK 为 `164.2k/114.9k/111.4k IOPS`；
     HFS QD1 read/write 为 `22.0k/21.0k`，原生为 `33.0k/30.8k`，所有 errors=0。
  3. `hfs_write_qd32` 的 `meta_write_lock=337` 对 `io_write_ops=369,579`，已从“每次写锁”
     降为批量级；`data_reactor_2_submit/complete=369,579/369,578` 的差异仅发生在统计快照
     与退出并发窗口，专项多 reactor 测试仍严格闭合。
  4. 64 KiB QD32 HFS read/write 为 `14.9k/13.5k IOPS`，相比旧基线 `11.2k/10.3k` 有
     改善，但仍明显落后原生 `60.6k/46.6k`，且 p99 仍在 `72–85 ms`，不能视为最终收口。
- **修复方向**：
  1. 为可验证的 DMA-capable 用户缓冲区增加真正的异步 direct path；普通缓冲区保留
     bounce fallback，并提供足够的 pipeline/outstanding depth。
  2. 将数据 I/O 与 mtime/ctime、dirty 标记解耦，采用按 inode/version 的延迟元数据发布，
     避免每次写入获取全局 metadata 写锁。
  3. 增大或改造 range lock 粒度，优先使用 per-inode/finer stripe；对已证明不重叠的
     aligned I/O 避免长时间持锁跨越设备 completion。
  4. 重新评估 DMA pool 各 size class 的 buffer 数和 backpressure，特别是 64 KiB 以上
     请求；所有读写对比必须使用相同 QD/并发和 stats-off 口径。
- **验证**：最新 VM 专项结果位于 `/mnt/hfs-data/performance-hfs-vs-spdk-rw-notiming`
  和 `/mnt/hfs-data/performance-hfs-vs-spdk-64k`；第一轮代码修复待提交后继续做 direct DMA、
  range lock 和 64 KiB buffer/backpressure 优化。

### 64. data reactor 逐请求投递导致 bounce read 队头阻塞（已修复，严重级）

- **状态**：已修复；data reactor 增加与 owner 等价的 batch dispatcher
- **时间**：2026-09-08（shard-47 性能回归）
- **等级**：严重（无数据错误，但会使开启多 reactor 的读吞吐崩溃）
- **现象**：首次把 pipeline bounce request 直接切换到 data reactor 后，4 KiB QD32 read
  从约 85.6k IOPS 降至约 1.1k IOPS，p99 超过 170 ms；write 受影响较小。
- **排查过程**：多 reactor accounting 和 completion 均正常，确认不是 NVMe 错误或未完成请求；
  对比 owner-side batch 与 data-side 单消息路径后，发现原 owner 路径会把并发请求合并为一次
  `spdk_thread_send_msg`，而 data reactor 改动按请求发送消息，导致 reactor 消息调度和同步
  completion 成为队头瓶颈。
- **根因**：data reactor 只提供逐消息提交接口，没有对应的批量队列；高并发 bounce read
  的消息数与 I/O 数相同，放大跨线程唤醒、锁和 futex completion 成本。
- **修复**：为每个 data reactor 增加独立 batch queue/dispatcher；`hfsIoStartRequest`
  通过 `hfsRequestSubmitDataBatch` 投递，保留 `HFS_RUNTIME_BATCH=0` 的逐请求回退语义。
- **验证**：修复后孤立 4 KiB read QD32 达到 `115.4k IOPS`、p99 `528 µs`；完整对照
  达到 `110.4k IOPS`，errors=0；reactor=4 多文件专项 `192/192` 闭合。

### 65. shard-49 vector I/O 使用字节偏移选择 stripe（已修复，一般级）

- **状态**：已修复；vector 路径现在把字节偏移转换为 logical unit 后再调用 stripe 选择器
- **时间**：2026-09-09（shard-49 stripe-to-reactor 开发）
- **等级**：一般（不直接破坏数据，但会让 vector 与 direct/pipeline 路径进入不同的 reactor 同步域）
- **现象**：stripe 选择器的接口语义是 `logicalUnit`，新增 vector 路由初版却直接传入了 `offset` 字节值；
  对 4 KiB unit 的 vector I/O，选择器会把偏移放大 4096 倍计算 stripe，导致同一逻辑 stripe 的
  vector/direct 请求路由不一致。
- **排查过程**：逐一核对 direct、bounce、vector、pipeline 和 async child 的调用点，发现 vector
  请求结构内部使用 `offset / unitSize`，但调用选择器的位置仍保留字节偏移；`git diff --check` 无法发现
  这种单位错误，需结合接口注释和 request 字段语义检查。
- **根因**：路由接口新增后缺少统一的单位命名/转换约束，调用方混用了 public byte offset 和内部 logical unit。
- **修复**：vector 调用改为 `offset / file->fs->unitSize`；其它 direct、pipeline、async child 和
  metadata publish 调用均确认使用 logical unit。
- **验证**：VM 上 `make -B app lib`、CMake Release、单 blob stripe 并行读写/punch 测试，以及
  2/4/8 reactor 异步多文件回归全部通过，submit/complete 无差异。

### 66. data reactor 统计快照出现伪 submit/complete 不平衡（已修复，一般级）

- **状态**：已修复；data reactor submit/complete 使用独立原子计数，分片统计仍保留用于聚合诊断
- **时间**：2026-09-09（shard-50 per-reactor DMA pool 回归）
- **等级**：一般（不影响 I/O 数据和实际 completion，但会误报资源泄漏并阻断自动化验收）
- **现象**：高并发专项退出时偶发看到单个 reactor 的 `submit` 与 `complete` 相差 1～2，随后
  `active/submit/complete` 总计有时也短暂不一致；同一 workload 重新运行又可能闭合。
- **排查过程**：逐项确认 SPDK completion、DMA lease acquire/release 和 runtime active 计数均已闭合；
  发现统计使用 thread-local shard，submit 在 data reactor 线程写入，而进程退出快照可能在另一个
  线程读取，快照时机与 shard 合并存在竞态。该差异只出现在观测值，数据校验和实际请求状态不受影响。
- **根因**：用非原子 thread-local 计数作为跨线程、跨 shard 的即时快照，缺少退出阶段的同步边界。
- **修复**：增加按 reactor 的 `_Atomic uint_fast64_t` submit/complete 计数；记录路径同步更新原子值，
  snapshot 直接读取原子数组，同时保留原 shard 字段供周期聚合和诊断使用。随后发现 waiter 会在
  `hfsIoDone` 的 reactor-complete 统计之前被唤醒，故将 complete 统计移到 completion wake 之前，
  使统计闭合与请求生命周期具有同一发布顺序。
- **验证**：VM 上强制 Make/CMake、stripe accounting、DMA pool backpressure、request-local fast I/O
  和 reactor=2/8 多 reactor 回归通过；最新 4-reactor stripe 测试 `266/266`，DMA lease
  `128/128`，重复退出快照未再出现伪不平衡。

### 67. 异步 range lock 跨线程释放导致后续 I/O 永久阻塞（已修复，严重级）

- **状态**：已修复；同步/异步 I/O 共用线程无关的 range lock，直接窗口读写与既有契约回归通过
- **时间**：2026-09-09（shard-57 registered direct window 开发）
- **等级**：严重（不一定立即报错，但会使同一 inode 的后续读请求和进程退出永久卡住）
- **现象**：registered DMA buffer 的异步 pwrite 完成后，紧随其后的同范围 async pread 不返回；
  `hfsAsyncWait` 超时，进程退出时析构函数卡在等待后台 worker。问题只在异步路径出现，普通同步
  I/O 和单独 write 回归可能掩盖它。
- **排查过程**：gdb 检查卡住的 worker，确认它在 `hfsIoRangeLock` 等待 `runtime->rangeGate`；
  主线程提交阶段先执行 `pthread_rwlock_try*`，SPDK completion 回调所在 reactor 线程随后执行
  `pthread_rwlock_unlock`。glibc 的 rwlock 保留了提交线程的 writer owner/TID（`__cur_writer`
  非零），跨线程释放后内部计数不再可用于下一次获取。临时 range-lock 诊断和同一进程 write→read
  复现稳定重现该状态，排除了设备 completion 与 DMA buffer 生命周期问题。
- **根因**：POSIX `pthread_rwlock_t` 要求锁的使用协议满足其实现的 owner/状态约束；HFS 却把
  range lock 的获取放在调用线程、释放放在异步 completion 线程，形成未定义的跨线程 unlock。
- **修复**：新增 `HfsRangeLock`，用 mutex/condition 维护 readers、writer 和 waiting writers，
  解锁不依赖线程身份；mount 级 hash buckets 与 per-inode large-range gate 全部切换到该类型，
  同步阻塞、异步 try-lock 和反序释放仍遵循原有冲突/死锁顺序。`syncLock` 等真正的线程归属锁
  保持 pthread rwlock，不扩大替换范围。
- **验证**：VM 上强制 Make/CMake Release、registered direct window（512 KiB read/write、
  两个 direct children、`copy_ops=0`）、registered DMA 生命周期、request-local trace、DMA
  pool backpressure、POSIX contract 全部通过；write→read async 回归不再阻塞，SPDK
  submit/complete 保持闭合。

### 68. completion 先唤醒 waiter 后继续访问栈 request 导致段错误（已修复，严重级）

- **状态**：已修复；completion trace 在唤醒同步 waiter 前完成，异步 callback 元数据提前快照
- **时间**：2026-09-09（shard-59 性能矩阵回归）
- **等级**：严重（会使 FIO/数据库进程随机 SIGSEGV，且通常只在退出或 JSON 输出路径暴露）
- **现象**：Aio FIO 使用 `--output-format=json` 时偶发 `libhfs.so` 段错误；普通文本输出和
  短样本可能通过。内核日志显示 fault 位于 `hfsIoDone`，地址落在
  `request->file->fs->unitSize` 的解引用。
- **排查过程**：将 fault 地址按 VM 当前 ELF text load 偏移还原到
  `src/hfs_io.c:822`；反汇编确认 `hfsIoDone()` 先执行 `hfsRequestComplete()`，该函数
  会唤醒同步 waiter，随后 completion 线程又继续读 `request->file`、`request->units` 和
  trace 字段。FIO JSON 的收尾时序更容易让 waiter 释放栈上 request，复现由低概率变为稳定。
- **根因**：同步 request 的生命周期只保证到 `hfsRequestComplete()` 被调用前；唤醒 waiter
  后，completion 回调仍把 request 当作有效对象使用，形成 use-after-return/use-after-free。
- **修复**：在唤醒前快照 trace 所需的 request/file 字段、完成 units 和异步 callback 指针；
  先导出 completion trace，再调用 `hfsRequestComplete()`，之后只使用已快照的 callback，
  不再访问 waiter 可能已经回收的 request 内容。
- **验证**：修复后 Aio FIO `psync` 文本/JSON 输出重复样本、shard-58 request reduction、
  I/O error injection 和后续 shard-59 矩阵回归均无段错误；SPDK submit/complete 保持闭合。

### 69. MariaDB bootstrap 强制回收后立即重启触发 backend lock 竞争（已修复，一般级）

- **状态**：已修复；性能测试入口等待共享 backend flock 可再次获取后才启动服务
- **时间**：2026-09-09（shard-59 性能矩阵）
- **等级**：一般（只影响性能测试脚本的启动稳定性，不改变 HFS 数据语义）
- **现象**：MariaDB bootstrap 在已提交系统表后出现 signal-handler 超时，测试器按既定
  边界回收进程组；紧接着启动 HFS server 时，路径操作返回 `ENODEV`。
- **排查过程**：检查 HFS 日志发现新进程的 `hfs_acquire_backend_lock()` 返回 `EBUSY`；
  `lslocks`/精确 PID 检查确认旧 bootstrap 子进程的 `HFS_LOCK_PATH` flock 释放存在短暂
  延迟。等待锁重新可获取后，新的 HFS 进程即可完成 mount 并正常启动。
- **根因**：测试脚本只等待 bootstrap leader 的 `wait`，没有等待所有 MariaDB 子进程的
  文件锁句柄释放，导致下一进程在 mount 前被拒绝。
- **修复**：`mysql_stage2_benchmark.sh` 在 bootstrap 边界后以
  `HFS_MYSQL_LOCK_WAIT_SECONDS`（默认 15 秒）轮询 `flock -n`，锁释放后再启动 HFS
  server；shard-59 同时规范化 wrapper 的换行并显式启用 signal 边界。
- **验证**：干净 VM 上 shard-59 FIO、RocksDB、MariaDB/MySQL 矩阵全绿，输出
  `MYSQL STAGE2 OK`、`SHARD59 PERFORMANCE MATRIX OK`。

### 70. NVMe FIO 对照脚本错误使用 Malloc0 且误判 SPDK PCI 设备缺失（已修复，一般级）

- **状态**：已修复；raw SPDK bdev 和 NVMe 前置检查均按 SPDK 直通设备语义处理
- **时间**：2026-09-09（当前 NVMe FIO 回归）
- **等级**：一般（不影响 HFS 数据，但会使性能结论失真或跳过应执行的测试）
- **现象**：`fio_stage2_compare.sh` 即使传入 `config/hfs_nvme.json`，raw SPDK 分支仍固定
  使用 `--filename=Malloc0`；`shard59_performance_matrix.sh` 仅检查 `/dev/nvme*`，而 SPDK
  绑定 PCI 控制器后内核节点本来就不存在，导致 direct NVMe 矩阵被错误标记为 SKIP。
- **排查过程**：VM 上 `lsblk` 无 `/dev/nvme*`，但 `lspci -Dnn` 显示
  `0000:0b:00.0 VMware NVMe SSD Controller`，SPDK plugin 和 NVMe JSON 均存在；首轮命令
  因 library 路径不匹配退出后，核对 helper 发现 raw 文件名硬编码为 `Malloc0`。将 raw
  目标改为可配置 bdev 后，HFS 与 raw 均成功访问 `Nvme0n1`，所有 FIO errors=0。
- **根因**：测试脚本把“内核 NVMe 节点”错误当作“SPDK NVMe 可用”的必要条件，并遗留
  Malloc 基线的固定 bdev 名称，未区分 SPDK PCI 直通模型。
- **修复**：新增 `HFS_FIO_SPDK_BDEV_NAME`（默认保持 `Malloc0` 兼容），direct NVMe 分支
  设置 `Nvme0n1`；前置检查接受配置中 `traddr` 对应的 PCI 控制器，并记录 BDF/存在性。
  同时增加 `HFS_FIO_NUMJOBS`，用于 HFS `numjobs=32` 与 raw SPDK `iodepth=32` 的同盘对照。
- **验证**：VM 上 `bash -n` 通过；NVMe 4K/8K/16K/32K/64K 两轮 FIO 均完成，HFS/raw
  errors=0，结果保存在 `/var/tmp/hfs-fio-nvme-current-20260909-r1`（单线程）和
  `r2`（HFS numjobs=32）。

### 71. 大范围 I/O 的 rangeGate 未进入统计计时（已修复，一般级）

- **状态**：已修复；新增独立 `range_gate` wait/hold 类别，并覆盖阻塞与跨线程释放的 try-lock direct async 路径
- **时间**：2026-09-09（NVMe FIO stats attribution）
- **等级**：一般（可观测性缺口）
- **现象**：带 `HFS_STATS_TIMING=1` 的 `numjobs=32` NVMe FIO 中，64 KiB 样本的
  `lock_io_wait_ns=179`、`lock_io_contention=0`，但 HFS p99 达到 41.7 ms，吞吐只有
  raw SPDK 的 9.8%；4/8/16/32 KiB 样本则出现数百秒累计 `lock_io_wait_ns` 和大量竞争。
- **排查过程**：核对 `src/hfs_io.c` 的 `hfsIoRangeLock()`：小范围请求在每个 bucket
  外围调用 `hfsStatsLockBefore/After/ReleaseFast(HFS_STATS_LOCK_IO)`；达到
  `HFS_IO_RANGE_GATE_THRESHOLD` 的大范围请求直接对 `runtime->rangeGate` 调用
  `hfsRangeLockWriteLock()`，没有对应的 stats timing。64 KiB 请求因此绕过了当前统计，
  并不能证明它没有等待锁。
- **根因**：range bucket 和 per-inode rangeGate 复用了同一个逻辑 guard，但统计埋点只覆盖
  bucket 分支，未覆盖 gate 的阻塞/持有生命周期。
- **修复**：新增 `HFS_STATS_LOCK_RANGE_GATE` 与 `lock_range_gate_*` 快照字段；阻塞
  `hfsRangeLockRead/WriteLock` 在获取前后记录 wait，释放时记录 hold。非阻塞 direct async
  try-lock 增加失败取消 timing frame，成功路径把 hold 起点挂在 guard 上并在 completion
  线程直接入 shard；同时修复失败 bucket try-lock 的 wait frame 泄漏。direct async parent
  完成时补记 `io_direct_ops/io_direct_bytes`。
- **验证**：VM 强制 `make -B lib` 通过。NVMe 64 KiB、numjobs=32、stats timing 样本得到
  `lock_range_gate_wait_count=59588`、`lock_range_gate_hold_count=59588`、累计 wait
  `465007278381 ns`、hold `6834935917 ns`，不再误报为零等待；`spdk_submit/complete`
  `59757/59757`、errors=0。registered direct window 512 KiB read/write 通过，trace
  `registered_children=4,copies=0`，统计 `io_direct_ops=2,io_direct_bytes=1048576`；
  registered DMA 生命周期测试也通过。

### 72. 单 inode rangeGate 导致大块 I/O 全局排队且掩盖真实 backend 并发（已修复，严重级）

- **状态**：已修复；数据路径改为 per-inode range stripes，并补充真实 SPDK backend outstanding
  统计
- **时间**：2026-09-09（rangeGate 修复与 NVMe FIO 复测）
- **等级**：严重（64 KiB 高并发吞吐和尾延迟显著恶化）
- **现象**：前一轮 64 KiB、`numjobs=32` FIO 中，`rangeGate` contention 为 53,900/59,588，
  平均等待约 7.80 ms，stats-on 吞吐约 3.7 kIOPS；`runtime_inflight_peak=1` 又被误解为
  HFS 没有异步并发。
- **排查过程**：确认大范围路径把同一 inode 的请求升级为 `runtime->rangeGate`，而小范围路径
  还要访问全局 hash bucket；不同文件也可能因为 hash 冲突相互等待。进一步检查
  `hfsRuntimeSubmitDataBatch()` 发现 per-reactor linked submission queue 已存在，
  `runtime_queue_depth_peak` 可到 26，但旧统计只显示 dispatcher callback 深度，不显示
  SPDK data callback 的真实 outstanding。
- **根因**：单个 inode gate 的粒度过粗，且 writer-priority 在高并发读写下放大排队；统计字段
  混淆 dispatcher depth 与 backend outstanding，掩盖了“队列已经异步提交”的事实。
- **修复**：每个 inode 增加 64 个 range stripe，每 stripe 覆盖 64 logical units；按升序获取、
  逆序释放并保留 bounded reader burst=8，数据热路径不再使用 `rangeGate`。新增
  `io_backend_outstanding`/`io_backend_outstanding_peak`，在 `spdk_blob_io_*` 提交和完成处
  统计真实后端窗口；同步 POSIX 继续等待 completion，异步调用使用既有 `hfs_lib_async_*`。
- **验证**：VM 上 Make/CMake、fast I/O、I/O concurrency（770/770）、4-reactor scaling（192/192）
  和 registered-DMA/direct smoke 全部通过。修复后 stats-on `range_gate_wait/hold=0`、
  `runtime_queue_depth_peak=26`、`io_backend_outstanding_peak=31`；stats-off 同口径 HFS
  64 KiB 为 30,233 IOPS，raw SPDK 为 35,287 IOPS，errors=0。

### 73. direct-DMA 完成先唤醒 waiter 导致退出阶段 blob close 竞态（已修复，严重级）

- **状态**：已修复；direct completion 在发布用户完成前先释放 `HfsOpenFile`，确保
  `hfsAsyncFini()` 的 in-flight 计数覆盖全部内部清理
- **时间**：2026-09-09（direct-DMA 异步并发矩阵）
- **等级**：严重（单 reactor、depth>=2 时可使 Python/FIO 进程在退出阶段随机 SIGSEGV）
- **现象**：`HFS_DATA_REACTORS=1` 的 direct-DMA 矩阵偶发在 depth=2 完成数据操作后段错误，
  内核 fault 位于 `spdk_blob_close()`；同一测试低轮数可能通过，导致问题具有时序相关性。
- **排查过程**：重复执行 1/2/4/8/16/32 深度矩阵并保留 dmesg。故障地址按共享库
  executable segment 偏移还原到 `spdk/lib/blob/blobstore.c:9291`。检查 direct async
  完成链发现 `hfsAsyncCompleteDirect()` 会先广播 `done` 并递减 `directInFlight`，
  随后 `hfs_lib_async_direct_done/finish()` 才释放 `HfsOpenFile`；进程退出可在后者仍
  访问 blob 时进入 `hfs_lib_fini()`/`spdk_bs_unload()`。
- **根因**：异步 API 对外宣称完成时，内部 open-file/blob 引用尚未退休；`hfsAsyncFini()`
  因提前递减的 in-flight 计数无法等待该尾部清理，卸载 blobstore 与 `spdk_blob_close()`
  并发，形成 use-after-free/非法 blob 访问。
- **修复**：在正常 direct 完成、metadata commit 完成以及 ENOMEM 错误分支中，先调用
  `hfs_release_open_file()`，再调用 `hfsAsyncCompleteDirect()`，最后释放 direct context；
  如此 waiter、`directInFlight` 和 blob 生命周期都以内部清理完成为边界。
- **验证**：VM 强制 `make -B lib` 后，`HFS_DATA_REACTORS=1`、depth=1/2/4/8/16/32、
  64 KiB、16 rounds 连续 3 轮全部通过（errors=0、copy_ops=0、async submit/complete
  均为 32/64/128/256/512/1024）；reactors=4 主矩阵继续通过。

### 74. DMA pool slow acquire 与 teardown 的 API 生命周期竞态（已修复，一般级）

- **状态**：已修复；pool closing bit 在 API 使用计数中原子发布，慢等待者整个 acquire
  生命周期持有 API 引用
- **时间**：2026-09-10（shard-61 POSIX 兼容路径实现复核）
- **等级**：一般（主要影响关闭/卸载窗口，正常 I/O 不触发）
- **现象**：原 pool fast path 只在 CAS 前短暂增加 `apiUsers`；当 class 无空闲 slot 时，
  该引用在进入 condition wait 前已释放。teardown 可能观察到 `apiUsers==0` 并释放
  `HfsIoPoolShard`，随后慢路径重新取得已销毁的 mutex/condition，存在 UAF 风险。
- **排查过程**：对 free-mask/CAS 与 `hfsIoAcquireSlow()` 的时序进行交叉检查，构造
  “fast path 返回 exhausted → fini 设置 closing → waiter lock”序列；发现独立的
  `closing` bool 无法阻止关闭检查后的新 API 进入。
- **根因**：pool 生命周期闸门与单次 slot 获取绑定，而不是覆盖整个可能阻塞的 acquire；
  `closing` 和 `apiUsers` 分离造成 check-then-free 窗口。
- **修复**：`apiUsers` 最高位作为 `HFS_IO_POOL_API_CLOSING`，teardown 先原子置位，
  新 API 通过 CAS gate 拒绝进入；`hfsIoAcquire()`/`hfsIoTryAcquire()` 从进入到快/慢
  路径结束都保留引用，fini 等待 active lease 和 API users 清零后才释放 DMA/mutex。
- **验证**：VM 上 `make -B app lib`、CMake Release、DMA backpressure、32 线程 runtime
  stress、inode/range concurrency、fdatasync coordinator 和所有 crash/error injection
  回归通过，acquire/release 计数闭合且无关闭超时。

### 75. VM 重启后 NVMe 恢复为内核驱动导致 HFS 无法挂载（已处理，环境级）

- **状态**：已处理；测试前通过 SPDK `scripts/setup.sh config` 重新绑定设备，并保留
  1 GiB hugepage 预留
- **时间**：2026-09-10（VM 重启后的 NVMe FIO 映射复核）
- **等级**：一般（环境初始化问题，不改变 HFS 数据语义）
- **现象**：HFS 使用 `config/hfs_nvme.json` 启动时返回 `ENODEV`，FIO 报
  `/hfs: No such device`；此时 PCI NVMe 控制器仍可在 `lspci` 中看到，但由内核
  `nvme` 驱动占用，且重启后 hugepages 计数为零。
- **排查过程**：确认没有 fio、SPDK 或 HFS 测试进程；`lspci -Dnnk -s 0000:0b:00.0`
  显示 `Kernel driver in use: nvme`，`HugePages_Total=0`。设置
  `vm.nr_hugepages=1024` 后仍无法 attach，说明根因不是内存而是 PCI 驱动绑定。
- **根因**：VM 重启不会持久化本项目测试所需的 SPDK 用户态设备绑定；NVMe 被内核
  `nvme` 驱动重新接管，SPDK 无法通过 `traddr=0000:0b:00.0` 探测控制器。
- **处理**：在 VM 内执行
  `PCI_ALLOWED=0000:0b:00.0 DEV_TYPE=NVME SKIP_HUGE=yes ./scripts/setup.sh config`，
  并在测试前设置 `sysctl -w vm.nr_hugepages=1024`；确认设备显示为
  `uio_pci_generic` 后再启动 HFS。不得把 Windows 侧 `spdk/` 目录同步到 VM。
- **验证**：HFS 与 raw SPDK 均使用 `Nvme0n1` 完成 qd1/qd32 FIO，所有 errors=0；
  原生本地对照文件位于 `/mnt/hfs-data`，结果保存在
  `/mnt/hfs-data/performance-stage3-nvme-fio-shard61/{qd1,qd32}`。

### 76. 多生产者直接提交 SPDK 消息导致 DPDK ring reservation stall（已修复）

- **状态**：已修复（shard-62）
- **时间**：2026-09-10
- **等级**：严重
- **现象**：64 线程 POSIX fd churn 在 `open/close` 阶段长时间无响应；主线程等待
  worker join，多个 worker 卡在 `spdk_thread_send_msg`，测试在 120 秒超时。单线程、
  8 线程及 data reactor I/O 正常。
- **排查过程**：保留远端进程后用 gdb 查看线程栈，确认 owner 线程在
  `spdk_thread_poll`，64 个生产者停在 DPDK `rte_wait_until_equal_32` →
  `__rte_ring_update_tail` → `spdk_thread_send_msg`。`g_runtime.submitters=64`，
  owner/data queue 深度为 0，排除 HFS completion 或 bounded credit 泄漏。
- **根因**：所有 POSIX metadata/open/close 请求同时直接调用 SPDK 的多生产者 lockless
  ring。某生产者在保留 slot 后被调度延迟，后续生产者自旋等待 tail 提交；owner 在
  ring tail 前看不到后续消息，形成 reservation stall。
- **修复**：新增 runtime 级 `messageSubmitLock`，所有 owner、data、batch、stop 消息
  通过 `hfsRuntimeSendMessage()` 串行进入 SPDK ring；锁不跨 callback 持有，不改变
  POSIX 顺序或 data reactor 的 bounded credit。queue 满时仍使用 direct-submit 溢出
  路径，避免在 range lock 持有期间等待 queue credit。
- **验证**：VM 上 `fd_lifecycle_smoke`（含 64×250 churn）、data reactor stripe、
  DMA backpressure、runtime stress、POSIX contract、metadata transaction 和 IO trace
  全部通过；`spdk_submit=spdk_complete`，errors=0，trace 1621 records/27 phases。

### 77. Ticket 初始化失败路径读取未初始化状态并遗漏 waiter 清理（已修复）

- **状态**：已修复（shard-63）
- **时间**：2026-09-10
- **等级**：一般（资源分配/关闭异常窗口；正常路径不触发）
- **现象**：POSIX read/write 在 ticket mutex/condition 初始化失败，或 ticket admission 被
  stopping 状态拒绝时，错误分支可能读取未初始化的 `ticket.admitted`；初始化成功但 admission
  失败时还可能跳过 `pthread_mutex_destroy`/`pthread_cond_destroy`。
- **排查过程**：复核 `hfsIoReadAt()`/`hfsIoWriteAt()` 的短路条件发现，`HfsPosixIoTicket`
  原先是栈变量，`hfsPosixTicketInit()` 返回错误后仍检查 `ticket.admitted`；同一条件也没有
  记录“wait state 已初始化但尚未 admitted”的阶段。
- **根因**：把 init、admit、wait 三个有不同清理责任的阶段压缩为一个布尔表达式，未区分
  mutex/condition 的所有权。
- **修复**：ticket 置零初始化并新增 `ticketInitialized` 标记；只在 init 成功后调用 admit，
  所有 init 成功的失败分支都执行 complete（如已 admitted）和 destroy。
- **验证**：VM 上重新 `make -B app lib`、CMake Release、POSIX range-ticket smoke、全量
  Shard-62 回归、MariaDB/RocksDB recovery 和 NVMe FIO，均通过且 errors=0。

### 78. Group-commit 对同一 inode 重复获取 syncLock（已修复）

- **状态**：已修复（shard-64）
- **时间**：2026-09-10
- **等级**：严重（同一文件多线程 fsync 返回 EAGAIN/EBUSY）
- **现象**：开启 `HFS_DURABILITY_GROUP_COMMIT=1` 后，多个线程对同一文件并发
  `fsync`，首个请求成功，其他请求返回 `BlockingIOError(EAGAIN)`；runtime stress
  在 16/32 线程下出现批量 worker failure。关闭 group commit 时同一测试通过。
- **排查过程**：最小 4 线程用例确认 open/read/write 全部成功，只有 fsync 失败；
  日志显示请求在同一个 coordinator batch 中进入 `hfs_lib_process_durability_batch`。
  逐项检查批处理锁集合后发现不同 `HfsOpenFile` 共享同一个 inode runtime。
- **根因**：批处理初版仅按 `HfsOpenFile *` 去重，dup/独立 open 的描述共享同一
  `runtime->syncLock`，worker 在同一线程第二次 `pthread_rwlock_wrlock`，导致批次
  被错误中止；失败分支还可能留下当前描述的 ioLock。
- **修复**：锁集合改为按 `HfsInodeRuntime *` 去重；每个 open-file description
  仍保持独立 `ioLock`，同一 runtime 只同步一次 blob；锁获取失败立即释放当前
  ioLock，已获取锁按逆序释放。补充 4 线程同 inode fsync 用例。
- **验证**：开启 group commit 后最小并发用例、16 线程 10 秒 runtime stress、
  persistent/fd/POSIX contract、durability barrier、metadata group commit 和
  crash-recovery 全部通过。

### 79. durability worker 未屏蔽宿主终止信号导致 MariaDB bootstrap 停滞（已修复）

- **状态**：已修复（shard-64）
- **时间**：2026-09-10
- **等级**：严重（数据库初始化退出阶段可永久等待）
- **现象**：开启 `HFS_DURABILITY_GROUP_COMMIT=1` 时，MariaDB
  `mariadb-install-db` 在 bootstrap 完成后停留在 `unireg_abort()`，日志出现
  `Signal handler thread did not exit in a timely manner`；关闭 group commit 的同一
  NVMe/Aio 测试正常完成。
- **排查过程**：对 bootstrap 进程做 gdb 全线程回溯，主线程在等待 MariaDB signal
  thread，HFS durability worker 仍可见为独立 pthread；对照 group-disabled 运行确认
  不是元数据格式、bdev flush 或恢复逻辑错误。新建 worker 时会继承调用线程的信号屏蔽
  状态，而 MariaDB 的终止信号由专用 signal thread 管理。
- **根因**：durability worker 没有像 HFS owner/log 线程一样屏蔽 `SIGHUP`、`SIGQUIT`、
  `SIGALRM`、`SIGTERM` 和 `SIGTSTP`，宿主进程的信号管理与 HFS 后台线程的继承状态不一致，
  使 MariaDB bootstrap 的终止阶段出现信号处理线程无法收敛。
- **修复**：新增 `hfsDurabilityBlockManagedSignals()`，worker 启动后立即屏蔽同一组宿主
  管理信号，使信号只留给宿主 signal thread；不改变 coordinator 请求队列、锁或持久化
  顺序。
- **验证**：group commit 开启时 MariaDB 持久化初始化、正常重启、SIGKILL 恢复和
  mysqlslap 并发均通过；关闭 group commit 的 legacy 路径亦通过。

### 80. epoch pipeline 将同尺寸 fdatasync 误判为 metadata 变更（已修复）

- **状态**：已修复（shard-65）
- **时间**：2026-09-13
- **等级**：一般（不破坏数据正确性，但会放大每次数据库 durability 请求的 metadata 写放大）
- **现象**：启用新的 mount 级 durability epoch pipeline 后，普通同尺寸覆盖的
  `fdatasync` 每次都触发 v4 blob metadata sync 和全局 metadata commit；stats 中
  `durability_metadata_commits` 随请求数增长，`fdatasync_fast` 统计不再出现，MySQL
  写吞吐没有达到预期。
- **排查过程**：对比 `fdatasync_smoke.sh` 期望的 `fdatasync_fast` 与 epoch Stage A/B
  的状态转换，发现 Stage A 在 data-only 请求上先无条件调用
  `hfsInodeRuntimeDrainPendingLocked()`，随后 Stage B 看到时间戳 overlay 已被清空，无法
  区分“仅数据覆盖”和“size/extent/dirty inode 变化”。
- **根因**：metadata pending drain 被放在 data-only 语义判断之前，判断本身破坏了判断
  所需的 dirty 状态；因此同尺寸覆盖被保守地当作 metadata transaction。
- **修复**：在 Stage A 持有 metadata read lock 时先检查 inode dirty、runtime logical size
  与稳定 inode size；只有确认需要 metadata 时才在 Stage B 的 metadata write lock 下
  drain pending overlay。若 metadata 未变化，Stage A 直接执行一次 device flush，Stage B
  跳过全局 metadata write lock，同时保留真实数据/设备持久化屏障。
- **验证**：VM 上 fdatasync stats、POSIX contract、fd lifecycle、runtime stress、三个
  crash boundary、MariaDB/RocksDB recovery 和 NVMe FIO 全部通过；`spdk_submit` 与
  `spdk_complete` 闭合，未发现新的严重或致命问题。

### 81. profiling 公开头文件缺少 size_t 定义（已修复）

- **状态**：已修复（task90150022 P0-1）
- **时间**：2026-09-15
- **等级**：一般（新增 profiling 模块无法完成编译）
- **现象**：VM 上首次执行 `make -B app lib` 时，`src/hfs_profile.c` 编译失败，编译器
  报告 `hfs_profile.h` 中 `hfsProfileBegin()` 的 `size_t` 未定义。
- **排查过程**：确认报错来自新增公共头文件，而不是调用方 include 顺序；该头文件只
  引入了 `stdbool.h`、`stdatomic.h` 和 `stdint.h`，没有提供 `size_t` 的标准定义。
- **根因**：公共函数声明直接使用 `size_t`，但遗漏了对应的 `<stddef.h>` 依赖。
- **修复**：在 `src/hfs_profile.h` 显式加入 `<stddef.h>`，使头文件自洽并不依赖间接
  include。
- **验证**：修复后重新执行 VM 强制构建，继续检查后续编译与链接结果。

### 82. trace sampled mode 保留无 parent 的事件导致 ring 溢出（已修复）

- **状态**：已修复（task90150022 P0-1）
- **时间**：2026-09-15
- **等级**：一般（只影响 profiling 证据完整性，不改变 HFS 数据语义）
- **现象**：8 job NVMe profiling probe 设置 `HFS_TRACE_SAMPLE=100` 后，profile 行本身
  可以保持单调，但 trace 快照仍报告大量 `dropped`，无法满足可信基线的
  `trace_dropped=0` 守恒检查。
- **排查过程**：检查 `hfsTraceEmitDetail()` 的采样条件，发现只有非零
  `parentId/requestId` 且不命中采样间隔时才丢弃；metadata、thread-wait 等没有关联
  parent 的事件仍全部写入每线程 4096 项 ring，在高并发运行中持续覆盖旧事件。
- **根因**：采样策略只过滤“已关联但未命中”的事件，没有把未关联事件视为当前采样
  窗口之外的数据。
- **修复**：采样间隔大于 1 时，`sampleId==0` 的普通事件和未命中的 parent/request
  事件统一丢弃；parent profile 使用同一规则，保持 profile 与 child trace 的采样一致。
- **验证**：重新构建后用 `HFS_TRACE_SAMPLE=100` 复测，检查 `trace_dropped=0`、profile
  单调性和 `spdk_submit=spdk_complete`。

### 83. profiling ring 容量不足以容纳文档规定的 sample=100（已修复）

- **状态**：已修复（task90150022 P0-1）
- **时间**：2026-09-15
- **等级**：一般（只影响 profiling 快照完整性）
- **现象**：过滤掉无 parent 事件后，8 job、5 秒、`HFS_TRACE_SAMPLE=100` 的 probe
  仍在每个 data thread 产生约 2.6–3.1K 条覆盖丢弃记录，无法稳定满足
  `trace_dropped=0`。
- **排查过程**：按线程统计快照中的 ring 记录和 dropped marker，确认不是 profile 单调性
  或 SPDK submit/complete 不闭合；单线程 owner 没有丢失，data thread 在 4096 项容量
  下因 sampled request 仍产生多阶段事件而覆盖旧记录。
- **根因**：原固定 4096 项容量面向低并发 trace smoke，低于任务规定的高并发采样
  workload 的短时事件峰值。
- **修复**：将每线程 trace ring 扩大到 16384 项；仍保持 per-thread ownership、无锁
  写入和 bounded snapshot，不改变 profiling 关闭时的正式性能路径。
- **验证**：重新构建后再次运行 sample=100 probe，要求 dropped=0、profile_invalid=0、
  errors=0 且 `spdk_submit=spdk_complete`。

### 84. 独立 Blobstore benchmark 缺少 bdev event callback（已修复）

- **状态**：已修复（task90150022 P0-2 A2）
- **时间**：2026-09-15
- **等级**：一般（仅新增 benchmark 无法启动）
- **现象**：A2 首次运行在 SPDK 初始化阶段退出，日志为
  `spdk_bdev_open_ext_v2: Missing event callback function`。
- **排查过程**：确认 NVMe 已由 `uio_pci_generic` 接管、hugepages 正常，且错误发生在
  `spdk_bdev_create_bs_dev_ext()` 调用之后、blobstore 初始化之前。
- **根因**：SPDK blob-bdev bridge 要求调用方提供 bdev event callback；新 benchmark
  为简化代码传入了 `NULL`，不满足 API 前置条件。
- **修复**：增加最小的无状态事件回调并传给 `spdk_bdev_create_bs_dev_ext()`；benchmark
  不改变设备事件语义，只在事件发生时保持进程可继续运行。
- **验证**：重新编译 A2 benchmark 后重跑 NVMe Blobstore workload，并检查退出码和
  `errors=0`。

### 85. A2 Blobstore 基线与 HFS 共享 NVMe 时破坏 HFS 元数据（已规避）

- **状态**：已规避（task90150022 P0-2 测试隔离）
- **时间**：2026-09-15
- **等级**：严重（错误的基线顺序会让后续 HFS 挂载失败；不影响产品路径）
- **现象**：在已经格式化为 HFS 的 `Nvme0n1` 上运行 A2 后，随后启动 A3 报告
  `hfs_lib_init failed`；HFS 日志显示 `hfsMetaInit failed`。A2 自身的 I/O 计数仍为
  `errors=0`，但底层 Blobstore 已不再是可供 HFS 继续挂载的状态。
- **排查过程**：对比 A2 benchmark 与 HFS mount 的调用路径，确认 A2 使用
  `spdk_bs_init()`，而不是只读加载；在 SPDK Blobstore 实现中该调用会在底层设备上
  初始化新的 super block 和 metadata 区域。复现顺序为 HFS 格式化 → A2 → A3，A3
  失败；将 NVMe 重新 `HFS_FORMAT=1` 后，A3 立即恢复并通过。
- **根因**：A2 是 raw Blobstore 层基线，必须在独立的干净 Blobstore 状态上运行；
  HFS 自身也管理同一 SPDK Blobstore。两者不能在同一块介质上交替复用而不重新格式化，
  否则 A2 的初始化会覆盖 HFS metadata。
- **修复/规避**：结果口径将 A2 与 A3/A4 分成独立介质状态；A2 全部样本在
  `spdk_bs_init()` 建立的 raw Blobstore 状态下完成，A2 后显式重新执行 HFS 格式化再
  运行 A3/A4。没有修改正式 HFS 数据面，也没有把 A2 的设备重初始化误报为 HFS bug。
- **验证**：重新格式化后 A3 4K/8 线程、64 线程读写均通过且 `errors=0`；A4 正式
  4K/32-job 样本随后通过且 `err=0`。结果文档记录了该顺序约束。

## 通用经验总结
