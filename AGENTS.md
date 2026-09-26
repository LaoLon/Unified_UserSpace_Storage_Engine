# Workspace Instructions

## Codex && opencode 共用项目记忆

- `doc/bug.md` 是 HFS 2.0 当前 Bug 记录；HFS 1.x 的 Bug 历史已清理，HFS 2.0 的已知风险、停止条件和边界以 `doc/design/hfs2.0.md` §17 为准。
- `doc/design/hfs2.0.md` 是 HFS 2.0 当前唯一的架构、边界、接口和实施计划事实源；实现、评审和验收必须以它为准。
- `doc/performance/baseline.md` 是当前 VM 的 FIO/RocksDB 性能基线入口；重测前先核对设备、版本、配置和负载口径，在同环境交错对比。有效新结果按文档规则更新并保留旧数值及证据；不同 VM 或口径另建基线，不直接计算跨环境回退。性能目标仍以 `doc/design/hfs2.0.md` 为准。
- `doc/review/hfs2.0_design_review_20260916.md` 是 HFS 2.0 的设计评审依据；发现设计冲突时先更新设计文档，再开始实现。
- HFS 1.x 的旧设计、性能、review、shard、task 和 Bug 文档已从工作区清理；如需复盘只能查询 Git 历史，不得据此恢复旧功能、旧目录结构、旧容量或旧 API。
- 需要了解当前进展时，先查看 `git log --stat` 和 `git status`，再以当前源码、测试脚本和最新文档为准。
- SPDK 源码必须在 VM 内通过 `git clone`、`git fetch`、`git checkout` 和 `git -C spdk submodule update --init --recursive` 获取及维护；禁止从 Windows 工作区把 `spdk/` 源码目录或其子模块传到 VM 覆盖。Windows 到 VM 的同步仅限 HFS 自身源码、脚本和文档；若 VM 缺少 SPDK，应在 VM 内按锁定 commit 重新 clone/checkout。
- HFS 2.0 的目标配置和正式验证入口使用 `.conf`，单盘 baseline 为 `config/hfs_nvme.conf`、`Nvme0n1`；当前 VM 的目标盘序列号为 `VMware NVME_0002`，PCI BDF 为 `0000:13:00.0`。更换 VM 后先按 sysfs 拓扑核对并更新 BDF，不能凭内核 `nvmeXn1` 编号推断；现有 `config/*.json` 仅是旧 HFS 1.x 材料。除非任务明确要求故障注入或对照实验，不使用 Malloc/Aio 替代正式口径；大体积测试结果默认写入 VM 本地 `/home/hfs`。
- 旧源码已冻结到本地 `src_legacy_frozen/`，不进入 Git、不进入构建、不作为新代码依赖。HFS 2.0 的新源码从 `src/` 重新建立，使用 C++20；旧代码只能用于考古和对照。
- HFS 2.0 是 clean-break：直接管理 raw SPDK bdev，采用 coroutine-first core；不恢复 POSIX preload、fd/stdio wrapper、FUSE、通用 mmap、Blobstore 镜像、旧 async/registered-DMA ABI 或多进程共享挂载。
- 当前架构尚未完成实现。Phase 0 之前不要把旧 Make/CMake source list 当作有效构建入口；新实现必须同步重建 CMake/Make、C++20 ABI 和生命周期测试。
- Git 阶段提交完成后，立即在 `main` 上确认提交、工作树和必要的 VM 回归状态；保留用户已有的未跟踪文件，不使用破坏性 Git 操作覆盖它们。

## VM 与测试约束

- 读取 `E:\Code\workspace.yaml` 获取本地路径、VM 连接信息和 SSH 默认值。
- VM 操作使用 `E:\Code\ssh\pool.py` 提供的 `SSHPool`；所有临时脚本（包括 VM 操作脚本）及其临时依赖统一写入项目根目录 `temp_scripts/`。该目录除占位文件外由 Git 忽略；任务完成后只清理本任务创建的内容，保留他人文件。
- 每次先修改 Windows 工作区文件，再按需同步 HFS 自身源码、脚本和文档到 VM。SPDK 源码及子模块必须在 VM 内通过 Git 获取和维护，禁止从 Windows 工作区同步 `spdk/` 覆盖 VM。
- HFS 2.0 正式功能和性能验证固定使用 `.conf` 配置中的真实 NVMe：单盘 baseline 为 `Nvme0n1`，PCI BDF 必须与当前 VM 的目标控制器一致；多盘测试必须在同一 `.conf` 中声明完整 device set。除非任务明确要求故障注入或对照实验，不使用 Malloc/Aio 替代正式口径。
- VM 重启后先确认 SPDK 设备重新绑定，并确认 `vm.nr_hugepages=1024`；大体积测试结果默认写入 VM 本地 `/home/hfs`，使用前检查根盘剩余空间并控制测试产物大小。
- 不在没有对应实现和验证证据时声称 HFS 2.0 支持 RocksDB、FIO、恢复或性能目标；测试失败必须记录原因和当前边界。

## C/C++ 命名规范

- 类/接口/结构体：大驼峰 (StudentManager)
- 变量/函数/方法：小驼峰 (studentName)
- 宏定义：全大写下划线 (MAX_COUNT)

## 代码注释规范

- 每次写代码必须写英文注释，简洁即可，说明函数/结构体/关键逻辑的用途
- 复杂函数可适当增多，分步骤解释关键逻辑
- 注释用 `/* */` 风格，放在函数或结构体声明上方

## 项目记忆（必读）

任务文档输出规则：当用户输入的文件路径 basename 匹配 `task` 加 8 位日期/任务编号（可带 `.md` 扩展名，例如 `task09151930.md`、`task09150832.md`）时，按该任务执行，并将最终结果只输出为 `doc/task_result/<task_basename>_result.md`（`<task_basename>` 去掉 `.md` 扩展名，例如 `task09160035.md` → `doc/task_result/task09160035_result.md`）；默认不要在 `doc/task_result` 下创建 JSON、TSV、日志、证据目录或其他临时文件，临时证据应放在外部临时目录并在任务完成后清理。

- 凭据不得进入命令输出、日志或用户可见回复。
