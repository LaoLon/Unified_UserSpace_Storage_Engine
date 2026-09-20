/* HFS 2.0 RocksDB 8.9.1 buffered FileSystem adapter. */

#include "rocksdb/hfs_filesystem.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <sys/file.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/file.hpp"
#include "rocksdb/file_system.h"

namespace hfs::rocksdb_adapter {

namespace {

/* Temporary P0 call-pattern counters (env HFS_ROCKSDB_IO_STATS=1). */
std::atomic<std::uint64_t> gAppendCalls{0};
std::atomic<std::uint64_t> gAppendBytes{0};
std::atomic<std::uint64_t> gFlushCalls{0};
std::atomic<std::uint64_t> gSyncCalls{0};
std::atomic<std::uint64_t> gBufferFlushes{0};
std::atomic<std::uint64_t> gWriteThroughs{0};
std::atomic<std::uint64_t> gReadCalls{0};
std::atomic<std::uint64_t> gMultiReadCalls{0};
std::atomic<std::uint64_t> gMultiReadRequests{0};
std::atomic<std::uint64_t> gReadAsyncCalls{0};
bool ioStatsEnabled() {
    static const bool enabled = std::getenv("HFS_ROCKSDB_IO_STATS") != nullptr;
    return enabled;
}
void reportIoStats() {
    if (!ioStatsEnabled()) return;
    std::fprintf(stderr,
                 "hfs-rocksdb io stats: append_calls=%llu append_bytes=%llu flush_calls=%llu "
                 "sync_calls=%llu buffer_flushes=%llu write_throughs=%llu "
                 "read_calls=%llu multiread_calls=%llu multiread_requests=%llu "
                 "readasync_calls=%llu\n",
                 (unsigned long long)gAppendCalls.load(),
                 (unsigned long long)gAppendBytes.load(),
                 (unsigned long long)gFlushCalls.load(),
                 (unsigned long long)gSyncCalls.load(),
                 (unsigned long long)gBufferFlushes.load(),
                 (unsigned long long)gWriteThroughs.load(),
                 (unsigned long long)gReadCalls.load(),
                 (unsigned long long)gMultiReadCalls.load(),
                 (unsigned long long)gMultiReadRequests.load(),
                 (unsigned long long)gReadAsyncCalls.load());
}
struct IoStatsReporter {
    ~IoStatsReporter() { reportIoStats(); }
};
const IoStatsReporter gIoStatsReporter;

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

class HfsWritableFile;
class AsyncReadDispatcher;

/* One adapter-side write-back buffer entry (design §7 pending registry). */
struct PendingEntry {
    HfsWritableFile *owner = nullptr;
    std::size_t bytes = 0;           /* buffered bytes not yet in the HFS core */
    std::uint64_t logicalOffset = 0; /* HFS logical offset of the pending bytes */
};

struct HfsContext {
    std::shared_ptr<hfs::Engine> engine;
    std::shared_ptr<hfs::Client> client;
    std::unique_ptr<AsyncReadDispatcher> asyncDispatcher;
    /* Lets reads and size queries observe appended bytes before they reach
     * the HFS core; guarded by pendingMutex. */
    std::mutex pendingMutex;
    std::unordered_map<std::string, PendingEntry> pending;

    /* Stop the frontend owner before cleanly stopping the owned engine. */
    ~HfsContext();
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
        if (result == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null sequential read result");
        }
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
        if (result == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null positioned read result");
        }
        if (scratch == nullptr && n != 0) {
            return rocksdb::IOStatus::InvalidArgument("null positioned read buffer");
        }
        if (ioStatsEnabled()) gReadCalls.fetch_add(1, std::memory_order_relaxed);
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
    AsyncReadDispatcher *dispatcher = nullptr;
    hfs::IoHandle ioHandle;
    std::uint64_t requestId = 0;
    hfs::CompletionToken token{};
    bool submitted = false;
    bool callbackDone = false;
    bool terminal = false;
    bool aborted = false;
    bool callbackStarted = false;
    std::chrono::steady_clock::time_point deadline{};
    std::chrono::steady_clock::time_point notBefore{};

    /* A handle may be deleted after a timeout callback but before core drain. */
    ~AsyncReadState() {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this] { return terminal; });
    }
};

