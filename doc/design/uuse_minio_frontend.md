# UUSE MinIO frontend 设计（Phase 9 提案）

> 状态：**已实现并冻结（Shard-9.0–9.6，2026-10-11/12）**；性能口径按用户
> 2026-10-12 决定**冻结**：PUT 达标（103–106% ✅）、GET p99 达标（0.73–0.95x ✅）、
> GET 吞吐按已归因边界记录（62–76%，不再追求 gate），见
> `doc/performance/minio_shard95_attribution.md` §12/§15。
> 收敛后的架构/边界/契约以 `doc/design/uuse.md` **§22** 为准（本文件保留提案与
> 完整设计过程）。评审与实施记录：`doc/review/uuse_minio_storage_audit_20261011.md`、
> `minio/README.md`。
>
> 定位：本文是 UUSE 2.0 clean-break 架构（`doc/design/uuse.md`）上的第五个 frontend
> 提案（Phase 9），与前四个同构：
>
> | Frontend | seam | 落地 | 设计依据 |
> |---|---|---|---|
> | RocksDB 8.9.1 | `rocksdb::FileSystem` | `adapters/rocksdb/` | `uuse.md` §11 |
> | FIO 3.42 | external ioengine | `fio/` | `uuse.md` §12 |
> | MySQL 8.0.46 | InnoDB `os_file` | fork + seam patch | `uuse.md` §20 |
> | TiKV 8.5.8 | `components/file_system` + raft-engine FS | `tikv/` | `uuse.md` §21 |
> | **MinIO** | **`cmd/xl-storage` 盘层文件操作** | **fork + seam 抽象（本文）** | **本文** |
>
> 已由用户确认的三个口径：
> 1. **存储后端替换**：fork MinIO，在内部盘层（`cmd` 的 `xlStorage` 文件操作面，
>    集中收口在 `cmd/os-instrumented.go` + `internal/disk`/`internal/ioutil`）把
>    POSIX 文件操作整体换成 UUSE；MinIO 的 S3/对象/纠删码层完整保留，不做网关
>    （gateway/OSS）、不做通用 POSIX 兼容层。
> 2. **分布式 MinIO 多节点**：多节点 MinIO 集群（erasure set 跨节点），UUSE 作为
>    每节点的本地存储；每节点 1 个 MinIO 进程 + 每盘 1 个 UUSE engine；最小拓扑
>    为 4 节点 × 4 盘（单 erasure set，2+2）。
> 3. **环境与门槛（2026-10-11 用户确认）**：lab 5 台 VM 可自由使用，
>    **4 节点集群 = .250/.251/.252/.253**（配置按 workspace 角色，server 节点
>    可用时优先承载 UUSE 盘），.254 作为控制/压测编排节点；**性能门槛统一
>    提升为 ≥ 原生 95%**（较 `uuse.md` §16.2 的 85% 与 TiKV 的 85% 更严，
>    见 §7.4）。
>
> 版本冻结事实（2026-10-11 核查）：
> - MinIO 上游仓库 **2026-04-25 已归档（read-only）**；最后一个开源 tag 为
>   **`RELEASE.2025-10-15T17-29-55Z`**（commit `9e49d5e7a648f00e26f2246f4dc28e6b07f8c84a`）；
> - 该版本 `go.mod` 要求 **go 1.24.0 / toolchain go1.24.8**；
> - 审计锚点基于该 tag 的浅克隆（VM `host2:/build/minio-recon/minio`），
>   Shard-9.0 需产出与 TiKV 审计同规格的 file:line 审计文档。
>
> **Shard-9.0 已完成（2026-10-11）**：审计文档
> `doc/review/uuse_minio_storage_audit_20261011.md`（file:line 全清单 +
> 绕过点路由决定 + UUSE core 缺口复核）；冻结记录与构建入口见
> `minio/README.md`。本文已按审计回填 §3/§4.3/§5.3/§5.5/§5.6/§3.7。
>
> 与既有边界一致的声明：不恢复 POSIX preload/fd wrapper/FUSE/mmap/Blobstore；
> 不改动 RocksDB/FIO/MySQL/TiKV 的契约、默认值与性能口径；MinIO 使用独立
> `.conf`（`config/uuse_minio.conf`）。

---

## 1. 背景与目标

### 1.1 场景

MinIO 是最常用的开源 S3 兼容对象存储。在本实验室语境中，目标是把 UUSE 变成
MinIO 单机/分布式集群的**本地盘后端**：S3 协议、对象语义、纠删码（erasure
coding）、bitrot、heal/rebalance 全部由 MinIO 负责；块设备独占、namespace、
extent 分配、持久化（journal/checkpoint）与 crash recovery 全部由 UUSE 负责。

```text
S3 客户端 (mc / aws-cli / warp / SDK)
        │  标准 S3 over HTTP
        ▼
MinIO 进程（S3 API → ObjectLayer → erasure set → nil/bitrot 生命周期）
        │  cmd/xl-storage 盘层：openFile/openFileSync/openFileDirect/readDirN/...
        │      （原 POSIX → 本文替换为 ↓）
        ▼
UUSE（per-drive engine，N nvme bdev，namespace + journal + checkpoint）
        │  SPDK
        ▼
NVMe device set（每节点本地盘）
```

### 1.2 目标

1. MinIO 的**对象数据、元数据（xl.meta/format.json/tmp/.minio.sys）、bitrot 校验
   读**全部落在 UUSE namespace，宿主盘上不出现任何量级的数据文件；
2. 分布式 MinIO（≥4 节点，单 erasure set）可部署、可 heal、可重启恢复，
   对象语义与 ext4 后端一致；
3. 同盘 A/B：UUSE 后端对比宿主 ext4 后端，功能矩阵全过、正式口径吞吐与
   p99 不回退（**单节点与分布式统一 gate：吞吐 ≥ 原生 95%、p99 ≤ 110%**，
   见 §7.4；该门槛由用户 2026-10-11 确认，较 `uuse.md` §16.2 的 85% 更严）；
4. 保持 UUSE 现有边界：单进程单 engine、bdev 独占、fork fail-closed、
   第二实例 `Busy`。

### 1.3 非目标

- MinIO gateway / S3-over-S3 / NAS 模式（对象存储侧透传）——不做；
- 通用 POSIX 文件系统（小文件语义、mknod、xattr、ACL、稀疏洞、mmap）——不做，
  缺口清单见 §5.7；
- MinIO 加密（KMS/SSE）、ILM 转 tier、Kafka/NATS 事件等外围能力——不在本期
  验证范围（启用时按 fail-closed 或显式记录边界）；
- 多进程共享同一 UUSE 卷——维持 `Busy`（§14.3 语义不变）。

---

## 2. 现状与既有先例

### 2.1 先例（按接入面直径排序）

- **RocksDB adapter**（`uuse.md` §11）：`FileSystem` C++ 接口整面实现，worker-direct；
- **MySQL**（§20）：fork + `os_file` seam + 7 处绕过点 + per-thread client +
  信号转发 + zero-copy DMA；
- **TiKV**（§21）：C++ `rocksdb::FileSystem` + Rust `FileSystem` 双 seam、
  C ABI shim、共享 engine、进程锁协同、宿主白名单、一致性/性能矩阵。

MinIO 接入面与 **MySQL 最接近**（fork 一个 C/C++/Go 大应用，替换其内部文件层），
但 MinIO 是 Go 应用：需要 **cgo + C ABI shim**（接口风格与 TiKV 的
`uuse_tikv_shim.h` 对齐：opaque handle + 负 errno，见 §4.4），数据面并发模型
沿用 MySQL/RocksDB 已验证的 worker-direct 机制而不是 TiKV shim 的单共享
Client。另一个重要差异：MinIO **已有 `cmd/os-instrumented.go`
收口层**（见 §3.2），patch 面比 MySQL 的更集中。

### 2.2 MinIO 存储栈分层（冻结 tag 实测）

```text
S3 API (cmd/api-*.go)
  → ObjectLayer 接口 (cmd/object-api-interface.go)
     → erasureServerPools / erasureSets (cmd/erasure-sets.go, erasure-server-pool.go)
        → SetDisks/erasureObjects → StorageAPI 接口 (cmd/storage-interface.go)
           └── ErasureSD → *xlStorage (cmd/xl-storage.go)      ← 盘层 seam 主体
        → bitrot/纠删码写读 (cmd/erasure-encode/decode, cmd/bitrot-{streaming,whole}.go)
           └── 流式校验读/写经 internal/ioutil → internal/disk（冻结 tag 无 internal/bitrot 包）
```

### 2.3 已确认的可复用收口层（审计起点）

| 收口 | 位置 | 覆盖 | 事实 |
|---|---|---|---|
| syscall 包装/度量 | `cmd/os-instrumented.go` | `OpenFile/OpenFileDirectIO/Open/Create/Remove/RemoveAll/Rename/Mkdir/MkdirAll/Stat/Lstat/Access/Fdatasync` | 全部函数化并带 osMetric 计数；`internal/ioutil` 通过可注入变量（`OsOpenFile/OpenFileDirectIO/OsOpen`）回调这里 |
| 盘层句柄 | `cmd/xl-storage.go:1955/1975/1979` | `openFileDirect/openFileSync/openFile` 三个 open 出口 | 全盘层文件句柄的来源 |
| 目录枚举 | `cmd/os-readdir-common.go:34` `readDirN` + `cmd/os_unix.go`（`syscall.ReadDirent`） | `ListDir`/`is-dir-empty` | 绕过 `os.ReadDir`，需单独 seam |
| 直接 I/O | `internal/disk/directio_unix.go` | `OpenFileDirectIO/DisableDirectIO/AlignedBlock` | O_DIRECT 语义 |
| fsync 类 | `internal/disk/fdatasync_linux.go` | `Fdatasync/FadviseDontNeed` | `cmd.Fdatasync` 的底 |
| 容量/磁盘信息 | `internal/disk/*_linux.go`、`cmd/xl-storage.go:368 getDiskInfo` | `statfs`/`Statfs` | 宿主盘视角，必须替换（§5.6） |

