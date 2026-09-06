# HFS asynchronous request model

> Status: shard-45 first-stage executor foundation (2026-09-06). The executor is not yet the
> final SPDK child-request implementation.

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
3. A worker invokes the existing checked synchronous libhfs operation. This preserves the current
   SPDK owner-thread, range-lock, RMW, metadata and fsync contracts while exposing caller-level QD.
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

The next implementation step is to move the data path from worker-blocking wrappers to a parent
request with SPDK child requests and a completion aggregator. That step must preserve this public
lifecycle, propagate the first error, and only then claim SPDK-level QD>1.