/* One bounded frontend owner replaces the old per-request blocking workers. */
class AsyncReadDispatcher {
public:
    explicit AsyncReadDispatcher(std::size_t queueCapacity = 1024)
        : queueCapacity_(std::max<std::size_t>(1, queueCapacity)) {}

    AsyncReadDispatcher(const AsyncReadDispatcher &) = delete;
    AsyncReadDispatcher &operator=(const AsyncReadDispatcher &) = delete;

    void setClient(std::shared_ptr<hfs::Client> client) noexcept {
        client_ = std::move(client);
    }

    static void wake(void *context) noexcept {
        auto *dispatcher = static_cast<AsyncReadDispatcher *>(context);
        if (dispatcher == nullptr) {
            return;
        }
        dispatcher->wakeEpoch_.fetch_add(1, std::memory_order_release);
        dispatcher->condition_.notify_one();
    }

    hfs::Result<void> start() {
        if (client_ == nullptr) {
            return hfs::makeError(hfs::Status::InvalidState,
                                  "RocksDB frontend dispatcher has no Client");
        }
        std::promise<std::string> ready;
        auto future = ready.get_future();
        try {
            thread_ = std::thread([this, ready = std::move(ready)]() mutable {
                auto bound = client_->bindFrontendThread();
                if (!bound.ok()) {
                    ready.set_value(bound.message());
                    return;
                }
                ready.set_value({});
                run();
            });
        } catch (...) {
            return hfs::makeError(hfs::Status::NoMemory,
                                  "cannot start RocksDB frontend dispatcher");
        }
        const std::string error = future.get();
        if (!error.empty()) {
            if (thread_.joinable()) {
                thread_.join();
            }
            return hfs::makeError(hfs::Status::InvalidState, error);
        }
        return hfs::Result<void>(hfs::Status::Ok);
    }