> 结论（影响设计的关键事实）：**MinIO 的数据面 read/write 不是通过注入点集中
> 的**——`xl-storage.go` 上全部 I/O 发生在 `openFile*` 返回的 `*os.File` 句柄方法
> 上（`.ReadAt/.Write/.Truncate/.Seek/.Stat/.Sync/.Close`；实测 call-site 计数：
> Close 17、Stat 5、Write 4、Truncate 3、Seek 1、ReadAt 1、Read 1）。因此接入
> 设计不能停留在“替换 `os.*` 函数”，而必须把**句柄类型**从 `*os.File` 整体换成
> 抽象句柄（§4.3）。这正是本设计与 3FS/MySQL 先例最大的结构差异。

---

## 3. 接入面分析（审计清单，Shard-9.0 产出 file:line）

### 3.1 路径/挂载

- 启动参数：`minio server /mnt/uuse{1..N}/.../data`（本地路径 endpoint）；
- 分布式判定：`cmd/endpoint.go` `IsLocal`/`UpdateIsLocal`、`cmd/globals.go`
  （`globalIsErasureSD`：多盘时 true）——分布式/多盘形态下 `checkODirectDiskSupport`
  被跳过（`cmd/xl-storage.go:316-320`），减少一个 O_DIRECT 探测绕过点，但审计需
  确认 `globalIsErasureSD` 在分布式部署下的取值与副作用；
- POC/单盘回退（`ErasureSD`）路径：本期不作为正式验收对象，但 seam 必须覆盖，
  防止用户用单盘模式绕过。

### 3.2 盘层 seam（`cmd/` 核心 patch 面）

实测（冻结 tag，非测试文件）：`cmd/*.go` 里直接调用 `os.*` 文件操作的点共
约 24 处，其中活跃（Linux 编译）的集中在 `cmd/os-instrumented.go`（9 处，
即收口层本身）、`cmd/xl-storage.go`（4 处）与 `cmd/os-readdir*.go`、`cmd/utils.go`
等少数文件（`os_windows.go`/`os-rename_nolinux.go` 不在 Linux 构建内）。
`cmd/xl-storage.go` 对其余文件系统调用全部经收口函数（实测无 `os.Rename`/
`os.Remove` 直用）。审计清单如下：

| # | 类别 | 现状位置（冻结 tag 锚点） | 处理 |
|---|---|---|---|
| 1 | open 出口 | `xl-storage.go:1955 openFileDirect`（O_CREATE\|O_WRONLY\|O_EXCL，healing 标记与 O_DIRECT 探测复用同一出口）、`:1975 openFileSync`、`:1979 openFile`（`OpenFileDirectIO/OpenFile`） | 换 UUSE 句柄（§4.3） |
| 2 | 数据写 | `xl-storage.go:2092 CreateFile → writeAllDirect`（`CopyAligned`+`Fdatasync`）、`:2269/:2351` `w.Write` | 句柄方法 + `File::datasync` |
| 3 | 数据读 | `xl-storage.go:1868 ReadFile`（`:1927 file.ReadAt`）、`:2017 ReadFileStream`（`io.ReadCloser`） | 句柄方法 + `File::read` 流包装 |
| 4 | 元数据文件 | `xl.meta`/`format.json`：`WriteMetadata:1464`/`readMetadata:522`/`checkFormatJSON:795`/`readAllData:1757`（sync 写走 openFileSync O_DSYNC，>1 MiB 走 direct+fdatasync） | 句柄方法；`fsyncDir` 类语义（§5.5） |
| 5 | 小文件整读 | `os.ReadFile` 直用：`readAllData:847`（format.json）、healing 标记读（`:434`） | `uuseos.ReadFile` 包装（基于 stat+read 或流式读） |
| 6 | 目录枚举 | `ListDir:1045`（`readDirN:1070`）、`is-dir-empty_linux.go:32`、`readDirFn`（`os_unix.go:167`，healing/renameLegacyMetadata 使用）、NSScanner（`:529`） | `uuseos.ReadDir*`（§5.4）：不得走 fd+`syscall.ReadDirent` |
| 7 | 命名空间变更 | `MakeVol/MakeVolBulk:906-946`、`DeleteVol:1011`、`deleteVersions:1086`、`moveToTrash:1292`、`DeleteVersion`、`renameLegacyMetadata:1534`、多版本合并 `Rename` | `createDirectories/remove/rename`（§5.3/§5.4） |
| 8 | 递归清理 | `cmd/os-reliable.go:29 removeAll → RemoveAll`（收口层 `os.RemoveAll` 直用）+ `reliableRemoveAll` ENOTEMPTY 重试（`:57`）、trash 立即清理（`xl-storage.go:1276-1281`） | shim 侧 `remove_all`（递归）或保留 cmd 侧逐层删除（§5.3） |
| 9 | 容量信息 | `getDiskInfo:368`（→ `disk.GetInfo`/`IsRootDisk`）、`DiskInfo:773`、`diskAlmostFilled:1221` | UUSE 引擎容量扩展 + 宿主探测 fail-closed（§5.6） |
| 10 | xattr 计数 | `getDeleteAttribute:734`/`setDeleteAttribute:755`/`setWriteAttribute:763`（盘级删除/写计数） | v1 fail-closed 或 engine 级持久计数（§5.7，建议 fail-closed+日志） |
| 11 | 健康探测 | `xl-storage.go` 挂载点/在线探测、`diskHealthWriter`（写路径包裹） | 路由到 UUSE 状态；`diskHealthWriter` 保持 |
| 12 | sync 收口 | `xl-storage.go:2195 Fdatasync(w)`；`cmd/os_unix.go:357`/`os_other.go:162` `syscall.Sync()`（极端路径） | `File::sync`；全局 sync 审计后 fail-closed 或映射 engine barrier（§5.5） |
| 13 | 引擎生命周期钩子 | `cmd/object-api-common.go:63 newXLStorage`（每盘构造点）、`cmd/storage-rest-server.go:1400` | UUSE engine 的 open/close 挂接点（§5.1）；磁盘初始化前不得有宿主 I/O |

审计补充（Shard-9.0，见 `doc/review/uuse_minio_storage_audit_20261011.md`）：

- **`internal/lock` 文件锁**（`internal/lock/lock_nix.go:55` `syscall.Flock`）：
  `.minio.sys` 锁文件位点，UUSE 无 flock 语义 → v1 用 engine 进程锁覆盖或
  fail-closed（审计 §5 #9）；
- **`os.ReadFile` 不在收口层**（`xl-storage.go:434/847`）→ `uuseos.ReadFile`
  （审计 §3 #1）；
- **`DisableDirectIO`**（`internal/disk/directio_unix.go:41`）在 UUSE 盘上为
  no-op（UUSE 恒 direct，不触发 `Fd()`）（审计 §5 #7）；
- **`syscall.Sync()`** 仅 `os_unix.go:357`/`os_other.go:162`（极端路径），
  审计其可达性后 fail-closed 或映射 engine barrier（审计 §5 #16）。

### 3.3 两条独立路径：目录 fd/`syscall.ReadDirent` 与 `os.RemoveAll`（实测新增）

审计发现两条不经过 `*os.File` 句柄方法的独立路径，必须在 seam 设计中单独覆盖：

1. **目录枚举走裸 fd**：`readDirFn`（`cmd/os_unix.go:167`）与 `readDirWithOpts`
   （`:259`）经 `openFileWithFD`（`:43`）打开目录 fd，直接 `syscall.ReadDirent`
   循环解析 `dirent`。`readDirN`/`ListDir`/`is-dir-empty`/healing 扫描全走这里。
   **结论**：目录 fd 不构成可移植的文件句柄（需要能把 `Read` 合成合法
   `dirent` 缓冲，且 open 语义不同），UUSE 侧不应合成该行为；应把
   `readDirFn`/`readDirWithOpts` 改为 `uuseos.ReadDir*`（返回名字+类型的有界
   分页），即 §3.6 的 seam 必须**函数级替换**这两个入口；
2. **递归删除直用 `os.RemoveAll`**：`RemoveAll`（`cmd/os-instrumented.go:122`）
   包装的就是 `os.RemoveAll`；`removeAll`（`cmd/os-reliable.go:29`）在其上加重试。
   UUSE `remove` 只删空目录/单文件，因此 `RemoveAll` 必须映射为
   shim 侧递归删除（或 cmd 侧逐层枚举+删除），**不得**保留 `os.RemoveAll`
   兜底（宿主 `os.RemoveAll` 对 namespace 路径会 no-op 地删宿主目录或报错）。

### 3.4 校验读/写流（`cmd/bitrot*` + `internal/ioutil`）

- `internal/ioutil`：`OsOpenFile/OpenFileDirectIO/OsOpen` 注入变量（`read_file.go:29-34`）、
  `read_file_noatime_*`、`append-file_nix.go:34`、`ioutil.go:390`（摊销 Fdatasync）；
