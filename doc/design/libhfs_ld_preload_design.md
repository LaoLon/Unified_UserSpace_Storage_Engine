# HFS libhfs 设计（LD_PRELOAD 文件系统库）

> 目标：把当前 HFS（SPDK Blobstore 上的实验文件系统）封装成动态库 `libhfs.so`，
> 通过 `LD_PRELOAD` 透明拦截 POSIX 文件系统调用，使普通程序（fio、MySQL、RocksDB、
> TiDB 等）无需修改即可把 I/O 落到 HFS 上，并具备目录、多文件、持久化元数据能力。
> 本文是设计稿，接口与文件名以落地实现为准。

## 1. 要做什么

| 项目 | 规则 |
| --- | --- |
| 产物 | `libhfs.so`（动态库，不含 `main`） |
| 接入方式 | `LD_PRELOAD=/path/libhfs.so <app>`，无需改程序 |
| 拦截前缀 | 默认挂载前缀 `/hfs/`（可经环境变量 `HFS_MOUNT_PREFIX` 覆盖），前缀内走 HFS，前缀外透传 glibc |
| 元数据 | 目录树 + 文件名 → blobId 持久化映射，重启后文件仍可访问 |
| 并发 | 多线程应用安全，SPDK 侧保持单线程模型 + 调用方串行化 |
| 测试 | fio 测 IOPS；MySQL / RocksDB / TiDB 兼容性验证 |
| 底层 | SPDK Blobstore，malloc bdev（当前）→ 后续可换 NVMe bdev |

第一版明确不做：多进程共享同一 blobstore 实例（每个进程独立挂载实例）、网络分布式、
mmap 语义完整模拟（见 8.4 限制）、完整 POSIX 权限模型。

## 2. 总体架构

```text
┌───────────────────────────── 应用进程 ─────────────────────────────┐
│   fio / mysqld / rocksdb / tidb ...                                 │
│        │  open/read/write/fsync/fallocate/mmap ...                  │
│   ┌────▼─────────────────────────────────────────────────────────┐  │
│   │ libhfs.so = hfs_preload.o（LD_PRELOAD 拦截层）                │  │
│   │  · dlsym(RTLD_NEXT) 取 glibc 真符号，前缀内劫持，前缀外透传    │  │
│   │  · 维护 fd → HfsFile 句柄映射表                               │  │
│   └────┬─────────────────────────────────────────────────────────┘  │
│   ┌────▼─────────────────────────────────────────────────────────┐  │
│   │ libhfs-core = hfs_lib.o + hfs.o + hfs_mount.o + hfs_file.o   │  │
│   │  · 挂载/卸载、目录树、元数据读写、文件数据读写                │  │
│   │  · waiter 同步化 + 互斥锁串行化多线程调用                     │  │
│   └────┬─────────────────────────────────────────────────────────┘  │
│   ┌────▼─────────────────────────────────────────────────────────┐  │
│   │ SPDK（blobstore / bdev / env）                                │  │
│   └───────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

- **库层**：当前 `hfs.c` 的 `main()` 逻辑下沉为 `hfs_lib_init()` / `hfs_lib_fini()`；
  原有 `hfs.c/hfs_mount.c/hfs_file.c/hfs_log.c` 全部编入 `.so`。
- **拦截层**：新增 `hfs_preload.c`，用 `__attribute__((constructor))` 在进程启动时
  初始化库，用 `dlsym(RTLD_NEXT, ...)` 取得原始符号后封装劫持函数。
- **示例程序**：`main()` 挪到独立 `hfs_main.c`，保留原自测流程，不再进入 `.so`。

## 3. 构建与部署

```bash
# 在 VM（SPDK 已构建）上：
make lib                       # 产出 build/lib/libhfs.so
make app                       # 产出 build/bin/spdk-hfs（自测程序，含 main）

# 使用
LD_PRELOAD=/root/code/HFS_BASE_SPDK/build/lib/libhfs.so \
    fio --name=test --rw=randwrite ...
