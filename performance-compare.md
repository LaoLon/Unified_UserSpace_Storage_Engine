# Performance Comparison

UUSE replaces the host file system and the kernel block path with a single-process
userspace engine that drives raw NVMe through SPDK. This document summarizes how it
compares with the native path across six workloads: **fio**, **3FS**, **MySQL**,
**RocksDB**, **TiKV (TiDB)**, and **MinIO**.

Measurements were collected between 2026-09-27 and 2026-10-12 on single-node lab VMs (see
the status note below). Absolute throughput drifts between sessions, so **only ratios
measured in the same session are meaningful**. Per-workload evidence (raw logs, commands,
closures) is linked at the end.

> **Status: virtual-machine measurements only, real-hardware numbers pending.**
> Every result below was collected inside VMware VMs backed by a virtual NVMe controller
> (4–6 vCPU, 7–8 GiB RAM, one disk). No bare-metal or production-hardware run has been
> performed yet. Virtualization changes both absolute throughput and some ratios: in the
> 4K block profile ~45% of reactor CPU is VMware MMIO doorbells and `rdtsc`, costs that
> do not exist on real hardware. Treat these results as directional lab data and
> re-measure on the target platform before any capacity, SLA, or procurement decision.

> The project was renamed from HFS to UUSE on 2026-10-04. Older documents under `doc/`
> keep the HFS name and their evidence paths; the measurements are unchanged.

## At a glance

| Workload | Native baseline | UUSE / native | Tail latency | Comment |
|---|---|---:|---|---|
| fio, 4K `randrw` QD32/64 | SPDK raw bdev | 94.9–102.6% | p99 0.87–0.96× | Device-bound, single reactor |
| fio gate, 1j32 / 8j4 | SPDK raw bdev | 93.5–95.5% / 86.0–89.9% (host4) | p99 0.19–1.00× | Gate is ≥85%, p99 ≤110% |
| 3FS, RF2 + 2 clients | XFS + Rust `chunk_engine` | 101–104% (4K rw within noise) | — | Client-bound, parity |
| MySQL `oltp_*` | ext4 datadir | 87.6–99.8% | p99 0.57–1.15× | `write_only` is the low point |
| RocksDB | ext4 + direct I/O | writes 93–110%, cold reads 1.6–7.8× | — | Read gap is a native-path artifact |
| TiKV + sysbench | ext4 host | 93.7–98.4% | p99 1.02–1.09× | 5 ms sync window |
| MinIO `warp` | ext4 | PUT 103–106%, GET 71–76% | p99 0.73–0.98× | GET gap is page cache |

A ratio above 100% means UUSE was faster. The native baseline differs by workload and is
named in every table. All numbers are virtual-machine measurements; the status note above
applies to every row.

## Test platforms

| ID | Date | Host | Device | Used for |
|---|---|---|---|---|
| A | 2026-09-27/29 | 6 vCPU, 7.2 GiB | `VMware NVME_0002`, `0000:13:00.0` | fio, fio gate (09-29), MySQL |
| B | 2026-10-09/10 | host4, 4 vCPU, 7.2 GiB | `VMware NVME_0002`, `0000:13:00.0` | RocksDB, fio gate, TiKV |
| C | 2026-10-07 | 2 storage + 2 client VMs (clients 4 vCPU) | `NVME_0002` per storage node | 3FS |
| D | 2026-10-12 | 6 vCPU, 7.4 GiB | `VMware NVME_0002`, INTx (`pci=nomsi`) | MinIO |

UUSE builds share SPDK `f0b63ed8`; fio cells use fio 3.42; reactor pins in the UUSE
`.conf` match the test cpuset. Storage devices are bound to `uio_pci_generic` for UUSE
legs and returned to the kernel driver (`ext4`) for native legs.

## Method

- **Interleaved A/B.** Both implementations run on the same device in the same session;
  the volume is rebuilt (`mkfs.ext4` or `uusectl format --yes`) before every leg.
- **Medians over repeated runs** (3–5 runs, or 2 interleaved rounds), never a single run.
- **"Native" is defined per workload**: raw SPDK `bdev` for fio, ext4 for file-level
  workloads, XFS + `chunk_engine` for 3FS.