- 冻结 tag 下 bitrot 逻辑在 `cmd/bitrot.go`/`bitrot-streaming.go`/`bitrot-whole.go`
  （无独立 `internal/bitrot` 包），
  审计需按实际包布局修正锚点；
- 处理：这些包被 `cmd` 经注入变量接管；需要把注入点的返回类型从 `*os.File`
  泛化（§4.3），或让 `cmd` 侧不再经 `internal` 打开文件。

### 3.5 启动/自检/裸盘假设（绕过点候选）

| # | 位置 | 用途 | 处理 |
|---|---|---|---|
| 1 | `xl-storage.go:316` `globalIsErasureSD`/`checkODirectDiskSupport` | O_DIRECT 探测 | 分布式下跳过；单盘模式路由到 UUSE |
| 2 | `cmd/is-dir-empty*`、`NSScanner` | 目录扫描 | `getChildren` 递归 |
| 3 | `getDiskInfo`（`statfs`/mountinfo/rotational） | 容量、FS 类型、旋转判定 | UUSE 应答 + 宿主探测结果禁用 |
| 4 | `format.json` 磁盘 ID/`fsUUID`、`disk ID` 与挂载点校验 | 盘身份 | UUSE volume UUID 应答 |
| 5 | `xl.meta` 内联 vs part 文件的 `O_DIRECT` 选择 | 对齐/阈值 | UUSE 无 O_DIRECT 概念，`AlignedBlock` 映射到 arena/对齐缓冲（§5.5） |
| 6 | `cmd/os_other.go:162`/`os_unix.go:357` `syscall.Sync()`（全局 sync） | 关机/极端路径 | 审计；若可达则映射为 engine 级 sync 或 fail-closed 日志 |
| 7 | `cmd/`、`internal/` 其余 `os.*` 直用 | 分散调用 | Shard-9.0 全量清单：`grep -rn 'os\.(OpenFile\|Open(\|ReadDir\|Remove\|Rename\|Stat\|Lstat\|Mkdir)'`（实测 `cmd/xl-storage.go` 内除句柄方法外 0 处直用 `os.`——收益于既有收口，见 §2.3） |
| 8 | `cmd/object-api-utils.go:1286` | inode 数校验（`disk.FreeInodes`/`disk.UsedInodes`，`globalIsErasureSD` 为 false 时生效） | 分布式（ErasureSD）下跳过；单盘模式需 UUSE inode 统计应答（§5.6） |

### 3.6 seam 策略结论

**A 案（本文采用）**：在 `cmd/` 内引入 `internal/uuseos`（或等价包）作为
盘层文件抽象：
- 所有 `openFile*` 返回该抽象句柄（接口含 `ReadAt/Write/WriteAt/Read/Seek/
  Truncate/Stat/Sync/Name/Close`，覆盖 §2.3 的 call-site 集合）；
- `OpenFile/OpenFileDirectIO/Open/...` 等收口函数在运行时按“盘根是否为 UUSE 盘”
  分流：UUSE 盘 → shim；其余（日志/证书/console/`.minio.sys` 若配置在宿主）→
  原生 `os`，保持 stock 行为；
- `readDirN`/`readDirFn`、`Fdatasync`、`statfs` 等同层分流；目录枚举必须
  **函数级替换**（§3.3：`openFileWithFD` + `syscall.ReadDirent` 不构成可移植
  句柄面），`RemoveAll` 必须映射为 UUSE 递归删除（§5.3）；
- 禁止静默回落：UUSE 盘上的任一操作若 shim 不可用必须 fail-closed（错误上报 +
  审计计数），不得落宿主；启动自检必须复用同一分流逻辑（§3.5），不得走独立
  的宿主路径。

**B 案（备选，评审时决策）**：直接替换 `Disk`/`StorageAPI` 后端实现（不动
`xl-storage` 内部）。缺点：`xlStorage` 内部对 `os.File` 的依赖已被逐点证实
（§2.3），替换后端等于在本目录外复制一份 3400 行 `xl-storage.go` 的等价实现，
且 bitrot/`internal/ioutil` 路径仍会绕过；维护成本更高。**建议 A 案**。

### 3.7 对现有 UUSE core API 的差距（v1 结论 + 增补）

| MinIO 需求 | 现有能力 | 差距/处理 |
|---|---|---|
| open/create/append/truncate/stat | `Client::open`/`File::*` | 够用；`OpenFlags::Create` 已含父目录创建 |
| pread/pwrite（offset 读写） | `File::read/write`（IoVec+offset） | 够用；≤1 MiB child 内部拆分 |
| fdatasync/fsync | `File::datasync/sync` | 够用（`sync` 为 namespace 级屏障，强于 per-file）；审计定稿：part 写用 `sync` 覆盖 `Fdatasync`，元数据 O_DSYNC = Close 时一次 `sync` |
| 目录枚举（有界、可续扫） | `Client::getChildren`（cursor 分页、按名有序） | 够用；需流式迭代 shim（TiKV 的整目录缓冲不可搬）；替换 `readDirN/readDirFn`（§5.4） |
| mkdir 递归/单层、rename、unlink、link | `createDirectories`/`rename`/`remove`/`link` | 够用；`remove` 仅空目录/文件，递归删除由 shim `remove_all` 或 Go 侧枚举（§5.3） |
| 盘容量/FS 类型/旋转 | `Engine::freeDataBytes()`、`deviceCapacityBytes()`、`dataExtentStats()`（已核验） | 仅需 shim 换算 total/free/used 与 `Files/Ffree`（`DataExtentStats.inodes` + config `max_inodes`）；FS 类型合成、rotational 固定应答 |
| 目录 fsync（dirfd 语义） | 无 dirfd 概念 | 审计未发现 `fsync(dirfd)` 调用点；如后续出现，映射 `File::sync` |
| 文件锁（`.minio.sys` LOCK） | 无 flock | v1 engine 进程锁覆盖或 fail-closed（审计 §5 #9） |
| xattr（盘级写/删计数） | 无 | v1 fail-closed + 日志（建议），或二期 engine 持久计数（Q2） |

---

## 4. 架构与组件设计

### 4.1 组件图（单节点本地视图，4 盘示例）

```text
MinIO 进程（1/节点）
├── internal/uuseos（patch 新增，盘层抽象）
│     ├── 分流：UUSE 盘 → CGO shim；宿主路径 → os
│     └── 句柄类型 uuseos.File（ReadAt/Write/Truncate/Sync/Stat/...）
├── cgo binding（cmd/uuse_bridge_*.go，build tag: uuse）
│     └── C ABI: libuuse_minio_shim.so
├── disk1 (/mnt/uuse1/data) ── UUSE engine 1 ── nvme0n1
├── disk2 (/mnt/uuse2/data) ── UUSE engine 2 ── nvme1n1
├── ...
└── 日志/console/证书等宿主路径 → os（白名单）

libuuse_minio_shim.so（新增，C++20）
├── minio_engine_open/close（每盘一 engine；按 .conf 路径的进程级弱引用注册表，
│   与 tikv shim 的 gEngineRegistry 同模式）
├── minio_client_*（per-thread client registry —— 沿用 MySQL/RocksDB worker-direct
│   机制；TiKV shim 的共享单 Client + mutex 仅作参考，不满足本负载的并发面）
├── minio_file_*（open/read/write/sync/stat/close，句柄表 + refcount）
├── minio_dir_*（createDirectories/readdir 迭代/remove/rename/remove_all）
└── minio_diskinfo（容量/统计，调 Engine::dataExtentStats + 新增接口）
```

### 4.2 进程与线程模型

- 每节点 1 个 minio 进程；**每盘 1 个 UUSE engine**（`.conf` 单盘口径），
  避免多盘共享 volume 时 MinIO 单盘故障语义被掩盖；
- Go 侧并发是 goroutine：采用 **per-thread client registry**（惰性创建、weak
  注册、上限回落共享 client 并计数/日志——机制复用 MySQL §20.4 与 RocksDB
  worker-direct，不是 TiKV shim 的单共享 Client；TiKV 的实测教训是共享
  Client + mutex 会把路径操作串行化，不适合 MinIO 的目录/元数据并发量）；
- `File` 句柄由进程级 owner client 持有；线程用自己的 client
  `submitRead/submitWrite` + 本线程 `poll`（worker-direct，Shard-106 机制）；
- engine 单例：同 `.conf` 路径的第二个 `engine_open` 复用同一 engine
  （weak 注册表）；全部句柄释放后可重新打开（TiKV shim 已实现该模式）；
- cgo 线程注意：Go runtime 会大量创建/销毁 OS 线程，registry 必须有硬上限与
  回收策略（线程退出时自关 client），防止 client/线程耗尽；另注意 core 要求
  `Client::close` 在 client 的 frontend 线程调用（TiKV shim 用专用 client 线程
  承接），MinIO 侧需要同样的 owner 线程约定；
- SPDK 线程空信号掩码 + MinIO 的信号处理：MinIO 用 `signal.Notify`（Go
  runtime 信号线程）；参考 MySQL Shard-103 先例，安装转发 handler 把
  SIGTERM/SIGINT 转发到 MinIO 的 signal 通知路径，保证优雅停机；
- 停机顺序（§14.3 不变）：拒绝新请求 → drain outstanding → 关闭全部 open
  File/Client → `Engine::close`（shim）→ engine 持久化与退出。

