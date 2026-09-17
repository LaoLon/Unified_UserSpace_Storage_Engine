# HFS 2.0 Shard-85 verification record

时间：2026-09-17  
分支：`main`  
正式设备：`Nvme0n1`, PCI BDF `0000:0b:00.0`  
配置：`config/hfs_nvme.conf`（`metadata_devices=nvme0`, `data_devices=nvme0`,
`metadata_replication=1`）

## 已完成

在真实 NVMe 上执行了 FIO external engine 单盘矩阵：

| 维度 | 覆盖 |
| --- | --- |
| block size | 4K、16K、64K、1M |
| workload | write、read、randwrite、randread、randrw |
| iodepth | 1 |
| numjobs | 2 |
| 结果 | 20/20 运行 `err=0`，submit/complete 闭合 |

在加入 worker-backed pending/completion 队列后，补跑了 QD=1/8/32/64/128、
numjobs=1/2/4/8、4K/16K/64K/1M、write/read/randwrite/randread/randrw 子矩阵，
共 400/400 组合通过，每项 `err=0`。这仍是单盘证据；双盘矩阵尚未据此宣称完成。

此前已完成同一正式设备上的 RocksDB direct/async、正常 reopen、SIGKILL recovery、
`hfs_ldb checkconsistency` 和 `hfsctl check`；FIO 4K loader smoke 加 `--verify=crc32c`
也通过且 `err=0`。

在 FIO arena admission 修正后再次执行单盘组合回归：fillseq/readrandom/overwrite/
readwhilewriting、`checkconsistency`、SIGKILL 后 reopen/check、`hfsctl check` 与
`file_api_test` 均通过；SIGKILL 后 superblock 显示 `clean=0`，随后 mount 与一致性
检查成功完成。该复测仍不替代双盘和完整错误/取消矩阵。

RocksDB FileSystem smoke 现已额外覆盖 DB identity 锁竞争：首个 `LockFile` 成功，
第二个同名锁请求被拒绝，释放后可再次获取；该检查在同一真实 NVMe 回归中通过。

RocksDB FileSystem smoke 还验证了 allocator-backed `GetFreeSpace`：真实 NVMe 单盘
上初始可分配空间为 `31,398,559,744` 字节，写入一个 extent 后为 `31,397,511,168`
字节，释放最后一个硬链接并关闭读句柄后恢复为 `31,398,559,744` 字节。该结果只
证明单盘 data extent 统计和 open-handle 回收语义，不替代多盘容量矩阵。

新增 `hfs_lock_probe` 只执行 `Engine::create` 并持有 `/tmp/hfs2.lock`，不启动 SPDK
或接管 NVMe。真实 VM 上第一进程输出 `lock acquired`，第二进程返回 `4`，并输出
`another engine already holds '/tmp/hfs2.lock'`；该结果直接证明跨进程独占锁边界，
但不替代跨进程 RocksDB workload 的顺序 reopen 矩阵。

RocksDB smoke 另加入 8 个线程共享单个 `FSRandomAccessFile` 的 32 次 positioned-read
回归，全部读取结果与原始 payload 一致；该证据覆盖并发 SST 风格读取，但不替代完整
stock RocksDB SST/compact 压力矩阵。

Shard-80 的 FIO loader 仍以共享 DMA arena 提供整块 `orig_buffer`；arena 结构测试覆盖
了 2 MiB 连续分配、3 MiB cap 下第二次分配返回 `Busy`，以及释放后 bytes 归零。真实
NVMe `--verify=crc32c` loader smoke 继续通过。另执行了 QD=128、numjobs=8、1MiB 的
真实 NVMe 超限探针：FIO 返回非零并记录 `requested fio DMA arena exceeds mount cap`；
由于 FIO 外层仍打印已创建 job 的统计，该结果只证明 cap 拒绝路径，不证明完整高负载
容量矩阵，也不宣称零 worker 启动。

随后在同一真实 NVMe 上完成 QD=128 的 80 项高深度功能矩阵：numjobs=1/2/4/8、
4K/16K/64K/1M、write/read/randwrite/randread/randrw；脚本逐项检查 `err=0`，
80/80 通过。该结果补强了高深度功能正确性，但不替代完整 QD×jobs 矩阵和性能 gate。

带持久化结果的完整矩阵重跑在前 315 项通过后，于 QD=64、numjobs=8、1MiB、write
首次触及正式 512 MiB 共享 arena cap；FIO 返回 `rc=1` 并报告 DMA arena allocation
failure。该项是可定位的资源边界，不是数据错误；矩阵因此停止，不能记录为 400/400
通过，后续需在 arena 预算或 admission 设计修复后重跑。

DmaArena 单元回归还覆盖了共享 cap 满载时的背压：释放在 200ms 等待窗口内唤醒
成功，无释放时 10ms 后返回 `Busy`；该路径未引入无界等待。