    bool enqueue(AsyncReadState *state) noexcept {
        if (state == nullptr) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || queue_.size() >= queueCapacity_) {
                return false;
            }
            queue_.push_back(state);
        }
        condition_.notify_one();
        return true;
    }

    rocksdb::IOStatus poll(const std::vector<void *> &handles,
                           std::size_t minCompletions) noexcept {
        const std::size_t required = std::min(minCompletions, handles.size());
        if (required == 0) {
            return rocksdb::IOStatus::OK();
        }
        while (true) {
            std::size_t completed = 0;
            AsyncReadState *waitState = nullptr;
            for (void *handle : handles) {
                if (handle == nullptr) {
                    continue;
                }
                auto *state = static_cast<AsyncReadState *>(handle);
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->callbackDone) {
                    ++completed;
                } else if (waitState == nullptr) {
                    waitState = state;
                }
            }
            if (completed >= required) {
                return rocksdb::IOStatus::OK();
            }
            if (waitState == nullptr) {
                return rocksdb::IOStatus::IOError("async read handle is invalid");
            }
            std::unique_lock<std::mutex> lock(waitState->mutex);
            waitState->condition.wait(lock, [waitState] {
                return waitState->callbackDone;
            });
        }
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() noexcept {
        while (true) {
            const std::uint64_t observedWake = wakeEpoch_.load(std::memory_order_acquire);
            processCompletions();
            processTimeouts();
            processSubmissions();

            bool shouldStop = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                shouldStop = stopping_ && queue_.empty() && active_.empty();
            }
            if (shouldStop) {
                break;
            }

            std::unique_lock<std::mutex> lock(mutex_);
            bool timedWork = !queue_.empty();
            if (!timedWork) {
                for (const auto &entry : active_) {
                    if (entry.second->deadline !=
                        std::chrono::steady_clock::time_point{}) {
                        timedWork = true;
                        break;
                    }
                }
            }
            if (timedWork) {
                condition_.wait_for(lock, std::chrono::microseconds(100));
            } else {
                condition_.wait(lock, [this, observedWake] {
                    return stopping_ || !queue_.empty() ||
                           wakeEpoch_.load(std::memory_order_acquire) != observedWake;
                });
            }
        }

        auto closed = client_->close().wait();
        if (!closed.ok() || !closed.value().ok()) {
            std::fprintf(stderr, "hfs-rocksdb: frontend dispatcher close failed\n");
        }
    }

    void processSubmissions() noexcept {
        std::deque<AsyncReadState *> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(queue_);
        }
        while (!pending.empty()) {
            AsyncReadState *state = pending.front();
            pending.pop_front();
            if (state == nullptr) {
                continue;
            }

            bool aborted = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                aborted = state->aborted;
            }
            if (aborted) {
                completeWithoutIo(state);
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (state->deadline != std::chrono::steady_clock::time_point{} &&
                now >= state->deadline) {
                completeWithoutIo(state, rocksdb::IOStatus::TimedOut(
                                           "async read timed out"));
                continue;
            }
            if (state->notBefore != std::chrono::steady_clock::time_point{} &&
                now < state->notBefore) {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.push_front(state);
                while (!pending.empty()) {
                    queue_.push_back(pending.front());
                    pending.pop_front();
                }
                return;
            }
            if (client_->outstanding() >= client_->maxOutstanding()) {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.push_front(state);
                while (!pending.empty()) {
                    queue_.push_back(pending.front());
                    pending.pop_front();
                }
                return;
            }

            if (state->request->len == 0) {
                state->request->result = rocksdb::Slice(state->request->scratch, 0);
                completeWithoutIo(state, rocksdb::IOStatus::OK());
                continue;
            }

            const std::size_t slots = std::max<std::size_t>(1, client_->maxOutstanding());
            const std::uint64_t sequence = nextToken_++;
            state->token.slot = static_cast<std::uint32_t>(sequence % slots);
            state->token.generation = static_cast<std::uint32_t>(sequence / slots + 1);
            hfs::IoVec buffer{reinterpret_cast<std::byte *>(state->request->scratch),
                              state->request->len};
            auto submitted = client_->submitRead(state->file, buffer,
                                                  state->request->offset, state->token);
            if (!submitted.ok()) {
                if (submitted.status() == hfs::Status::Busy ||
                    submitted.status() == hfs::Status::TryAgain) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.push_front(state);
                    while (!pending.empty()) {
                        queue_.push_back(pending.front());
                        pending.pop_front();
                    }
                    return;
                }
                completeWithoutIo(state, toIoStatus(submitted));
                continue;
            }
            state->ioHandle = std::move(submitted).value();
            state->requestId = state->ioHandle.requestId();
            state->submitted = true;
            active_.emplace(state->requestId, state);
        }
    }

    void processCompletions() noexcept {
        std::array<hfs::CompletionEvent, 64> events{};
        auto polled = client_->poll(events, 0, events.size(), std::chrono::microseconds(0));
        if (polled.status != hfs::Status::Ok) {
            return;
        }
        for (std::size_t index = 0; index < polled.count; ++index) {
            const hfs::CompletionEvent &event = events[index];
            auto found = active_.find(event.requestId);
            if (found == active_.end()) {
                continue;
            }
            AsyncReadState *state = found->second;
            rocksdb::IOStatus status = toIoStatus(hfs::Result<void>(event.status, "HFS async read"));
            if (event.status == hfs::Status::Ok) {
                status = rocksdb::IOStatus::OK();
            }
            publishCallback(state, status, true, event.status == hfs::Status::Ok,
                            event.completedBytes);
            state->ioHandle.reset();
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->terminal = true;
            }
            state->condition.notify_all();
            active_.erase(found);
        }
    }

    void processTimeouts() noexcept {
        const auto now = std::chrono::steady_clock::now();
        for (const auto &entry : active_) {
            AsyncReadState *state = entry.second;
            bool expired = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                expired = state->deadline != std::chrono::steady_clock::time_point{} &&
                          now >= state->deadline && !state->callbackStarted &&
                          !state->aborted;
            }
            if (expired) {
                publishCallback(state, rocksdb::IOStatus::TimedOut("async read timed out"),
                                false, false, 0);
            }
        }
    }

    void completeWithoutIo(AsyncReadState *state,
                           rocksdb::IOStatus status = rocksdb::IOStatus::Aborted(
                               "async read aborted")) noexcept {
        publishCallback(state, status, true, false, 0);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->terminal = true;
        }
        state->condition.notify_all();
    }

    void publishCallback(AsyncReadState *state, const rocksdb::IOStatus &status,
                         bool terminal, bool successful, std::size_t completedBytes) noexcept {
        bool invoke = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->aborted && !state->callbackStarted) {
                if (successful) {
                    state->request->result = rocksdb::Slice(state->request->scratch,
                                                             completedBytes);
                }
                state->request->status = status;
                state->callbackStarted = true;
                invoke = true;
            }
        }
        if (invoke) {
            try {
                state->callback(*state->request, state->callbackArg);
            } catch (...) {
                /* RocksDB callbacks must not escape the dispatcher thread. */
            }
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (terminal) {
                state->callbackDone = true;
            }
        }
        if (terminal) {
            state->condition.notify_all();
        }
    }

    std::shared_ptr<hfs::Client> client_;
    const std::size_t queueCapacity_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<AsyncReadState *> queue_;
    std::unordered_map<std::uint64_t, AsyncReadState *> active_;
    std::thread thread_;
    bool stopping_ = false;
    std::uint64_t nextToken_ = 0;
    std::atomic<std::uint64_t> wakeEpoch_{0};
};

