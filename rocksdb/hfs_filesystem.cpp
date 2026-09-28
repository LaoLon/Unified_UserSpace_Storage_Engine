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
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <pthread.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/diagnostics.hpp"
#include "hfs/file.hpp"
#include "rocksdb/file_system.h"
#include "rocksdb/version.h"

namespace hfs::rocksdb_adapter {

namespace {

/* Test-only async-read delay cache; see setAsyncReadDelayMs() in the header. */
std::atomic<std::uint32_t> gAsyncReadDelayMs{0};
std::atomic<bool> gAsyncReadDelayResolved{false};

} // namespace

void setAsyncReadDelayMs(std::uint32_t milliseconds) noexcept {
    gAsyncReadDelayMs.store(milliseconds, std::memory_order_relaxed);
    gAsyncReadDelayResolved.store(true, std::memory_order_release);
}

namespace {

/* Resolves the test-only delay once so ReadAsync stays getenv-free. */
std::uint32_t asyncReadDelayMs() noexcept {
    if (!gAsyncReadDelayResolved.load(std::memory_order_acquire)) {
        std::uint32_t value = 0;
        const char *text = std::getenv("HFS_TEST_ASYNC_READ_DELAY_MS");
        if (text != nullptr && *text != '\0') {
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(text, &end, 10);
            if (end != text && *end == '\0' && parsed <= 60000) {
                value = static_cast<std::uint32_t>(parsed);
            }
        }
        gAsyncReadDelayMs.store(value, std::memory_order_relaxed);
        gAsyncReadDelayResolved.store(true, std::memory_order_release);
    }
    return gAsyncReadDelayMs.load(std::memory_order_relaxed);
}

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
/* Extent-lifecycle diagnostics for the large-library NoSpace investigation. */
std::atomic<std::uint64_t> gAllocateCalls{0};
std::atomic<std::uint64_t> gDeleteCalls{0};
std::atomic<std::uint64_t> gRenameCalls{0};
/* Live adapter handle counts by file kind (destructor-decremented). */
std::atomic<std::int64_t> gLiveWritable{0};
std::atomic<std::int64_t> gLiveRandom{0};
std::atomic<std::int64_t> gLiveSequential{0};
std::atomic<std::int64_t> gLiveDirectory{0};
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
                 "readasync_calls=%llu allocate_calls=%llu delete_calls=%llu "
                 "rename_calls=%llu live_writable=%lld live_random=%lld "
                 "live_sequential=%lld live_directory=%lld\n",
                 (unsigned long long)gAppendCalls.load(),
                 (unsigned long long)gAppendBytes.load(),
                 (unsigned long long)gFlushCalls.load(),
                 (unsigned long long)gSyncCalls.load(),
                 (unsigned long long)gBufferFlushes.load(),
                 (unsigned long long)gWriteThroughs.load(),
                 (unsigned long long)gReadCalls.load(),
                 (unsigned long long)gMultiReadCalls.load(),
                 (unsigned long long)gMultiReadRequests.load(),
                 (unsigned long long)gReadAsyncCalls.load(),
                 (unsigned long long)gAllocateCalls.load(),
                 (unsigned long long)gDeleteCalls.load(),
                 (unsigned long long)gRenameCalls.load(),
                 (long long)gLiveWritable.load(),
                 (long long)gLiveRandom.load(),
                 (long long)gLiveSequential.load(),
                 (long long)gLiveDirectory.load());
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
struct DirectWorkerEntry;

/* Selects the first explicitly configured frontend CPU; auto leaves affinity
 * to the process launcher so existing deployments remain unchanged. */
std::optional<unsigned int> configuredFrontendCpu(const std::string &value) {
    if (value.empty() || value == "auto") {
        return std::nullopt;
    }
    const std::size_t comma = value.find(',');
    const std::string item = value.substr(0, comma);
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(item.c_str(), &end, 10);
    if (end == item.c_str() || *end != '\0' || parsed >= CPU_SETSIZE) {
        return std::nullopt;
    }
    return static_cast<unsigned int>(parsed);
}

/* Pins only within the inherited cpuset; this prevents a config typo from
 * widening a taskset/cgroup restriction. */
bool pinFrontendToCpu(unsigned int cpu) noexcept {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) != 0 ||
        !CPU_ISSET(cpu, &allowed)) {
        return false;
    }
    cpu_set_t selected;
    CPU_ZERO(&selected);
    CPU_SET(cpu, &selected);
    return pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected) == 0;
}

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
    /* [rocksdb].worker_direct_reads, env-overridable for diagnostics. */
    bool workerDirectReads = true;
    /* Lets reads and size queries observe appended bytes before they reach
     * the HFS core; guarded by pendingMutex. */
    std::mutex pendingMutex;
    std::unordered_map<std::string, PendingEntry> pending;
    /* Direct worker channel registry. Entries hold non-owning context refs;
     * HfsContext must retire all of them before stopping the engine. The
     * registry keeps shared ownership so teardown never touches a freed entry. */
    std::mutex directMutex;
    std::unordered_map<DirectWorkerEntry *, std::shared_ptr<DirectWorkerEntry>>
        directEntries;
    bool directShutdown = false;

    /* Registers one direct worker channel; fails after shutdown starts. */
    bool registerDirectEntry(const std::shared_ptr<DirectWorkerEntry> &entry) noexcept;
    void unregisterDirectEntry(DirectWorkerEntry *entry) noexcept;
    /* Retires every live channel on the calling thread while engine is alive. */
    void retireAllDirectEntries() noexcept;
    /* Drops the cached per-path File in every worker channel so unlinked or
     * renamed files can close their handles and release extents. */
    void dropDirectPath(const std::string &path) noexcept;

    /* Retire worker channels, then stop the frontend owner and the engine. */
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
    HfsSequentialFile(std::shared_ptr<hfs::File> file) : file_(std::move(file)) {
        gLiveSequential.fetch_add(1, std::memory_order_relaxed);
    }
    ~HfsSequentialFile() override {
        gLiveSequential.fetch_sub(1, std::memory_order_relaxed);
    }

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

    /* Sequential file I/O keeps the existing HFS DMA alignment contract. */
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
#if ROCKSDB_MAJOR >= 9
    std::function<void(rocksdb::FSReadRequest &, void *)> callback;