### 4.3 文件句柄抽象（本设计的核心改动）

```go
// internal/uuseos/file.go（示意，评审后定稿）
type File interface {
    ReadAt(p []byte, off int64) (int, error)
    Write(p []byte) (int, error)
    Read(p []byte) (int, error)
    Seek(off int64, whence int) (int64, error)
    Truncate(size int64) error
    Stat() (os.FileInfo, error)
    Sync() error
    Name() string
    Close() error
}
```

- `xl-storage.go` 中全部 `*os.File` 调用点改为该接口（实测方法面：
  `Write/ReadAt/Read/Seek/Truncate/Stat/Close` 共 7 个，31 处调用；无
  `Fd()`/`WriteAt`/`Name` 使用——见审计 §2.2）；接口收敛为
  `Read/ReadAt/Write/Seek/Truncate/Stat/Sync/Close`（`Name` 只读元数据）；
- 另需 `uuseos.ReadFile(path)`（整读小文件；替换 `os.ReadFile` 直用点，§3.2 #5）
  与 `uuseos.ReadDir*`（目录枚举；替换 fd+dirent 路径，§3.3）；
- `Stat()` 的 `os.FileInfo` 合成：size/mtime/mode 从 UUSE `FileStat` 映射，
  `IsDir()=false`（目录句柄不走该接口）；
- `Seek` 仅用于 append 语义，映射为内部偏移跟踪；
- O_DIRECT/O_DSYNC 标志不进入 UUSE 语义层，但需保留语义等价：
  O_DIRECT → 无宿主页缓存（UUSE 本就 direct，天然满足）；
  O_DSYNC → open 后立即/每次写入后 `Fdatasync`？**不**——审计已定稿
  （审计 §2.3/§6）：`writeMode` 即 `O_DSYNC`（`cmd/xl-storage_noatime_supported.go:33`），
  且 `writeAllInternal` 的实际写模式是"一次 `Write(b)` + `Close`"，因此
  O_DSYNC 等价于 **Close 时一次 `File::sync`**；`readMode` 为
  `O_RDONLY|O_NOATIME|O_NONBLOCK`（`:30`，不支持时回落 `O_RDONLY`）。
- 只读打开（`O_RDONLY`）在 UUSE 层没有权限模型：句柄一律返回可读写接口，
  只读语义由调用方遵守；`xl-storage.go` 中 read 打开点（`:499/:1766/:1894/
  :2033/:3084`）不会发生写入，审计需确认无"只读句柄被写"的路径；
- 不能改 `internal/ioutil` 注入点签名的地方（如 `ReadFileDirectIO` 返回
  `*os.File`）：在 `cmd` 层不再使用这些注入点，或把注入点类型泛化为接口。

### 4.4 C ABI shim（`minio/shim/`）

命名与风格对齐 `tikv/shim/uuse_tikv_shim.{h,cc,hpp}`（opaque handle + 负 errno；
TiKV C ABI 实测只有 engine 级 `open/close/remove/rename/create_dirs/read_dir/
exists` + 文件级 `pread/pwrite/append/truncate/allocate/sync/file_size`，没有
client/handle 面——MinIO 需要在其上补齐 client registry、句柄 refcount、
流式目录迭代与 fdatasync/variant 映射）：

```c
/* opaque handles */
typedef struct uuse_minio_engine uuse_minio_engine; /* per-drive engine */
typedef struct uuse_minio_client uuse_minio_client; /* per-thread client */
typedef struct uuse_minio_file   uuse_minio_file;   /* open file */
typedef struct uuse_minio_dir    uuse_minio_dir;    /* dir iterator */

int  uuse_minio_engine_open(const char *conf_path, int format_if_needed,
                            uuse_minio_engine **out, char *errbuf, size_t errlen);
void uuse_minio_engine_close(uuse_minio_engine *e);
int  uuse_minio_diskinfo(uuse_minio_engine *e, uint64_t *total, uint64_t *free,
                         uint64_t *used, char *errbuf, size_t errlen);

uuse_minio_client *uuse_minio_client_get(uuse_minio_engine *e); /* thread-local */
void uuse_minio_client_put(uuse_minio_client *c);

int uuse_minio_open(uuse_minio_client *c, const char *path, int flags,
                    int mode, uuse_minio_file **out, char *errbuf, size_t errlen);
int uuse_minio_pread(uuse_minio_file *f, void *buf, size_t len, uint64_t off, size_t *done);
int uuse_minio_pwrite(uuse_minio_file *f, const void *buf, size_t len, uint64_t off, size_t *done);
int uuse_minio_fdatasync(uuse_minio_file *f);
int uuse_minio_fstat(uuse_minio_file *f, uuse_minio_stat *out);
int uuse_minio_ftruncate(uuse_minio_file *f, uint64_t size);
void uuse_minio_close(uuse_minio_file *f);

int uuse_minio_mkdir_all(uuse_minio_client *c, const char *path);
int uuse_minio_remove(uuse_minio_client *c, const char *path);
/* Recursive delete for MinIO RemoveAll semantics; optional pure addition
 * (v1 may instead enumerate + remove per entry on the Go side). */
int uuse_minio_remove_all(uuse_minio_client *c, const char *path);
int uuse_minio_rename(uuse_minio_client *c, const char *from, const char *to);
int uuse_minio_link(uuse_minio_client *c, const char *from, const char *to);
int uuse_minio_stat(uuse_minio_client *c, const char *path, uuse_minio_stat *out);

/* bounded directory iteration (cursor-based, mirrors getChildren) */
int uuse_minio_dir_open(uuse_minio_client *c, const char *path, uuse_minio_dir **out);
int uuse_minio_dir_next(uuse_minio_dir *d, uuse_minio_dirent *ents, size_t max,
                        size_t *count, int *eof);
void uuse_minio_dir_close(uuse_minio_dir *d);

int uuse_minio_version(char *buf, size_t len); /* ABI/version check */
```

约定（与 TiKV shim 一致）：
- 全部同步 API，错误经 `errbuf` + 负 errno 返回；Go 侧映射到 `os.SyscallError`
  风格错误，保证 MinIO 的 `osErrToFileErr`/`isSysErr*` 判定继续工作；
- `uuse_minio_close` 与句柄 refcount：Go 侧不得在 UUSE callback 仍可见时释放，
  shim 内部用 intrusive ref（File 的 shared_ptr 在 close 时不立即析构）；
- CGO 回调路径（cgo → C 是同步调用，无 Go 回调进 C++）——无跨语言回调面，
  生命周期风险显著低于 TiKV（无 Rust async）。