- **UUSE has no host page cache by design.** Comparisons against buffered ext4 therefore
  include that asymmetry rather than isolating engine cost.
- Kernel-side cells that were unstable in the VM are marked and used only as
  directional evidence; SPDK/UUSE cells were stable across reruns.

## Block layer — fio

`randrw` 50/50, `direct=1`, 256 MiB working set, 10 s per run, 3-run median. UUSE runs as
an external fio ioengine in the same process; the raw baseline is SPDK `spdk_bdev` on the
same device, so both bypass the kernel. Total queue depth is matched: 1 job × QD for raw,
1 job × 32 (QD32) or 2 × 32 (QD64) for UUSE.

| I/O | Total QD | ext4 + libaio | SPDK raw | UUSE | UUSE / raw | p99 read (UUSE / raw) |
|---|---:|---:|---:|---:|---:|---:|
| 4 KiB | 32 | 11,546 | 95,986 | 91,081 | **94.9%** | 733 / 766 µs (0.96×) |
| 4 KiB | 64 | 14,058 | 94,092 | 96,534 | **102.6%** | 1,221 / 1,384 µs (0.88×) |
| 16 KiB | 32 | 47,737 | 77,568 | 76,091 | **98.1%** | 840 / 897 µs (0.94×) |
| 16 KiB | 64 | 6,723 | 77,837 | 78,186 | **100.4%** | 1,417 / 1,581 µs (0.90×) |
| 64 KiB | 32 | 18,356 | 45,402 | 44,152 | **97.2%** | 1,237 / 1,384 µs (0.89×) |
| 64 KiB | 64 | 21,412 | 47,311 | 46,042 | **97.3%** | 2,212 / 2,441 µs (0.91×) |

UUSE stays within ~5% of raw SPDK and its p99 is 4–13% lower. The residual cost is the
per-operation reactor work (namespace and extent bookkeeping, completion publication),
measured at ~1.2 µs/op, about 12% of a single data reactor. The ext4 column shows the
kernel path is 12–15% of raw at 4 KiB, which is what the application-level results below
have to be read against.

### Formal gate

4K shared-file `randrw`, same shape on both sides, UUSE/raw ≥ 85%, p99 ≤ 1.10×, errors = 0.

| Shape | Session | UUSE / raw | p99 ratio | Result |
|---|---|---:|---:|---|
| 1 job × QD32 | 2026-09-29 | 90.6% | 1.00× | pass |
| 8 jobs × QD4 | 2026-09-29 | 120.8% | 0.19× | pass (raw collapses at this shape) |
| 1 job × QD32 | 2026-10-09 | 95.5% | 0.92× | pass |
| 8 jobs × QD4 | 2026-10-09 | 86.0% | 0.20× | pass |
| 1 job × QD32 | 2026-10-10 | 93.5% | 0.96× | pass |
| 8 jobs × QD4 | 2026-10-10 | 89.9% | 0.20× | pass |

4 KiB throughput is **job-shape sensitive**: at the same total QD=32, `1 job × 32` reached
101,691 IOPS while `8 × 4` reached 81,248 in the same session (2026-09-23), because the
single data reactor saturates either way. The gate therefore fixes the shape on both
sides instead of comparing across shapes; raw at `8j4` degrades to ~65k IOPS with
3.46 ms p99, which is recorded as a known boundary, not as a UUSE win.

## 3FS storage target

UUSE runs as a third `StorageTarget` backend (`UuseChunkStore`) next to the native Rust
`chunk_engine`. Comparison is RF2 (two replicas), two clients writing concurrently, and a
chain that differs only in which engine serves one replica. 1 MiB sequential writes and
4 KiB random writes are aggregated over both clients; reads are single-pass cold reads
after `drop_caches`.

| Dimension (2-client aggregate) | `chunk_engine` | UUSE | Ratio |
|---|---:|---:|---:|
| Sequential write 1 MiB, 20 s | 65.1 / 66.9 MiB/s | 67.6 / 67.9 MiB/s | 103.8% / 101.5% |
| Random write 4 KiB, 15 s | 286 / 246 IOPS | 317 / 316 IOPS | 110.8% / 128.5% |
| Cold read 1 MiB, single pass | 151.0 MiB/s | 153.6 / 152.3 MiB/s | 101.7% / 100.9% |