/* Deletes a request only after the dispatcher has drained core completion. */
void deleteAsyncReadHandle(void *handle) {
    delete static_cast<AsyncReadState *>(handle);
}

HfsContext::~HfsContext() {
    if (asyncDispatcher != nullptr) {
        asyncDispatcher->stop();
    }
    if (client != nullptr) {
        client->close().wait();
    }
    asyncDispatcher.reset();
    client.reset();
    if (engine != nullptr) {
        engine->stop().wait();
        engine.reset();
    }
}

class HfsRandomAccessFile final : public rocksdb::FSRandomAccessFile {
public:
    HfsRandomAccessFile(std::shared_ptr<hfs::File> file,
                        std::shared_ptr<HfsContext> context)
        : file_(std::move(file)), context_(std::move(context)) {}

    /* Performs a synchronous facade over the shared asynchronous read path. */
    rocksdb::IOStatus Read(uint64_t offset, size_t n, const rocksdb::IOOptions &options,
                           rocksdb::Slice *result, char *scratch,
                           rocksdb::IODebugContext *debug) const override {
        if (result == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null random read result");
        }
        if (scratch == nullptr && n != 0) {
            return rocksdb::IOStatus::InvalidArgument("null random read buffer");
        }
        if (ioStatsEnabled()) gReadCalls.fetch_add(1, std::memory_order_relaxed);
        rocksdb::FSReadRequest request;
        request.offset = offset;
        request.len = n;
        request.scratch = scratch;
        void *handle = nullptr;
        rocksdb::IOHandleDeleter deleter = {};
        auto submitted = const_cast<HfsRandomAccessFile *>(this)->ReadAsync(
            request, options, [](const rocksdb::FSReadRequest &, void *) {}, nullptr,
            &handle, &deleter, debug);
        if (!submitted.ok()) {
            return submitted;
        }
        std::vector<void *> handles{handle};
        auto polled = context_->asyncDispatcher->poll(handles, 1);
        deleteAsyncReadHandle(handle);
        if (!polled.ok()) {
            return polled;
        }
        *result = request.result;
        return request.status;
    }