带持久化结果的完整 FIO 矩阵最终收集到 385 项 `PASS`、15 项 `RESOURCE_BOUNDARY`、
0 项功能性 `FAIL`。资源边界来自正式 512 MiB arena cap 下的大 buffer/高并发组合，
其中最大组合为 QD=128、numjobs=8、1MiB；结果文件为 VM 上的
`/mnt/hfs-data/hfs-fio-matrix-20260917.log`。

随后修正了 FIO 对 arena `Busy` 的诊断：共享 cap 耗尽现在输出
`requested fio DMA arena exceeds mount cap`，而非与真实 allocator `NoMemory` 混淆；
低深度真实 NVMe `--verify=crc32c` smoke 在该修正后继续通过。

## Shard-81 raw 对照结果

按 §16.2 口径在同一 `0000:0b:00.0` 上完成 raw SPDK `bdevperf` 与 HFS FIO
各 5 次：4K、randrw、读写 50%、QD=32、32 jobs、10 秒、trace/stats 关闭。raw
五次 IOPS 为 `99894.69, 102524.48, 100148.85, 98855.09, 99633.24`，中位数
`99894.69 IOPS`；五次均 `Fail=0/TO=0`。HFS 五次 FIO 均 `err=0`，但从每次
READ/WRITE 吞吐换算的总 IOPS 约为 `7066, 11418, 3550, 6554, 26266`，中位数约
`7066 IOPS`，约为 raw 的 `7.1%`，未达到 85% gate。当前输出未包含正式 p99，
因此不补写 p99 结论；该性能失败触发 §17.3 上游结构修复边界，不能继续扩大性能
测试或宣称 Shard-81 完成。

随后将 FIO engine 从每个 I/O 创建线程改为每个 FIO worker 固定 4 个持久线程，
消除了 qd32×32 jobs 下的千级线程创建。对应 64 MiB/10 秒探索性复测的总 IOPS
约为 `40800, 35800, 30400, 43700, 43200`，明显高于原实现但仍低于 raw；正式
256 MiB gate 下尝试 16 个线程/job 后总 IOPS 约为 `7685, 2424, 5405, 9778,
7020`，并出现 60--80 ms 级 p99，说明盲目放大并发会造成排队和尾延迟放大。
因此保留 4 worker/job 作为当前有界实现，性能 gate 仍未通过。

随后修复 `DataPath` 将请求固定投递到 reactor 0 的问题，按请求在已启动的 data
reactor 间轮询，并在真实 NVMe 上按相同 4K/randrw/50%/QD=32/32 jobs/10 秒口径
重新各执行 5 次。HFS FIO 结果为 `28539.39, 26803.64, 28739.75, 23720.16,
28577.68 IOPS`，中位数 `28539.39 IOPS`；read p99 为 `1941.50--2088.96 us`，
write p99 为 `1957.89--2113.54 us`，5 次均 `err=0`。raw SPDK bdevperf 结果为
`24988.20, 27774.53, 32968.52, 36885.66, 39526.06 IOPS`，中位数
`32968.52 IOPS`，5 次均 `Fail=0/TO=0`。本轮 IOPS 比值为 `86.56%`，但 bdevperf
输出只提供 average/min/max，没有 p99；同时 raw 使用单个 bdevperf core，HFS 使用
两个 data reactor，故不能据此宣称 §16.2 已通过。两组原始结果分别保留在 VM
`/mnt/hfs-data/hfs2-shard81-r2-20260917-134845` 和
`/mnt/hfs-data/hfs2-shard81-raw-20260917-135017`。

为补齐尾延迟证据，随后在同一设备、同一 QD=32 和单个 SPDK core 上启用 bdevperf
latency histogram 重新执行 5 次。raw IOPS 为 `47736.68, 51027.24, 51113.11,
53942.88, 55152.98`，中位数 `51027.24 IOPS`；raw p99 为 `2012.891, 1825.646,
1863.095, 1750.747, 1722.661 us`，中位数 `1825.646 us`，5 次均
`Fail=0/TO=0`。按现有 HFS 5-run 结果，read p99 中位数约 `2007.04 us`、write
p99 中位数约 `2056.192 us`；raw×110% 为 `2008.211 us`，write p99 超出门限。
本轮 raw 仍为单 core、HFS 为两个 data reactor，且采集批次不同，故该结果只补充
了门禁所需的 raw 尾延迟证据，不能把 Shard-81 标记为通过。原始日志保留在 VM
`/mnt/hfs-data/hfs2-shard81-raw-p99-20260917-1355`。

## 当前硬门禁

VM 现有两块独立 NVMe 控制器：`Nvme0n1`（`0000:0b:00.0`，30 GiB）和 `Nvme1n1`
（`0000:13:00.0`，25 GiB）；其余 `sda/sdb` 是虚拟 SATA/块设备，不能代替正式 NVMe
device set。双盘配置 `config/hfs_nvme_dual.conf` 已完成 format、mount、metadata
replica check，以及 2 jobs/4 KiB/16 MiB 的 `randrw` CRC verify，返回 `err=0`。
因此以下项目仍没有伪造通过：

