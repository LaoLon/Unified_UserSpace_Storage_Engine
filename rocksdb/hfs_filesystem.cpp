/* HFS 2.0 RocksDB 8.9.1 buffered FileSystem adapter. */

#include "rocksdb/hfs_filesystem.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <sys/file.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/file.hpp"
#include "rocksdb/file_system.h"

namespace hfs::rocksdb_adapter {

namespace {

/* Maps an HFS status into the status vocabulary expected by RocksDB. */
template <typename T>
rocksdb::IOStatus toIoStatus(const hfs::Result<T> &result) {
    if (result.ok()) {
        return rocksdb::IOStatus::OK();
    }
    auto retryableBusy = [&result]() {
        auto status = rocksdb::IOStatus::Busy(result.message());
        status.SetRetryable(true);
        return status;
    };
    switch (result.status()) {
    case hfs::Status::NotFound:
        return rocksdb::IOStatus::NotFound(result.message());
    case hfs::Status::NoSpace:
        return rocksdb::IOStatus::NoSpace(result.message());
    case hfs::Status::AlreadyExists:
        return rocksdb::IOStatus::IOError(result.message());
    case hfs::Status::Busy:
        return retryableBusy();
    case hfs::Status::Cancelled:
        return rocksdb::IOStatus::Aborted(result.message());
    case hfs::Status::TimedOut:
        return rocksdb::IOStatus::TimedOut(result.message());
    case hfs::Status::TryAgain:
        /* RocksDB IOStatus has no TryAgain factory; preserve retryability as Busy. */
        return retryableBusy();
    case hfs::Status::InvalidArgument:
        return rocksdb::IOStatus::InvalidArgument(result.message());
    case hfs::Status::NotSupported:
        return rocksdb::IOStatus::NotSupported(result.message());
    case hfs::Status::Corrupt:
        return rocksdb::IOStatus::Corruption(result.message());
    default:
        return rocksdb::IOStatus::IOError(result.message());
    }
}

/* Converts a path while preserving HFS's absolute namespace contract. */
std::string hfsPath(const std::string &path) {
    std::string normalized = path;
    if (normalized.empty()) {
        return normalized;
    }
    if (normalized.front() != '/') {
        normalized.insert(normalized.begin(), '/');
    }
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

/* Builds a stable host-side lock path for a database in the HFS namespace. */
std::string hostLockPath(const std::string &path) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : path) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return "/var/tmp/hfs-rocksdb-lock-" + std::to_string(hash);
}

struct HfsContext {
    std::shared_ptr<hfs::Engine> engine;
    std::shared_ptr<hfs::Client> client;

    /* Release the frontend lease before cleanly stopping the owned engine. */
    ~HfsContext() {
        if (client != nullptr) {
            client->close().wait();
            client.reset();
        }
        if (engine != nullptr) {
            engine->stop().wait();
            engine.reset();
        }
    }
};

/* Opens one HFS file and returns the nested coroutine result consistently. */
rocksdb::IOStatus openFile(const std::shared_ptr<HfsContext> &context,
                           const std::string &path, hfs::OpenFlags flags,
                           std::shared_ptr<hfs::File> *file) {
    if (context == nullptr || context->client == nullptr) {
        return rocksdb::IOStatus::InvalidArgument("HFS context is not initialized");
    }
    auto outer = context->client->open(hfsPath(path), flags).wait();
    if (!outer.ok()) {
        return toIoStatus(outer);
    }
    auto inner = std::move(outer).value();
    if (!inner.ok()) {
        return toIoStatus(inner);
    }
    *file = std::move(inner).value();
    return rocksdb::IOStatus::OK();
}

class HfsSequentialFile final : public rocksdb::FSSequentialFile {
public:
    HfsSequentialFile(std::shared_ptr<hfs::File> file) : file_(std::move(file)) {}