Both arms are limited by the same non-engine ceiling: the FUSE client caps its worker
threads by vCPU count (two workers on the 4 vCPU clients) and the chain pipeline
saturates at ~64 ops/s, so sequential writes plateau at ~65–68 MiB/s for both engines.
These numbers show **no regression**, not the engine's ceiling; a fair headroom test
needs more client workers or a faster transport.

Two caveats belong with this table: early sessions used Soft-RoCE and produced retry
storms unrelated to the backend, and the stable configuration for the A/B was inline TCP.
The 4K random-write difference is small in absolute terms (tens of IOPS) and within
run-to-run noise.

## MySQL

sysbench 1.0.20, 10 tables × 10,000 rows, 10 s per run, `innodb_flush_log_at_trx_commit=1`,
`innodb_use_native_aio=OFF`. UUSE datadir and ext4 datadir alternate (two rounds, mean).

| Scenario | 16 threads | 32 threads | 64 threads | UUSE TPS (16/32/64) |
|---|---:|---:|---:|---|
| `oltp_read_write` (70/30) | 89.9% | 98.9% | 91.8% | 3,126 / 4,112 / 4,256 |
| `oltp_read_only` | 99.8% | 98.7% | 98.5% | 11,287 / 10,671 / 9,741 |
| `oltp_write_only` | 93.4% | 87.6% | 89.7% | 5,262 / 7,512 / 9,227 |
| `oltp_point_select` | 98.2% | 98.9% | 95.4% | 270,315 / 262,171 / 240,292 |

p99 lands between 0.57× and 1.15× of native across the 12 cells; the worst cell is
`point_select` at 64 threads (0.77 vs 0.67 ms), and `write_only` p99 is consistently
lower on UUSE (6.2 vs 10.8 ms at 16 threads).

Read-heavy scenarios are at parity because the 100k-row data set fits the default InnoDB
buffer pool: the bottleneck is CPU, not the device. `write_only` is the consistent low
point (87.6–93.4%) because every transaction waits for group commit and a redo barrier,
and the engine's barrier is more expensive than ext4's. Large working set (1M rows,
64 MiB buffer pool + `O_DIRECT`, 32 threads): `read_only` 96.1%, `point_select` 93.0%,
`write_only` 121.3% (native was slower in that session), `read_write` not comparable
(a 22 s device stall hit the native leg).

An earlier large-working-set run exposed a real defect: `read_only` fell to 15% because
~91% of I/O took the synchronous fallback path (no per-thread client on InnoDB connection
threads) and the single data reactor spun on a full message ring. Lazy per-thread client
creation raised it to 96.1% (p99 493 ms → 9.2 ms). The fix is part of the current tree; the
number is kept here because it defines the shape of the remaining risk.

## RocksDB

RocksDB 9.11.2, 1,280,000 keys (16 B key, 1 KiB value), 4 KiB blocks, `compression=none`,
1 GiB block cache, direct reads, WAL on, sync off. Native is ext4 with
`use_direct_reads=1`; UUSE uses worker-direct reads and a write-back adapter. Fresh volume
per phase, interleaved A/B.

| Workload | native (32t) | UUSE (32t) | Ratio | native (64t) | UUSE (64t) | Ratio |
|---|---:|---:|---:|---:|---:|---:|
| `fillseq` | 96,213 | 98,532 | 102.4% | 96,852 | 95,293 | 98.4% |
| `readseq` (cold) | 180,285 | 285,606 | 158.4% | 37,927 | 296,342 | 781.3% |
| `readrandom` (cold) | 38,716 | 179,405 | 463.4% | 38,330 | 184,236 | 480.7% |
| `fillrandom` | 91,352 | 93,206 | 102.0% | 93,281 | 90,818 | 97.4% |
| `overwrite` | 81,177 | 89,528 | 110.3% | 81,988 | 85,138 | 103.8% |
| `readwhilewriting` | 2,053,283 | 1,909,473 | 93.0% | 1,900,565 | 2,229,415 | 117.3% |