    /* Processes a batch through the shared dispatcher while preserving status. */
    rocksdb::IOStatus MultiRead(rocksdb::FSReadRequest *requests, size_t count,
                                const rocksdb::IOOptions &options,
                                rocksdb::IODebugContext *debug) override {
        if (requests == nullptr && count != 0) {
            return rocksdb::IOStatus::InvalidArgument("null multi-read requests");
        }
        if (ioStatsEnabled()) {
            gMultiReadCalls.fetch_add(1, std::memory_order_relaxed);
            gMultiReadRequests.fetch_add(count, std::memory_order_relaxed);
        }
        std::vector<void *> handles;
        handles.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            auto &request = requests[index];
            void *handle = nullptr;
            rocksdb::IOHandleDeleter deleter = {};
            auto status = ReadAsync(
                request, options,
                [](const rocksdb::FSReadRequest &, void *) {}, nullptr,
                &handle, &deleter, debug);
            if (!status.ok()) {
                request.status = status;
                continue;
            }
            handles.push_back(handle);
        }
        auto firstError = rocksdb::IOStatus::OK();
        if (!handles.empty()) {
            firstError = context_->asyncDispatcher->poll(handles, count);
        }
        for (void *handle : handles) {
            deleteAsyncReadHandle(handle);
        }
        for (size_t index = 0; index < count; ++index) {
            if (!requests[index].status.ok() && firstError.ok()) {
                firstError = requests[index].status;
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
        if (ioStatsEnabled()) gReadAsyncCalls.fetch_add(1, std::memory_order_relaxed);
        *ioHandle = nullptr;
        *deleter = {};
        if (context_ == nullptr || context_->asyncDispatcher == nullptr) {
            return rocksdb::IOStatus::InvalidArgument(
                "HFS async frontend dispatcher is unavailable");
        }
        try {
            auto state = std::make_unique<AsyncReadState>();
            state->file = file_;
            state->request = &request;
            state->options = options;
            state->callback = std::move(callback);
            state->callbackArg = callbackArg;
            state->dispatcher = context_->asyncDispatcher.get();
            if (options.timeout.count() > 0) {
                state->deadline = std::chrono::steady_clock::now() + options.timeout;
            }
            const char *delayText = std::getenv("HFS_TEST_ASYNC_READ_DELAY_MS");
            if (delayText != nullptr && *delayText != '\0') {
                char *end = nullptr;
                const unsigned long delay = std::strtoul(delayText, &end, 10);
                if (end != delayText && *end == '\0' && delay <= 60000) {
                    state->notBefore = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(delay);
                }
            }
            if (!context_->asyncDispatcher->enqueue(state.get())) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->callbackDone = true;
                    state->terminal = true;
                }
                state->condition.notify_all();
                return rocksdb::IOStatus::Busy("HFS async frontend queue is full");
            }
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
    std::shared_ptr<HfsContext> context_;
};

/* Buffers appends in adapter memory and flushes them to the HFS core in
 * batches (2026-09-19 design, decision B). Flush crosses only the HFS logical
 * layer; Sync/Fsync/Close cross the durable-epoch boundary. */
class HfsWritableFile final : public rocksdb::FSWritableFile {
public:
    HfsWritableFile(std::shared_ptr<hfs::File> file, std::shared_ptr<HfsContext> context,
                    std::string path)
        : file_(std::move(file)), context_(std::move(context)), path_(std::move(path)) {
        if (file_ != nullptr) {
            auto stat = file_->stat();
            if (stat.ok()) {
                logicalOffset_ = stat.value().logicalSize;
            }
        }
        registerPending();
    }

    ~HfsWritableFile() override {
        /* A close failure must not silently drop buffered bytes. */
        if (file_ != nullptr) {
            auto flushed = flushPendingBuffer();
            if (!flushed.ok()) {
                std::fprintf(stderr,
                             "hfs-rocksdb: dropping %zu buffered bytes for %s\n",
                             buffer_.size(), path_.c_str());
            }
            auto synced = file_->sync().wait();
            if (!synced.ok() || !synced.value().ok()) {
                std::fprintf(stderr, "hfs-rocksdb: final sync failed for %s\n",
                             path_.c_str());
            }
        }
        unregisterPending();
    }

    /* Copies small appends into the pending buffer; large appends are written
     * through after the buffer is flushed to preserve byte order. */
    rocksdb::IOStatus Append(const rocksdb::Slice &data, const rocksdb::IOOptions &,
                             rocksdb::IODebugContext *) override {
        if (file_ == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("writable file is closed");
        }
        if (data.empty()) {
            return rocksdb::IOStatus::OK();
        }
        if (ioStatsEnabled()) gAppendCalls.fetch_add(1, std::memory_order_relaxed);
        if (ioStatsEnabled()) gAppendBytes.fetch_add(data.size(), std::memory_order_relaxed);
        if (data.size() >= kFlushThreshold) {
            auto flushed = flushPendingBuffer();
            if (!flushed.ok()) {
                return flushed;
            }
            return writeThrough(data.data(), data.size(), logicalOffset_);
        }
        buffer_.insert(buffer_.end(), data.data(), data.data() + data.size());
        updatePending();
        if (buffer_.size() >= kFlushThreshold) {
            return flushPendingBuffer();
        }
        return rocksdb::IOStatus::OK();
    }