    /* Reads from the current sequential offset into RocksDB scratch storage. */
    rocksdb::IOStatus Read(size_t n, const rocksdb::IOOptions &, rocksdb::Slice *result,
                           char *scratch, rocksdb::IODebugContext *) override {
        if (scratch == nullptr && n != 0) {
            return rocksdb::IOStatus::InvalidArgument("null sequential read buffer");
        }
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(scratch), n};
        auto read = file_->read(std::span<hfs::IoVec>(&buffer, 1), offset_).wait();
        if (!read.ok()) {
            return toIoStatus(read);
        }
        auto value = std::move(read).value();
        if (!value.ok()) {
            return toIoStatus(value);
        }
        offset_ += value.value().bytes;
        *result = rocksdb::Slice(scratch, value.value().bytes);
        return rocksdb::IOStatus::OK();
    }

    /* Advances the sequential cursor without reading user-visible data. */
    rocksdb::IOStatus Skip(uint64_t n) override {
        if (offset_ > UINT64_MAX - n) {
            return rocksdb::IOStatus::InvalidArgument("sequential offset overflow");
        }
        offset_ += n;
        return rocksdb::IOStatus::OK();
    }

    /* Implements RocksDB's direct positioned-read entry point. */
    rocksdb::IOStatus PositionedRead(uint64_t offset, size_t n,
                                     const rocksdb::IOOptions &options,
                                     rocksdb::Slice *result, char *scratch,
                                     rocksdb::IODebugContext *debug) override {
        if (scratch == nullptr && n != 0) {
            return rocksdb::IOStatus::InvalidArgument("null positioned read buffer");
        }
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(scratch), n};
        auto read = file_->read(std::span<hfs::IoVec>(&buffer, 1), offset).wait();
        if (!read.ok()) return toIoStatus(read);
        auto value = std::move(read).value();
        if (!value.ok()) return toIoStatus(value);
        *result = rocksdb::Slice(scratch, value.value().bytes);
        return rocksdb::IOStatus::OK();
    }

    /* HFS exposes aligned SPDK buffers for direct sequential reads. */
    bool use_direct_io() const override { return true; }

    /* Matches the SPDK DMA alignment used by the HFS data path. */
    size_t GetRequiredBufferAlignment() const override { return 4096; }

private:
    std::shared_ptr<hfs::File> file_;
    std::uint64_t offset_ = 0;
};

/* Owns one submitted async read until RocksDB releases its opaque handle. */
struct AsyncReadState {
    std::shared_ptr<hfs::File> file;
    rocksdb::FSReadRequest *request = nullptr;
    rocksdb::IOOptions options;
    std::function<void(const rocksdb::FSReadRequest &, void *)> callback;
    void *callbackArg = nullptr;
    std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;
    bool aborted = false;
    bool callbackStarted = false;
    std::thread worker;

    /* Joins the worker before the opaque handle is reclaimed. */
    ~AsyncReadState() {
        if (worker.joinable()) {
            worker.join();
        }
    }
};