#else
    std::function<void(const rocksdb::FSReadRequest &, void *)> callback;
#endif
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
    std::chrono::steady_clock::time_point enqueuedAt{};
    std::chrono::steady_clock::time_point submittedAt{};
    DirectWorkerEntry *directEntry = nullptr;
    std::shared_ptr<hfs::Client> directClient;
    std::shared_ptr<hfs::File> directFile;

    /* A handle may be deleted after a timeout callback but before core drain. */
    ~AsyncReadState() {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this] { return terminal; });
    }
};

/* Owns the Client and completion table for one RocksDB worker thread.  The
 * Client API is frontend-thread-owned, so this object is deliberately kept in
 * thread-local storage rather than shared between RocksDB workers.  The
 * context reference is non-owning: thread-local storage must never extend the
 * HFS engine lifetime into a background thread's destructor. */
struct DirectWorkerEntry {
    HfsContext *context = nullptr;
    std::shared_ptr<hfs::Client> client;
    std::unordered_map<std::string, std::shared_ptr<hfs::File>> files;
    std::unordered_map<std::uint64_t, AsyncReadState *> active;
    std::uint64_t nextToken = 0;
    mutable std::mutex mutex;
    bool retired = false;

    bool isRetired() const noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        return retired;
    }

    /* Closes this channel once; callable from the owner thread or from the
     * context teardown on the main thread. */
    void retire(bool fromOwnerThread) noexcept;

    /* Drops one cached per-path File so a deleted/renamed file can close its
     * namespace handle and release its extents (large-library NoSpace fix). */
    void dropPath(const std::string &path) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        files.erase(path);
    }

    ~DirectWorkerEntry() { retire(true); }
};

thread_local std::unordered_map<HfsContext *, std::shared_ptr<DirectWorkerEntry>>
    gDirectWorkerEntries;

