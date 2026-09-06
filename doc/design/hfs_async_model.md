# HFS asynchronous request model

> Status: shard-45 bounded child-pipeline foundation (2026-09-06). The public executor still
> wraps the checked libhfs entry points; the aligned data path now submits multiple SPDK child
> requests under one parent operation.

## Scope

The synchronous preload API remains the compatibility surface. The first async layer exposes
`hfs_lib_async_pread`, `hfs_lib_async_pwrite`, `hfs_lib_async_preadv`, `hfs_lib_async_pwritev`,
and `hfs_lib_async_fsync`. Each call returns immediately with an opaque `HfsAsyncRequest`.

The request owns its operation arguments until completion. The caller must keep the referenced
buffer/iovec storage valid until the request completes, and must keep the fd open while the
operation is in flight. A close racing before the worker acquires the fd is reported as `EBADF`;
there is no use-after-free of the fd table entry.

## Lifecycle

1. Submit allocates one request with a caller reference and a worker reference.
2. A bounded executor queues the request. `HFS_ASYNC_WORKERS` selects 1–32 workers and
   `HFS_ASYNC_QUEUE_DEPTH` selects a 1–4096 queue limit.
3. A worker invokes the existing checked libhfs operation. Aligned full-unit ranges are split into
   a bounded parent batch controlled by `HFS_IO_PIPELINE_DEPTH` (default 4, maximum 16); each child
   owns a fixed DMA lease and completes on the SPDK owner thread. Edge units still use the existing
   synchronous RMW path, preserving range-lock, metadata and fsync contracts.
4. Completion stores result and positive errno, wakes `hfsAsyncWait`, and optionally invokes the
   callback on the worker thread.
5. The caller reads the result and releases its reference. The worker reference is released after
   callback delivery.

`hfsAsyncWait` may return before a concurrently scheduled callback has finished executing; users
that need callback-side state must synchronize that state independently. `hfsAsyncResult` is valid
after wait completion and is independent of callback timing.

## Cancellation and shutdown

`hfsAsyncCancel` marks a request and suppresses callback delivery. A queued request completes with
`ECANCELED`; an already-running operation is allowed to finish because cancellation must not free
SPDK-owned or caller-owned buffers early. During `hfs_lib_fini`, new submissions are rejected,
workers drain, and only then are metadata/blobstore resources released.

## Instrumentation and next step

When `HFS_STATS_PATH` is enabled, the snapshot includes `async_submit`, `async_complete`,
`async_cancel`, and `async_queue_depth_peak`. These counters describe the executor queue and must
not be confused with SPDK `spdk_submit`/`spdk_complete` or device queue depth.

The current child batch is the low-level completion-aggregation foundation: the parent waits for
all children, propagates the first observed error, and releases DMA leases only after callbacks.
The next step is to connect this parent state directly to `HfsAsyncRequest`, so a public async
request can complete without occupying an executor worker. `io_pipeline_peak` reports the largest
aligned child fan-out and must be evaluated separately from executor queue depth.
