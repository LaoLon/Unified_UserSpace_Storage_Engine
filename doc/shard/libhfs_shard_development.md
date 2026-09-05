# HFS libhfs 分片开发说明书

> 配套设计文档：`doc/design/libhfs_ld_preload_design.md`。
> 开发规则：**每个分片（shard）代码量 ≤ 500 行，完成一个分片立即编译验证并 git 提交一次**。
> 每次提交信息格式：`shard-NN: <分片名> - <一句话说明>`。

## 0. 开发流程约定

1. 每个 shard 只做一件事，改动的文件数量尽量少。
2. 每个 shard 完成后：
   - 同步 VM → 编译（`make app` 或 `make lib`）→ 跑现有自测 → 通过才提交。
   - 提交信息写明分片编号和内容。
3. 所有 shard 按顺序执行，不允许跳号（依赖关系决定顺序）。
4. 每个 shard 的"验收"必须可观察（日志、命令输出、文件存在）。
5. 代码风格遵循现有工程（Tab 缩进、小驼峰函数、大驼峰结构体）。

---

## 阶段 A：库化（M1）—— 把 main 下沉成库

### shard-01：把 main 移到 hfs_main.c（约 120 行改动）

**目标**：`src/hfs.c` 中删除 `main`，原自测流程移到新文件 `src/hfs_main.c`，hfs.c 只保留库函数。

**改动文件**：
- `src/hfs.c`：删除 `main()`（153 行中的 main 部分，约 50 行）
- `src/hfs_main.c`（新增）：从 hfs.c 复制原 main，包含 `hfs_internal.h` + `hfs_log.h`，调用现有接口，约 60 行
- `Makefile`：`SRC` 拆成两套——`APP_SRC`（含 hfs_main.c）和 `LIB_SRC`（不含 hfs_main.c），`make app` 用 APP_SRC，新增 `make lib` 用 LIB_SRC

**Makefile lib 目标骨架**（约 40 行）：

```make
LIB := $(BUILD_DIR)/lib/libhfs.so
LIB_SRC := $(PROJECT_ROOT)/src/hfs.c $(PROJECT_ROOT)/src/hfs_mount.c \
           $(PROJECT_ROOT)/src/hfs_file.c $(PROJECT_ROOT)/src/hfs_log.c
LIB_OBJ := $(patsubst $(PROJECT_ROOT)/src/%.c,$(BUILD_DIR)/obj/%.o,$(LIB_SRC))

lib: check-spdk $(LIB)
$(LIB): $(LIB_OBJ) | $(BUILD_DIR)/lib
	$(CC) -shared -o $@ $^ $(SPDK_LIBS) -lpthread -ldl
```

（SPDK 静态库链接方式在实现时对照 spdk.app.mk 的 LINK 命令确认）

**验收**：`make app` 后自测程序行为与之前完全一致；`make lib` 产出 `build/lib/libhfs.so`（可先不要求能跑，只要求链接成功）。

---

### shard-02：新增 hfs_lib.h 与空实现骨架（约 300 行）

**目标**：定义库层公共接口（不含 preload），全部先返回 ENOSYS 占位，为后续 shard 逐步填充。

**改动文件**：
- `src/hfs_lib.h`（新增，约 80 行）：声明
  - 生命周期：`hfs_lib_init` / `hfs_lib_fini`
  - 文件：`hfs_lib_open/read/write/pread/pwrite/lseek/close/unlink/ftruncate/fsync/fstat`
  - 目录：`hfs_lib_mkdir/rmdir/rename/opendir/readdir/closedir/stat`
- `src/hfs_lib.c`（新增，约 200 行）：上述函数空实现（`return -1; errno = ENOSYS;`）+ 内部互斥锁 `g_libLock` 定义 + fd 表定义（`struct hfs_handle g_fdTable[65536]`，此处只定义不填充逻辑）

**注意**：本 shard 不做任何真实逻辑，只保证接口和符号存在（`nm build/lib/libhfs.so` 能看到符号）。