    /* Flushes pending bytes into the HFS logical layer without a durability
     * barrier; retries are idempotent (same bytes at the same offsets). */
    rocksdb::IOStatus flushPendingBuffer() {
        if (file_ == nullptr || buffer_.empty()) {
            return rocksdb::IOStatus::OK();
        }
        if (ioStatsEnabled()) gBufferFlushes.fetch_add(1, std::memory_order_relaxed);
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(buffer_.data()), buffer_.size()};
        auto written =
            file_->write(std::span<const hfs::IoVec>(&buffer, 1), logicalOffset_).wait();
        if (!written.ok()) {
            return toIoStatus(written);
        }
        auto value = std::move(written).value();
        if (!value.ok()) {
            return toIoStatus(value);
        }
        logicalOffset_ += buffer_.size();
        buffer_.clear();
        updatePending();
        return rocksdb::IOStatus::OK();
    }

    /* Flushes the final RocksDB metadata before releasing the HFS handle. */
    rocksdb::IOStatus Close(const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        if (file_ == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("writable file is closed");
        }
        auto flushed = flushPendingBuffer();
        if (!flushed.ok()) {
            return flushed;
        }
        auto synced = file_->sync().wait();
        if (!synced.ok()) {
            return toIoStatus(synced);
        }
        auto value = std::move(synced).value();
        if (!value.ok()) {
            return toIoStatus(value);
        }
        file_.reset();
        unregisterPending();
        return rocksdb::IOStatus::OK();
    }

    /* RocksDB 8.9 calls Flush after every write group (measured: flush_calls
     * equals append_calls). Honoring it with a device write would defeat the
     * write-back buffer, and Flush is not a durability barrier, so bytes stay
     * buffered; visibility is provided by the pending registry and the buffer
     * is forced out by the threshold, Sync/Fsync, RangeSync, Close and read
     * handle opens. */
    rocksdb::IOStatus Flush(const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        if (ioStatsEnabled()) gFlushCalls.fetch_add(1, std::memory_order_relaxed);
        return rocksdb::IOStatus::OK();
    }

    /* Writes at an explicit offset for RocksDB preallocation and manifests. */
    rocksdb::IOStatus PositionedAppend(const rocksdb::Slice &data, uint64_t offset,
                                        const rocksdb::IOOptions &,
                                        rocksdb::IODebugContext *) override {
        if (file_ == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("writable file is closed");
        }
        return writeThrough(data.data(), data.size(), offset);
    }