/* Runs the HFS read off the caller thread and publishes one completion. */
void executeAsyncRead(AsyncReadState *state) {
    rocksdb::IOStatus status = rocksdb::IOStatus::IOError("async read failed");
    try {
        /* Test-only delay makes the pre-callback AbortIO boundary deterministic. */
        const char *delayText = std::getenv("HFS_TEST_ASYNC_READ_DELAY_MS");
        if (delayText != nullptr && *delayText != '\0') {
            char *end = nullptr;
            const unsigned long delay = std::strtoul(delayText, &end, 10);
            if (end != delayText && *end == '\0' && delay <= 60000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        }
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(state->request->scratch),
                          state->request->len};
        auto read = state->file->read(std::span<hfs::IoVec>(&buffer, 1),
                                      state->request->offset).wait();
        if (!read.ok()) {
            status = toIoStatus(read);
        } else {
            auto value = std::move(read).value();
            if (!value.ok()) {
                status = toIoStatus(value);
            } else {
                state->request->result = rocksdb::Slice(
                    state->request->scratch, value.value().bytes);
                status = rocksdb::IOStatus::OK();
            }
        }
    } catch (const std::exception &error) {
        status = rocksdb::IOStatus::IOError(
            std::string("async read worker failed: ") + error.what());
    } catch (...) {
        status = rocksdb::IOStatus::IOError("async read worker failed");
    }

    bool invokeCallback = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->aborted) {
            state->request->status = status;
            /* Establish the cancellation boundary before releasing the lock;
             * AbortIO after this point cannot rewrite callback-visible state. */
            state->callbackStarted = true;
            invokeCallback = true;
        }
    }
    /* Invoke user code outside the state lock so callbacks may re-enter Poll/AbortIO. */
    if (invokeCallback) {
        try {
            state->callback(*state->request, state->callbackArg);
        } catch (...) {
            /* RocksDB callbacks must not escape the worker thread. */
        }
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->completed = true;
    }
    state->condition.notify_all();
}

/* Deletes a submitted async request only after its worker has stopped. */
void deleteAsyncReadHandle(void *handle) {
    delete static_cast<AsyncReadState *>(handle);
}

class HfsRandomAccessFile final : public rocksdb::FSRandomAccessFile {
public:
    HfsRandomAccessFile(std::shared_ptr<hfs::File> file) : file_(std::move(file)) {}

    /* Performs a buffered random read through the HFS File API. */
    rocksdb::IOStatus Read(uint64_t offset, size_t n, const rocksdb::IOOptions &,
                           rocksdb::Slice *result, char *scratch,
                           rocksdb::IODebugContext *) const override {
        if (scratch == nullptr && n != 0) {
            return rocksdb::IOStatus::InvalidArgument("null random read buffer");
        }
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(scratch), n};
        auto read = file_->read(std::span<hfs::IoVec>(&buffer, 1), offset).wait();
        if (!read.ok()) {
            return toIoStatus(read);
        }
        auto value = std::move(read).value();
        if (!value.ok()) {
            return toIoStatus(value);
        }
        *result = rocksdb::Slice(scratch, value.value().bytes);
        return rocksdb::IOStatus::OK();
    }

    /* Processes a batch of non-overlapping reads while preserving per-request status. */
    rocksdb::IOStatus MultiRead(rocksdb::FSReadRequest *requests, size_t count,
                                const rocksdb::IOOptions &options,
                                rocksdb::IODebugContext *debug) override {
        if (requests == nullptr && count != 0) {
            return rocksdb::IOStatus::InvalidArgument("null multi-read requests");
        }
        rocksdb::IOStatus firstError = rocksdb::IOStatus::OK();
        for (size_t index = 0; index < count; ++index) {
            auto &request = requests[index];
            request.status = Read(request.offset, request.len, options,
                                  &request.result, request.scratch, debug);
            if (!request.status.ok() && firstError.ok()) {
                firstError = request.status;
            }
        }
        return firstError;
    }

    /* HFS requires aligned offsets, lengths, and scratch buffers for direct I/O. */
    bool use_direct_io() const override { return true; }

    /* Matches the SPDK DMA alignment used by the HFS data path. */
    size_t GetRequiredBufferAlignment() const override { return 4096; }

    /* Submits a real background read and returns before the HFS coroutine completes. */
    rocksdb::IOStatus ReadAsync(
        rocksdb::FSReadRequest &request, const rocksdb::IOOptions &options,
        std::function<void(const rocksdb::FSReadRequest &, void *)> callback,
        void *callbackArg, void **ioHandle, rocksdb::IOHandleDeleter *deleter,
        rocksdb::IODebugContext *) override {
        if (ioHandle == nullptr || deleter == nullptr || callback == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("invalid async read handle");
        }
        if (request.scratch == nullptr && request.len != 0) {
            return rocksdb::IOStatus::InvalidArgument("null async read buffer");
        }
        *ioHandle = nullptr;
        *deleter = {};
        try {
            auto state = std::make_unique<AsyncReadState>();
            state->file = file_;
            state->request = &request;
            state->options = options;
            state->callback = std::move(callback);
            state->callbackArg = callbackArg;
            state->worker = std::thread(executeAsyncRead, state.get());
            *ioHandle = state.release();
            *deleter = deleteAsyncReadHandle;
            return rocksdb::IOStatus::OK();
        } catch (const std::exception &error) {
            return rocksdb::IOStatus::IOError(
                std::string("async read submission failed: ") + error.what());
        } catch (...) {
            return rocksdb::IOStatus::IOError("async read submission failed");
        }
    }