**验收**：`make lib` 成功；`nm -D build/lib/libhfs.so | grep hfs_lib` 输出全部接口符号；`make app` 自测仍通过。

---

### shard-03：hfs_lib_init/fini 真实实现（约 200 行）

**目标**：把 `hfs.c` 里的 `hfsEnvSetup` + 挂载逻辑接到库生命周期接口上。

**改动文件**：
- `src/hfs_lib.c`（约 150 行）：实现
  - `hfs_lib_init()`：调 `hfs_log_init` → `hfsEnvSetup` → `hfsMount`，保存全局 `g_fs`（Hfs*），失败依次回滚
  - `hfs_lib_fini()`：`hfsUmount` → `hfs_log_fini`，置空全局指针
- `src/hfs_main.c`（约 20 行）：main 改为调 `hfs_lib_init` / `hfs_lib_fini` 包住原有读写流程（暂时仍用旧 `hfsOpen/hfsWrite` 接口，等 shard-05 切换）

**验收**：`make app` 自测通过；日志出现 `hfs_lib_init` 相关记录（临时加一条 `HFS_LOG_INFO("lib init")`）。

---

### shard-04：`make lib` 产出可用 .so（约 100 行）

**目标**：libhfs.so 链接 SPDK 全部依赖，能被 `LD_PRELOAD` 加载（构造函数打印一行日志证明生效）。

**改动文件**：
- `Makefile`（约 40 行）：完整 lib 链接规则——对照 spdk.app.mk 的 LINK 命令，把 `$(OBJS)` 换成 `$(LIB_OBJ)`，加 `-shared -fPIC -Wl,-soname,libhfs.so`，并追加 `-ldl`（preload 需要）
- `src/hfs_lib.c`（约 30 行）：加 `__attribute__((constructor))` 占位函数，仅 `HFS_LOG_INFO("libhfs loaded")`（本 shard 不 init，只验证可加载）
- 检查 `src/hfs.c` 等是否需 `-fPIC` 编译（Makefile 里 LIB_OBJ 编译加 `-fPIC`）

**验收**：`make lib` 成功；`LD_PRELOAD=./build/lib/libhfs.so true` 退出码 0 且日志文件出现 "libhfs loaded"。

---

## 阶段 B：元数据持久化（M2）

### shard-05：hfsRead/hfsWrite 支持 offset/count（约 300 行）

**目标**：现有读写只支持"偏移 0、单簇、strlen 语义"，改为真正按偏移和长度读写。

**改动文件**：
- `src/hfs.h`（约 10 行）：`HfsFile` 增加 `uint64_t offset`（当前读写位置）
- `src/hfs_file.c`（约 250 行）：
  - `hfsSpdkBlobReadCb`：不再用 strlen，按实际请求长度拷贝
  - `hfsDoRead`：按 `offset / ioUnitSize` 计算起始块，`count / ioUnitSize` 计算块数，允许跨多簇
  - `hfsDoWrite`：同样按 offset/count 计算；写前若超出 blob 大小先 resize
  - `hfsWrite` 保持签名兼容（内部转 offset=0），新增 `hfsWriteAt(file, buf, count, offset)`
  - `hfsRead` 同理
- `src/hfs_main.c`（约 10 行）：改用带 offset 的读写验证（写 offset=0 读 offset=0，仍打印 READ BUFFER）

**验收**：自测通过；额外加一条验证：`hfsWriteAt(f, "BBBB", 4, 100)` + `hfsReadAt(f, buf, 4, 100)` 读回 "BBBB"（在 hfs_main.c 临时加，确认后保留）。

---

### shard-06：hfsClose 不再删除 blob（约 100 行）

**目标**：close 只关闭句柄 + 同步元数据，删除动作归 unlink。

**改动文件**：
- `src/hfs_file.c`（约 80 行）：
  - `hfsDoClose`：`spdk_blob_close` 后回调只 `sync_md`（保留 blob），不再 `spdk_bs_delete_blob`
  - 删除 `hfsSpdkBlobDeleteCb`（或其改为无操作）
