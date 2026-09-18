# HFS 2.0 Repair-97：FIO 文件拓扑与 shard 利用率

日期：2026-09-18
状态：已实现，待真实 NVMe 多拓扑性能对照

## 问题与边界

HFS 2.0 按 `hash(file_id) % R` 固定 file owner shard。同一个 shared file 的请求
集中在一个 shard 是架构不变式，不能通过在 FIO adapter 中随机迁移请求来“制造”多
reactor 利用率；当前 actor-local SPDK API 也要求在目标 reactor 调用。

## 修复

FIO adapter 新增：

```text
hfs_file_per_job=0   # 默认，保持 shared-file 正式 gate
hfs_file_per_job=1   # 每个 fio worker 使用 <path>.job-<thread_number>
```

开启后，每个 worker 获得独立 HFS file，因而可以在不改变 core ownership、inode
ordering、取消和 late completion 语义的情况下，测量多 file→多 shard 的真实拓扑。
默认值为 `0`，因此正式 shared-file 结果的比较口径不变。

## 验证计划

同一设备、同一配置和同一 FIO 参数下分别运行：

1. shared-file：`hfs_file_per_job=0`；
2. per-job file：`hfs_file_per_job=1`；
3. `data_reactors=1` 与 `data_reactors=2` 对照。

trace-on 归因跑应检查 `ShardMap`、`RequestShardSubmit/Complete` 和每个 shard 的
`ShardPump`；只有实际出现 submit/complete 的 shard 才计入有效利用率。正式 gate
仍然使用 shared-file、trace/stats off，不用 per-job 结果替代 gate。

## 当前验证

VM Release 构建会覆盖 `fio-hfs` 目标；FIO 3.42 提供 `td->thread_number`，用于生成
稳定的 worker 后缀。该修复本身不宣称吞吐提升，真实 NVMe 的 shared/per-job 对照和
最终性能门禁留给 Repair-98。
