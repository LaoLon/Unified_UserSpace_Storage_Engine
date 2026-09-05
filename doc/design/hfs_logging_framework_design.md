# HFS 日志设计（C / SPDK 简单版）

> 面向当前基础版 HFS，先实现够用的运行日志。本文是设计稿，未核对项目源码，文件名和接口可按现有工程调整。

## 1. 要做什么

| 项目 | 规则 |
| --- | --- |
| 开发语言 | C，使用 pthread |
| 当前日志 | `/var/log/hfs/run/hfs_base_spdk.log` |
| 轮转大小 | 超过 `1024 * 1024` 字节，按 1 MiB 理解“1 MB” |
| 归档目录 | `/var/log/hfs/bak/` |
| 归档格式 | `.tar.gz`，每包一个日志文件 |
| 保留数量 | 最多 300 个归档，删除最旧的 |
| 过期含义 | 本版只按数量淘汰，不增加按天过期 |
| 运行方式 | 一个 HFS 进程、一个日志文件、一个后台日志线程 |

日志写在 Linux 自己管理的文件系统上，不能写到 HFS 自己的挂载点。业务数据仍走 SPDK，运行日志通过普通文件接口写入 `/var/log/hfs`。

本版不做多进程共写、配置热更新、分布式日志、复杂崩溃恢复，也不把运行日志当作文件系统 WAL。

## 2. 整体实现：两个文件就够

| 文件 | 内容 |
| --- | --- |
| `hfs_log.h` | 等级、日志宏、初始化和退出接口 |
| `hfs_log.c` | 队列、后台线程、文件写入、轮转、压缩和清理 |

业务线程只负责格式化日志、放入队列；后台线程负责写文件，超过大小后轮转、压缩、清理。

**第一版只开一个后台线程，写文件和压缩都由它完成。** 压缩期间队列可能积压，但不会让业务线程等待磁盘。以后实测压缩影响明显，再拆出独立压缩线程。

## 3. 日志接口

保留四个等级：`DEBUG`、`INFO`、`WARN`、`ERROR`。默认输出 INFO 及以上，DEBUG 排查时开启。

建议头文件接口：

```c
#ifndef HFS_LOG_H
#define HFS_LOG_H

#include <stdbool.h>

enum hfs_log_level {
    HFS_LEVEL_DEBUG,
    HFS_LEVEL_INFO,
    HFS_LEVEL_WARN,
    HFS_LEVEL_ERROR
};

int hfs_log_init(void);
void hfs_log_fini(void);
bool hfs_log_enabled(enum hfs_log_level level);

void hfs_log_write(enum hfs_log_level level,
                   const char *file, int line,
                   const char *fmt, ...);

#define HFS_LOG(level, ...) do {                       \
    if (hfs_log_enabled(level)) {                       \
        hfs_log_write(level, __FILE__, __LINE__,         \
                      __VA_ARGS__);                    \
    }                                                  \
} while (0)

#define HFS_LOG_DEBUG(...) HFS_LOG(HFS_LEVEL_DEBUG, __VA_ARGS__)
#define HFS_LOG_INFO(...)  HFS_LOG(HFS_LEVEL_INFO,  __VA_ARGS__)
#define HFS_LOG_WARN(...)  HFS_LOG(HFS_LEVEL_WARN,  __VA_ARGS__)
#define HFS_LOG_ERROR(...) HFS_LOG(HFS_LEVEL_ERROR, __VA_ARGS__)

#endif
```

使用方式：

```c
HFS_LOG_INFO("hfs started");
HFS_LOG_DEBUG("read inode=%lu offset=%lu len=%u",
              (unsigned long)inode, (unsigned long)offset, len);
HFS_LOG_ERROR("bdev read failed, rc=%d", rc);
```

格式示例：

```text
2026-08-31 14:00:00.123Z [INFO] [tid=2091] hfs.c:80 hfs started
2026-08-31 14:00:01.456Z [ERROR] [tid=2091] hfs_io.c:218 bdev read failed, rc=-5
```

时间、线程号在调用处获取。文件名仅保留 basename；消息中的换行转义，保持一条日志一行。正常读写成功不逐条打印 INFO，以免日志反过来影响 I/O 性能。

## 4. 队列和后台线程

配置先写成宏，不额外引入 JSON 配置模块：

```c
#define HFS_LOG_MAX_SIZE       (1024U * 1024U)
#define HFS_LOG_MAX_BACKUPS     300U
#define HFS_LOG_QUEUE_SIZE     1024U
#define HFS_LOG_LINE_SIZE      1024U
#define HFS_LOG_POLL_MS         10U
```

实现一个固定数组环形队列，保存已经格式化好的日志行及长度。队列约占 1 MiB，不需要 malloc 每条日志，也不需要 SPDK DMA 内存。

`hfs_log_write()`：

1. 保存 errno，使用受限缓冲区格式化整行，超长截断并保留换行。
2. 使用 `pthread_mutex_trylock()` 尝试获得队列锁。
3. 队列有空间则复制入队；锁竞争或队列满则丢弃，并增加原子计数 `dropped`。
4. 解锁、恢复 errno，返回业务代码。

这里用 trylock 是为了不让 SPDK 线程等待队列锁；**它不是无锁方案，而且锁竞争时也可能丢日志**。第一版接受这个取舍。队列不能保存栈指针或 `va_list`，必须复制最终文本。

后台线程每轮取出一批记录后立即释放队列锁，再写文件。空闲时休眠 10 ms；禁止拿着队列锁执行文件写入或压缩。它定期输出丢弃数量摘要，不通过原队列递归记录自己的错误。

