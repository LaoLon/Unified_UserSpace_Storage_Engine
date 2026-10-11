# UUSE × MinIO 存储接入点审计（Shard-9.0，2026-10-11）

> 冻结版本：MinIO **`RELEASE.2025-10-15T17-29-55Z`**（commit
> `9e49d5e7a648f00e26f2246f4dc28e6b07f8c84a`，upstream 最后开源 tag；仓库
> 2026-04-25 归档为 read-only）、Go `1.24.0`（toolchain `go1.24.8`，取自
> 冻结 commit 的 `go.mod`）；审计副本位于 host2 `/build/minio-recon/minio`
> （`git clone --depth 1 --branch RELEASE.2025-10-15T17-29-55Z`，commit 校验
> 通过）。
>
> 依据：`doc/design/uuse_minio_frontend.md`（本文同步修正/固化其 §3 各锚点）；
> 行号以冻结 commit 为准；升级版本必须重跑本审计。
>
> 纪律：本审计只做 read-only `grep`/`sed` 证据采集，不改冻结源码；
> 采集脚本 `temp_scripts/minio_audit_shard90.sh`（VM 副本
> `/tmp/minio_audit_shard90.sh`，原始输出 `/tmp/minio_audit_shard90.log`）。

---

## 0. 结论摘要

1. **POSIX 接入面比设计初稿预估更集中**：`cmd/` 直接 `os.*` 活跃点共
   **~25 处**（非测试），其中 9 处就是收口层自身（`cmd/os-instrumented.go`）；
   `cmd/xl-storage.go` 只剩 **5 处**直接 `os.*`（2 处 `os.ReadFile` + 3 类
   flags/类型引用）。设计初稿的“实测 24 处”成立，且 patch 面结算为
   **3 个函数族 + 3 类独立路径**：
   - 函数族 1：`openFile/openFileSync/openFileDirect`（句柄来源，3 个定义点）；
   - 函数族 2：`OpenFile/OpenFileDirectIO/Open/Create/Remove/RemoveAll/Rename/
     Mkdir/MkdirAll/Stat/Lstat/Access/Fdatasync`（收口层，13 个函数）；
   - 函数族 3：`newXLStorage`/`xlStorage.Close`/`IsOnline`（生命周期 3 个点）；
   - 独立路径 A：目录枚举（`openFileWithFD` + `syscall.ReadDirent`，2 个入口）；
   - 独立路径 B：`RemoveAll`（`os.RemoveAll` 直用 + 9 个调用点）；
   - 独立路径 C：`os.ReadFile` 直用（2 处，不在收口层）；
2. **句柄方法面实测定型**：`xl-storage.go` 对文件句柄的全部调用 = `Write/Read/
   ReadAt/Seek/Truncate/Stat/Close`（7 个方法，共 31 处），没有 `Fd()`、
   `WriteAt`、`Name()`；设计 §4.3 的接口可收敛为 7 方法接口；
3. **`Fd()` 面实测定型**：仅 3 类——`internal/disk` 的 `Fdatasync/Fadvise`、
   `internal/disk` 的 O_DIRECT 开关（`directio_unix.go:41`）、`internal/lock`
   的文件锁；`cmd/` 侧只有 stdout/stderr 的终端探测。**新增发现**：
   `internal/lock`（`syscall.Flock`）此前未进设计，锁语义需在 §5 基准上
   补一行决策（UUSE 无 flock 语义，见 §5 清单 #16）；
4. **`os.ReadFile`/`os.ReadDir`/`readDirFn` 都不经过 `cmd/os-instrumented.go`
   收口层**（`os.ReadFile` 直用 2 处、目录枚举走裸 fd 路径），设计 §3.3 的
   “函数级替换”结论成立；
5. **持久化契约以实测定稿**：对象数据 = `writeAllDirect` + 单次
   `Fdatasync`（`:2195`）；元数据 = 小文件 `openFileSync`（O_DSYNC）/
   ≥`DirectioAlignSize` 走 direct + fdatasync（`:2247-2263`）；**sync 语义
   是 MinIO 的完成点（比 ext4 更近 POSIX）**，UUSE 的 `File::sync`
   （namespace 级 barrier）可直接覆盖，不需要“close 兜底”以外的额外发明；