- 双盘 device set 和配置中全部 data device 矩阵；
- metadata replica=2 的掉盘、CRC 不一致、generation 落后组合；
- HFS 与 raw SPDK 的性能 gate 已执行但失败，且当前输出未提供正式 p99；不能作为
  Shard-81 完成证据。

此前缺盘环境下的重复 BDF 探针仍保留为边界证据；当前双盘测试使用两个独立 BDF，且
已确认 `vm.nr_hugepages=1024`、`HugePages_Free=1024`。生命周期验收中发现逐盘更新
clean/dirty 标志会造成 superblock generation 漂移，已修复为共享版本发布；修复后
双盘 `hfsctl check` 连续通过。此前“12 字节写入后 free space 必须立即减少”这一
与 extent 粒度统计不一致的断言已修正；当前双盘 RocksDB smoke 的 free-space reclaim
与 `hfsctl check` 均通过。

真实双盘 fail-closed 探针结果：

| 故障 | 结果 |
| --- | --- |
| 只接管 `0000:0b:00.0`，缺失 required `0000:13:00.0` | `hfsctl info` 非零，SPDK 报 `No such device` |
| `Nvme1n1` 两个合法 superblock 都回退一个 generation | mount 非零，报告 metadata replica 不一致 |
| `Nvme1n1` 两个 superblock CRC 损坏 | mount 非零，报告设备未格式化 |

每项探针后均重新 format 双盘并恢复两个 BDF 到 kernel `nvme`，未留下损坏测试状态。

随后修复了三项双盘实现缺陷：控制 flush 的块/字节单位错误、镜像 superblock 未应用
目标盘 geometry、以及 mirrored journal 保存临时 geometry 引用造成的悬空引用。修复
后的真实双盘 `hfs_rocksdb_smoke` 和 `hfsctl check` 均返回 0；两盘分别报告有效
superblock、checkpoint 和相同 journal 状态。

双盘 RocksDB 验收随后完成：`hfs_db_bench` 的 `fillseq/readrandom/overwrite/
readwhilewriting`（32 keys、2 threads）通过，首次 `checkconsistency` 报告
`entries=32`；SIGKILL 中断 overwrite 后再次 `checkconsistency` 报告 `entries=98`，
最后双盘 `hfsctl check` 返回 0，两盘 generation=393、mount_epoch=8、journal
transaction/corruption 状态一致。该结果补强 Shard-84 的双盘恢复证据，但不替代
完整跨进程错误/取消矩阵或 Shard-81 性能 gate。

双盘 FIO 完整矩阵随后执行 400/400 组合：320 项 `PASS`、80 项
`RESOURCE_BOUNDARY`、0 项功能性 `FAIL`。资源边界集中于 64K/1M 与高 QD×jobs，最大
QD=128、numjobs=8、1M；持久化结果为 `/mnt/hfs-data/hfs-fio-matrix-dual-20260917.log`。

随后复跑同一双盘 400 项矩阵，并额外要求 FIO 的 `issued rwts` 至少包含一个已完成
读写，避免零 I/O 的打开/文件名错误被误记为通过；结果仍为 320 `PASS`、80
`RESOURCE_BOUNDARY`、0 `FAIL`，结果文件为 `/mnt/hfs-data/hfs-fio-matrix-20260917.log`。
测试结束后确认两个 BDF 均恢复到内核 `nvme` 驱动，`vm.nr_hugepages=1024`，无残留
FIO/hfsctl 进程。

Shard-79 的真实 NVMe `file_api_test` 还验证了 `HFS_TEST_FAIL_FLUSH_DEVICE=0`：在
data child 已完成、device flush 尚未提交时，`datasync` 返回 `IoError`；清除变量后
同一数据可继续完成 datasync/sync。该钩子默认关闭且只提供顺序边界证据，不替代掉电
模拟，也不表示其余四个 §16.4 crash 断点已经完成。

Shard-83 的真实 NVMe `file_data_test` 还覆盖了 pre-cancelled synchronous read/write：
两者均在提交数据面前返回 `Cancelled`，且取消写不会改变文件 logical size；in-flight
取消仍由已验证的 RocksDB `AbortIO` 路径负责。

Shard-84 的 RocksDB lock 回归现要求第二个同名 `LockFile` 返回 `Busy`；真实 NVMe
`file_api_test` 通过。该测试同时将 checkpoint/metadata fault-injection 写入扩展到
新的 logical range，避免已 durable epoch 的幂等 sync 绕过注入点。

RocksDB `GetFileSize` 与 `GetFileModificationTime` 的底层 stat 失败现统一保留 HFS
错误类别；缺失路径回归已随真实 NVMe RocksDB smoke 通过。