bool HfsContext::registerDirectEntry(
    const std::shared_ptr<DirectWorkerEntry> &entry) noexcept {
    std::lock_guard<std::mutex> lock(directMutex);
    if (directShutdown || entry == nullptr) {
        return false;
    }
    directEntries.emplace(entry.get(), entry);
    return true;
}

void HfsContext::unregisterDirectEntry(DirectWorkerEntry *entry) noexcept {
    std::lock_guard<std::mutex> lock(directMutex);
    directEntries.erase(entry);
}

/* Moves channel resources out under the entry lock, then releases them while
 * the engine is still alive.  Active reads at retire time are a contract
 * violation: leaking them is safer than waiting forever on a lost completion. */
void DirectWorkerEntry::retire(bool fromOwnerThread) noexcept {
    std::shared_ptr<hfs::Client> ownedClient;
    std::unordered_map<std::string, std::shared_ptr<hfs::File>> ownedFiles;
    std::unordered_map<std::uint64_t, AsyncReadState *> ownedActive;
    HfsContext *owner = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (retired) {
            return;
        }
        retired = true;
        owner = context;
        context = nullptr;
        ownedClient = std::move(client);
        ownedFiles = std::move(files);
        ownedActive = std::move(active);
    }
    if (owner != nullptr) {
        owner->unregisterDirectEntry(this);
    }
    if (!ownedActive.empty()) {
        std::fprintf(stderr, "hfs-rocksdb: direct channel retired with %zu active reads\n",
                     ownedActive.size());
    }
    /* Close namespace handles before the client so openFiles drains first. */
    ownedFiles.clear();
    if (ownedClient != nullptr) {
        if (fromOwnerThread) {
            auto closed = ownedClient->close().wait();
            if (!closed.ok() || !closed.value().ok()) {
                std::fprintf(stderr, "hfs-rocksdb: direct channel close failed\n");
            }
        }
        ownedClient.reset();
    }
}

void HfsContext::retireAllDirectEntries() noexcept {
    std::vector<std::shared_ptr<DirectWorkerEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(directMutex);
        directShutdown = true;
        entries.reserve(directEntries.size());
        for (const auto &item : directEntries) {
            entries.push_back(item.second);
        }
    }
    for (const auto &entry : entries) {
        if (entry != nullptr) {
            entry->retire(false);
        }
    }
}

/* Drops cached per-path handles across all worker channels. Called after a
 * namespace unlink/rename so the orphaned inode's last reference closes and
 * its extents return to the allocator (large-library NoSpace fix). The entry
 * list is copied under directMutex and dropped outside it to keep the lock
 * order directMutex -> entry mutex. */
void HfsContext::dropDirectPath(const std::string &path) noexcept {
    std::vector<std::shared_ptr<DirectWorkerEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(directMutex);
        entries.reserve(directEntries.size());
        for (const auto &item : directEntries) {
            entries.push_back(item.second);
        }
    }
    for (const auto &entry : entries) {
        if (entry != nullptr) {
            entry->dropPath(path);
        }
    }
}

/* Worker-owned read channels are the configured default; the environment
 * variable stays as a process-level override for diagnostics and A/B runs. */
bool configuredWorkerDirectReads(const hfs::Config &config) noexcept {
    const char *value = std::getenv("HFS_ROCKSDB_DIRECT_WORKER_READS");
    if (value != nullptr) {
        return std::string(value) == "1";
    }
    return config.rocksdbWorkerDirectReads;
}

/* Parses the completion batch bound used by the shared frontend experiment. */
std::size_t configuredCompletionBatch() noexcept {
    static const std::size_t batch = [] {
        const char *value = std::getenv("HFS_ROCKSDB_COMPLETION_BATCH");
        if (value == nullptr || *value == '\0') return std::size_t{64};
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || *end != '\0') return std::size_t{64};
        return std::clamp<std::size_t>(parsed, 1, 64);
    }();
    return batch;
}

/* Creates one direct Client on the calling RocksDB worker thread.  Returns
 * null after the context starts shutting down so callers fall back to the
 * shared dispatcher. */