6. **容量报告缺口比设计初稿结论小**：UUSE core 已有 `Engine::freeDataBytes()`
   （core.hpp:100）与 `deviceCapacityBytes()`（:114）、`dataExtentStats()`
   （:141）；Shard-9.1 只需要 shim 侧换算 total/free/used，`Files/Ffree`
   需 engine 级 inode 计数（`DataExtentStats.inodes` 已有，可换算）；
7. **构建前置缺口**：审计 VM 无 Go 工具链（`no-go`）；磁盘余量 4.5 GiB
   （74%），Go 1.24.8 + 首轮 module 下载需先腾出 ≥6 GiB 或换节点/挂载点
   （记入 Shard-9.1/9.2 环境清单）。

---

## 1. 冻结版本与源码位置

| 组件 | 版本/位置 | 说明 |
|---|---|---|
| MinIO | `RELEASE.2025-10-15T17-29-55Z` `9e49d5e` | host2 `/build/minio-recon/minio`；Shard-9.0 审计副本 |
| Go | 1.24.0 / toolchain 1.24.8 | `go.mod` 冻结；VM 尚未安装 |
| 上游状态 | 2026-04-25 归档（read-only） | 无上游 rebase；补丁栈自管 |
| 许可 | AGPLv3 | 仅实验室内部使用（设计 §11 Q5） |

## 2. 存储接入面（盘层 seam 主体）

### 2.1 `cmd/xl-storage.go`（3 423 行）直接 POSIX 调用全清单

| 行 | 调用 | 用途 | 处理 |
|---|---|---|---|
| 434 | `os.ReadFile(healingFile)` | healing 标记读（`isHealing`） | `uuseos.ReadFile`（新增） |
| 475 | `s.openFileDirect(..., O_CREATE\|O_WRONLY\|O_EXCL)` | O_DIRECT 探测 + healing 标记写 | 句柄抽象（`uuseos.OpenFile`） |
| 847 | `os.ReadFile(s.formatFile)` | `disk-id`/format.json 重读 | `uuseos.ReadFile` |
| 1955/1975/1979 | `openFileDirect/openFileSync/openFile` 定义 | 全部句柄来源 | 返回 `uuseos.File` |
| 2128 | `writeAllDirect(..., O_CREATE\|O_WRONLY\|O_EXCL, ...)` | 对象 part 写入口（CreateFile） | 句柄抽象 |
| 2248/2261/2263/2345 | `writeAllInternal/writeAll` 的 flags 与 open 调用 | xl.meta 同步写、日志追加（`O_APPEND`） | 句柄抽象 |
| 2149/2250/2342 | `*os.File` 声明 | 句柄类型替换点 | 接口类型 |

其他 `os.*` 引用均为类型/错误判定（`os.FileInfo`、`os.ErrNotExist`、
`os.IsPermission`、`os.PathError`、`os.O_*` flags），不产生 I/O。

### 2.2 句柄方法调用面（全量 31 处，实测）

```text
Write      4   479, 1940, 2269, 2351
Close     17   多数为 defer/错误路径
Stat       5   504, 1804, 1915, 2062, 3091
Truncate   3   2182, 2188, 2271
Seek       1   2083
ReadAt     1   1927
Read       0   （无）
Fd/WriteAt/Name 0
```

结论：`uuseos.File` 接口 = `Read/ReadAt/Write/Seek/Truncate/Stat/Sync/Close`
（合成 `Sync` 供 `Fdatasync` 包装；`Name` 可为只读元数据）。`ReadFileStream`
（`:2017`）是 `io.ReadCloser` 包装（内部走 `ReadFile` + `io.Pipe`/`sendFileReader`），
跟随句柄接口即可，不需要新接口。

### 2.3 open 出口与 flags 矩阵（实测定稿）