> **实现状态（Shard-9.1，2026-10-11）**：`minio/shim/` 已落地并过真机测试
> （smoke 14/14、lifecycle 8/8、`corruption=none`）。v1 采用"每 .conf 弱引用
> 注册表 + 单个共享 path-op Client（专用 frontend 线程 + mutex）+ 文件数据走
> 调用线程 coroutine"的模型（TiKV shim 已验证的形态）；per-thread client 与
> worker-direct 优化按 §4.2 留待性能阶段。实测修正：`OpenFlags::Create`
> **不自动创建父目录**（`Namespace::resolveParent` fail-closed），MinIO 侧
> 先 `mkdir_all` 再 `open`——§5.1 表格已同步；容量统计按 `dataBlocks ×
> metadata_block_size` 与 `freeDataBytes()` 同单位换算（扇区与 metadata block
> 单位不同，v1 曾出现 8× 误差，已修复并留测试）。
>
> **实现状态（Shard-9.3/9.4，2026-10-11）**：单节点功能 **21/21**（真实路由；
> 含多版本列表修复——`RenameData` 的 `xioutil.ReadFile` 未路由导致旧版本元数据
> 读不到而丢版本）；崩溃重跑 59/60（末对象 kill 边界记为 RPO 边界，两轮 sweep
> 一致、无损坏）。**两节点分布式集群（.250/.251）核心路径通过**：跨节点
> PUT→GET 字节一致、LIST、交叉读写（`DIST-CROSS-PASS`）；韧性：单节点宕机读
> 可用、写 fail-closed、恢复后正常（`RESILIENCE-PASS`）。待补：逐节点 namespace
> 分布核对、strace 无宿主落盘、4 节点扩展。本轮共修复 **12 个真实缺陷**（含
> 分布式 `oDirect` panic），详见 `minio/README.md` §2/§3.2。
>
> **实现状态（Shard-9.5 性能归因与优化，2026-10-12）**：**Round 1** trace
> 归因确认写路径短板 = **每次 barrier 的全量 checkpoint**（MinIO 每对象
> create/rename 都是 shape change）：barrier p50 3.4–3.9ms，内部
> `snapshot`(freeze+encode) 1.5–2.0ms + `ckpt_write` 1.7–1.9ms，payload 随
> namespace 线性增长（≈141B/slot，p50 1.3MB）；数据路径另有 extent 尾部
> zero-fill 写放大（512B xl.meta → ≈1MiB 零写）。优化：①`File::write` 的
> zero-fill 只覆盖**写后文件大小以内**，`File::truncate` 增长时对已映射区间
> 补零——不变量（文件大小内未写字节读为 0）保持，设备写放大 ≈3×→≈1×；
> ②checkpoint freeze/encode 快路径（`InodeExtentMap::appendTo`、目录名
> arena、raw-cursor 两遍编码、按 hash 桶遍历）；③控制路径
> `spdk_dma_malloc` 去零填充；④trace 追加 `SYNC_PHASE`/`SYNC_BATCH` 与
> `CHKPT_FREEZE.arg3`（append-only，旧布局不变，uuse.md §16.5 同步）。
> **Round 2** 结构性修复：**shape change 改为 journal 可回放记录**
> （append-only 类型 `CreateInode/RemoveEntry/RenameEntry/LinkEntry/FreeInode`，
> `RecordPayload.reserved1` 变长扩展携带名字；replay 精确复刻 live mutation
> 含 allocator 与 free-slot 决策），barrier 常规只写 journal 事务 + delta
> checkpoint（barrier p50 3.9ms→0.62ms，checkpoint 由每轮 1800+ full 降为
> 2 full + 3–4 delta）；`uuse.md` §9.3/§10.4(b) 已同步修订，新增
> `UUSE_TEST_FORCE_CHECKPOINT` 测试钩子（设计 §5.8）。A/B：**PUT 47%→103%
> （已达标）**、GET 78%→71%（ext4 冷缓存 GET 3249 MiB/s 为设备带宽限制，
> UUSE 读路径探针峰值 5270 MiB/s@8 线程，GET 差距在每对象小读排队 +
> MinIO 自身开销）；GET p99 0.72–0.78x（优于 ext4）；gate 当时尚未全达标，
> 剩余差距与 Round 3 方向见 `doc/performance/minio_shard95_attribution.md`
> §10（**2026-10-12 已冻结，见文首与 §7.4**）。
>
> **Round 4（元数据缓存，2026-10-12）**：按用户决策实现「仅元数据」有界缓存
> （**shim 层**，core 无页缓存不变）：≤64 KiB 元数据文件、64 MiB + LRU 上限、
> 每次命中用内存 stat（size/mtime/generation）复核、写/truncate/allocate/
> open(TRUNC)/remove(_all)/rename 严格失效、`UUSE_MINIO_META_CACHE=0` 可关、
> 计数经 shim diag。实测小读平均 1627→977µs（−40%）；最终 A/B：PUT 106%
> （r1 ✅）、**GET 71.3%/76.0%（❌）、GET p99 0.73x（✅）**。负结果：
> 分 reactor、`maxChildBytes` 拆分子传输均回退。剩余 GET 差距为设备在单
> qpair、QD≈8–16 下的持续读吞吐（≈2.0–2.5GB/s）vs ext4 内核多队列
> （2.7–3.2GB/s）；引擎读路径自身 8 线程顺序读 5270 MiB/s，差距在并发深度/
> 驱动路径。9.5（**2026-10-12 冻结，见文首与 §7.4**）。
>
> **2+2 拓扑验证（2026-10-12，用户要求）**：MinIO 拆为 2 服务端（.250/.251，
> 2 盘 EC:1：写 quorum 2/读 quorum 1）+ 2 外部客户端（.252/.253）：外部矩阵
> 24/24、跨节点 verify/sweep 60/60、kill 一端后读 60/60 而**写挂起**（>90s
> 无错误返回，恢复后写/校验/清扫全部复原）；单客户端 PUT 233–243 /
> GET 454–458 MiB/s（errors=0，链路实际 ≈3.8–4 Gbps）。2 客户端并发聚合未
> 取到（.252 测试中整机失联，需宿主侧重启）。详见 `minio/README.md` §2
> "2 存储 + 2 客户端"行与"环境备注（2026-10-12 拓扑测试）"。
>
> **实现状态（Shard-9.2 完成，2026-10-11）**：Go 侧盘层抽象
> `minio/internal/uuseos/` + `cmd` 补丁栈（`minio/patches/apply-uuse-seam.py`，
> 24 锚点）已在 251 真机跑通端到端：**bucket/PUT/GET（字节一致）/LIST/remove
> 全过 + 重启持久化**。要点：
> - **路由模型**：进程级注册表按"盘根前缀"查找，UUSE 盘上的任何操作都经
>   cgo → shim（禁止宿主回落）；未注册路径保持 stock os；`Rename` 跨盘或
>   混合后端返回 `EXDEV`（fail-closed）；`MINIO_UUSE_DRIVES= root=conf[,..]`
>   声明 UUSE 盘；
> - **seam 面**：`xl-storage.go`（openFile 三出口/ReadFile/ReadFileStream/
>   healing/format.json/getDiskInfo/ListDir/引擎挂载钩子）+ `os-instrumented.go`
>   （RemoveAll/MkdirAll/Rename/Stat/Lstat/Remove 分流）+ `os_unix.go`
>   （readDirFn/readDirWithOpts/access）；
> - **File 接口为 drop-in**（含 `Fd()/Name()`）：除 writeAllDirect 的
>   `CopyAligned/Fdatasync` 两处显式判型外，31 处句柄调用点零改动；UUSE 的
>   `Fd()` 返回无效描述符且 O_DIRECT 探测路径对 UUSE 盘直接走 shim open；
> - **实测修正**：`Stat` 的 `ModeDir` 位、`createDirectories` 父目录语义、
>   Go `main` 在仓库根（`./cmd/` 是库包——用 `go build .`）、cgo 外链需显式
>   `-luuse_core` + rpath、`MINIO_UUSE_DRIVES` 解析等，全部记录在
>   `minio/README.md` §3.2；
> - 已知边界：`.minio.sys` 元数据/bitrot 流式路径复用既有的字节流接口（已
>   经 `readFileUuse`/句柄 Read + Seek 覆盖）；4 节点分布式、`uusectl check`
>   与结构化矩阵属 Shard-9.3/9.4。

### 4.5 路径路由

| 路径 | 路由 |
|---|---|
| drive 根（`/mnt/uuseN/data`）及以下（`bucket/object/xl.meta`、`part.N`、`.minio.sys/tmp`、`.trash`） | UUSE（`createDirectories` 按需创建） |
| 日志（`--log-dir`/stdout）、console、证书、`--certs-dir`、临时文件 | 宿主白名单 |
| 无 UUSE 配置的盘（stock 模式） | 原生 os（运行时可混布，但正式验收要求所有盘为 UUSE） |

- 挂载点与盘根：MinIO 会把 endpoint 路径规范化（`filepath.Abs`/EvalSymlinks），
  审计需确认到 UUSE 路径不发生宿主 EvalSymlinks 破坏（`/mnt/uuseN` 可以是普通
  目录作为“路径锚”）；此锚目录在宿主上仅是一个空目录，不承载任何 MinIO 数据
  文件；**shim 引擎在 `newXLStorage` 构造时打开（§3.2 #13），早于该盘的任何
  目录探测；锚目录本身的 `stat`/`mkdir` 由宿主完成（UUSE 引擎打开失败时
  fail-closed，不做宿主回落）**；
- 每盘 `.conf` 绑定真实 NVMe（`config/uuse_minio_nodeN.conf`），BDF 按节点本地
  拓扑填写（约定同 AGENTS.md：核对序列号，不凭 `nvmeXn1` 编号推断）。

---

## 5. 详细设计

### 5.1 文件打开与句柄表

- `openFile/openFileSync/openFileDirect` 三个出口统一进 `uuseos.OpenFile`；
- flags 映射（`os` 常量 → UUSE `OpenFlags`）：

| MinIO flags | UUSE | 备注 |
|---|---|---|
| `O_CREATE` | `OpenFlags::Create` | **不自动创建父目录**（实测修正，Shard-9.1）：`Namespace::resolveParent` 对缺失父目录返回 `NotFound`，MinIO 侧先 `mkdir_all` 再 open；无 `O_EXCL` 时对已存在文件幂等 |
| `O_EXCL` | `OpenFlags::Exclusive` | `AlreadyExists` 映射 `EEXIST`；`openFileDirect` 的 healing 标记/探测即走此组合（§3.2 #1） |
| `O_TRUNC` | `OpenFlags::Truncate` | |
| `O_RDONLY/O_RDWR/O_WRONLY/O_APPEND` | 句柄内记录 | UUSE `OpenFlags` 无权限位：只读打开返回可读写句柄，写权限由调用方逻辑保证；`O_APPEND` 在句柄层合成（写前 seek 到 EOF） |
| `O_DIRECT` | 忽略（UUSE 恒 direct） | 对齐检查由句柄缓冲层合成（§5.5） |
| `O_DSYNC` | 句柄记录 `syncOnWrite` | 见 §5.5 精确映射 |

- Go 句柄表：`uint64 id → shim handle`，Go 侧 `Close` 幂等；cgo 不可跨调用栈
  保存 Go pointer，全部经 id/表访问。

### 5.2 数据读写

- `ReadAt/Write/Read`：直接映射 `File::read/write`（`std::span` + offset），
  长度与 offset 用 uint64；短读语义与 POSIX `pread` 一致（EOF 返回短读）；
- MinIO 的 O_DIRECT 对齐缓冲（`xioutil.ODirectPool*`/`AlignedBlock`）：
  UUSE 内部已有 DMA/bounce 机制，句柄层不需要 O_DIRECT 对齐；`CopyAligned`
  可以保留（对齐缓冲仍正确），但审计可优化为直接 `io.CopyBuffer`（性能项，
  不作为功能门禁）；
- `ReadFileStream`（`io.ReadCloser`）用小型顺序读实现（bounce 缓冲复用），
  遵守 `length` 上界；
- 并发：MinIO 可对同一 `xl.meta`/part 文件并发读；同一 inode 的写串行由 UUSE
  scheduler 保证（§7.2 契约）；