DirectWorkerEntry *directWorkerEntry(const std::shared_ptr<HfsContext> &context) {
    if (context == nullptr || context->engine == nullptr) return nullptr;
    auto found = gDirectWorkerEntries.find(context.get());
    if (found != gDirectWorkerEntries.end()) {
        if (!found->second->isRetired()) {
            return found->second.get();
        }
        /* A retired entry is empty; replace it for a later context at the
         * same address in this thread. */
        gDirectWorkerEntries.erase(found);
    }

    auto entry = std::make_shared<DirectWorkerEntry>();
    hfs::ClientOptions options;
    options.maxOutstanding = 32;
    auto client = hfs::Client::create(*context->engine, options);
    if (!client.ok()) return nullptr;
    entry->client = std::move(client).value();
    if (!context->registerDirectEntry(entry)) {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->retired = true;
        entry->client.reset();
        return nullptr;
    }
    entry->context = context.get();
    auto *raw = entry.get();
    gDirectWorkerEntries.emplace(context.get(), std::move(entry));
    return raw;
}

/* Opens a file through the worker-owned Client so its async File handle is
 * valid for submissions made directly by that same worker. */
std::shared_ptr<hfs::File> directWorkerFile(DirectWorkerEntry *entry,
                                            const std::string &path) {
    if (entry == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->retired || entry->client == nullptr) return nullptr;
    auto found = entry->files.find(path);
    if (found != entry->files.end()) return found->second;
    auto outer = entry->client->open(path).wait();
    if (!outer.ok()) return nullptr;
    auto inner = std::move(outer).value();
    if (!inner.ok()) return nullptr;
    auto file = std::move(inner).value();
    entry->files.emplace(path, file);
    return file;
}

/* One bounded frontend owner replaces the old per-request blocking workers. */
class AsyncReadDispatcher {
public:
    explicit AsyncReadDispatcher(std::size_t queueCapacity = 1024,
                                 std::optional<unsigned int> frontendCpu = std::nullopt,
                                 std::size_t completionBatch = 64,
                                 bool workerDirectReads = true)
        : queueCapacity_(std::max<std::size_t>(1, queueCapacity)),
          frontendCpu_(frontendCpu),
          completionBatch_(std::clamp<std::size_t>(completionBatch, 1, 64)),
          workerDirectReads_(workerDirectReads) {}

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
                const auto tid = static_cast<unsigned long long>(::syscall(SYS_gettid));
                const bool pinned = frontendCpu_.has_value() &&
                                    pinFrontendToCpu(*frontendCpu_);
                std::fprintf(stderr,
                             "[CPU MAP] frontend dispatcher: tid=%llu configured_cpu=%s "
                             "actual_cpu=%d pin=%s completion_batch=%zu direct_worker=%s\n",
                             tid, frontendCpu_.has_value()
                                       ? std::to_string(*frontendCpu_).c_str()
                                       : "auto",
                             sched_getcpu(), pinned ? "ok" : "inherited", completionBatch_,
                             workerDirectReads_ ? "enabled" : "disabled");
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
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || queue_.size() >= queueCapacity_) {
                return false;
            }
            notify = queue_.empty();
            queue_.push_back(state);
        }
        if (notify) {
            condition_.notify_one();
        }
        return true;
    }

    /* Open HFS handles held by the shared frontend client (diagnostics). */
    std::size_t openFileCount() const noexcept {
        return client_ != nullptr ? client_->openFileCount() : 0;
    }

    rocksdb::IOStatus poll(const std::vector<void *> &handles,
                           std::size_t minCompletions) noexcept {
        const std::size_t required = std::min(minCompletions, handles.size());
        if (required == 0) {
            return rocksdb::IOStatus::OK();
        }
        std::vector<DirectWorkerEntry *> directEntries;
        directEntries.reserve(handles.size());
        for (void *handle : handles) {
            if (handle == nullptr) continue;
            auto *state = static_cast<AsyncReadState *>(handle);
            if (state->directEntry == nullptr) continue;
            if (std::find(directEntries.begin(), directEntries.end(),
                          state->directEntry) == directEntries.end()) {
                directEntries.push_back(state->directEntry);
            }
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
            if (waitState->directEntry != nullptr &&
                waitState->directEntry->isRetired()) {
                /* The channel was retired by context teardown; fail this state
                 * instead of spinning on a completion that will never arrive. */
                publishCallback(waitState,
                                rocksdb::IOStatus::Aborted("direct channel retired"),
                                true, false, 0);
                continue;
            }
            if (!directEntries.empty()) {
                const bool waitForDirect = waitState->directEntry != nullptr;
                const auto directStatus = processDirectCompletions(
                    directEntries, waitForDirect);
                if (directStatus.status != hfs::Status::Ok) {
                    return toIoStatus(hfs::Result<void>(
                        directStatus.status, "worker direct completion poll"));
                }
                if (directStatus.count != 0) {
                    continue;
                }
                if (waitForDirect) {
                    continue;
                }
            }
            hfs::diagnostics::recordCompletionWait();
            const bool profile = hfs::diagnostics::profilingEnabled();
            const auto waitStart = profile ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            std::unique_lock<std::mutex> lock(waitState->mutex);
            waitState->condition.wait(lock, [waitState] {
                return waitState->callbackDone;
            });
            if (profile) {
                hfs::diagnostics::recordCompletionWaitNs(
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - waitStart)
                            .count()));
            }
        }
    }

    /* Workers return their handles through this deferred list so the
     * dispatcher thread is the only one that frees shared-path state.  Freeing
     * from the worker can race with an in-flight notify_all() on the state's
     * condition variable and corrupt the glibc condvar group state. */
    void retireHandle(AsyncReadState *state) noexcept {
        if (state == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(retiredMutex_);
            retired_.push_back(state);
        }
        /* Wake the dispatcher so retired states (and their open File handles)
         * are released promptly instead of waiting for an unrelated wake. */
        wakeEpoch_.fetch_add(1, std::memory_order_release);
        condition_.notify_one();
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
        /* Free everything the workers retired before the loop exited. */
        drainRetired();
    }