- `src/hfs_main.c`（约 10 行）：close 后不删文件，方便后续 shard 验证"再打开同一文件"

**验收**：自测通过；日志不再出现 delete 相关输出；连续两次 `hfsOpen`（第二次用上次的 blobId）能打开同一 blob。

---

### shard-07：超级块 + 首次格式化（约 250 行）

**目标**：mount 时检查固定 blob 0 是否为 HFS 超级块，不存在则创建（首次格式化）。

**改动文件**：
- `src/hfs_meta.h`（新增，约 60 行）：`HFS_SUPER_MAGIC`、`struct hfs_super_block`（magic/version/metaBlobId/rootInodeId/checksum）、`struct hfs_inode`（id/type/parentId/blobId/size/mtimeNs/ctimeNs/name）、`HFS_INODE_DIR/FILE` 常量
- `src/hfs_meta.c`（新增，约 180 行）：
  - `hfsMetaFormat(fs)`：创建 super blob（blobId=0 固定）、写入超级块
  - `hfsMetaLoad(fs)`：打开 blob 0 读超级块，校验 magic，失败返回 -1
  - `hfsMetaSave(fs)`：写回超级块（sync_md）
- `src/hfs_mount.c`（约 30 行）：`hfsSpdkBsInitCb` 成功后调 `hfsMetaLoad`，未格式化则 `hfsMetaFormat`
- `src/hfs_lib.c`（约 10 行）：init 里挂载后记录格式化结果

**验收**：首次运行日志出现 "format"（临时 NOTICE）；删除 `/var/log/hfs` 无关——重启后再次 mount 走 load 分支（日志区分 format/load）。

---

### shard-08：元数据 blob 的读写与 inode 表内存副本（约 350 行）

**目标**：元数据 blob 整块读写，进程内维护 inode 表。

**改动文件**：
- `src/hfs_meta.c`（约 300 行）：
  - `hfsMetaAddInode` / `hfsMetaRemoveInode` / `hfsMetaFindByName(parentId, name)` / `hfsMetaFindById(id)`：内存数组操作
  - `hfsMetaSaveAll`：把 inode 表序列化写入元数据 blob + sync_md
  - `hfsMetaLoadAll`：读元数据 blob 反序列化填充内存表
  - `HFS_MAX_INODES 65536`，内存表为 `struct hfs_inode g_inodes[]` + `g_inodeCount`，写入时全量序列化
- `src/hfs_meta.h`（约 30 行）：上述函数声明
- `src/hfs_mount.c`（约 20 行）：load 超级块后 loadAll；save 接口由后续 shard 调用

**验收**：`hfsMetaFormat` 后能看到 inode 表文件（自测临时打印 inodeCount）；重启进程后元数据仍能读出（先手动造一条根目录 inode 验证）。

---

### shard-09：路径解析 + 根目录（约 300 行）

**目标**：`/hfs/a/b.txt` 这类路径逐级解析为 inode；首次格式化为根目录建 inode。

**改动文件**：
- `src/hfs_meta.c`（约 250 行）：
  - `hfsMetaFormat` 里创建根目录 inode（type=DIR, id=1, name="/"）
  - `hfsMetaResolvePath(fs, path)`：按 `/` 分段逐级 findByName，返回 inode 指针或 NULL（区分"不存在"与"中间目录缺失"）
  - `hfsMetaResolveParent(path, &parentInode, &leafName)`：取最后一段
- `src/hfs_meta.h`（约 20 行）：声明
- `src/hfs_lib.c`（约 20 行）：`hfs_lib_stat` 先用 resolvePath 占位实现（后续 shard 补全 stat 字段）

**验收**：自测里临时调用 `hfsMetaResolvePath(fs, "/")` 返回根 inode；`/hfs/nonexist` 返回 NULL。

---

### shard-10：mkdir / rmdir（约 300 行）