```

环境要求（与当前一致）：

- root 权限（hugepages、/var/log/hfs 可写）
- hugepages：`make run` 已内置自动分配逻辑，`make lib` 运行前同样检查
- JSON 配置 `config/hfs.json` 描述 malloc bdev（Malloc0，16 MB）
- 挂载点目录：应用打开 `/hfs/xxx` 前无需真实存在，由 HFS 内部创建

## 4. 元数据与数据布局（Blobstore 上）

### 4.1 布局总览

| 对象 | 存储 | 说明 |
| --- | --- | --- |
| 超级块 | 固定 blob 0（super blob） | 魔数、版本、根目录 blobId、元数据 blob 指针 |
| 元数据 | 元数据 blob（meta blob） | 序列化的 inode 表 + 目录项（见 4.2） |
| 文件数据 | 每文件一个数据 blob | 按需 resize，cluster 粒度 |
| 日志 | Linux 文件系统 `/var/log/hfs/` | 走既有 hfs_log，不占 blobstore |

### 4.2 元数据格式（第一版：整块加载）

元数据 blob 保存一张扁平 inode 表，进程内维护内存副本，修改后回写：

```c
struct hfs_inode {
    uint64_t id;            /* inode 号 */
    uint8_t type;           /* HFS_INODE_DIR / HFS_INODE_FILE */
    uint64_t parentId;      /* 父目录 inode */
    uint64_t blobId;        /* 目录项数据 blob / 文件数据 blob */
    uint64_t size;          /* 文件字节数 */
    uint64_t mtimeNs;
    uint64_t ctimeNs;
    char name[FILENAME_LENGTH];   /* 本目录内名字，含 parent 内唯一 */
};
```

```c
struct hfs_super_block {
    uint64_t magic;         /* HFS_SUPER_MAGIC */
    uint32_t version;
    uint64_t metaBlobId;    /* 元数据 blob 的 blobId */
    uint64_t rootInodeId;   /* 根目录 inode 号 */
    uint32_t checksum;      /* 简单校验 */
};
```

要点：

- **只支持一层到若干层目录**，路径解析为逐级 inode 查找（`/hfs/a/b.txt` →
  root → a → b.txt），单文件系统内 inode 表上限第一版取 65536 项。
- 元数据 blob 每次 mount 全量读入内存；任何目录/文件变更（mkdir/unlink/rename/
  创建/截断）时更新内存副本并 `spdk_blob_sync_md` 回写。文件数规模几千时开销可接受。
- 数据 blob 与元数据分离：元数据回写不干扰大数据写入。

### 4.3 文件数据读写

沿用现有 waiter + 回调链，但改造为支持偏移与长度：

```c
int hfsRead(HfsFile *file, uint8_t *buffer, size_t count, uint64_t offset);
int hfsWrite(HfsFile *file, const uint8_t *buffer, size_t count, uint64_t offset);
```

- 按 ioUnitSize（512 B）对齐折算 cluster 偏移，`spdk_blob_io_read/write` 支持
  多 cluster 连续 I/O；首尾非对齐部分按现有单块路径处理。
- 写入超过当前 blob 大小时先 `spdk_blob_resize`（预留 cluster），再写数据。
- `hfsClose` 语义改为：关闭句柄 + `sync_md` 落盘元数据，**不再删除数据 blob**
  （删除由 `hfsUnlink` 完成，这是与当前 demo 的关键行为差异）。

## 5. 库层核心接口（hfs_lib.h）

```c
/* 生命周期 */
int  hfs_lib_init(void);            /* env + JSON + 挂载 + 元数据加载 */
void hfs_lib_fini(void);            /* 元数据回写 + 卸载 + 日志收尾 */

/* 文件操作（前缀路径由 preload 层解析为内部路径） */
int  hfs_lib_open(const char *path, int flags, mode_t mode);
ssize_t hfs_lib_read(int fd, void *buf, size_t count);
ssize_t hfs_lib_write(int fd, const void *buf, size_t count);
ssize_t hfs_lib_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t hfs_lib_pwrite(int fd, const void *buf, size_t count, off_t offset);
off_t  hfs_lib_lseek(int fd, off_t offset, int whence);
int    hfs_lib_close(int fd);
int    hfs_lib_unlink(const char *path);
int    hfs_lib_ftruncate(int fd, off_t length);
int    hfs_lib_fsync(int fd);      /* sync_md + 数据刷盘 */

/* 目录操作 */
int    hfs_lib_mkdir(const char *path, mode_t mode);
int    hfs_lib_rmdir(const char *path);
int    hfs_lib_rename(const char *oldPath, const char *newPath);
struct hfs_dir *hfs_lib_opendir(const char *path);
struct hfs_dirent *hfs_lib_readdir(struct hfs_dir *dir);
int    hfs_lib_closedir(struct hfs_dir *dir);