- 多版本并发 PUT 会并发修改同一卷内多个 key 的 xl.meta：UUSE 调度器只做
  同 inode 的区间依赖，不同文件天然并行；同一 xl.meta 的"last writer wins"
  由 MinIO 的版本 ID 去重兜底（验收矩阵有并发覆盖用例）。

### 5.3 目录与命名空间

| MinIO 操作 | UUSE 映射 | 语义说明 |
|---|---|---|
| `MakeVol`/`MakeVolBulk` | `createDirectories` | 幂等；已存在不报错 |
| `StatVol` | `stat(path)` 合成 `VolInfo` | mtime 用 stat.mtimeNs |
| `ListVols` | `getChildren("/")` 过滤目录 | 正式模式 `.minio.sys` 隐藏由上层处理 |
| `DeleteVol` | 分页 `getChildren` + 自底向上 `remove`（可选 shim 侧 `remove_all` 纯加法优化） | `remove` 仅空目录/文件（非空返回 `EINVAL`）；不用 `os.RemoveAll`（§3.3）；递归实现留在实现 shard 定稿 |
| `DeleteVersion(s)`/trash | `remove`/`rename`（moveToTrash 为 rename 到 `.trash`） | rename 目标覆盖规则：UUSE `rename` 已实现原子替换（类型/非空检查），与 POSIX 语义一致 |
| `renameLegacyMetadata`、目录级 `Rename` | `Client::rename` | 单一 journal transaction |
| 目录 fsync（如有） | `File::sync`/metadata barrier | 见 §5.5 |
| `readDirN`/`readDirFn` | `uuse_minio_dir_*`（cursor 分页） | 每次 `dir_next` 返回 ≤count 项；MinIO 只在 count 限制枚举数量时用；排序不做保证（UUSE 按名字有序更适合稳定输出） |
| `checkPathLength` | 保留（纯字符串检查） | |
| `LinkFile`（如被调用） | `Client::link`（inode 级硬链接） | Rename 族由 `Client::rename` 覆盖；`link` 是"先 link 后删源"读旧版本链路的备选（若启用则按 link 实现） |

### 5.4 目录迭代的 cursor 语义

- `getChildren(path, cursor, maxEntries)`：cursor = 上一页最后一项的 name；
  `uuse_minio_dir` 持有 cursor/缓冲/physical 句柄；
- 该 seam 必须同时替换 `readDirN`（`ListDir`/`is-dir-empty`）与 `readDirFn`
  （`os_unix.go:167`，healing/renameLegacyMetadata 的遍历）——两者原实现都经
  `openFileWithFD` + `syscall.ReadDirent`（§3.3），不是句柄方法；
- `readDirN` 的 `count` 语义（至多 count 项）与分页天然对齐；
- NSScanner 全量扫描大目录：按页消费，不物化全表（TiKV shim 的
  `uuse_tikv_read_dir` 是整目录缓冲实现，**不可**直接照搬）；
- 目录项排序：按名字（UUSE ordered index），MinIO 不依赖具体顺序但稳定有好处。

### 5.5 持久化与一致性（关键设计）

**MinIO 的持久化契约（冻结 tag 实测）**：

1. 对象数据写：`writeAllDirect` 完成后 **`Fdatasync(w)`**（`xl-storage.go:2195`），
   然后 `Close`；之后才写 `xl.meta`（元数据先于数据可见的窗口不存在）；
2. 元数据同步写（`writeAllInternal(sync=true)`）：小文件走 `openFileSync`
   （`writeMode = O_DSYNC`，`xl-storage_noatime_supported.go:33`）；>
   `DirectioAlignSize` 的 xl.meta（含 inline data）改走 `writeAllDirect` +
   `Fdatasync`（`:2250-2256` 注释明示该取舍）；两个分支之后都经 `renameAll`
   原子替换（写 tmp → `Rename`）；实测该函数是"一次 `Write` + `Close`"，
   O_DSYNC 在 UUSE 侧等价于 **Close 时一次 `File::sync`**（审计 §6 定稿）；
3. 删路径先 `moveToTrash`（rename）再异步清理；
4. 目录创建无显式 fsync（ext4 上依赖 rename 的目录项持久性）；
5. 全局 `fsync`：`syscall.Sync()` 仅出现在 `os_other.go`/`os_unix.go` 的极端路径。

**UUSE 映射与要求**：

| MinIO 语义 | UUSE 映射 | 约束 |
|---|---|---|
| `Fdatasync(part 文件)` | `File::datasync()`（或 `sync()`） | TiKV C ABI 只暴露 `sync`；MinIO shim 增加 `fdatasync` 入口，底层允许走两者之一，并在 shim 头写明"`sync` 覆盖 `datasync` 语义"（§10.2 已实现）；不允许 early return 跳过 data flush |
| `O_DSYNC` 元数据写 | 句柄 `Close`（或显式 `Sync`）→ `File::sync()` → namespace checkpoint + 设备 flush | 复刻 ext4 O_DSYNC 语义（rename 前 durable） |
| `Rename`（tmp→final） | `Client::rename`（journal） | 持久性依赖 UUSE checkpoint/barrier；结合上一条保证“rename 后 crash 不丢最终对象” |
| `Remove/moveToTrash` | `Client::remove/rename` | 同 journal 事务语义 |
| 目录 fsync（如审计发现） | 映射 metadata barrier；若 MinIO 依赖“dir fsync 后 rename 持久”，用 `File::sync` 保证 | Shard-9.0 审计给出点位；无点位则显式记录“无 dir fsync 依赖” |
| `syscall.Sync()` | 审计；可达则 fail-closed 或映射 engine 级 `syncMetadata` | 不允许静默 no-op |

- 目录级元数据（mkdir 的持久性）：UUSE 的 shape 变更走 checkpoint；`File::sync`
  已是 namespace 级屏障（`uuse.md` §20.3），单点调用即可覆盖“rename + 目录项”的
  durable 需求，但需在验收矩阵中专门覆盖 crash 断点（§7.3）；
- 审计补充（Shard-9.0）：`cmd/utils.go:413/448/514` 的 `RemoveAll` 是 pprof
  输出目录（宿主白名单，不进 UUSE）；启动清理 `prepare-storage.go:104/108`
  与 `xl-storage.go:1213/1252/1281/2123` 在 shim 打开后执行（清单见审计 §4.2）。
  结论：**不引入 core 新语义**，全部经现有 `datasync/sync/rename/remove` 表达；
  若审计发现 MinIO 依赖 ext4 的“数据不 fsync 也随进程退出持久”假设（类似
  TiKV bug #50），以“MinIO 显式 sync 点”为准并逐点记录，必要时在 shim 侧
  close 兜底 sync（代价与语义必须写入评审）。

### 5.6 磁盘信息与容量

- `getDiskInfo`/`DiskInfo` 当前读宿主 `statfs`/mountinfo/rotational——对 UUSE 盘
  必须替换（审计 §4.4/§7：缺口已收敛为 shim 侧换算）：
  - `total/free/used`：shim `uuse_minio_diskinfo` ← `Engine::freeDataBytes()`
    （core.hpp:100）+ Σ `deviceCapacityBytes()`（:114）；used = total − free，
    不需要 core 新接口；
  - `FSType`：合成 `"uuse"`（审计确认无逻辑依赖具体值）；
  - `rotational=false`（NVMe；`disk.GetInfo` 的 sysfs 读取在 `firstTime`
    分支，固定应答即可）；
  - `Files/Ffree`：`DataExtentStats.inodes`（core.hpp:131）与 config
    `max_inodes` 换算；
- inode 数校验：`cmd/object-api-utils.go:1286` 在 `globalIsErasureSD=false`
  （单盘模式）时检查 `disk.FreeInodes < diskMinInodes`；分布式（ErasureSD）下
  跳过。单盘模式需 UUSE inode 统计应答，否则该检查 fail-closed；
- `getDiskInfo` 的 root-disk 判定（`disk.IsRootDisk`，读 mountinfo）在
  `globalIsErasureSD` 下已跳过（`:372`）；单盘模式下用 UUSE 应答替代或禁用
  该告警（宿主盘判定对 UUSE 盘无意义）；
- `diskAlmostFilled`（`:1221`）依赖 free 比例：走同一 shim；
- 若某检查无法回答，**fail-closed 并记录**，不得回落宿主 `statfs`（宿主根盘
  数字会误导 MinIO 的容量判定与拒绝写入策略）。

### 5.7 禁用项与 fail-closed 清单（v1）

| 项 | 处理 |
|---|---|
| xattr（`setDeleteAttribute`/`setWriteAttribute` 等） | fail-closed + 日志（或二期 engine 持久计数；评审决策点 Q3） |
| mmap / `fallocate` punch hole | 不适用（MinIO 用 `Truncate`，映射 `File::truncate`；审计确认无 punch hole 调用） |
| `os.File.Fd()` 语义（fsyncDir/`unix.*Fd`） | 审计为 0 后记录；出现即 fail-closed |
| 加密 SSE/KMS、ILM tier、gateway 模式 | 本期禁用；配置出现 fail-closed（错误消息明确） |
| 单盘无 UUSE 配置却混布 UUSE 盘 | 允许混布为诊断模式，正式验收要求全盘 UUSE |
| 宿主路径直接写 datadir | strace 校验，出现即失败（§7.1） |

### 5.8 故障注入与可测试性

- shim 提供与 TiKV shim 相同的测试钩子：`UUSE_MINIO_TEST_FAIL_*`（open/read/
  write/sync/rename/remove 指定点注入 errno），用于 MinIO 错误路径验证；