**目标**：目录创建与删除，修改内存表并回写元数据 blob。

**改动文件**：
- `src/hfs_meta.c`（约 250 行）：
  - `hfsMetaMkdir(parent, name)`：查重 → 新建 DIR inode（无数据 blob）→ 更新父目录 → saveAll
  - `hfsMetaRmdir(fs, path)`：解析 → 校验是目录 → 校验空（无子项）→ 删除 → saveAll
- `src/hfs_lib.c`（约 40 行）：实现 `hfs_lib_mkdir` / `hfs_lib_rmdir`（加锁、调用 meta、设 errno）
- `src/hfs_lib.h`：已有声明，无需改

**验收**：hfs_main.c 临时加：mkdir `/hfs/d1` → stat 存在 → rmdir → stat 不存在；日志确认 saveAll 次数。

---

### shard-11：创建/打开文件 + 分配数据 blob（约 350 行）

**目标**：O_CREAT 语义——路径不存在时新建 FILE inode + 数据 blob；已存在时打开。

**改动文件**：
- `src/hfs_meta.c`（约 150 行）：
  - `hfsMetaCreateFile(fs, parent, name)`：新建 FILE inode（`spdk_bs_create_blob` 得到 blobId）、size=0、saveAll
  - `hfsMetaLookup(fs, path, &inode)`：封装 resolvePath
- `src/hfs_lib.c`（约 150 行）：实现 `hfs_lib_open`：
  - resolvePath 存在 → 打开数据 blob，分配 `HfsFile`，登记 fd 表
  - 不存在且 flags 含 O_CREAT → createFile + 打开
  - O_TRUNC → ftruncate 到 0
  - 失败设 errno（ENOENT/EACCES/EMFILE）
- `src/hfs_lib.h`：无需改

**注意**：本 shard 复用旧 `hfsOpen`（其内部仍是 create 新 blob）——需要先给 `hfs_file.c` 加一个"按 blobId 打开"的接口 `hfsOpenById`（约 40 行，`spdk_bs_open_blob` 替代 create）。

**验收**：`hfs_lib_open("/hfs/a.txt", O_CREAT)` 返回 fd；`hfs_lib_stat` 显示 size=0；再次 open 不带 O_CREAT 也成功（证明是同一个 inode）。

---

### shard-12：unlink（约 200 行）

**目标**：删除文件——删 inode + 删数据 blob。

**改动文件**：
- `src/hfs_meta.c`（约 80 行）：`hfsMetaRemoveFile(fs, parent, name)`：从表删除 + 返回 blobId
- `src/hfs_lib.c`（约 100 行）：`hfs_lib_unlink`：resolveParent + 校验是文件 → removeFile → `spdk_bs_delete_blob`（经 waiter 回调）→ saveAll
- `src/hfs_file.c`（约 20 行）：恢复一个 `hfsDeleteBlobById(fs, blobId)` 辅助（waiter 链）

**验收**：创建 → unlink → stat 返回 ENOENT；再创建同名文件成功（blobId 不同）。

---

### shard-13：rename（约 250 行）

**目标**：同目录/跨目录重命名，元数据原子更新。

**改动文件**：
- `src/hfs_meta.c`（约 200 行）：
  - `hfsMetaRename(fs, oldPath, newPath)`：解析两端 → 校验（目标存在时报错或覆盖按 POSIX 语义）→ 修改 name/parentId → saveAll（单次写回即原子）
- `src/hfs_lib.c`（约 50 行）：`hfs_lib_rename` 加锁调用

**验收**：`/hfs/a.txt` rename 为 `/hfs/b.txt` 后旧路径 ENOENT、新路径 stat 正常；跨目录 rename 验证 parentId 更新正确。

---

### shard-14：ftruncate / stat / fstat（约 300 行）

**目标**：文件大小管理与 inode 元数据查询。