| 打开点 | flags | 语义 | UUSE 映射 |
|---|---|---|---|
| `openFileDirect:1955` | 调用方决定（475: `O_CREATE\|O_WRONLY\|O_EXCL`） | O_DIRECT 能力探测/标记写 | `Create\|Exclusive`；buffer 对齐由 UUSE 内部处理 |
| `openFileSync:1975` | `mode\|writeMode`（O_DSYNC） | 小 xl.meta/format.json 同步写 | 句柄 `syncOnWrite` → Close 时 `File::sync` |
| `openFile:1979` | `readMode` 或写 flags | 通用（读占多数） | 原样映射；读打开点：499/1766/1894/2033/3084 |
| `writeAllDirect` 内 | `O_CREATE\|O_WRONLY\|O_EXCL`（对象 part）| 大对象写 | `Create\|Exclusive` |
| `writeAllInternal` 内 | `O_CREATE\|O_WRONLY\|O_TRUNC`（xl.meta 重写） | 元数据重写 | `Create\|Truncate` |
| `writeAll`（append 路径 `:2345`） | `O_CREATE\|O_APPEND\|O_WRONLY` | split 元数据追加 | 句柄级 append 合成 |

`readMode`/`writeMode` 定义点需在 Shard-9.1 从 `cmd/xl-storage-*.go` 常量处
确认取值（本期证据里只见使用点；不影响 seam 设计）。

## 3. 收口层（`cmd/os-instrumented.go`）覆盖与缺口

覆盖（13 个函数，全部带 osMetric 计数）：`RemoveAll/Mkdir/MkdirAll/Rename/
OpenFile/OpenFileDirectIO/Open/Lstat/Remove/Stat/Access/Create/Fdatasync`，
其中 `OpenFile/OpenFileDirectIO/Open` 通过 `internal/ioutil` 的注入变量
（`read_file.go:30-35`）回连。

**缺口（必须在 seam 设计中单列）**：

| # | 缺口 | 证据 | 处理 |
|---|---|---|---|
| 1 | `os.ReadFile` 不在收口层 | `xl-storage.go:434/847` | 新增 `uuseos.ReadFile`（stat+read 组合） |
| 2 | `readDirFn`（裸 fd + `syscall.ReadDirent`）| `os_unix.go:167/200`、`os-readdir-common.go:260/292` | 函数级替换为 `uuseos.ReadDir*` |
| 3 | `RemoveAll` 直用 `os.RemoveAll` | `os-instrumented.go:125` | 映射 UUSE 递归删除（`remove_all` 或逐层枚举） |
| 4 | `internal/ioutil` 直用 `os.OpenFile` | `append-file_nix.go:34` | 经注入变量/`uuseos` 收口 |
| 5 | `internal/lock` 文件锁 | `internal/lock/lock_nix.go:55`（`syscall.Flock`）| **决策点**：UUSE 无 flock；`.minio.sys` 锁文件在 UUSE 上需改为 engine 进程锁语义或 fail-closed（二期可加 `lock` shim） |

## 4. 独立路径与调用点全清单

### 4.1 目录枚举（独立路径 A）

入口：`readDirFn`（`os_unix.go:167`）与 `readDirWithOpts`（`:259`），均经
`openFileWithFD`（`:43`）裸 fd。

调用点（非测试，共 14 处）：

```text
readDir:   xl-storage.go:956, 1072, 2698
readDirN:  is-dir-empty_linux.go:32, xl-storage.go:1070
readDirFn: data-scanner.go:441, erasure.go:364, erasure-multipart.go:171/172/230,
           metacache-server-pool.go:38, xl-storage.go:3336
```

### 4.2 `RemoveAll`（独立路径 B）

定义链：`RemoveAll`（`os-instrumented.go:125`，直接 `os.RemoveAll`）←
`reliableRemoveAll`（`os-reliable.go:57`，ENOTEMPTY 重试）← `removeAll`
（`os-reliable.go:29`，加 checkPathLength/错误映射）。

调用点（非测试，9 处）：`erasure.go:368`、`prepare-storage.go:104/108`、
`utils.go:413/448/514`、`xl-storage.go:1213/1252/1281/2123`。

