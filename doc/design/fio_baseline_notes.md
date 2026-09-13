# FIO 基线索引

当前项目只维护一份有效性能基线：

[`doc/performance/hfs_final_performance_baseline_20260913.md`](../performance/hfs_final_performance_baseline_20260913.md)

该基线固定了 VM NVMe `Nvme0n1`、FIO `randrw=50/50`、`O_DIRECT`、4/8/16/32/64 KiB、
HFS `numjobs=32` 与 raw SPDK `iodepth=32` 的测试口径，并记录最新 HFS/raw SPDK 数据。
旧阶段和诊断样本不再作为性能基线；需要了解历史原因时请查阅对应的 review、shard 或
`doc/bug.md` 条目。