**改动文件**：
- `src/hfs_meta.c`（约 100 行）：`hfsMetaSetSize` / `hfsMetaGetSize`；inode 的 mtime/ctime 更新辅助
- `src/hfs_lib.c`（约 180 行）：
  - `hfs_lib_ftruncate`：更新 inode.size + resize 数据 blob + saveAll
  - `hfs_lib_stat` / `hfs_lib_fstat`：inode → `struct stat`（st_size/st_mode/st_ino/st_mtime，UID/GID 用当前进程）
- `src/hfs_lib.h`：无需改

**验收**：写 4 字节后 stat size=4；ftruncate 100 后 stat size=100；fstat 与 stat 一致。

---

## 阶段 C：拦截层（M3）

### shard-15：preload 框架 + open/close 劫持（约 350 行）

**目标**：`hfs_preload.c` 建立 dlsym 框架，先劫持 open/close。

**改动文件**：
- `src/hfs_preload.c`（新增，约 300 行）：
  - `#define _GNU_SOURCE` + `#include <dlfcn.h>` + `dlsym(RTLD_NEXT, ...)` 取 `real_open/real_close/...`
  - `hfs_path_managed(path)`：前缀判断（`getenv("HFS_MOUNT_PREFIX")` 默认 `/hfs/`）
  - `open`：managed → `hfs_lib_open` + fd 表标记 isHfs；否则 `real_open`
  - `close`：isHfs → `hfs_lib_close`；否则 `real_close`
  - constructor/destructor 调 `hfs_lib_init/fini`
- `Makefile`：`LIB_SRC` 增加 `hfs_preload.c`（约 10 行）

**验收**：`LD_PRELOAD=libhfs.so touch /hfs/t1` 退出 0；日志出现 open 记录；`ls /hfs` 前缀外文件不受影响。

---

### shard-16：read/write/lseek 劫持（约 250 行）

**目标**：数据读写落到 HFS 句柄。

**改动文件**：
- `src/hfs_preload.c`（约 200 行）：`read`/`write`/`pread`/`pwrite`/`lseek`——isHfs 走库接口并维护句柄 offset；否则透传
- `src/hfs_lib.c`（约 50 行）：补 `hfs_lib_read/write/pread/pwrite/lseek` 真实实现（内部用 shard-05 的 offset 读写 + 更新 g_fdTable 偏移）

**验收**：`LD_PRELOAD=... sh -c 'echo hello > /hfs/t1; cat /hfs/t1'` 输出 hello。

---

### shard-17：fsync/ftruncate/fallocate/unlink/mkdir/rmdir/rename 劫持（约 350 行）

**目标**：补齐常用管理类调用。

**改动文件**：
- `src/hfs_preload.c`（约 350 行）：`fsync`/`fdatasync`/`ftruncate`/`fallocate`/`unlink`/`mkdir`/`rmdir`/`rename` 全部按 isHfs 分支；fallocate 仅支持 KEEP_SIZE 预分配，其余 flag 返回 EOPNOTSUPP

**验收**：`LD_PRELOAD=... sh -c 'mkdir -p /hfs/d; dd if=/dev/zero of=/hfs/d/f bs=1k count=4; sync; rm /hfs/d/f; rmdir /hfs/d'` 全部成功。

---

### shard-18：stat 族 + access + dup 族（约 250 行）

**目标**：查询与 fd 复制。

**改动文件**：
- `src/hfs_preload.c`（约 200 行）：`stat`/`fstat`/`lstat`/`access`/`faccessat`/`dup`/`dup2`/`fcntl(F_DUPFD)` 分支
- `src/hfs_lib.c`（约 50 行）：`hfs_lib_dup` 内部复制 fd 表项（引用计数或整表拷贝，第一版整表拷贝）

**验收**：`LD_PRELOAD=... sh -c 'test -f /hfs/t1 && echo yes'` 输出 yes；dup 后双 fd 读写同一文件位置符合预期（lseek 共享）。

---

### shard-19：opendir/readdir/closedir（约 300 行）

**目标**：目录枚举，`ls` 可用。

