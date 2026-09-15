# Shard-66 baseline v2

This document defines the reproducible measurement contract for the sync-boundary
topology work. It does not replace the completed NVMe baseline until the harness has
run on the target VM and its medians have been reviewed.

## Formal workload

Run `scripts/db/shard66_baseline_v2.sh` on the VM with
`config/hfs_nvme.json`, `HFS_BDEV_NAME=Nvme0n1`, and output under `/mnt/hfs-data`.
The script formats before every sample and prewrites every file. Formal samples are
trace-off and stats-off; the optional `HFS_BASELINE_ATTRIBUTION=1` sample is kept
separate and is never included in formal medians.

The matrix is:

| file model | block size | operation | jobs | samples |
|---|---|---|---:|---:|
| shared-file | 4K/16K/64K | randread/randwrite | 1/8/16/32 | 5 |
| per-job-file | 4K/16K/64K | randread/randwrite | 1/8/16/32 | 5 |

The default run uses the compatibility contract `HFS_DATA_REACTORS=5` (owner plus
four data reactors) with owner CPU0, data CPUs 1–4, and application CPUs 5–7. Set
`HFS_BASELINE_TOTAL_REACTORS=3` and `HFS_BASELINE_DATA_CPUS=1,2` for the 66B recommended
two-data-reactor comparison. Both file models must be reported independently.

On 2026-09-16, the harness smoke completed 16 trace-off/stats-off NVMe samples using
the default 5-total-reactor layout: both file models, 4K/16K, randread/randwrite, and
jobs 1/8. This validates the measurement path only; the full five-run matrix is still
required before comparing the performance gates below.

`shard66_summary.py` writes `formal_samples.tsv`, `formal_medians.tsv`, and
`summary.json` beside the raw FIO evidence. The result review must include IOPS,
average/p99 latency, CPU%, FIO errors, and the runtime stats from an independent
attribution run. For a bounded harness smoke, the same script accepts space-separated
`HFS_BASELINE_MODELS`, `HFS_BASELINE_BLOCK_SIZES`, `HFS_BASELINE_OPERATIONS`, and
`HFS_BASELINE_JOB_COUNTS`; the default values still execute the complete matrix, and any
failed formal sample makes the harness exit nonzero.

## Gate decisions

- Keep `HFS_REACTOR_PIN=0` as the default unless the same-day 4K randrw and randread
  32-job comparison gains at least 15% total IOPS with no CPU regression beyond the
  shard target.
- Keep `HFS_WAIT_MODE=futex` as the default. Recommend an adaptive window only when
  five-run medians at both 16 and 32 jobs gain at least 5% IOPS with less than 10%
  application CPU growth.
- Treat `reactor_N_poll_gap_complete_ns` as poll-delay evidence. If it is not more
  than ten percent of T6–T7 on the best topology, close the backend-poll direction.
- Use `HFS_IO_POOL_CLASS_COUNTS` only for a gated 16K pool A/B when lease wait exceeds
  three percent. Do not change allocator behavior or introduce registered buffers.