### 4.3 生命周期钩子（函数族 3）

- `newXLStorage` 定义：`xl-storage.go:217`（构造时读 format.json、探测
  O_DIRECT）；调用点：`object-api-common.go:63`（本地盘分发）、
  `storage-rest-server.go:1400`（RPC 盘）；
- `newXLStorageDiskIDCheck` 包装：`xl-storage-disk-id-check.go:183`
  （healthCheck 门控，`globalDriveMonitoring`）；
- `xlStorage.Close`：`xl-storage.go:409`；`IsOnline`：`:414`；
- **engine 打开/关闭应挂接在 `newXLStorage`/`Close` 内（每盘一 engine）**；
  分布式模式下 `endpoint.IsLocal` 为假时走 `newStorageRESTClient`
  （`object-api-common.go:383-392`），本地盘（我们的 UUSE 盘）恒为本地
  endpoint，不受影响。

### 4.4 容量/inode/身份（绕过点候选）

| 位置 | 内容 | 处理 |
|---|---|---|
| `internal/disk/stat_linux.go:39-41` | `GetInfo` → `syscall.Statfs`（capacity/free/inodes/FSType/Major/Minor/Device） | UUSE 盘：shim 应答（`freeDataBytes`/`deviceCapacityBytes`）；FSType 合成 |
| `stat_linux.go:103-105` | sysfs rotational/NRRequests（`firstTime` 分支） | 固定 `rotational=false`；NRRequests 可留宿主探测或忽略（审计确认无逻辑依赖） |
| `internal/disk/root_disk.go:23` | `IsRootDisk`（mountinfo） | `globalIsErasureSD` 下已跳过（`xl-storage.go:372`）；单盘模式禁用该告警 |
| `cmd/object-api-utils.go:1286` | `disk.FreeInodes < diskMinInodes`（`!globalIsErasureSD`） | 分布式跳过；单盘用 `DataExtentStats.inodes` 换算 |
| `xl-storage.go:795/847` | format.json 重读与 `disk-id`（`GlobalDeploymentID` 校验） | `uuseos.ReadFile` |
| `xl-storage.go:823-893` | `format.json` 写（`WriteAll` 经收口） | 句柄抽象即可 |

### 4.5 xattr（盘级计数）

`xl-storage.go:53` 引入 `github.com/pkg/xattr`；4 个函数（`:734/744/755/763`）
全部只作用于 `s.formatFile`（`drivePath/.minio.sys/format.json`）上的两个属性
（delete/write 计数）。UUSE 无 xattr → 决策点 Q2（fail-closed 或 engine 级
持久计数）；注意调用点分散在 moveToTrash/delete 路径，需逐一给降级日志。

### 4.6 `globalIsErasureSD` 分支效应（实测定稿）

```text
server-main.go:406        单盘模式判定（ErasureSDSetupType）
xl-storage.go:318         O_DIRECT 能力探测跳过（多盘/分布式）
xl-storage.go:372         root-disk 告警跳过
object-api-utils.go:1286  inode 数检查跳过
data-scanner.go:116/136   scanner 行为分支（缓存/共享）
server-main.go:1144       StandardSCParity==0 时的纠删码校验分支
utils.go:878              启动期分支（默认并行度/扫描）
```

分布式（4 节点单 set）与单盘模式的行为差异必须在 seam 里逐点确认（尤其
`data-scanner` 两处与 `server-main.go:1144`）。

## 5. 绕过点清单（全量，含路由决定）