private:
    void run() noexcept {
        while (true) {
            const std::uint64_t observedWake = wakeEpoch_.load(std::memory_order_acquire);
            processCompletions();
            processTimeouts();
            processSubmissions();
            drainRetired();

            bool shouldStop = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                shouldStop = stopping_ && queue_.empty() && active_.empty();
            }
            if (shouldStop) {
                break;
            }

            std::unique_lock<std::mutex> lock(mutex_);
            /*
             * Keep polling while an I/O is outstanding.  The completion wake
             * is only a hint: a producer can arm the queue before the
             * dispatcher enters its condition-variable wait, and the wake
             * can then be coalesced or lost across the adapter/core boundary.
             * Sleeping indefinitely with active requests leaves every RocksDB
             * reader blocked in poll() even though the SPDK reactor is still
             * making progress.  A short bounded poll closes that race and
             * keeps the normal wake path low-latency.
             */
            bool timedWork = !queue_.empty() || !active_.empty();
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
            const bool profile = hfs::diagnostics::profilingEnabled();
            state->submittedAt = profile ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
            if (profile && state->enqueuedAt != std::chrono::steady_clock::time_point{}) {
                hfs::diagnostics::recordFrontendQueueWaitNs(
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            state->submittedAt - state->enqueuedAt)
                            .count()));
            }
            hfs::diagnostics::recordFrontendSubmit();
            active_.emplace(state->requestId, state);
        }
    }

    /* Polls worker-owned clients on the submitting worker.  Client::poll is
     * frontend-thread-owned, which is exactly why each RocksDB worker gets a
     * separate Client in the direct-ring experiment. */
    hfs::PollResult processDirectCompletions(
        const std::vector<DirectWorkerEntry *> &entries, bool wait) noexcept {
        hfs::PollResult result;
        bool first = true;
        for (DirectWorkerEntry *entry : entries) {
            if (entry == nullptr) continue;
            std::lock_guard<std::mutex> entryLock(entry->mutex);
            if (entry->retired || entry->client == nullptr) continue;
            std::array<hfs::CompletionEvent, 64> events{};
            const std::size_t maxEvents = completionBatch_;
            auto polled = entry->client->poll(
                std::span<hfs::CompletionEvent>(events.data(), maxEvents),
                wait && first ? 1 : 0, maxEvents,
                wait && first ? std::chrono::microseconds(1000)
                              : std::chrono::microseconds(0));
            if (polled.status != hfs::Status::Ok) return polled;
            result.count += polled.count;
            first = false;
            for (std::size_t index = 0; index < polled.count; ++index) {
                const hfs::CompletionEvent &event = events[index];
                auto found = entry->active.find(event.requestId);
                if (found == entry->active.end()) continue;
                AsyncReadState *state = found->second;
                rocksdb::IOStatus status =
                    toIoStatus(hfs::Result<void>(event.status, "HFS async read"));
                if (event.status == hfs::Status::Ok) status = rocksdb::IOStatus::OK();
                publishCallback(state, status, true,
                                event.status == hfs::Status::Ok,
                                event.completedBytes);
                state->ioHandle.reset();
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->terminal = true;
                }
                state->condition.notify_all();
                entry->active.erase(found);
                hfs::diagnostics::recordDirectWorkerCompletion();
            }
        }
        return result;
    }

    void processCompletions() noexcept {
        std::array<hfs::CompletionEvent, 64> events{};
        auto polled = client_->poll(events, 0, completionBatch_,
                                    std::chrono::microseconds(0));
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
            if (hfs::diagnostics::profilingEnabled() &&
                state->submittedAt != std::chrono::steady_clock::time_point{}) {
                hfs::diagnostics::recordFrontendIoNs(
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - state->submittedAt)
                            .count()));
            }
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

    /* Frees worker-retired shared states once the dispatcher no longer touches
     * them; requeues states still present in active_. */
    void drainRetired() noexcept {
        std::vector<AsyncReadState *> retired;
        {
            std::lock_guard<std::mutex> lock(retiredMutex_);
            retired.swap(retired_);
        }
        for (AsyncReadState *state : retired) {
            if (state == nullptr) {
                continue;
            }
            if (active_.find(state->requestId) != active_.end()) {
                std::lock_guard<std::mutex> lock(retiredMutex_);
                retired_.push_back(state);
                continue;
            }
            bool terminal = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                terminal = state->terminal;
            }
            if (!terminal) {
                /* Contract violation: leak instead of blocking the dispatcher. */
                std::fprintf(stderr,
                             "hfs-rocksdb: retiring non-terminal async state\n");
                continue;
            }
            delete state;
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
            hfs::diagnostics::recordCompletion();
            state->condition.notify_all();
        }
    }

    std::shared_ptr<hfs::Client> client_;
    const std::size_t queueCapacity_;
    const std::optional<unsigned int> frontendCpu_;
    const std::size_t completionBatch_;
    const bool workerDirectReads_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<AsyncReadState *> queue_;
    std::unordered_map<std::uint64_t, AsyncReadState *> active_;
    std::thread thread_;
    bool stopping_ = false;
    std::uint64_t nextToken_ = 0;
    std::atomic<std::uint64_t> wakeEpoch_{0};
    std::mutex retiredMutex_;
    std::vector<AsyncReadState *> retired_;
};

