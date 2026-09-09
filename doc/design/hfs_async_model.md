# HFS asynchronous request model

> Status: shard-52/53 registered-DMA direct path and data-plane closeout (2026-09-09). Large, full-unit aligned positioned I/O
> binds a public request directly to a bounded SPDK child parent; unsupported shapes retain the
> bounded executor fallback.

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
3. A positioned request with at least two full, unit-aligned units first tries the direct path.
   It allocates the public request, retains the open-file description, and binds it to a bounded
   parent batch controlled by `HFS_IO_PIPELINE_DEPTH` (default 8, maximum 16). Each child owns a
   fixed DMA lease and completes on the SPDK owner thread, so no executor worker waits for data
   I/O. Reads are trimmed to the snapshotted file size; writes use direct mode only when the
   existing blob capacity already covers the range. Resize, edge-unit RMW, vector, fsync, small,
   unaligned, and resource-contended requests use the bounded executor fallback.
4. The parent completion stores result and positive errno, wakes `hfsAsyncWait`, optionally invokes
   the callback on the SPDK owner thread for direct requests (or the worker thread for fallback),
   then releases the retained open-file description and operation reference.
5. The caller reads the result and releases its reference. The worker reference is released after
   callback delivery.

`hfsAsyncWait` may return before a concurrently scheduled callback has finished executing; users
that need callback-side state must synchronize that state independently. `hfsAsyncResult` is valid
after wait completion and is independent of callback timing.

When `HFS_DATA_REACTORS` is greater than one, reactor 0 remains the metadata/compatibility owner and
the direct data path selects one extra reactor from a stable blob-id hash. Each selected reactor has
its own SPDK channel and submit/complete counters (`data_reactor_N_submit` and
`data_reactor_N_complete`). A successful direct write publishes inode size on the metadata owner;
if the owner is waiting in another metadata transaction, a low-rate owner poller retries the publish
instead of blocking the owner thread. The public completion is then sent back to the originating data
reactor. Unsupported shapes, resize/RMW, vector, fsync, small, and resource-contended requests keep
using the bounded executor fallback.

## Cancellation and shutdown

`hfsAsyncCancel` marks a request and suppresses callback delivery. A queued request completes with
`ECANCELED`; an already-running operation is allowed to finish because cancellation must not free
SPDK-owned or caller-owned buffers early. During `hfs_lib_fini`, new submissions are rejected,
workers drain, and only then are metadata/blobstore resources released.

## Instrumentation and next step

When `HFS_STATS_PATH` is enabled, the snapshot includes `async_submit`, `async_complete`,
`async_cancel`, and `async_queue_depth_peak`. These counters describe the executor queue and must
not be confused with SPDK `spdk_submit`/`spdk_complete` or device queue depth.

The child batch is now connected to `HfsAsyncRequest` for the direct shape described above. The
parent waits for all children, propagates the first observed error, and releases DMA leases only
after callbacks. `io_pipeline_peak` reports the largest aligned child fan-out and must be evaluated
separately from executor queue depth; `async_submit`/`async_complete` include both direct and
fallback public requests. Direct mode deliberately returns `EAGAIN` to the wrapper when it would
need resize or cannot reserve its bounded resources, allowing the compatibility fallback to retain
the existing metadata and RMW semantics.

The request-local DMA pool now selects a 4 KiB, 16 KiB, 64 KiB, or 1 MiB class from the child
size, so small requests do not pin a 1 MiB buffer. Pool leases are partitioned by data reactor and
bounded by `HFS_IO_POOL_CLASS_COUNTS`; wait/exhaust counters make backpressure observable. Aligned
full-span I/O first probes `spdk_vtophys` and submits caller memory directly when the span is
DMA-addressable. Applications that need a stable direct buffer may use the experimental
`hfsRegisterBuffer`/`hfsUnregisterBuffer` API; each request holds a registry reference until its
completion, and unregister rejects outstanding I/O. Vector read/write uses the same probe and
attempts one native `spdk_blob_io_*v`; ordinary heap buffers continue through the bounded bounce
path. No mmap, native libaio, or native io_uring support is implied.