Writes are at parity or slightly ahead. **The large read ratios must not be read as a
UUSE speedup.** Controlled attribution in the same session showed:

- warm reads (block-cache hits) are identical on both backends, ~3.2M ops/s;
- single-threaded device read latency is identical, 55–62 µs/read;
- the gap appears only with 32+ threads and cache misses, where the native synchronous
  `pread` path collapses to ~170–620 µs per device read while UUSE stays at ~16 µs.

So the multipliers describe a **native-path concurrency collapse**, not the engine's
throughput. A separate fio anchor in the same session (ext4 + libaio, 4K `randread` QD32,
98.5k IOPS) confirms the kernel path itself is not slow; the issue is how RocksDB's
synchronous reads behave at high thread counts in this VM.

## TiKV (TiDB)

TiKV v8.5.8 with the UUSE storage backend, sysbench point-select and read-write, 5
interleaved rounds on the same device, ext4 as the host backend. Worker and apply paths
use a 5 ms sync window.

| Metric | UUSE | native ext4 | Ratio | Gate | Result |
|---|---:|---:|---:|---|---|
| `point_select` TPS | 18,534 | 18,838 | 98.4% | ≥ 95% | pass |
| `point_select` p99 | 1.30 ms | 1.27 ms | 1.02× | ≤ 1.10× | pass |
| `read_write` TPS | 378 | 404 | 93.7% | ≥ 85% | pass |
| `read_write` p99 | 33.12 ms | 30.26 ms | 1.09× | ≤ 1.10× | pass |

The 5 ms window is a deliberate durability trade-off, not a free win: it cut fsync
barriers from 55k to 21.8k per 60 s (sync time 17.4 s → 5.3 s) and moved `read_write` from
90.9% to 93.7%. Crash RPO is therefore ≤ 5 ms; graceful shutdown still forces a full sync
(RPO = 0).

## MinIO

`warp` against a single node with one disk (ErasureSD, no redundancy), 1 MiB objects, 16
concurrent streams over loopback, 8 s legs, interleaved ext4 ↔ UUSE with a fresh format
per leg.

| Metric | ext4 (r1 / r2) | UUSE (r1 / r2) | Ratio |
|---|---:|---:|---:|
| PUT throughput | 871.7 / 869.1 MiB/s | 900.2 / 901.1 MiB/s | **103.3% / 103.7%** |
| PUT p99 | 32.2 / 32.6 ms | 31.5 / 30.4 ms | 0.98× / 0.93× |
| GET throughput | 2,721.9 / 2,758.0 MiB/s | 1,944.7 / 1,938.8 MiB/s | **71.4% / 70.3%** |
| GET p99 | 18.2 / 18.5 ms | 13.9 / 14.4 ms | **0.76× / 0.78×** |

Writes reached parity after a structural fix: MinIO's create/rename on every PUT forced a
full namespace checkpoint per barrier (p50 3.9 ms). Making shape changes replayable through
the journal dropped the barrier to 0.62 ms and PUT from 47–64% to 103–106% of ext4.

GET is the one **structural regression**, and it is attributed rather than left open:

- ext4 serves ~90% of the payload from the page cache on this loopback workload; UUSE has
  no page cache, so every read reaches the device;
- ext4 absorbs the per-object `xl.meta` reads in cache; UUSE pays ~2 extra device reads per
  object, which queue behind the 1 MiB transfer on a single queue (head-of-line blocking);
- the engine read path itself is fast: 5,270 MiB/s at 8 threads in a direct read probe.

A bounded shim metadata cache removed ~40% of the small reads and gained +4.6% GET
(1,935 → 2,023 MiB/s). The remaining gap is the device's sustained single-queue read
throughput (~2.0–2.5 GB/s) versus ext4's kernel multi-queue path (2.7–3.3 GB/s). The
throughput gate was frozen at 71–76% rather than pursued further; PUT, p99, and error
gates stay green.

## Discussion

1. **Device-bound paths reach parity.** When the workload actually reaches the device —
   block I/O, 3FS chunk writes, MinIO PUT — UUSE matches or slightly beats the native
   path. The engine's own overhead is ~1.2 µs/op, small relative to device service time
   on this VM.