- Go 侧 smoke：`minio/shim/minio_smoke_test.cc`（C++）+ `cmd/uuse_bridge_test.go`
  （Go，build tag `uuse`）；
- 崩溃探针：kill -9 在 PUT 完成各阶段（Fdatasync 前/后、rename 前/后）。

---

## 6. 构建、部署与配置

### 6.1 构建

- MinIO：VM 内 git 获取冻结 tag（浅克隆已验证可行），Go 1.24.8（官方 tarball）；
  `make build`（`buildscripts/`）→ `minio` 二进制 + cgo 链接
  `libuuse_minio_shim.so`（`-L`/`-rpath`，运行期 `LD_LIBRARY_PATH`）；
- UUSE 侧：`minio/shim/` 加入 CMake（与 `tikv/shim` 同模式：
  `add_library(uuse_minio_shim SHARED ...)` + smoke/lifecycle target）；
- 版本冻结记录：MinIO tag/commit、Go 版本、UUSE core ABI（`UUSE_CORE_ABI_MAJOR=2`）；
  升级任一组件需重跑 Shard-9.0 审计与全矩阵；MinIO 上游已归档，无 rebase
  需求，但补丁栈仍按 `minio/patches/` 管理（锚点补丁脚本 + 从干净 tag 重放，
  参照 `tikv/patches/README.md` 的登记方式）；

### 6.2 配置（每盘一份）

```conf
# config/uuse_minio_node1.conf（示例）
[uuse]
schema_version = 1
instance_name = uuse-minio-node1
volume_uuid = auto
process_lock = exclusive

[storage]
metadata_devices = nvme0
data_devices = nvme0
metadata_replication = 1
metadata_block_size = 4096
journal_mb = 256
checkpoint_mb = 512
extent_mb = 1
max_extents_per_file = 65536
...  # 其余沿用 uuse_nvme.conf 的正式口径；dma_arena_mb 按 MinIO 并发上调
```

- `journal_mb`/`dma_arena_mb`：按 MinIO 并发写面（每盘 QD 高、并发 PUT 多）
  在 Shard-9.5 校准后写入 `config/uuse_minio_nodeN.conf` 并在文档记录；
- **每 engine 资源（hugepage/RSS）必须按多 engine 预算收敛**：现状
  `config/uuse_nvme.conf` 的 `dma_arena_mb=512`、（除 DMA 外的）journal 256 MiB
  与 metadata reactor 是**单盘单引擎**口径；单节点 4 盘会叠加约 2 GiB+ 的
  hugepage/metadata 需求，在小规格 lab VM（3.3–7.2 GiB）上会超预算或触发
  预检失败。正式集群按每节点 1–2 个 UUSE 盘规划，其余盘保持原生；
  若确需 4 盘/节点，先把各盘 `dma_arena_mb` 降到 64–128 MiB 并在预检通过后
  再上矩阵；
- MinIO 侧：`MINIO_VOLUMES`/命令行只声明 endpoint 列表；日志/console 宿主路径；
  环境变量不进 `.conf`（project 规则：UUSE 配置唯一入口 `.conf`）。

### 6.3 分布式拓扑（lab）

- **4 节点单 erasure set**（用户 2026-10-11 确认）：.250、.251、.252、.253 各跑
  1 个 MinIO 进程；每节点 1 个 UUSE 盘（`VMware NVME_0002` @ `0000:13:00.0`，
  逐节点按序列号核对）；.254 作为控制/压测编排节点，同时承担负载与验收脚本；
- 各节点盘数一致（MinIO 要求）；每盘为独立 NVMe（BDF 按节点核对）；
- 资源约束（各 VM 2–4 vCPU / 3.3–7.2 GiB）：每节点 1 盘时按 §6.2 预算收敛；
  `.252/.253` 若作为集群节点承载 UUSE 盘（3.3 GiB），则该 VM **不再同时当
  压测客户端**——负载由 .254 统一发起，或对两台 VM 采用分时（先当节点收集
  节点内数字，再释放给客户端角色）；
- **与现有 3FS 实验的冲突（必须处理）**：.250/.251 当前作为 3FS storage 节点
  （其中 .251 的 UUSE target 就占用同一块 `NVME_0002`），.250 的
  `vm.nr_hugepages` 被设为 0；MinIO 验收前需：
  1. 停止 .250/.251 的 3FS storage（以及 .252/.253 的 FUSE 挂载、.254 的
     管理服务，若需要）；
  2. .250 恢复 `vm.nr_hugepages=1024`；.251 重新绑定 `0000:13:00.0` 到
     `uio_pci_generic`；
  3. 对 4 块目标盘做 `uusectl format`（**破坏性**：会清空 3FS UUSE target 等
     既有数据，需在维护窗口执行并记录恢复路径）；
  - 3FS 集群的恢复手册在 `uuse_3fs_handover.md`，验收完成后按需恢复；
- 单节点部署（1 VM、1 盘）用于开发/冒烟与同盘 A/B，不作为分布式验收替代。

### 6.4 部署与运行脚本

- `minio/run_single_node_vm.sh`：单节点（1 盘）冒烟；
- `minio/run_cluster_vm.sh`：4 节点集群拉起/停止（生成每节点 `.conf` +
  systemd unit 或前台进程，按 lab 惯例前台 + 日志文件）；
- `minio/run_functional_matrix_vm.sh`：S3 功能矩阵（mc/aws-cli 驱动）；
- `minio/run_perf_ab_vm.sh`：同一物理盘 ext4（内核 nvme）↔ UUSE（uio）交错 A/B
  （复用 TiKV 的同盘切换脚本模式）；
- 预检：`0000:13:00.0` → `uio_pci_generic`、`vm.nr_hugepages=1024`、构建产物
  `/root/code/build-uuse`（与 TiKV 相同）。

---

## 7. 验收矩阵

### 7.1 结构门禁（所有出口）

- Client/File/Dir 句柄闭合（open==close、无泄漏）；`spdk_submit==spdk_complete`；
- 无宿主 datadir 落盘（strace：`openat` 白名单只允许日志/证书/`/proc` 等）；
- fail-closed 清单全部生效（§5.7）+ 审计计数为 0；
- CTest：UUSE 侧新增 shim smoke/lifecycle 用例；RocksDB/FIO/MySQL/TiKV 门禁
  不回退（`uuse.md` §16.3、§20.7、§21.6 不变）。

### 7.2 功能矩阵（单节点 + 分布式）

| 类别 | 用例（mc / aws-cli / warp 驱动） |
|---|---|
| bucket | 创建/删除/列举（空、非空、特殊名） |
| 基本对象 | put/get 各尺寸（0/1 B/4 K/64 K/1 M/5 M+）；gzip/range/get-metadata；空对象；多并发同名覆盖 |
| 多版本 | versioning enable；同 key 多版本；delete marker；按版本取/删 |
| multipart | 分片上传全流程、断点续传（list-parts）、complete/abort、分片大小边界 |
| copy/rename | server-side copy、对象与目录语义（S3 无 rename，由 copy+delete 在 MinIO 层验证） |
| 列举 | 前缀/分隔符/delimiter + max-keys 分页；`mc ls --recursive`；大目录扫描（NSScanner） |
| bitrot | 静默位翻转注入（测试钩子）→ 读校验失败上报；heal 后恢复 |
| 特殊路径 | `.minio.sys/tmp` 残留；`.trash` 清理；重启后 trash 清理路径 |
| 重启 | SIGTERM 优雅重启；kill -9 重启；集群滚动重启 |
| 分布式 | 4 节点单 erasure set：多对象写入后各节点数据文件齐全；kill 1 节点 → 读可用 + heal；kill 1 节点写入 → 恢复后 heal 补齐；加盘/换盘（视范围） |
| 边界 | 磁盘满（`diskAlmostFilled`）拒绝写；inode/namespace 上限（`max_inodes`）错误面（§5.6 的 inode 校验）；超长路径 |

### 7.3 崩溃与恢复

- 断点：① part 数据写完、Fdatasync 前；② Fdatasync 后、xl.meta 写前；
  ③ xl.meta 写后、rename 前；④ rename 后、客户端 200 前（MinIO 侧）；
  ⑤ `.trash` 清理中；⑥ checkpoint 写前/后；
- 判定：重启后对象完整（md5/etag 校验）、不出现半对象；`uusectl check` corruption=none；
  MinIO `mc admin heal --dry-run` 无意外条目；
- 分布式：kill -9 单节点后集群继续服务；重启节点触发 heal 后数据一致。

### 7.4 性能（同盘交错 A/B）

- 工具：`warp`（S3 基准，put/get、分布报告）为主，`mc` 大文件 + `dd` 类探针为辅；
- 口径：单节点 1 盘，ext4（内核 nvme + XFS/ext4 挂载）vs UUSE（uio），
  同 VM 同盘同参数，5 轮 median，交错执行；
- **主 gate（用户 2026-10-11 确认，单节点与分布式统一）**：
  - PUT 吞吐 **≥ ext4 基线 95%**；GET 吞吐 ≥ 95%；混合（warp mixed）≥ 95%；
  - p99 ≤ ext4 基线 110%；errors=0；
  - 该门槛显著高于 `uuse.md` §16.2 / TiKV 的 85% 口径，达到前不得宣称
    性能验收通过；