/* 查询 */
int    hfs_lib_stat(const char *path, struct stat *st);
int    hfs_lib_fstat(int fd, struct stat *st);
```

线程安全：SPDK 全局线程模型不变，库入口用 `pthread_mutex_lock` 串行化后经
`waiter()` 驱动 SPDK 线程；锁只保护库内部状态，不让业务线程直接进 SPDK。

## 6. POSIX 拦截层设计（hfs_preload.c）

### 6.1 劫持清单（第一版）

| 函数 | 说明 |
| --- | --- |
| `open` / `open64` / `openat` / `openat64` | 前缀内：解析路径→inode→打开句柄；O_CREAT 时建 inode |
| `close` | fd 属于 HFS：释放句柄；否则透传 |
| `read` / `write` | 按句柄当前偏移读写，更新偏移 |
| `pread` / `pwrite` / `pread64` / `pwrite64` | 显式偏移，不动句柄偏移 |
| `lseek` / `lseek64` | 更新句柄偏移，越界允许（写时触发 resize） |
| `fsync` / `fdatasync` | HFS：数据 + 元数据落盘 |
| `ftruncate` / `ftruncate64` | resize 数据 blob + 更新 size |
| `fallocate` | 预分配：resize blob（POSIX_FALLOCATE_FL_KEEP_SIZE 时仅预留） |
| `unlink` / `unlinkat` | 删 inode + 删数据 blob |
| `mkdir` / `mkdirat` / `rmdir` | 目录 inode 增删 |
| `rename` / `renameat` | 改名（同目录/跨目录） |
| `stat` / `fstat` / `lstat` | 映射 inode 元数据 → `struct stat` |
| `opendir` / `readdir` / `closedir` / `fdopendir` | 目录枚举 |
| `dup` / `dup2` / `fcntl(F_DUPFD)` | 复制 fd 表项 |
| `mmap` / `munmap` / `msync` | 见 6.4 |
| `access` / `faccessat` | 查 inode 存在性 |

### 6.2 fd 与句柄管理

```c
struct hfs_handle {
    int fd;                 /* 与应用可见 fd 相同，高位留标签区分 */
    HfsFile *file;
    uint64_t offset;        /* 文件读写偏移 */
    int flags;              /* 打开标志（O_APPEND 等） */
    int isHfs;              /* 1=HFS 句柄，0=透传 fd */
    bool writable;
};

static struct hfs_handle g_fdTable[65536];   /* 固定表，无锁读、加锁写 */
```

- fd 复用 glibc 编号范围，HFS 句柄与透传 fd 共用一个 fd 空间；关闭时回收表项。
- `isHfs==0` 的项直接调用 `real_*`（dlsym 得到的原符号）。

### 6.3 前缀与透传规则

```c
static bool hfs_path_managed(const char *path);
/* 返回 true：path == HFS_MOUNT_PREFIX 或以其开头（/hfs/xxx、/hfs/） */
```

- 前缀外（如 `/etc/xxx`、数据库自己的日志、库文件）：一律透传 glibc，
  避免拦截 SPDK/glibc 内部文件操作造成递归。
- 相对路径：先 `getcwd` 拼绝对路径再判断前缀。
- 软链接：第一版不解析链接，`lstat` 语义直接返回 inode 信息。

### 6.4 mmap 策略（第一版）

数据库（尤其 RocksDB）会用 mmap。第一版策略：

- 拦截 `mmap`：若 fd 为 HFS 句柄，映射到 `MAP_ANONYMOUS` 私有内存并复制文件
  内容；`msync`/`munmap` 时将脏页写回（用 pwrite 语义），即"读时拷贝 + 回写"，
  不做页缓存全仿真。
- 风险：大数据集下内存放大；只保证小文件（< 1 MB）与测试用例语义正确。
- 备选：对 mmap 请求直接透传并返回 `MAP_FAILED`（ENODEV），逼迫应用走
  read/write（fio libaio、InnoDB O_DIRECT 路径不受影响）。**默认采用备选**，
  保证正确性优先；mmap 仿真作为后续里程碑。

### 6.5 初始化时序

```c
__attribute__((constructor))
static void hfs_preload_init(void) {
    hfs_lib_init();       /* 失败时置全局 flag，后续 HFS 调用返回 ENODEV */
}
__attribute__((destructor))
static void hfs_preload_fini(void) {
    hfs_lib_fini();
}
```

构造器在 `main` 之前运行，SPDK env 初始化（hugepages 依赖）此时可用；
若程序 fork，子进程继承已初始化的库状态（第一版假定单进程场景）。

## 7. 一致性、错误处理与崩溃恢复

| 场景 | 第一版处理 |
| --- | --- |
| 写数据中途崩溃 | 数据 blob 可能含部分簇；元数据仅在 sync 时更新 size，恢复后按 size 截断访问 |
| 元数据回写失败 | 返回错误，句柄操作失败，不覆盖内存副本（下次可重试） |
| mount 时超级块损坏 | 返回失败并记日志，不自动格式化（防误删数据） |
| 首次 mount 无超级块 | 创建 super blob + 根目录 inode + 空元数据 blob |
| fd 表满 | `EMFILE` |
| 前缀外路径 | 永不走 HFS 路径，透传 |
| 并发访问同一文件 | 库内互斥锁串行化（第一版不做 per-file 锁） |
| `hfs_lib_init` 失败 | 所有 HFS 调用返回 `ENODEV`，应用可见为"目录不存在" |

崩溃恢复第一版承诺：**不丢已 fsync 的数据与元数据**；未 fsync 的写入可丢失。
日志（`/var/log/hfs`）沿用既有 hfs_log 框架，不进 blobstore。

## 8. FIO 基准测试方案

### 8.1 环境

```bash
# VM 上安装 fio（如未装）
apt-get install -y fio
```

### 8.2 配置示例（随机写 IOPS）

```ini
[global]
ioengine=libaio
direct=1
bs=4k
size=64m
numjobs=1
iodepth=32
directory=/hfs/fio
group_reporting=1