private:
    std::shared_ptr<hfs::File> file_;
};

class HfsWritableFile final : public rocksdb::FSWritableFile {
public:
    HfsWritableFile(std::shared_ptr<hfs::File> file) : file_(std::move(file)) {}

    /* Appends buffered-mode bytes at the current HFS logical EOF. */
    rocksdb::IOStatus Append(const rocksdb::Slice &data, const rocksdb::IOOptions &,
                             rocksdb::IODebugContext *) override {
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(const_cast<char *>(data.data())),
                          data.size()};
        auto written = file_->append(std::span<const hfs::IoVec>(&buffer, 1)).wait();
        if (!written.ok()) {
            return toIoStatus(written);
        }
        auto value = std::move(written).value();
        return value.ok() ? rocksdb::IOStatus::OK() : toIoStatus(value);
    }

    /* Flushes the final RocksDB metadata before releasing the HFS handle. */
    rocksdb::IOStatus Close(const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        if (file_ != nullptr) {
            auto synced = file_->sync().wait();
            if (!synced.ok()) return toIoStatus(synced);
            auto value = std::move(synced).value();
            if (!value.ok()) return toIoStatus(value);
        }
        file_.reset();
        return rocksdb::IOStatus::OK();
    }

    /* Makes buffered RocksDB metadata visible across process reopen. */
    rocksdb::IOStatus Flush(const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto synced = file_->sync().wait();
        if (!synced.ok()) return toIoStatus(synced);
        return toIoStatus(std::move(synced).value());
    }

    /* Writes at an explicit offset for RocksDB preallocation and manifests. */
    rocksdb::IOStatus PositionedAppend(const rocksdb::Slice &data, uint64_t offset,
                                        const rocksdb::IOOptions &,
                                        rocksdb::IODebugContext *) override {
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(const_cast<char *>(data.data())),
                          data.size()};
        auto written = file_->write(std::span<const hfs::IoVec>(&buffer, 1), offset).wait();
        if (!written.ok()) return toIoStatus(written);
        auto value = std::move(written).value();
        return value.ok() ? rocksdb::IOStatus::OK() : toIoStatus(value);
    }

    /* Reserves and zero-initializes the requested HFS logical range. */
    rocksdb::IOStatus Allocate(uint64_t offset, uint64_t length,
                               const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto allocated = file_->allocate(offset, length).wait();
        return allocated.ok() ? toIoStatus(std::move(allocated).value()) : toIoStatus(allocated);
    }

    /* Buffered HFS writes have no separate range cache; sync the file safely. */
    rocksdb::IOStatus RangeSync(uint64_t, uint64_t, const rocksdb::IOOptions &options,
                                rocksdb::IODebugContext *dbg) override {
        return Sync(options, dbg);
    }

    /* The core allocator performs all reservation work during Append/Allocate. */
    void PrepareWrite(size_t, size_t, const rocksdb::IOOptions &,
                      rocksdb::IODebugContext *) override {}

    /* Persists user data and file metadata through the HFS durable epoch path. */
    rocksdb::IOStatus Sync(const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto synced = file_->sync().wait();
        return synced.ok() ? toIoStatus(std::move(synced).value()) : toIoStatus(synced);
    }