- 归因：4 K/64 K/1 M/4 M+ 分档；multipart、list、heal 各种子项单独报告；
- 分布式 gate：4 节点集群整体吞吐 ≥ 同拓扑 ext4 集群 95%（.252/.253 网络为
  千兆，必须同时报告各节点绝对数字以避免网络瓶颈误判）；
- 注：gate 是 **UUSE vs 原生（同拓扑、同硬件）的比值**。若两侧同时受网络
  （千兆/Soft-RoCE）限制，比值仍按 ≥95% 判定，但报告必须同时给出绝对值并
  标注瓶颈归属；不得用"网络受限"豁免比值。
- 已知差异记录：UUSE 无宿主页缓存 → GET 命中冷数据的行为与 ext4 不同；
  该差异在报告中必须列出（不是回退）。
- **性能口径冻结（用户 2026-10-12 决定，取代本节的 gate 追求）**：
  - 冻结结论：PUT ≥95% ✅（103.3–106.0%）、GET p99 ≤110% ✅（0.73–0.95x）、
    errors=0；**GET 吞吐不再追求 gate**，冻结值 62.1–76.0%（随 ext4 页缓存状态/
    数据集大小；GET-only 口径 62.1%，混合口径 71.3–76.0%）；
  - 冻结依据：GET 差距已完整归因——① 设备在单 qpair、QD≈8–16 下持续读吞吐
    ≈2.0–2.5 GB/s vs ext4 内核多队列 2.7–3.3 GB/s；② 无宿主页缓存导致每对象
    ~2 次元数据小读（已由 shim 元数据缓存消除约一半，净收益 +4.6%）；
    ③ 负结果已回退：size-based reactor 拆分、`maxChildBytes` 子传输拆分；
  - **不引入通用 page cache / payload 级缓存**（2026-10-12 分析结论）：MySQL/
    RocksDB/TiKV 自带 buffer pool/block cache 且正式口径用 direct I/O；3FS 缓存
    属客户端 + storage 侧 chunk metadata；MinIO 是唯一"无自有缓存"例外，
    已由 shim 元数据缓存覆盖。若需重开本 gate，触发条件见归因报告 §15。

### 7.5 与既有 frontend 的回归

- RocksDB L1–L4、FIO 正式门禁、MySQL、TiKV 矩阵：每 shard 出口不回退
  （对齐 `uuse.md` §20/§21 的执行纪律）；
- UUSE 代码增补（如 `Engine` 容量报告、`remove_all` 快捷路径）必须为纯加法。

---

## 8. 风险、边界与停止条件

| 风险 | 缓解 |
|---|---|
| `cmd/` 内 `os.*` 直用点遗漏 → 数据落宿主 | Shard-9.0 全量审计 + 每 shard strace 校验；未白名单即失败 |
| 句柄抽象漏方法（`*os.File` 方法面） | 审计的实测方法面（§2.3）+ 编译期全量替换（类型系统强制） |
| cgo 线程模型与 UUSE frontend-owner 约束冲突 | per-thread registry + 上限回落；stress 用例覆盖线程退出/回收 |
| `statfs` 等宿主视角检查误导 MinIO | §5.6 全部替换；`diskAlmostFilled` 专项用例（写满语义） |
| xattr 缺失导致盘计数逻辑静默失效 | v1 fail-closed + 日志；评审决策 |
| 分布式模式下 erasure set 对盘数/端点的一致性要求 | 部署模板固定盘数与布局；验收矩阵覆盖 |
| MinIO 上游已归档（无上游安全修复） | 冻结 tag + 内部补丁栈管理；升级只能换 tag 并重跑审计 |
| AGPLv3 许可与 fork 维护 | 仅实验室内部使用/不分发修改后的二进制；补丁栈与新代码分离登记（`minio/patches/`），必要时按决策点 Q4 处理 |
| 性能回退（Go/cgo 边界、同步语义加严） | 同盘 A/B 早测（Shard-9.5 前置）；sync 语义逐点计量 |
| 与 3FS/其它实验争抢 lab VM | 已按用户确认的 .250–.253 集群口径执行（§6.3）；3FS 期间暂停，验收后按恢复手册还原；不对 3FS 做未记录的破坏性操作 |
| 95% 门槛（严于既往 85%） | 早测（Shard-9.5 前置 + Shard-9.2 冒烟后立即单盘梯度）；同步语义/批量/arena 调优列为专项 |

**停止条件**（对齐 `uuse.md` §17.3）：datadir 文件落宿主；对象语义/校验和不一致；
heal 无法补齐；kill -9 后已提交对象丢失或目录残缺；句柄/帧泄漏；
fail-closed 清单出现静默降级；RocksDB/FIO/MySQL/TiKV 门禁回退 >5%；
MinIO 同盘/同拓扑 A/B 回退 >5%（原 95% gate 口径；2026-10-12 用户决定冻结
性能口径后，GET 吞吐不再作为停止/关闭条件；PUT、p99 与既有 frontend 门禁不变）。

---

## 9. 实施计划（Shard-9.x，编号待并入 shard 计划）

| Shard | 内容 | 出口门禁 |
|---|---|---|
| 9.0 | 版本冻结（tag/commit/Go）+ 审计文档（`doc/review/uuse_minio_storage_audit_YYYYMMDD.md`，file:line）+ seam 策略定稿（A/B） | 审计覆盖 §3 全部清单；无未定锚点 |
| 9.1 | UUSE core/shim：`minio/shim` C ABI（engine 注册表 + per-thread client + 句柄表 + 流式目录迭代 + fdatasync/整读小文件映射；TiKV C ABI 为起点）+ smoke/lifecycle + 容量报告增补（如需） | CTest 通过；ASAN/TSAN 干净 |
| 9.2 | MinIO fork patch：`internal/uuseos` 抽象 + `cmd` 逐点替换 + Go 桥接 | 单节点冒烟（PUT/GET/list/multipart）通过；strace 无宿主落盘 |
| 9.3 | 单节点功能矩阵 + 崩溃矩阵 + 错误注入 | §7.2/7.3 单节点全过 |
| 9.4 | 集群环境准备 + 分布式集群验收（.250–.253 四节点）：3FS 暂停/恢复手册、hugepage/uio 复位、4 盘 format；功能 + heal + kill/重启 + 无宿主落盘 | §7.2 分布式全过；环境恢复路径已记录 |
| 9.5 | 性能同盘 A/B + 分布式 A/B（统一 ≥95% gate）；调优（journal/arena/批量/同步窗口） | **已冻结（2026-10-12 用户决定）**：PUT ✅ 103.3–106.0%、GET p99 ✅ 0.73–0.95x；GET 吞吐按边界记录（62–76%）不再追求；分布式 A/B 未按口径测量（网络受限）；详见 `doc/performance/minio_shard95_attribution.md` §15 |
| 9.6 | 收口：支持边界、补丁栈 README、设计文档回填（`uuse.md` 新增 §22 + Phase 9 条目）、bug 归档 | §7.5 回归全过 |

每个 shard 出口执行 §7.5 的既有 frontend 回归（对齐 TiKV 纪律）。

---

## 10. 对 `uuse.md` 的必要增补（2026-10-12 已执行）

1. §3.1 支持列表新增“MinIO S3 frontend（Phase 9）”；§3.2 保持“无 POSIX 兼容层”
   表述不变（MinIO 接入是应用内替换，不是兼容层）——**已执行**；
2. 新增 §22 “MinIO frontend（Phase 9）”，内容以本文 §1–§7 收敛——**已执行**
   （`doc/design/uuse.md` §22，含实现状态与支持边界指针）；
3. §17.2 风险表新增 MinIO 行；§17.3 停止条件新增 MinIO 条目——**已执行**；
4. §18 实施顺序追加 Phase 9（Shard-9.0…9.6）——**已执行**；
5. `doc/shard/` 新增 shard 计划（或并入现有计划文件）；`config/` 新增
   `uuse_minio_nodeN.conf`——执行记录改为收敛在 `minio/README.md`（本环境以
   `/etc/uuse-minio.conf` 为唯一运行配置，四节点一致），`doc/shard/` 不新增
   计划文件。

---

## 11. 待评审决策点（Q1–Q4）

> 环境口径（4 节点 = .250–.253、.254 控制/压测）与性能门槛（≥95%）已于
> 2026-10-11 由用户确认，见文首与 §7.4，不再作为待决项。
> O_DSYNC 语义（原 Q3）已由 Shard-9.0 审计定稿（Close 时一次 sync），
> 见 `doc/review/uuse_minio_storage_audit_20261011.md` §6。
> **Q1 seam 策略：已确认采用 A 案（2026-10-11，用户拍板）**，B 案经审计证据
> 排除（补丁面虽宽但覆盖完整；B 案无法覆盖目录枚举/`RemoveAll`/bitrot 路径）。

- **Q2 xattr 行为**：盘级写/删计数（`setWriteAttribute`/`setDeleteAttribute`）
  v1 fail-closed+日志，还是实现 engine 级持久计数？
- **Q3 片内 parity 口径**：4 节点单 erasure set 的 parity（EC:2 默认 vs EC:1）
  在分布式性能矩阵中如何固定？（默认 EC:2；如 95% gate 受 parity 影响，
  需在章前声明 parity 作为报告字段，不降 gate。）
- **Q4 许可与分发**：MinIO 为 AGPLv3 且上游已归档；确认仅限实验室内部分发
  定制二进制（不分发/不提供网络服务给第三方），补丁栈按 `minio/patches/` 与
  上游文件分离登记，是否认可？
