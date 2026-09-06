# HFS Durability Model

> Status: shard-44 v4 dirty-range checkpoint and bounded group-commit foundation (v5 runtime migration not enabled)
> Scope: one HFS owner process mounted on one persistent SPDK bdev

## 1. Contract and terms

This document describes the durability contract that HFS implements today. It
does not turn the current implementation into a general multi-process file
system, and it does not claim a power-fail guarantee that the tests cannot
prove.

- **Data write**: an `spdk_blob_io_write` or RMW operation to a file data blob.
- **Data-blob metadata**: blob allocation and resize metadata made durable by
  `spdk_blob_sync_md`.
- **HFS metadata transaction**: the v4 inode image transition
  `intent -> stable image -> clear intent` in the metadata blob. The current
  shard-44 foundation updates the stable image with aligned byte ranges; it
  does not yet replace the protocol with the v5 WAL.
- **Device barrier**: `spdk_bdev_flush` through the mount-owned descriptor and
  channel. A custom blobstore device uses its `flush` operation only when a
  bdev descriptor is unavailable.

The implementation treats a successful `fsync` and `fdatasync` as the same,
stronger operation for now: data, data-blob metadata, inode size, mtime and
ctime are all included. Dirty inode IDs are collected into one bounded metadata
epoch, so a synchronous caller may also make peer dirty inodes durable.

## 2. Current `fsync` order

For an HFS file descriptor, `hfs_lib_fsync` serializes the following sequence
while holding the open-file I/O lock and the mount I/O write lock:

1. Complete the target file's `hfsSync`: synchronize its data-blob metadata,
   then issue the first device barrier.
2. Under the metadata write lock, commit the dirty inode image through the
   v4 transaction protocol: write an active intent range, write the header and
   changed inode ranges, sync the metadata blob, then clear the intent range.
3. Release the metadata lock and repeat `hfsSync` for the target file. Its
   second device barrier is ordered after the metadata-blob writes and is the
   barrier required before returning success.

The second `hfsSync` may redundantly synchronize the unchanged file data blob;
this is deliberate in the first correct implementation. It gives the device
flush an existing `HfsFile` anchor and avoids adding a separate, unverified
metadata-only flush path. The group-commit foundation is bounded by the four
v4 intent slots; larger dirty sets use the existing full-image fallback. A
dedicated v5 WAL flush epoch remains a later optimization.

```text
file data / allocation writes
            |
            v
data-blob sync -> device flush #1
            |
            v
metadata intent -> stable image -> clear intent
            |
            v
data-blob sync -> device flush #2 -> fsync/fdatasync returns 0
```

## 3. Failure behavior

| Boundary | Caller result | In-process state | Recovery rule |
| --- | --- | --- | --- |
| Data sync or barrier #1 fails | `-1/EIO` | No inode transaction is attempted | Retry `fsync` after the device recovers. |
| Metadata failure before active intent is written | `-1/EIO` | A successful earlier data write remains visible and `metaDirty` remains set | Retry `fsync`; this is Bug #52's regression. |
| Metadata failure after active intent is written | `-1/EIO` | HFS sets `metaRecoveryRequired` and rejects further writes | Remount and replay the valid intent. |
| Barrier #2 fails | `-1/EIO` | Metadata transaction is committed but not reported durable | Retry `fsync`; never infer durability from the failed call. |

Namespace mutation APIs (create, unlink, rename, mkdir, setattr) retain their
existing short-transaction rule: if the intent was not published, they restore
the stable inode image because the mutation itself did not report success.
This is intentionally different from a write that already returned a byte
count before its later `fsync` checkpoint.

## 4. Directory and `fdatasync` behavior

- A directory descriptor uses the same `hfs_lib_fsync` entry point. With the
  current mount-wide `metaDirty` flag, a metadata transaction serializes the
  current inode image before the second barrier.
- `fdatasync` is currently routed to `hfs_lib_fsync`, so it is stronger than
  the minimum POSIX contract. It is not yet a data-only fast path.
- `close` and normal teardown are not substitutes for a successful caller
  `fsync`. Applications needing a recoverable acknowledgement must check the
  `fsync`/`fdatasync` result.

## 5. Evidence and test boundary