| # | 位置 | 用途 | 路由决定 |
|---|---|---|---|
| 1 | `cmd/os-instrumented.go` 全族 | 13 个 syscall 收口 | 运行时按盘根分流；UUSE 盘 → shim |
| 2 | `xl-storage.go` openFile 三族 + 475/2128/2261/2263/2345 | 句柄来源 | `uuseos.File` 接口替换 |
| 3 | `xl-storage.go:434/847` `os.ReadFile` | healing 标记/format.json | `uuseos.ReadFile` |
| 4 | `readDirFn/readDirWithOpts`（14 调用点） | 目录枚举 | `uuseos.ReadDir*`（流式分页） |
| 5 | `RemoveAll` 链（9 调用点） | 递归清理 | UUSE 递归删除 |
| 6 | `internal/disk` `GetInfo/IsRootDisk` | 容量/身份 | shim 应答/跳过 |
| 7 | `internal/disk/directio_unix.go:41` `DisableDirectIO` | O_DIRECT 关闭 | UUSE 恒 direct：no-op（不触 `Fd()`） |
| 8 | `internal/disk/fdatasync_linux.go:41` | `Fdatasync` | 句柄 `sync` 映射 |
| 9 | `internal/lock/lock_nix.go:55` | `.minio.sys` 锁文件 | **决策点**：engine 进程锁覆盖或 fail-closed |
| 10 | xattr 4 函数 | 盘级计数 | Q2 决策（fail-closed/engine 计数） |
| 11 | `cmd/bitrot-streaming.go:173` | `ReadFileStream` | 句柄流包装 |
| 12 | `internal/ioutil/append-file_nix.go:34` | split 元数据追加 | 经注入点/`uuseos` |
| 13 | `cmd/utils.go:413/448/514`、`prepare-storage.go:104/108` | 启动清理 `.minio.sys/tmp` | 同 #5（在引擎挂载前执行的调用要按序移到 shim 打开后） |
| 14 | `cmd/storage-rest-server.go`（grid RPC） | 远程盘访问 | 本地盘不受影响；grid 协议不触 POSIX |
| 15 | `cmd/service.go`/`update.go`/`signals.go` | `os.Executable`/信号/服务注册 | 宿主白名单 |
| 16 | `cmd/os_unix.go:357`、`os_other.go:162` `syscall.Sync()` | 全局 sync | 审计可达性后 fail-closed 或引擎级 barrier |
| 17 | `cmd/common-main.go:74` stdout/err `Fd()` | 终端探测 | 宿主白名单（不涉及盘） |

宿主白名单（明确不进 UUSE）：日志/console、证书、`$TMPDIR` 锁文件、
`os.Executable`、终端探测、`--certs-dir`、配置文件。

## 6. 持久化契约（实测定稿，对照设计 §5.5）

| 场景 | 证据 | durable 语义 | UUSE 映射 |
|---|---|---|---|
| 对象 part 写 | `writeAllDirect:2131`：写完 `Fdatasync(w)`（:2195）后 Close | 单文件数据 durable 后才写 xl.meta | `File::sync`（覆盖数据+size，≥datasync）|
| xl.meta 小文件 | `writeAllInternal:2247` → `openFileSync`（O_DSYNC，:2261）| O_DSYNC 写即 durable | 句柄 `syncOnWrite` + Close `sync` |
| xl.meta 大文件（≥DirectioAlignSize）| `:2250-2256` direct + fdatasync | 同上 | 同 part 写 |
| 元数据替换 | `renameAll`（写 tmp → Rename） | rename 提交 | `Client::rename`（journal）|
| 删除 | `deleteFile` → `moveToTrash`（rename）→ 异步 `removeAll` | rename 提交 + 后续删除尽力 | `rename`/`remove` |
| `.minio.sys/tmp` 启动清理 | `prepare-storage.go:104/108`（`go removeAll`） | 尽力清理 | UUSE 递归删除（#5） |
| 全局 sync | `syscall.Sync()` 极端路径 | 全盘 flush | #16 |

结论（固化设计 Q3/Q4）：**MinIO 在 put/complete 点都有显式 sync；UUSE 的
`File::sync`（namespace 级屏障）语义足够且强于 ext4 page-cache 模型**。
“close 兜底 sync”不需要（也不必），O_DSYNC 的映射选择 = `syncOnWrite`
（写路径逐次 sync）还是 Close 时一次 sync，由 Shard-9.1 按 `writeAllInternal`
的实际写模式（单次 `Write(b)` 后立即 Close）定稿——**实测该函数是
“一次 Write + Close”，两种选择等价，取 Close 时 sync**。