private:
    std::shared_ptr<hfs::File> file_;
};

class HfsDirectory final : public rocksdb::FSDirectory {
public:
    HfsDirectory(std::shared_ptr<hfs::File> file) : file_(std::move(file)) {}

    /* Commits directory metadata through the HFS durable epoch path. */
    rocksdb::IOStatus Fsync(const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto synced = file_->sync().wait();
        return synced.ok() ? toIoStatus(std::move(synced).value()) : toIoStatus(synced);
    }

    /* Directory-specific options do not change HFS's single durable barrier. */
    rocksdb::IOStatus FsyncWithDirOptions(const rocksdb::IOOptions &options,
                                          rocksdb::IODebugContext *dbg,
                                          const rocksdb::DirFsyncOptions &) override {
        return Fsync(options, dbg);
    }

private:
    std::shared_ptr<hfs::File> file_;
};

struct HfsFileLock final : public rocksdb::FileLock {
    int fd = -1;
};

class HfsFileSystem final : public rocksdb::FileSystem {
public:
    explicit HfsFileSystem(std::shared_ptr<HfsContext> context)
        : context_(std::move(context)) {}

    /* Identifies this implementation to RocksDB's Customizable registry. */
    const char *Name() const override { return "HfsFileSystem"; }

    /* Advertises the implemented callback, poll, and abort async contract. */
    void SupportedOps(int64_t &supported_ops) override {
        supported_ops = (1LL << rocksdb::FSSupportedOps::kAsyncIO);
    }