**改动文件**：
- `src/hfs_meta.c`（约 100 行）：`hfsMetaListDir(fs, inodeId, cb)`：遍历内存表收集 children
- `src/hfs_lib.c`（约 100 行）：`hfs_lib_opendir/readdir/closedir`：`struct hfs_dir` 持有快照数组 + 游标
- `src/hfs_preload.c`（约 100 行）：`opendir`/`readdir`/`closedir`/`fdopendir` 劫持

**验收**：`LD_PRELOAD=... ls /hfs` 列出此前创建的文件；`ls /` 不受影响。

---

### shard-20：拦截层冒烟测试脚本（约 200 行）

**目标**：把 shard-15~19 的验收命令固化为脚本 `scripts/preload_smoke.sh`，CI 可重复跑。

**改动文件**：
- `scripts/preload_smoke.sh`（新增，约 150 行）：覆盖 open/read/write/lseek/fsync/ftruncate/mkdir/unlink/rename/stat/opendir/dup 的断言式检查（`set -e` + `test`）
- `README.md`（约 20 行）：补 LD_PRELOAD 使用说明

**验收**：脚本连续跑 3 次全绿。

---

## 阶段 D：FIO 基线（M4）

### shard-21：安装 fio + 配置模板（约 100 行）

**目标**：fio 环境与配置文件。

**改动文件**：
- `scripts/fio/hfs_randrw.ini`（新增，约 40 行）：global（libaio/direct=1/bs=4k/size=64m/iodepth=32/directory=/hfs/fio）+ randwrite/randread job
- `scripts/fio/hfs_seq.ini`（新增，约 30 行）：seqwrite/seqread 128k
- `scripts/fio/run_fio.sh`（新增，约 30 行）：循环跑两类配置，输出 IOPS/BW/p99 到 `build/fio_results/`

**验收**：`bash scripts/fio/run_fio.sh` 产出结果文件，数字非零。

---

### shard-22：fio 冒烟 + 修复（约 300 行）

**目标**：跑通 fio，修复暴露出的拦截层问题（预期：libaio 的 io_setup/io_submit 走 pread/pwrite 透传路径、O_DIRECT 对齐等）。

**改动文件**：
- `src/hfs_preload.c`（约 200 行）：按 fio 实际暴露的问题修（如 ioctl、O_DIRECT 对齐校验、statvfs 缺失——`statvfs` 加劫持返回合理值）
- `src/hfs_lib.c`（约 100 行）：相应补充

**验收**：randwrite 4k 至少跑完一轮（fio 报 error 为不通过）；记录首个 IOPS 数字。

---

### shard-23：与 ext4 基线对比 + 结果记录（约 150 行）

**目标**：同配置跑 ext4（临时目录 /tmp/fio-ext4）与 HFS，产出对比报告。

**改动文件**：
- `scripts/fio/run_fio.sh`（约 60 行）：加 ext4 对照目录参数
- `doc/design/fio_baseline_notes.md`（新增，约 80 行）：记录环境、命令、IOPS/p99 对比表、结论

**验收**：报告中有 HFS 与 ext4 两列完整数据。

---

## 阶段 E：数据库冒烟（M5）

### shard-24：MySQL 安装 + 启动冒烟（约 250 行）

**目标**：`LD_PRELOAD` 下 mysqld 能初始化 datadir 并启动。

**改动文件**：
- `scripts/db/mysql_smoke.sh`（新增，约 150 行）：apt 安装 mariadb-server 或下载 mysql 二进制 → `--initialize-insecure --datadir=/hfs/mysql` → 前台启动 → `mysqladmin ping` → 建表插入查询 → 关闭
- `src/hfs_preload.c`（约 100 行）：按报错补劫持（预期：`fallocate` 更多 flag、`fsync` 目录 fd、`open` O_TMPFILE——O_TMPFILE 若 HFS 不支持则透传或返回 EOPNOTSUPP）

**验收**：脚本末尾 `mysql -e "create table t(a int); insert into t values(1); select * from t"` 输出 1。