/* Deletes a request only after the dispatcher has drained core completion.
 * Shared-path states are freed by the dispatcher thread so a worker can never
 * free memory while the dispatcher is still inside a completion wake. */
void deleteAsyncReadHandle(void *handle) {
    auto *state = static_cast<AsyncReadState *>(handle);
    if (state == nullptr) {
        return;
    }
    if (state->directEntry == nullptr && state->dispatcher != nullptr) {
        state->dispatcher->retireHandle(state);
        return;
    }
    delete state;
}

HfsContext::~HfsContext() {
    /* Worker channels must release client/file/lease resources while the
     * engine is still alive; never let a background thread do this. */
    retireAllDirectEntries();
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
                        std::shared_ptr<HfsContext> context,
                        std::string path)
        : file_(std::move(file)), context_(std::move(context)), path_(std::move(path)) {
        gLiveRandom.fetch_add(1, std::memory_order_relaxed);
    }
    ~HfsRandomAccessFile() override {
        gLiveRandom.fetch_sub(1, std::memory_order_relaxed);
    }

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
            request, options, [](auto &, void *) {}, nullptr,
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
                [](auto &, void *) {}, nullptr,
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

    /* Expose the NVMe sector granularity for random direct reads; the HFS
     * read planner still keeps 4 KiB layout geometry for writes and metadata. */
    size_t GetRequiredBufferAlignment() const override { return 512; }

    /* Submits a real background read and returns before the HFS coroutine completes. */
    rocksdb::IOStatus ReadAsync(
        rocksdb::FSReadRequest &request, const rocksdb::IOOptions &options,
#if ROCKSDB_MAJOR >= 9
        std::function<void(rocksdb::FSReadRequest &, void *)> callback,
#else
        std::function<void(const rocksdb::FSReadRequest &, void *)> callback,
#endif
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
            if (hfs::diagnostics::profilingEnabled()) {
                state->enqueuedAt = std::chrono::steady_clock::now();
            }
            if (options.timeout.count() > 0) {
                state->deadline = std::chrono::steady_clock::now() + options.timeout;
            }
            /* Test-only delay injection: cached in an atomic so the read path
             * never calls getenv; tests switch it at runtime. */
            const std::uint32_t delayMs = asyncReadDelayMs();
            if (delayMs != 0) {
                state->notBefore = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(delayMs);
            }

            /* Experimental fast path: submit from the RocksDB worker itself so
             * Client::submitRead publishes directly to a data-reactor ring.
             * A failed direct setup falls back to the production dispatcher.
             * Timing-injection reads stay on the dispatcher path, which owns
             * the notBefore delay used by the contract tests. */
            if (request.len != 0 && context_->workerDirectReads &&
                state->notBefore == std::chrono::steady_clock::time_point{}) {
                DirectWorkerEntry *entry = directWorkerEntry(context_);
                auto directFile = directWorkerFile(entry, path_);
                if (entry != nullptr && directFile != nullptr) {
                    bool submittedDirect = false;
                    {
                        std::lock_guard<std::mutex> entryLock(entry->mutex);
                        if (!entry->retired && entry->client != nullptr) {
                            const std::size_t slots =
                                std::max<std::size_t>(1, entry->client->maxOutstanding());
                            const std::uint64_t sequence = entry->nextToken++;
                            state->token.slot = static_cast<std::uint32_t>(sequence % slots);
                            state->token.generation =
                                static_cast<std::uint32_t>(sequence / slots + 1);
                            hfs::IoVec buffer{
                                reinterpret_cast<std::byte *>(request.scratch),
                                request.len};
                            auto submitted = entry->client->submitRead(
                                directFile, buffer, request.offset, state->token);
                            if (submitted.ok()) {
                                state->directEntry = entry;
                                state->directClient = entry->client;
                                state->directFile = std::move(directFile);
                                state->ioHandle = std::move(submitted).value();
                                state->requestId = state->ioHandle.requestId();
                                state->submitted = true;
                                entry->active.emplace(state->requestId, state.get());
                                submittedDirect = true;
                            }
                        }
                    }
                    if (submittedDirect) {
                        hfs::diagnostics::recordDirectWorkerRead(request.len);
                        *ioHandle = state.release();
                        *deleter = deleteAsyncReadHandle;
                        return rocksdb::IOStatus::OK();
                    }
                }
            }

            /* HFS 2.0 has no adapter read cache; every shared-path read is a
             * miss.  Direct-worker reads are accounted separately above. */
            hfs::diagnostics::recordFrontendRead(false, request.len);
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
    std::string path_;
};