[hfs-randwrite]
rw=randwrite

[hfs-randread]
rw=randread
```

### 8.3 运行

```bash
LD_PRELOAD=/root/code/HFS_BASE_SPDK/build/lib/libhfs.so fio hfs_fio.ini
```

记录指标：`IOPS`、`BW`、`latency avg/p99`。

### 8.4 测试矩阵与预期

| 用例 | 说明 | 关注点 |
| --- | --- | --- |
| randread / randwrite 4k | 基础 IOPS | 数据路径无元数据写放大 |
| seqread / seqwrite 128k | 顺序带宽 | blob 连续 cluster 分配 |
| mixed 70/30 | 混合读写 | 锁竞争影响 |
| fsync 密集（fdatasync=1） | 元数据回写频率 | 与原生 ext4 对比写放大 |
| 多 job（numjobs=8） | 并发 | 库互斥锁串行化损耗 |

对照基线：同一台 VM 上 ext4 直接跑同样 fio 配置，对比 IOPS 与 p99 延迟。

### 8.5 预期结论

- malloc bdev 是内存盘，IOPS 理论上限远高于真实盘；重点测**拦截层与元数据
  回写引入的损耗**（与裸 malloc bdev 的 SPDK 直测对比）。
- 若元数据每次 fsync 全量回写成为瓶颈，后续里程碑改为"元数据分页 + 仅回写脏页"。

## 9. 数据库兼容性设计

### 9.1 MySQL（InnoDB）

| 行为 | 需要的能力 | HFS 支持 |
| --- | --- | --- |
| `O_DIRECT` 打开数据文件 | 对齐 I/O 透传 | 数据路径按 512B 对齐实现 |
| `fallocate(PUNCH_HOLE)` | 打洞 | 里程碑 2：簇释放 + size 更新 |
| 大文件预分配 | `fallocate(KEEP_SIZE)` | resize blob |
| `fsync` 频繁 | 元数据低开销回写 | 第一版全量回写，观察瓶颈 |
| `fdatasync` | 只落数据 | 实现 |
| `lseek` 大偏移 | 空洞读零 | 未写簇读 0（blob 未分配簇返回 0） |

部署验证：`LD_PRELOAD=... ./bin/mysqld --datadir=/hfs/mysql` 建库、建表、
TPC-C 冒烟；失败项记录到 bug_summary。

### 9.2 RocksDB

| 行为 | 需要的能力 | HFS 支持 |
| --- | --- | --- |
| WAL 小文件高频 fsync | 低延迟 fsync | 关注元数据回写路径 |
| SST 大文件顺序写 | 顺序带宽 | 连续 cluster |
| `rename` 原子替换 | rename 事务性 | 元数据单 blob 更新天然原子 |
| 多文件并发打开 | fd 表 + 多 inode | 支持 |
| mmap 读取（`use_mmap_reads`） | mmap 语义 | 默认透传/ENODEV，禁用该选项 |

部署验证：`db_bench --use_direct_io_for_flush_and_compaction` 等，先跑
`fillseq`、`readrandom`、`overwrite` 三个 benchmark 冒烟。

### 9.3 TiDB

- TiKV 底层即 RocksDB，验证项与 9.2 相同；TiDB Server 本身主要是网络与
  SQL 层，I/O 集中在 TiKV 数据目录（`--data-dir=/hfs/tidb`）。
- 验证：`tiup playground` 单机启动 + `sysbench oltp_read_write` 冒烟。
- 依赖 `rename`/`fsync`/`fallocate` 语义正确，优先级同 RocksDB。

### 9.4 数据库通用注意点

- 所有数据库目录必须放在挂载前缀内，日志、临时文件、配置文件放前缀外不影响
  正确性（透传）。
- 第一版元数据全量回写：写入放大明显，数据库场景应先测 fsync 密集负载，
  若 p99 恶化超过 2 倍则提前进入"脏页回写"里程碑。

## 10. 里程碑划分

| 里程碑 | 内容 | 验收 |
| --- | --- | --- |
| M1 库化 | `main` 下沉为 `hfs_lib_init/fini`，`make lib` 产出 `.so`，`make app` 保留自测 | 自测程序行为不变 |
| M2 元数据持久化 | super blob、inode 表、mkdir/unlink/rename、重启后文件可读 | 重启验证 + 多文件读写 |
| M3 拦截层 | open/read/write/close/lseek/fsync/ftruncate/stat/unlink/mkdir/rename/opendir | 基础 shell 命令（`cp`、`ls`、`dd`）在前缀目录可用 |
| M4 FIO 基线 | fio 矩阵 + 与 ext4 对比 | 拿到 IOPS/p99 报告 |
| M5 数据库冒烟 | MySQL / RocksDB / TiDB 各自建库写读 | 三个数据库冒烟通过 |
| M6 性能优化 | 元数据脏页回写、mmap 仿真（按需）、per-file 锁 | 数据库负载 p99 不劣化超 2 倍 |

## 11. 风险与限制

| 风险 | 说明 | 缓解 |
| --- | --- | --- |
| 元数据全量回写放大 | 每次 fsync 全量写 inode 表 | M6 脏页回写；当前先量化 |
| 多线程串行化 | 库锁使并发 I/O 变串行 | 数据库/fio 多线程场景先测，若明显劣化引入 per-file 锁 |
| mmap 语义不完整 | RocksDB mmap 选项不可用 | 默认透传，禁用 mmap 选项 |
| malloc bdev 与真实盘差异 | IOPS 不代表真实硬件 | 后续接 NVMe bdev 复用同一上层 |
| LD_PRELOAD 全局生效 | 对应用所有文件生效（前缀外透传） | 前缀过滤已隔离 |
| glibc 版本差异 | 拦截符号随 glibc 演化 | 用 `dlsym(RTLD_NEXT)` 运行时解析，不硬编码符号地址 |

## 12. 与现有代码的衔接

| 现有文件 | 改动 |
| --- | --- |
| `src/hfs.c` | 删除 `main`；`hfsEnvSetup` 保留并改名 `hfs_lib_env_init`；`waiter` 保留 |
| `src/hfs_main.c`（新增） | 原自测 `main` 移入，调用库接口 |
| `src/hfs_mount.c` | `hfsMount` 增加元数据加载；新增 `hfs_lib_mount` |
| `src/hfs_file.c` | `hfsRead/hfsWrite` 支持 offset/count；`hfsClose` 不再删 blob |
| `src/hfs_meta.c`（新增） | inode 表、路径解析、目录操作 |
| `src/hfs_lib.c`（新增） | 库 API 封装 + 互斥锁 + fd→句柄管理 |
| `src/hfs_preload.c`（新增） | dlsym 拦截层 |
| `src/hfs_log.[ch]` | 不变 |
| `Makefile` | 新增 `make lib` 目标（编 .so，含 preload 与 core，不含 main） |
| `config/hfs.json` | 不变（malloc bdev） |