---

### shard-25：RocksDB 安装 + db_bench 冒烟（约 250 行）

**目标**：RocksDB 在 HFS 上完成 fillseq/readrandom/overwrite。

**改动文件**：
- `scripts/db/rocksdb_smoke.sh`（新增，约 150 行）：下载编译 rocksdb（或 apt librocksdb）→ `db_bench --benchmarks=fillseq,readrandom,overwrite --db=/hfs/rocks` → 断言退出码
- `src/hfs_preload.c`（约 100 行）：按报错补劫持（预期：`fallocate`、`fdatasync` 目录 fd、`ioctl(FS_IOC_*)`——不透传则返回 EOPNOTSUPP）

**验收**：db_bench 三个 benchmark 正常完成（ops/sec 非零）。

---

### shard-26：TiDB 冒烟（约 200 行）

**目标**：单机 TiDB playground 启动 + sysbench 读写冒烟。

**改动文件**：
- `scripts/db/tidb_smoke.sh`（新增，约 150 行）：`tiup playground`（data-dir=/hfs/tidb）→ 等 ready → `sysbench oltp_read_write` 短跑 → 关闭
- `src/hfs_preload.c`（约 50 行）：按报错补充

**验收**：sysbench 报告输出非零吞吐；TiDB/TiKV 日志无致命错误。

---

### shard-27：数据库问题汇总到 bug_summary（约 100 行）

**目标**：三个数据库冒烟中发现的问题归类记录。

**改动文件**：
- `doc/bug_summary.md`（新增，约 100 行）：按"提示/一般/严重/致命"分级记录每个问题、根因、修复方式

**验收**：文档包含每库至少一条实测记录（无问题也记录"通过"）。

---

## 阶段 F：性能优化（M6，按需）

### shard-28：元数据脏页回写（约 400 行）

**目标**：inode 表按脏标记只回写变更页，降低 fsync 写放大。

**改动文件**：
- `src/hfs_meta.c`（约 350 行）：元数据 blob 改为分页布局（页头 + 若干 inode/页），`hfsMetaMarkDirty(id)`，saveAll 只写脏页
- `src/hfs_lib.c`（约 50 行）：fsync 时只回写脏页

**验收**：重复 fsync 时日志确认回写字节数远小于全表；fio fsync 用例 p99 对比 shard-23 改善。

---

### shard-29：per-file 锁替代全局锁（约 300 行）

**目标**：不同文件并发读写不再串行。

**改动文件**：
- `src/hfs_lib.c`（约 250 行）：`HfsFile` 挂 `pthread_mutex_t`，全局锁只保护 fd 表分配，数据路径锁粒度到文件
- `src/hfs_preload.c`（约 50 行）：无需大改（仍走库接口）

**验收**：fio numjobs=8 randwrite 的 IOPS 相对 shard-23 提升 ≥ 50%（或记录实测结论）。

---

### shard-30：mmap 仿真（可选，约 400 行）

**目标**：为需要 mmap 的数据库开启读路径支持。

**改动文件**：
- `src/hfs_preload.c`（约 400 行）：`mmap`（HFS 句柄 → 匿名映射 + 拷贝）、`msync`（脏页回写）、`munmap`；仅支持小于 1 MB 文件，其余返回 ENODEV

**验收**：RocksDB `use_mmap_reads=true` 冒烟通过（若性能不可接受，文档记录并维持默认关闭）。

---

## 附：提交信息模板

```text
shard-NN: <分片名>

- <具体改动点 1>
- <具体改动点 2>
- 验证：<编译/命令/输出>
```

示例：

```text
shard-15: preload open/close 劫持

- 新增 src/hfs_preload.c：dlsym(RTLD_NEXT) 框架 + /hfs 前缀判定
- open/close 按 isHfs 分流到 hfs_lib_open/close
- constructor/destructor 接入 hfs_lib_init/fini
- 验证：LD_PRELOAD touch /hfs/t1 成功，日志有 open 记录
```