2. **Application stacks hide the block-layer advantage.** InnoDB, RocksDB, and TiKV each
   bring a buffer pool or block cache plus a durability barrier; the device stops being
   the bottleneck, so end-to-end results sit at 88–100% while the fio table shows ext4
   6–8× behind raw at 4 KiB.
3. **Large read multipliers are baseline artifacts.** The 1.6–7.8× RocksDB read ratios
   come from the native synchronous read path degrading under concurrency, not from UUSE
   being faster than the device. Warm-cache and single-thread numbers are identical.
4. **The remaining gaps are structural and known.** MinIO GET (no page cache) and MySQL
   `write_only` (barrier cost) are design trade-offs, not tuning bugs. Both are bounded
   and documented rather than papered over.
5. **Ratios do not transfer.** All measurements are from 4–6 vCPU VMs with one virtual
   NVMe; ~45% of reactor CPU in the 4K profile is VMware MMIO doorbells and `rdtsc`,
   costs that are absent on bare metal. The gap is therefore expected to narrow there,
   but that has not been measured yet.

## Limitations

- Single device, single node. The only multi-node setup is 3FS RF2; MinIO was measured
  over loopback. No erasure-coded or multi-disk placement results.
- Virtual NVMe: MSI-X completion behaviour required `pci=nomsi` on one host, and kernel
  NVMe cells showed intermittent `I/O tag timeout` stalls in several sessions. Cells
  affected by a stall are marked and excluded from conclusions.
- 3FS numbers are capped by the FUSE client (two worker threads on the 4 vCPU clients),
  so they prove parity, not headroom.
- UUSE has no host page cache; results against buffered ext4 include that asymmetry.
- **No bare-metal validation.** All results are from virtual machines (see the status note
  at the top). Absolute values and some ratios will differ on real hardware, so these
  numbers are not a sizing, SLA, or acceptance baseline.

## Reproducing

| Workload | Entry point |
|---|---|
| fio | [`fio/run_three_way_matrix_vm.sh`](fio/run_three_way_matrix_vm.sh), [`fio/run_four_engine_vm.sh`](fio/run_four_engine_vm.sh) |
| 3FS | [`integrations/3fs/README.md`](integrations/3fs/README.md), [`doc/design/uuse_3fs_local_engine.md`](doc/design/uuse_3fs_local_engine.md) §5 |
| MySQL | [`mysql/run_native_vs_uuse_sysbench_vm.sh`](mysql/run_native_vs_uuse_sysbench_vm.sh) |
| RocksDB | [`rocksdb/run_native_vs_uuse_vm.sh`](rocksdb/run_native_vs_uuse_vm.sh) |
| TiKV | [`tikv/run_perf_ab_vm.sh`](tikv/run_perf_ab_vm.sh), [`tikv/run_perf_ab_target_vm.sh`](tikv/run_perf_ab_target_vm.sh) |
| MinIO | [`minio/run_perf_ab_vm.sh`](minio/run_perf_ab_vm.sh) |

## References

- [`doc/performance/fio_baseline.md`](doc/performance/fio_baseline.md) — three-way fio matrix, engine comparison
- [`doc/performance/rocksdb_baseline.md`](doc/performance/rocksdb_baseline.md) — RocksDB A/B and read-gap attribution
- [`doc/performance/mysql_baseline.md`](doc/performance/mysql_baseline.md) — MySQL matrix and seam fixes
- [`doc/performance/uuse_tidb_baseline_20261009.md`](doc/performance/uuse_tidb_baseline_20261009.md) — TiKV Phase 8 baseline and gate
- [`doc/performance/minio_baseline.md`](doc/performance/minio_baseline.md), [`doc/performance/minio_shard95_attribution.md`](doc/performance/minio_shard95_attribution.md) — MinIO A/B, attribution, freeze decision
- [`doc/design/uuse_3fs_local_engine.md`](doc/design/uuse_3fs_local_engine.md) §5 — 3FS RF2 A/B
- [`doc/design/uuse.md`](doc/design/uuse.md) §16 — acceptance gates