## 7. UUSE core 能力缺口复核（对照设计 §3.7）

| 需求 | 现状（已核验） | 缺口 |
|---|---|---|
| 容量 total/free/used | `Engine::freeDataBytes()`（core.hpp:100）、`deviceCapacityBytes()`（:114） | shim 侧换算 total = Σ deviceCapacity；used = total − free |
| inode 统计（`Files/Ffree`） | `DataExtentStats.inodes`（:131） | shim 增加 inode 上限读取（`[metadata] max_inodes` 可从 config 带出） |
| 目录枚举 | `Client::getChildren`（cursor 分页，client.cpp:897） | 已有；需流式迭代 shim（TiKV shim 的整目录缓冲不可用） |
| 递归删除 | `remove` 仅空目录/文件（namespace.cpp:541-571） | shim `remove_all`（纯加法）或 Go 侧逐层 |
| hard link | `Client::link`（client.cpp:888） | 已有；MinIO 当前路径未使用（#5 决策的备选） |
| lock | 无 flock/file-lock | `.minio.sys` 锁文件按 #9 决策处理；engine 进程锁已有（exclusive） |
| xattr | 无 | Q2 决策 |
| 文件整读 | 无独立 API | shim 侧 `stat` + `read` 组合即可（`uuseos.ReadFile`） |

## 8. 对设计文档的修正与回填

1. §3.2 的清单按本文第 2/3/4 节收敛（新增 `internal/lock` #9、`os.ReadFile`
   #3、`data-scanner` 分支证据）；
2. §5.5 按第 6 节固化：O_DSYNC = Close 时 `File::sync`（单 Write + Close
   实测），part 写 = `writeAllDirect` 单次 Fdatasync；
3. §4.3 接口收敛为 7 方法 + `Sync`/`Name`；`ReadFileStream` 跟随句柄；
4. §5.6 容量报告缺口缩小为 shim 换算（core 已有 3 个接口）；
5. §3.7 表更新为第 7 节；
6. 新增环境前置（第 9 节）进 Shard-9.1/9.2 清单。

## 9. 环境与构建前置（进入 Shard-9.1/9.2）

| 项 | 现状 | 动作 |
|---|---|---|
| Go 工具链 | 审计 VM 无 `go` | 安装 go1.24.8（官方 tarball；走 lab 代理镜像） |
| 磁盘余量 | host2 `/` 4.5 GiB free（74%） | 构建前腾出 ≥6 GiB 或把 GOPATH/build 放 `/build`（现挂载点） |
| 冻结源码 | host2 `/build/minio-recon/minio`（浅克隆） | 提升为 `minio/` 交付物基线：从冻结 tag 重新 clone 到项目约定路径，补丁栈另存 `minio/patches/` |
| 目标盘 | 4 节点 `VMware NVME_0002`（`0000:13:00.0`） | Shard-9.4 前完成 3FS 暂停 + 绑核（设计 §6.3） |
| hugepage | .250 当前 `nr_hugepages=0` | 验收前恢复 1024 |

## 10. 未决项（转 Shard-9.1 或设计 Q）

1. `readMode`/`writeMode` 常量取值点确认（`cmd/xl-storage-*.go`）；
2. `internal/lock` 锁文件语义（绕过点 #9）：engine 进程锁 vs fail-closed；
3. `data-scanner.go:116/136` 与 `server-main.go:1144` 分支在多节点/单盘下
   的行为差异确认；
4. `NRRequests`/`Files/Ffree` 的逻辑依赖程度（是否可静态应答）；
5. grid RPC（远程盘）在 4 节点都本地盘时的影响面（预期为 0）。

---

> 证据：`/tmp/minio_audit_shard90.log`（394 行，逐段 grep 输出，含 A–U 全清单）；
> 复现：`node vm.mjs --vm host2 put temp_scripts/minio_audit_shard90.sh /tmp/` →
> `node vm.mjs --vm host2 exec "bash /tmp/minio_audit_shard90.sh"`。