    /* Reserves and zero-initializes the requested HFS logical range. */
    rocksdb::IOStatus Allocate(uint64_t offset, uint64_t length,
                               const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        if (file_ == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("writable file is closed");
        }
        auto allocated = file_->allocate(offset, length).wait();
        return allocated.ok() ? toIoStatus(std::move(allocated).value())
                              : toIoStatus(allocated);
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
        if (file_ == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("writable file is closed");
        }
        if (ioStatsEnabled()) gSyncCalls.fetch_add(1, std::memory_order_relaxed);
        auto flushed = flushPendingBuffer();
        if (!flushed.ok()) {
            return flushed;
        }
        auto synced = file_->sync().wait();
        return synced.ok() ? toIoStatus(std::move(synced).value()) : toIoStatus(synced);
    }

private:
    /* Writes one contiguous range through the HFS core. */
    rocksdb::IOStatus writeThrough(const char *data, std::size_t size, std::uint64_t offset) {
        if (ioStatsEnabled()) gWriteThroughs.fetch_add(1, std::memory_order_relaxed);
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(const_cast<char *>(data)), size};
        auto written = file_->write(std::span<const hfs::IoVec>(&buffer, 1), offset).wait();
        if (!written.ok()) {
            return toIoStatus(written);
        }
        auto value = std::move(written).value();
        if (!value.ok()) {
            return toIoStatus(value);
        }
        if (offset == logicalOffset_) {
            logicalOffset_ += size;
            updatePending();
        }
        return rocksdb::IOStatus::OK();
    }

    void registerPending() {
        if (context_ == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(context_->pendingMutex);
        PendingEntry entry;
        entry.owner = this;
        entry.bytes = 0;
        entry.logicalOffset = logicalOffset_;
        context_->pending[path_] = entry;
    }

    void updatePending() {
        if (context_ == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(context_->pendingMutex);
        auto entry = context_->pending.find(path_);
        if (entry != context_->pending.end()) {
            entry->second.bytes = buffer_.size();
            entry->second.logicalOffset = logicalOffset_;
        }
    }

    void unregisterPending() {
        if (context_ == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(context_->pendingMutex);
        auto entry = context_->pending.find(path_);
        if (entry != context_->pending.end() && entry->second.owner == this) {
            context_->pending.erase(entry);
        }
    }

    std::shared_ptr<hfs::File> file_;
    std::shared_ptr<HfsContext> context_;
    std::string path_;
    std::vector<char> buffer_;
    std::uint64_t logicalOffset_ = 0;
    static constexpr std::size_t kFlushThreshold = 1u << 20; /* 1 MiB */
};

/* Flushes any pending adapter buffer for path so read handles observe it. */
rocksdb::IOStatus flushPendingForPath(const std::shared_ptr<HfsContext> &context,
                                      const std::string &path) {
    if (context == nullptr) {
        return rocksdb::IOStatus::OK();
    }
    HfsWritableFile *owner = nullptr;
    {
        std::lock_guard<std::mutex> lock(context->pendingMutex);
        auto entry = context->pending.find(path);
        if (entry != context->pending.end()) {
            owner = entry->second.owner;
        }
    }
    return owner == nullptr ? rocksdb::IOStatus::OK() : owner->flushPendingBuffer();
}

/* Reports buffered bytes for size queries without forcing a flush. */
std::size_t pendingBytesForPath(const std::shared_ptr<HfsContext> &context,
                                const std::string &path) {
    if (context == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(context->pendingMutex);
    auto entry = context->pending.find(path);
    return entry == context->pending.end() ? 0 : entry->second.bytes;
}

/* Drops the pending entry after the file was removed or renamed away. */
void dropPendingForPath(const std::shared_ptr<HfsContext> &context,
                        const std::string &path) {
    if (context == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(context->pendingMutex);
    context->pending.erase(path);
}

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
        if (context_ == nullptr || context_->asyncDispatcher == nullptr) {
            return rocksdb::IOStatus::InvalidArgument(
                "HFS async frontend dispatcher is unavailable");
        }
        return context_->asyncDispatcher->poll(ioHandles, minCompletions);
    }

    /* Cancels pending reads and suppresses callbacks for reads not yet completed. */
    rocksdb::IOStatus AbortIO(std::vector<void *> &ioHandles) override {
        for (void *handle : ioHandles) {
            if (handle == nullptr) continue;
            auto *state = static_cast<AsyncReadState *>(handle);
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->callbackDone && !state->callbackStarted) {
                state->aborted = true;
                state->request->status = rocksdb::IOStatus::Aborted(
                    "async read aborted");
            }
        }
        return rocksdb::IOStatus::OK();
    }

    /* Opens an existing HFS regular file for sequential reads. Pending adapter
     * bytes are flushed first so reads observe prior appends. */
    rocksdb::IOStatus NewSequentialFile(const std::string &fname,
                                         const rocksdb::FileOptions &,
                                         std::unique_ptr<rocksdb::FSSequentialFile> *result,
                                         rocksdb::IODebugContext *) override {
        if (result == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null sequential file output");
        }
        auto flushed = flushPendingForPath(context_, hfsPath(fname));
        if (!flushed.ok()) return flushed;
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
        if (result == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null random file output");
        }
        auto flushed = flushPendingForPath(context_, hfsPath(fname));
        if (!flushed.ok()) return flushed;
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        *result = std::make_unique<HfsRandomAccessFile>(std::move(file), context_);
        return rocksdb::IOStatus::OK();
    }

    /* Creates or replaces a buffered HFS writable file. */
    rocksdb::IOStatus NewWritableFile(
        const std::string &fname, const rocksdb::FileOptions &,
        std::unique_ptr<rocksdb::FSWritableFile> *result,
        rocksdb::IODebugContext *) override {
        if (result == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null writable file output");
        }
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname,
                               hfs::OpenFlags::Create | hfs::OpenFlags::Truncate, &file);
        if (!status.ok()) return status;
        *result = std::make_unique<HfsWritableFile>(std::move(file), context_,
                                                    hfsPath(fname));
        return rocksdb::IOStatus::OK();
    }

    /* Opens a resident HFS directory handle. */
    rocksdb::IOStatus NewDirectory(const std::string &name, const rocksdb::IOOptions &,
                                    std::unique_ptr<rocksdb::FSDirectory> *result,
                                    rocksdb::IODebugContext *) override {
        if (result == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null directory output");
        }
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
        if (result == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null children output");
        }
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

    /* Removes a regular HFS namespace entry and drops any pending adapter
     * buffer for it (the file is gone; buffered bytes are discarded). */
    rocksdb::IOStatus DeleteFile(const std::string &fname, const rocksdb::IOOptions &,
                                 rocksdb::IODebugContext *) override {
        auto removed = context_->client->remove(hfsPath(fname)).wait();
        if (!removed.ok()) {
            return toIoStatus(removed);
        }
        auto status = std::move(removed).value();
        if (!status.ok()) {
            return toIoStatus(status);
        }
        dropPendingForPath(context_, hfsPath(fname));
        return rocksdb::IOStatus::OK();
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
        if (fileSize == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null file size output");
        }
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        auto stat = file->stat();
        if (!stat.ok()) return toIoStatus(stat);
        *fileSize = stat.value().logicalSize + pendingBytesForPath(context_, hfsPath(fname));
        return rocksdb::IOStatus::OK();
    }

    /* Reports HFS's namespace modification timestamp. */
    rocksdb::IOStatus GetFileModificationTime(const std::string &fname,
                                               const rocksdb::IOOptions &, uint64_t *mtime,
                                               rocksdb::IODebugContext *) override {
        if (mtime == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null file time output");
        }
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        auto stat = file->stat();
        if (!stat.ok()) return toIoStatus(stat);
        *mtime = stat.value().mtimeNs;
        return rocksdb::IOStatus::OK();
    }

    /* Performs an atomic HFS namespace rename. Pending adapter bytes for the
     * source are flushed first so the target observes the full content. */
    rocksdb::IOStatus RenameFile(const std::string &src, const std::string &target,
                                 const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto flushedSource = flushPendingForPath(context_, hfsPath(src));
        if (!flushedSource.ok()) return flushedSource;
        auto flushedTarget = flushPendingForPath(context_, hfsPath(target));
        if (!flushedTarget.ok()) return flushedTarget;
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
        auto flushed = flushPendingForPath(context_, hfsPath(src));
        if (!flushed.ok()) return flushed;
        auto linked = context_->client->link(hfsPath(src), hfsPath(target)).wait();
        return linked.ok() ? toIoStatus(std::move(linked).value()) : toIoStatus(linked);
    }

    /* Reports the shared inode link count for a regular HFS file. */
    rocksdb::IOStatus NumFileLinks(const std::string &fname, const rocksdb::IOOptions &,
                                   uint64_t *count, rocksdb::IODebugContext *) override {
        if (count == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null link count output");
        }
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
        if (same == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null file identity output");
        }
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
        if (path == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null test directory output");
        }
        *path = "/rocksdb-test";
        return rocksdb::IOStatus::OK();
    }

    /* HFS paths are already canonical absolute namespace paths. */
    rocksdb::IOStatus GetAbsolutePath(const std::string &dbPath, const rocksdb::IOOptions &,
                                      std::string *output, rocksdb::IODebugContext *) override {
        if (output == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null absolute path output");
        }
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
        if (isDirectory == nullptr) {
            return rocksdb::IOStatus::InvalidArgument("null directory predicate output");
        }
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
    auto context = std::make_shared<HfsContext>();
    context->engine = owned;
    context->asyncDispatcher = std::make_unique<AsyncReadDispatcher>();
    hfs::ClientOptions clientOptions;
    clientOptions.completionWake = &AsyncReadDispatcher::wake;
    clientOptions.completionWakeContext = context->asyncDispatcher.get();
    auto client = hfs::Client::create(*owned, clientOptions);
    if (!client.ok()) return nullptr;
    context->client = std::move(client).value();
    context->asyncDispatcher->setClient(context->client);
    auto dispatcherStarted = context->asyncDispatcher->start();
    if (!dispatcherStarted.ok()) {
        return nullptr;
    }
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