/* Buffers appends in adapter memory and flushes them to the HFS core in
 * batches (2026-09-19 design, decision B). Flush crosses only the HFS logical
 * layer; Sync/Fsync/Close cross the durable-epoch boundary. */
class HfsWritableFile final : public rocksdb::FSWritableFile {
public:
    HfsWritableFile(std::shared_ptr<hfs::File> file, std::shared_ptr<HfsContext> context,
                    std::string path)
        : file_(std::move(file)), context_(std::move(context)), path_(std::move(path)) {
        gLiveWritable.fetch_add(1, std::memory_order_relaxed);
        if (file_ != nullptr) {
            auto stat = file_->stat();
            if (stat.ok()) {
                logicalOffset_ = stat.value().logicalSize;
            }
        }
        registerPending();
    }

    ~HfsWritableFile() override {
        gLiveWritable.fetch_sub(1, std::memory_order_relaxed);
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

#if ROCKSDB_MAJOR >= 9
    /* Report buffered logical bytes to RocksDB 9's writable-file interface. */
    std::uint64_t GetFileSize(const rocksdb::IOOptions &,
                              rocksdb::IODebugContext *) override {
        return logicalOffset_ + buffer_.size();
    }
#endif

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
        if (ioStatsEnabled()) gAllocateCalls.fetch_add(1, std::memory_order_relaxed);
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
    HfsDirectory(std::shared_ptr<hfs::File> file) : file_(std::move(file)) {
        gLiveDirectory.fetch_add(1, std::memory_order_relaxed);
    }
    ~HfsDirectory() override {
        gLiveDirectory.fetch_sub(1, std::memory_order_relaxed);
    }

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
        *result = std::make_unique<HfsRandomAccessFile>(
            std::move(file), context_, hfsPath(fname));
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
        if (ioStatsEnabled()) gDeleteCalls.fetch_add(1, std::memory_order_relaxed);
        auto removed = context_->client->remove(hfsPath(fname)).wait();
        if (!removed.ok()) {
            return toIoStatus(removed);
        }
        auto status = std::move(removed).value();
        if (!status.ok()) {
            return toIoStatus(status);
        }
        /* Close cached worker handles before dropping the pending buffer so the
         * unlinked inode can release its extents immediately. */
        context_->dropDirectPath(hfsPath(fname));
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
        if (ioStatsEnabled()) gRenameCalls.fetch_add(1, std::memory_order_relaxed);
        auto flushedSource = flushPendingForPath(context_, hfsPath(src));
        if (!flushedSource.ok()) return flushedSource;
        auto flushedTarget = flushPendingForPath(context_, hfsPath(target));
        if (!flushedTarget.ok()) return flushedTarget;
        auto renamed = context_->client->rename(hfsPath(src), hfsPath(target)).wait();
        if (!renamed.ok()) return toIoStatus(renamed);
        auto renameStatus = std::move(renamed).value();
        if (!renameStatus.ok()) return toIoStatus(renameStatus);
        /* Source keys are stale and a replaced target may be unlinked; drop
         * both cached worker handles so no orphan inode keeps its extents. */
        context_->dropDirectPath(hfsPath(src));
        context_->dropDirectPath(hfsPath(target));
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
        if (ioStatsEnabled()) {
            std::fprintf(stderr, "hfs open files: frontend=%zu dispatcher=%zu\n",
                         context_->client != nullptr ? context_->client->openFileCount() : 0,
                         context_->asyncDispatcher != nullptr
                             ? context_->asyncDispatcher->openFileCount()
                             : 0);
            auto stats = context_->engine->dataExtentStats();
            if (stats.ok()) {
                const auto &value = stats.value();
                std::fprintf(stderr,
                             "hfs extent stats: total=%llu free=%llu reserved=%llu "
                             "initialized=%llu committed=%llu inodes=%llu orphans=%llu "
                             "alloc=%llu freed=%llu\n",
                             (unsigned long long)value.totalExtents,
                             (unsigned long long)value.freeExtents,
                             (unsigned long long)value.reservedExtents,
                             (unsigned long long)value.initializedExtents,
                             (unsigned long long)value.committedExtents,
                             (unsigned long long)value.inodes,
                             (unsigned long long)value.orphanInodes,
                             (unsigned long long)value.allocatedExtents,
                             (unsigned long long)value.freedExtents);
            }
        }
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
    const bool workerDirectReads = configuredWorkerDirectReads(config.value());
    auto context = std::make_shared<HfsContext>();
    context->engine = owned;
    context->workerDirectReads = workerDirectReads;
    context->asyncDispatcher = std::make_unique<AsyncReadDispatcher>(
        1024, configuredFrontendCpu(config.value().frontendCpus),
        configuredCompletionBatch(), workerDirectReads);
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
