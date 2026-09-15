# FIO 基线索引

当前项目只维护一份有效性能基线：

[`doc/performance/hfs_final_performance_baseline_20260913.md`](../performance/hfs_final_performance_baseline_20260913.md)

该基线固定了 VM NVMe `Nvme0n1`、FIO `randrw=50/50`、`O_DIRECT`、4/8/16/32/64 KiB、
HFS `numjobs=32` 与 raw SPDK `iodepth=32` 的测试口径，并记录最新 HFS/raw SPDK 数据。
旧阶段和诊断样本不再作为性能基线；需要了解历史原因时请查阅对应的 review、shard 或
`doc/bug.md` 条目。
# Shard-66 v2 measurement contract

The reproducible 66A harness is `scripts/db/shard66_baseline_v2.sh`; its offline
aggregator is `scripts/db/shard66_summary.py`. It formats and prewrites the mount for
each sample, keeps shared-file and per-job-file results separate, and uses five
trace-off/stats-off repetitions. Raw evidence belongs under `/mnt/hfs-data`. The
shard-66 implementation record and gate decisions are in
`doc/performance/hfs_shard66_baseline_v2.md` and
`doc/shard/hfs_posix_sync_boundary_topology_shard_plan.md`.