活动文件只在初始化和轮转时打开，使用追加模式；可用 `FILE *` 配合 `fwrite()`。每批写入后 `fflush()`，最长按 1 秒周期主动 flush。检查短写及错误，失败时停止使用该流，关闭后退避重开；不无限重试。

`fflush()` 只把用户态缓冲交给内核，**本版不承诺断电不丢日志**。突然退出可能留下半行，重启时先把活动文件尾部修剪到最后一个完整换行再追加。

## 5. 超过 1 MB 怎么处理

后台线程每写完一条完整日志，更新文件大小。大小超过阈值后执行轮转，因此允许最多多出一条日志，不把一行拆到两个文件。

归档命名示例：

```text
hfs_base_spdk.20260831_140000_123456.2082.001.tar.gz
```

名称包含时间、pid 和进程内序号；创建时检查冲突，不覆盖已有文件。run 和 bak 要在同一个本地文件系统上，便于直接 rename。

轮转步骤：

1. flush 并关闭当前日志文件；失败则停止本次归档，保留原文件。
2. rename 到 bak 下的唯一名称，例如 `hfs_base_spdk.<id>.log`。
3. 重新打开 `/var/log/hfs/run/hfs_base_spdk.log`，大小从零开始。
4. 将旧 `.log` 打包为同目录的 `.tar.gz.tmp`。
5. 确认打包成功，检查压缩包可列出预期成员。
6. 若已有 300 个正式归档，删除最旧归档，给新包腾出一个位置。
7. 将 `.tar.gz.tmp` rename 为 `.tar.gz`，成功后删除旧 `.log`。

可以调用 Linux 的 `tar`，无需第一版就引入压缩库。下面是命令形式，代码中建议通过 `posix_spawn()` 传 argv，并等待、检查子进程退出状态，不拼接 `system()` 命令：

```bash
tar -czf <临时压缩包> -C /var/log/hfs/bak -- <旧日志文件名>
tar -tzf <临时压缩包>
```

打包和等待均在后台线程执行；为子进程设置等待上限，超时后终止并回收，不能一直挂住日志线程。失败只删除自己的临时包，保留原始 `.log`。

## 6. 归档清理和简单异常处理

每次归档时扫描 bak，只处理符合本程序命名规则的普通 `.tar.gz` 文件。按修改时间从旧到新排序，同时间按文件名排序，发布前清理到最多 299 个；删除失败就不发布新包，避免出现第 301 个。

第一版假定系统时间正常，不处理复杂的时间回拨排序。清理不递归、不跟随软链接，也不删除其他程序的文件。

| 情况 | 第一版处理 |
| --- | --- |
| 初始化打不开目录或日志 | 返回失败，由 HFS 启动入口报告 |
| 队列满、锁竞争 | 丢日志并计数，不阻塞业务 |
| 写文件失败、磁盘满 | 后台退避重试，新增日志可丢弃，不拖住 HFS |
| tar 失败 | 保留旧 `.log`，稍后重试 |
| 存在未完成归档 | 先完成旧段，再允许下一次轮转 |
| 旧段长期归档失败 | 新活动文件达到阈值后暂停文件写入并统计丢弃，避免未压缩文件无限增加 |
| 重启 | 追加活动文件；先处理残留旧 `.log` 和本程序临时包 |

正常情况下最多保留一个未完成的旧段。如果正式包和同名旧 `.log` 同时存在，先验证包与源内容一致，再删除重复源；无法验证则报告错误，不覆盖文件。

最多 300 个限制针对完成的归档，不代表整个目录严格不超过 300 MB。压缩失败时可能另有一个旧段、一个临时包和当前日志。

## 7. 接到 SPDK HFS 中

HFS 自身先使用上面的 `HFS_LOG_*`。如果还希望 SPDK 自身报错进入同一个文件，再增加一个日志回调，复用同一队列，不单独建立日志系统。

使用 SPDK event framework 时，接入顺序示意：

```c
/* 示意：省略错误处理和原有业务参数。 */
hfs_log_init();

spdk_app_opts_init(&opts, sizeof(opts));
opts.log = hfs_spdk_log_cb;

/* 此处保留 HFS 原有 opts 配置。 */
spdk_app_start(&opts, hfs_start, NULL);
spdk_app_fini();

hfs_log_fini();
```

回调按 SPDK 要求接收 level、源码位置、format 和 `va_list`，转换等级并受限格式化后入队；不要保存 `va_list`，也不要在回调内再次调用 SPDK 日志宏。

映射：SPDK ERROR→ERROR、WARN→WARN、NOTICE/INFO→INFO、DEBUG→DEBUG。具体接口以项目 SPDK 版本为准；`opts.log` 见 [SPDK 应用选项](https://spdk.io/doc/structspdk__app__opts.html)，回调定义见 [SPDK 日志接口](https://spdk.io/doc/log_8h.html)。DPDK 的直接输出不保证被这个回调接管。

初始化要在最终服务进程中进行；如果 HFS 会 fork/daemonize，应先完成该步骤再创建日志线程。退出时先停业务和 SPDK，确认没有日志回调在执行，再排空日志队列、flush、关闭文件和回收日志线程。不要从 SPDK 回调中等待日志线程退出。

## 8. 实现与验收顺序

按下面顺序完成即可，不需要先搭完整框架：

1. 完成 `hfs_log.h/.c`，业务代码能输出四种等级。
2. 加队列和后台 pthread，业务线程不执行文件 I/O。
3. 加 1 MiB 轮转、tar.gz 和 300 个归档清理。
4. 加队列满、磁盘满、压缩失败的计数及重试。
5. 最后按需接 SPDK 自身日志回调。

验证六件事：日志能看到；多线程不写乱；超过阈值会轮转；压缩包可解开；第 301 个归档生成前最旧归档被删除；队列满或压缩失败时 HFS 仍能处理 I/O。