`scripts/db/io_error_injection_test.sh` verifies that a metadata-write
completion failure returns `EIO`, leaves a preceding successful `pwrite`
visible, and permits a later successful `fsync` retry. The same suite verifies
the request-local error paths and cross-process remount behavior.

`scripts/db/error_bdev_injection_test.sh` places an SPDK error vbdev above an
AIO bdev and injects one real flush failure. It verifies `fsync` returns
`EIO`, with the written data remaining readable in the running process.

The deterministic request hook names the first and final device boundaries
separately: `bdev-flush` and `bdev-flush-post-commit`. The I/O error suite
uses the latter to verify that an `EIO` after a completed metadata transaction
does not report a false `fsync` success and that a later barrier retry works.

The 2026-09-06 VM regression passed forced Make and CMake Release builds plus
the POSIX data-contract, metadata-transaction, fd-lifecycle, fast-I/O, error
injection and error-bdev suites. MariaDB completed initialization, restart,
SIGKILL recovery and concurrent-client smoke; RocksDB completed cross-process
reopen, WAL recovery and `ldb checkconsistency` with the final barrier hook.

`HFS_DURABILITY_TRACE_PATH` is an optional, bounded per-process trace. It
records `data-barrier`, `metadata-commit` and `post-metadata-barrier` events
with a sequence number, inode id and result while leaving the normal data path
untouched. `scripts/db/durability_barrier_test.sh` asserts this ordering for
both `fsync` and `fdatasync`.

`scripts/db/durability_crash_recovery_test.sh` uses the test-only
`HFS_FSYNC_CRASH_STEP=1|2|3` hook to terminate a process after the first data
barrier, after the metadata transaction, or after the final barrier. A new
process verifies that the earlier two unacknowledged operations recover to a
coherent old-or-new image and that the final-barrier image contains both the
extended inode size and payload. This is a process-crash protocol test: an AIO
file's host cache and the VM do not make it a physical power-fail test.

## 6. Explicitly unfinished work

1. The transaction's intent, stable image and clear intent are not separated
   by individual device barriers. The second barrier protects the completed
   transaction, but the exact device-cache/power-fail matrix still needs a
   controllable write-cache test environment.
2. Runtime now tracks dirty inode IDs and a mount-local durable metadata epoch.
   The v4 implementation can merge a bounded set of dirty inodes; it still
   lacks the v5 WAL epoch, background checkpoint, and per-file waiter queues
   needed for a fully asynchronous group-commit contract.
3. The trace and process-crash hook verify API-level phase order and the three
   fsync boundaries, not the device's physical cache order. They still need
   per-submit/per-completion coverage and a controllable power-fail backend.
4. Multi-process simultaneous mounts, `mmap`, native libaio and io_uring
   remain unsupported and outside this durability model.

These limits keep Bug #50 in progress. Do not describe HFS as having a fully
validated power-fail model until the above barriers, trace and recovery tests
are implemented.

## 7. v5 metadata format foundation

The shard-43 foundation adds an offline, portable v5 image codec in
`src/hfs_meta_v5.[ch]`. It is deliberately not selected by the current mount
path yet; v4 remains the active on-device format until copy-on-write migration
and a device-backed WAL commit path are complete.

- Superblock A/B, page headers, inode fields and WAL headers are encoded as
  explicit little-endian scalars; compiler padding and pointer width are not
  part of the format.
- Each 4 KiB inode page and each WAL record has an independent checksum. The
  newest valid superblock generation is selected, with the other copy retained
  for torn-write fallback.
- WAL recovery stages SET_INODE records by transaction id and applies them only
  after a matching commit marker with the declared record count. Incomplete
  transactions are ignored, duplicate inode after-images are idempotent, and
  zero alignment padding is excluded from logical payload validation while
  remaining covered by the physical payload checksum.
- `scripts/db/metadata_v5_format_test.sh` exercises round-trip load, committed
  and incomplete WAL, page corruption rejection and superblock-A fallback. It
  is an offline format test, not evidence that v5 is already mounted by HFS.

The next durability step is v4-to-v5 copy-on-write migration: build and verify
the v5 region, flush it, then publish one superblock generation while retaining
the v4 stable copy until the switch is durable. A failed migration must leave
the v4 image mountable.