    /* Waits until the requested number of submitted reads have completed. */
    rocksdb::IOStatus Poll(std::vector<void *> &ioHandles,
                           size_t minCompletions) override {
        const size_t required = std::min(minCompletions, ioHandles.size());
        if (required == 0) return rocksdb::IOStatus::OK();
        while (true) {
            size_t completed = 0;
            AsyncReadState *waitState = nullptr;
            for (void *handle : ioHandles) {
                if (handle == nullptr) continue;
                auto *state = static_cast<AsyncReadState *>(handle);
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->completed) {
                    ++completed;
                } else if (waitState == nullptr) {
                    waitState = state;
                }
            }
            if (completed >= required) return rocksdb::IOStatus::OK();
            if (waitState == nullptr) return rocksdb::IOStatus::IOError(
                "async read completion handle is invalid");
            std::unique_lock<std::mutex> lock(waitState->mutex);
            waitState->condition.wait(lock, [waitState] {
                return waitState->completed;
            });
        }
    }

    /* Cancels pending reads and suppresses callbacks for reads not yet completed. */
    rocksdb::IOStatus AbortIO(std::vector<void *> &ioHandles) override {
        for (void *handle : ioHandles) {
            if (handle == nullptr) continue;
            auto *state = static_cast<AsyncReadState *>(handle);
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->completed && !state->callbackStarted) {
                state->aborted = true;
                state->request->status = rocksdb::IOStatus::Aborted(
                    "async read aborted");
            }
        }
        return rocksdb::IOStatus::OK();
    }

    /* Opens an existing HFS regular file for sequential reads. */
    rocksdb::IOStatus NewSequentialFile(const std::string &fname,
                                         const rocksdb::FileOptions &,
                                         std::unique_ptr<rocksdb::FSSequentialFile> *result,
                                         rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        *result = std::make_unique<HfsSequentialFile>(std::move(file));
        return rocksdb::IOStatus::OK();
    }

    /* Opens an existing HFS regular file for concurrent random reads. */
    rocksdb::IOStatus NewRandomAccessFile(
        const std::string &fname, const rocksdb::FileOptions &,
        std::unique_ptr<rocksdb::FSRandomAccessFile> *result,
        rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        *result = std::make_unique<HfsRandomAccessFile>(std::move(file));
        return rocksdb::IOStatus::OK();
    }

    /* Creates or replaces a buffered HFS writable file. */
    rocksdb::IOStatus NewWritableFile(
        const std::string &fname, const rocksdb::FileOptions &,
        std::unique_ptr<rocksdb::FSWritableFile> *result,
        rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname,
                               hfs::OpenFlags::Create | hfs::OpenFlags::Truncate, &file);
        if (!status.ok()) return status;
        *result = std::make_unique<HfsWritableFile>(std::move(file));
        return rocksdb::IOStatus::OK();
    }

    /* Opens a resident HFS directory handle. */
    rocksdb::IOStatus NewDirectory(const std::string &name, const rocksdb::IOOptions &,
                                    std::unique_ptr<rocksdb::FSDirectory> *result,
                                    rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, name, hfs::OpenFlags::Directory, &file);
        if (!status.ok()) return status;
        *result = std::make_unique<HfsDirectory>(std::move(file));
        return rocksdb::IOStatus::OK();
    }

    /* Checks existence without using a host filesystem descriptor. */
    rocksdb::IOStatus FileExists(const std::string &fname, const rocksdb::IOOptions &,
                                 rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        return openFile(context_, fname, hfs::OpenFlags::None, &file);
    }

    /* Lists one HFS directory page and continues until all children are seen. */
    rocksdb::IOStatus GetChildren(const std::string &dir, const rocksdb::IOOptions &,
                                  std::vector<std::string> *result,
                                  rocksdb::IODebugContext *) override {
        result->clear();
        std::string cursor;
        do {
            auto page = context_->client->getChildren(hfsPath(dir), cursor, 4096).wait();
            if (!page.ok()) return toIoStatus(page);
            auto value = std::move(page).value();
            if (!value.ok()) return toIoStatus(value);
            for (const auto &entry : value.value().entries) result->push_back(entry.name);
            cursor = value.value().nextCursor;
        } while (!cursor.empty());
        return rocksdb::IOStatus::OK();
    }

    /* Removes a regular HFS namespace entry. */
    rocksdb::IOStatus DeleteFile(const std::string &fname, const rocksdb::IOOptions &,
                                 rocksdb::IODebugContext *) override {
        auto removed = context_->client->remove(hfsPath(fname)).wait();
        return removed.ok() ? toIoStatus(std::move(removed).value()) : toIoStatus(removed);
    }

    /* Creates one HFS directory entry and rejects an existing name. */
    rocksdb::IOStatus CreateDir(const std::string &dirname, const rocksdb::IOOptions &,
                                rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        return openFile(context_, dirname,
                        hfs::OpenFlags::Create | hfs::OpenFlags::Exclusive |
                            hfs::OpenFlags::Directory,
                        &file);
    }

    /* Creates a directory or accepts an already existing directory only. */
    rocksdb::IOStatus CreateDirIfMissing(const std::string &dirname,
                                         const rocksdb::IOOptions &options,
                                         rocksdb::IODebugContext *dbg) override {
        std::shared_ptr<hfs::File> existing;
        auto status = openFile(context_, dirname, hfs::OpenFlags::Directory, &existing);
        if (status.ok()) {
            return rocksdb::IOStatus::OK();
        }
        if (!status.IsNotFound()) {
            return status;
        }
        return CreateDir(dirname, options, dbg);
    }

    /* Removes an empty HFS directory entry. */
    rocksdb::IOStatus DeleteDir(const std::string &dirname, const rocksdb::IOOptions &,
                                rocksdb::IODebugContext *) override {
        auto removed = context_->client->remove(hfsPath(dirname)).wait();
        return removed.ok() ? toIoStatus(std::move(removed).value()) : toIoStatus(removed);
    }

    /* Returns the logical HFS size, not a host-file size. */
    rocksdb::IOStatus GetFileSize(const std::string &fname, const rocksdb::IOOptions &,
                                  uint64_t *fileSize, rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        auto stat = file->stat();
        if (!stat.ok()) return toIoStatus(stat);
        *fileSize = stat.value().logicalSize;
        return rocksdb::IOStatus::OK();
    }

    /* Reports HFS's namespace modification timestamp. */
    rocksdb::IOStatus GetFileModificationTime(const std::string &fname,
                                               const rocksdb::IOOptions &, uint64_t *mtime,
                                               rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        auto stat = file->stat();
        if (!stat.ok()) return toIoStatus(stat);
        *mtime = stat.value().mtimeNs;
        return rocksdb::IOStatus::OK();
    }

    /* Performs an atomic HFS namespace rename. */
    rocksdb::IOStatus RenameFile(const std::string &src, const std::string &target,
                                 const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto renamed = context_->client->rename(hfsPath(src), hfsPath(target)).wait();
        if (!renamed.ok()) return toIoStatus(renamed);
        auto renameStatus = std::move(renamed).value();
        if (!renameStatus.ok()) return toIoStatus(renameStatus);
        /* RocksDB uses rename for CURRENT and MANIFEST publication. */
        std::shared_ptr<hfs::File> file;
        auto opened = openFile(context_, target, hfs::OpenFlags::None, &file);
        if (!opened.ok()) return opened;
        auto synced = file->sync().wait();
        if (!synced.ok()) return toIoStatus(synced);
        return toIoStatus(std::move(synced).value());
    }

    /* Creates a native HFS hard link without duplicating file contents. */
    rocksdb::IOStatus LinkFile(const std::string &src, const std::string &target,
                               const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto linked = context_->client->link(hfsPath(src), hfsPath(target)).wait();
        return linked.ok() ? toIoStatus(std::move(linked).value()) : toIoStatus(linked);
    }

    /* Reports the shared inode link count for a regular HFS file. */
    rocksdb::IOStatus NumFileLinks(const std::string &fname, const rocksdb::IOOptions &,
                                   uint64_t *count, rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        auto stat = file->stat();
        if (!stat.ok()) return toIoStatus(stat);
        *count = stat.value().linkCount;
        return rocksdb::IOStatus::OK();
    }

    /* Compares stable inode identity rather than path strings. */
    rocksdb::IOStatus AreFilesSame(const std::string &first, const std::string &second,
                                   const rocksdb::IOOptions &, bool *same,
                                   rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> left;
        std::shared_ptr<hfs::File> right;
        auto leftStatus = openFile(context_, first, hfs::OpenFlags::None, &left);
        if (!leftStatus.ok()) return leftStatus;
        auto rightStatus = openFile(context_, second, hfs::OpenFlags::None, &right);
        if (!rightStatus.ok()) return rightStatus;
        auto leftStat = left->stat();
        auto rightStat = right->stat();
        if (!leftStat.ok()) return toIoStatus(leftStat);
        if (!rightStat.ok()) return toIoStatus(rightStat);
        *same = leftStat.value().inodeId == rightStat.value().inodeId &&
                leftStat.value().generation == rightStat.value().generation;
        return rocksdb::IOStatus::OK();
    }

    /* Acquires a process lock for RocksDB's DB identity. */
    rocksdb::IOStatus LockFile(const std::string &fname, const rocksdb::IOOptions &,
                               rocksdb::FileLock **lock, rocksdb::IODebugContext *) override {
        if (lock == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null RocksDB lock output");
        }
        auto *value = new HfsFileLock;
        value->fd = ::open(hostLockPath(fname).c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (value->fd < 0) {
            delete value;
            return rocksdb::IOStatus::IOError("RocksDB lock file open failed");
        }
        if (::flock(value->fd, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            ::close(value->fd);
            delete value;
            /* Only lock contention is retryable; preserve unrelated host I/O
             * failures so RocksDB can report the actual acquisition problem. */
            if (error == EWOULDBLOCK || error == EAGAIN) {
                return rocksdb::IOStatus::Busy("RocksDB lock is already held");
            }
            return rocksdb::IOStatus::IOError("RocksDB lock acquisition failed");
        }
        *lock = value;
        return rocksdb::IOStatus::OK();
    }

    /* Releases only locks returned by this adapter. */
    rocksdb::IOStatus UnlockFile(rocksdb::FileLock *lock, const rocksdb::IOOptions &,
                                 rocksdb::IODebugContext *) override {
        auto *value = static_cast<HfsFileLock *>(lock);
        const int rc = value == nullptr ? -1 : ::close(value->fd);
        delete value;
        return rc == 0 ? rocksdb::IOStatus::OK()
                       : rocksdb::IOStatus::IOError("RocksDB lock close failed");
    }

    /* Exposes a stable HFS test namespace path. */
    rocksdb::IOStatus GetTestDirectory(const rocksdb::IOOptions &, std::string *path,
                                       rocksdb::IODebugContext *) override {
        *path = "/rocksdb-test";
        return rocksdb::IOStatus::OK();
    }

    /* HFS paths are already canonical absolute namespace paths. */
    rocksdb::IOStatus GetAbsolutePath(const std::string &dbPath, const rocksdb::IOOptions &,
                                      std::string *output, rocksdb::IODebugContext *) override {
        *output = hfsPath(dbPath);
        return rocksdb::IOStatus::OK();
    }

    /* Reports only allocator-backed data capacity, excluding metadata regions. */
    rocksdb::IOStatus GetFreeSpace(const std::string &, const rocksdb::IOOptions &,
                                   uint64_t *freeSpace, rocksdb::IODebugContext *) override {
        *freeSpace = context_->engine->freeDataBytes();
        return rocksdb::IOStatus::OK();
    }

    /* Distinguishes HFS directory handles from regular file handles. */
    rocksdb::IOStatus IsDirectory(const std::string &path, const rocksdb::IOOptions &,
                                   bool *isDirectory, rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, path, hfs::OpenFlags::Directory, &file);
        if (status.ok()) {
            *isDirectory = true;
            return rocksdb::IOStatus::OK();
        }
        /* HFS uses InvalidArgument for an existing regular file opened with
         * Directory; that is a valid negative answer, like NotFound. */
        if (!status.IsNotFound() && !status.IsInvalidArgument()) {
            return status;
        }
        *isDirectory = false;
        return rocksdb::IOStatus::OK();
    }

private:
    std::shared_ptr<HfsContext> context_;
};

} // namespace

/* Builds and mounts one shared HFS engine for the RocksDB FileSystem. */
std::shared_ptr<rocksdb::FileSystem>
createFileSystem(const std::string &configPath) {
    auto config = hfs::parseConfigFile(configPath);
    if (!config.ok()) return nullptr;
    auto engine = hfs::Engine::create(config.value());
    if (!engine.ok()) return nullptr;
    auto owned = std::shared_ptr<hfs::Engine>(std::move(engine).value());
    auto mounted = owned->mount().wait();
    if (!mounted.ok() || !mounted.value().ok()) return nullptr;
    auto client = hfs::Client::create(*owned);
    if (!client.ok()) return nullptr;
    auto context = std::make_shared<HfsContext>();
    context->engine = std::move(owned);
    context->client = std::move(client).value();
    return std::make_shared<HfsFileSystem>(std::move(context));
}

/* Injects HFS into DBOptions::env without transferring raw ownership to HFS. */
std::unique_ptr<rocksdb::Env> createCompositeEnv(const std::string &configPath) {
    auto fileSystem = createFileSystem(configPath);
    if (fileSystem == nullptr) {
        return nullptr;
    }
    return rocksdb::NewCompositeEnv(fileSystem);
}

} // namespace hfs::rocksdb_adapter
