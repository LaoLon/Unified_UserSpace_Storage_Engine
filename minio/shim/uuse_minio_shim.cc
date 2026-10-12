/* C ABI shim over the UUSE core for the MinIO disk seam (Phase 9, Shard-9.1).
 *
 * v1 follows the TiKV shim model: one shared path-op Client serialized by a
 * mutex; the client is created and closed on a dedicated frontend thread owned
 * by the engine state (core contract: close() must run on the client's
 * frontend thread while the last handle can be dropped by any thread). File
 * data operations run on the calling thread through uuse::File coroutines.
 * The engine state is reference counted: open file handles keep the state (and
 * the engine) alive until closed; a per-.conf weak registry reuses one engine
 * per process when several drives share a .conf.
 *
 * Note: Task<Result<T>>::wait() yields a nested Result<Result<T>> (task level
 * + operation level); every call unwraps both layers. */

#include "uuse_minio_shim.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "uuse/config.hpp"
#include "uuse/core.hpp"
#include "uuse/file.hpp"
#include "uuse/types.hpp"

namespace {

/* Optional frontend diagnostics (UUSE_MINIO_SHIM_DIAG=1): per-entry call
 * count, elapsed time and shared-client mutex wait, printed when the engine
 * state is destroyed. Disabled the overhead is one cached flag per call. */
enum class ShimOp : std::size_t {
    Open = 0,
    Stat,
    Exists,
    MkdirAll,
    Remove,
    RemoveAll,
    Rename,
    Link,
    DirOpen,
    DirRead,
    Pread,
    Pwrite,
    Fdatasync,
    Ftruncate,
    Fstat,
    Allocate,
    DiskStats,
    InodeStats,
    Count,
};

struct ShimOpStats {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> totalNs{0};
    std::atomic<std::uint64_t> maxNs{0};
    std::atomic<std::uint64_t> waitNs{0};
    std::atomic<std::uint64_t> waitMaxNs{0};
};

/* Bounded content cache for small metadata files (xl.meta, format.json,
 * disk-id, healing markers). MinIO re-reads these 4 KiB metadata files on
 * every object access; without a host page cache each read is a device round
 * trip that queues behind multi-hundred-microsecond payload transfers (see
 * doc/performance/minio_shard95_attribution.md section 12). The cache lives in
 * the shim (frontend), not in the core, and keeps the core's data semantics:
 *  - only files whose whole content fits kMetaCacheMaxFileBytes are cached;
 *  - every cached read revalidates (size, mtime, generation) through the file
 *    handle's in-memory stat, so a missed invalidation still misses;
 *  - every mutation through the shim (write/truncate/allocate/open with
 *    truncate/remove/remove_all/rename) invalidates the affected paths;
 *  - bounded by total bytes with LRU eviction; disabled with
 *    UUSE_MINIO_META_CACHE=0; counters exposed through the shim diag. */
constexpr std::size_t kMetaCacheMaxFileBytes = 64 * 1024;

std::atomic<std::uint64_t> g_metaCacheHits{0};
std::atomic<std::uint64_t> g_metaCacheMisses{0};
std::atomic<std::uint64_t> g_metaCacheInserts{0};
std::atomic<std::uint64_t> g_metaCacheStale{0};
std::atomic<std::uint64_t> g_metaCacheBytes{0};
std::atomic<std::uint64_t> g_metaCacheInvalidations{0};

/* Cache byte budget from the environment (MiB, default 64, 0 disables). */
std::size_t metaCacheBudgetBytes() {
    const char *disabled = std::getenv("UUSE_MINIO_META_CACHE");
    if (disabled != nullptr && std::strcmp(disabled, "0") == 0) {
        return 0;
    }
    const char *megabytes = std::getenv("UUSE_MINIO_META_CACHE_MB");
    long long value = 64;
    if (megabytes != nullptr && *megabytes != '\0') {
        char *end = nullptr;
        const long long parsed = std::strtoll(megabytes, &end, 10);
        if (end != megabytes && *end == '\0' && parsed >= 0) {
            value = parsed;
        }
    }
    return static_cast<std::size_t>(value) * 1024 * 1024;
}

class MetaCache {
public:
    explicit MetaCache(std::size_t maxBytes) : maxBytes_(maxBytes) {}

    bool enabled() const noexcept { return maxBytes_ != 0; }

    /* Serves [offset, offset+len) from the cache when an entry exists and the
     * caller's validated stat still matches it. */
    bool read(const std::string &path, const uuse::FileStat &stat, std::uint64_t offset,
              void *out, std::size_t len, long long *bytes) {
        if (maxBytes_ == 0 || len == 0 || out == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(path);
        if (found == entries_.end()) {
            g_metaCacheMisses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        Entry &entry = found->second;
        if (entry.size != stat.logicalSize || entry.mtimeNs != stat.mtimeNs ||
            entry.generation != stat.generation) {
            g_metaCacheStale.fetch_add(1, std::memory_order_relaxed);
            eraseLocked(found);
            return false;
        }
        if (offset >= entry.size) {
            g_metaCacheMisses.fetch_add(1, std::memory_order_relaxed);
            return false; /* EOF: the real read reports the short read */
        }
        const std::size_t available = static_cast<std::size_t>(entry.size - offset);
        const std::size_t take = std::min(len, available);
        std::memcpy(out, entry.data.data() + offset, take);
        touchLocked(path, entry);
        *bytes = static_cast<long long>(take);
        g_metaCacheHits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void insert(const std::string &path, const uuse::FileStat &stat, const void *data,
                std::size_t size) {
        if (maxBytes_ == 0 || size == 0 || size > kMetaCacheMaxFileBytes ||
            data == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = entries_.find(path);
        if (existing != entries_.end()) {
            eraseLocked(existing);
        }
        while (bytes_ + size > maxBytes_ && !lru_.empty()) {
            const auto victim = entries_.find(lru_.back());
            if (victim == entries_.end()) {
                lru_.pop_back();
                continue;
            }
            eraseLocked(victim);
        }
        if (bytes_ + size > maxBytes_) {
            return;
        }
        Entry entry;
        entry.data.assign(reinterpret_cast<const std::byte *>(data),
                          reinterpret_cast<const std::byte *>(data) + size);
        entry.size = stat.logicalSize;
        entry.mtimeNs = stat.mtimeNs;
        entry.generation = stat.generation;
        lru_.push_front(path);
        entry.lru = lru_.begin();
        entries_.emplace(path, std::move(entry));
        bytes_ += size;
        g_metaCacheInserts.fetch_add(1, std::memory_order_relaxed);
        g_metaCacheBytes.store(bytes_, std::memory_order_relaxed);
    }

    void invalidate(const std::string &path) {
        if (maxBytes_ == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(path);
        if (found != entries_.end()) {
            eraseLocked(found);
        }
    }

    /* Drops every entry at or below the prefix (directory rename/remove_all). */
    void invalidatePrefix(const std::string &prefix) {
        if (maxBytes_ == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) {
                it = eraseLocked(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct Entry {
        std::vector<std::byte> data;
        std::uint64_t size = 0;
        std::uint64_t mtimeNs = 0;
        std::uint32_t generation = 0;
        std::list<std::string>::iterator lru;
    };

    void touchLocked(const std::string &path, Entry &entry) {
        lru_.erase(entry.lru);
        lru_.push_front(path);
        entry.lru = lru_.begin();
    }

    /* Erases one entry and returns the next iterator. */
    std::unordered_map<std::string, Entry>::iterator eraseLocked(
        std::unordered_map<std::string, Entry>::iterator it) {
        bytes_ -= it->second.data.size();
        lru_.erase(it->second.lru);
        g_metaCacheBytes.store(bytes_, std::memory_order_relaxed);
        g_metaCacheInvalidations.fetch_add(1, std::memory_order_relaxed);
        return entries_.erase(it);
    }

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::list<std::string> lru_;
    std::size_t bytes_ = 0;
    std::size_t maxBytes_ = 0;
};

struct ShimDiag {
    bool enabled = false;
    std::array<ShimOpStats, static_cast<std::size_t>(ShimOp::Count)> ops;
    /* Size-bucketed read/write latency: <4K, <64K, <1M, >=1M. */
    std::array<std::atomic<std::uint64_t>, 4> preadCounts{};
    std::array<std::atomic<std::uint64_t>, 4> preadNs{};
    std::array<std::atomic<std::uint64_t>, 4> pwriteCounts{};
    std::array<std::atomic<std::uint64_t>, 4> pwriteNs{};

    ShimDiag() {
        const char *value = std::getenv("UUSE_MINIO_SHIM_DIAG");
        enabled = value != nullptr && *value != '\0';
    }

    static std::size_t sizeBucket(std::size_t bytes) noexcept {
        if (bytes < 4096) return 0;
        if (bytes < 65536) return 1;
        if (bytes < (1U << 20)) return 2;
        return 3;
    }

    void recordIo(bool read, std::size_t bytes, std::uint64_t elapsedNs) noexcept {
        if (!enabled) return;
        const std::size_t bucket = sizeBucket(bytes);
        auto &counts = read ? preadCounts : pwriteCounts;
        auto &totals = read ? preadNs : pwriteNs;
        counts[bucket].fetch_add(1, std::memory_order_relaxed);
        totals[bucket].fetch_add(elapsedNs, std::memory_order_relaxed);
    }

    void record(ShimOp op, std::uint64_t elapsedNs, std::uint64_t wait) noexcept {
        if (!enabled) return;
        ShimOpStats &stats = ops[static_cast<std::size_t>(op)];
        stats.count.fetch_add(1, std::memory_order_relaxed);
        stats.totalNs.fetch_add(elapsedNs, std::memory_order_relaxed);
        stats.waitNs.fetch_add(wait, std::memory_order_relaxed);
        std::uint64_t current = stats.maxNs.load(std::memory_order_relaxed);
        while (elapsedNs > current &&
               !stats.maxNs.compare_exchange_weak(current, elapsedNs,
                                                  std::memory_order_relaxed)) {
        }
        current = stats.waitMaxNs.load(std::memory_order_relaxed);
        while (wait > current &&
               !stats.waitMaxNs.compare_exchange_weak(current, wait,
                                                      std::memory_order_relaxed)) {
        }
    }

    void dump() const noexcept {
        if (!enabled) return;
        static const char *const names[] = {
            "open", "stat", "exists", "mkdir_all", "remove", "remove_all", "rename",
            "link", "dir_open", "dir_read", "pread", "pwrite", "fdatasync",
            "ftruncate", "fstat", "allocate", "diskstats", "inodestats",
        };
        for (std::size_t index = 0; index < ops.size(); ++index) {
            const ShimOpStats &stats = ops[index];
            const std::uint64_t count = stats.count.load(std::memory_order_relaxed);
            if (count == 0) continue;
            std::fprintf(stderr,
                         "uuse-shim-diag: op=%s calls=%llu total_us=%llu avg_us=%llu "
                         "max_us=%llu wait_us=%llu wait_max_us=%llu\n",
                         names[index],
                         static_cast<unsigned long long>(count),
                         static_cast<unsigned long long>(
                             stats.totalNs.load(std::memory_order_relaxed) / 1000),
                         static_cast<unsigned long long>(
                             stats.totalNs.load(std::memory_order_relaxed) / count / 1000),
                         static_cast<unsigned long long>(
                             stats.maxNs.load(std::memory_order_relaxed) / 1000),
                         static_cast<unsigned long long>(
                             stats.waitNs.load(std::memory_order_relaxed) / 1000),
                         static_cast<unsigned long long>(
                             stats.waitMaxNs.load(std::memory_order_relaxed) / 1000));
        }
    }

    /* Emits one window of counters and clears them, so periodic reports stay
     * readable when the process never closes the engine gracefully. */
    void dumpAndReset() noexcept {
        if (!enabled) return;
        const std::uint64_t hits = g_metaCacheHits.load(std::memory_order_relaxed);
        const std::uint64_t misses = g_metaCacheMisses.load(std::memory_order_relaxed);
        const std::uint64_t inserts = g_metaCacheInserts.load(std::memory_order_relaxed);
        const std::uint64_t stale = g_metaCacheStale.load(std::memory_order_relaxed);
        const std::uint64_t invalidations =
            g_metaCacheInvalidations.load(std::memory_order_relaxed);
        const std::uint64_t bytes = g_metaCacheBytes.load(std::memory_order_relaxed);
        if (hits != 0 || misses != 0 || inserts != 0 || bytes != 0) {
            std::fprintf(stderr,
                         "uuse-shim-diag: meta_cache hits=%llu misses=%llu inserts=%llu "
                         "stale=%llu evictions=%llu bytes=%llu\n",
                         static_cast<unsigned long long>(hits),
                         static_cast<unsigned long long>(misses),
                         static_cast<unsigned long long>(inserts),
                         static_cast<unsigned long long>(stale),
                         static_cast<unsigned long long>(invalidations),
                         static_cast<unsigned long long>(bytes));
            g_metaCacheHits.store(0, std::memory_order_relaxed);
            g_metaCacheMisses.store(0, std::memory_order_relaxed);
            g_metaCacheInserts.store(0, std::memory_order_relaxed);
            g_metaCacheStale.store(0, std::memory_order_relaxed);
            g_metaCacheInvalidations.store(0, std::memory_order_relaxed);
        }
        dump();
        static const char *const bucketNames[] = {"lt4k", "lt64k", "lt1m", "ge1m"};
        for (std::size_t bucket = 0; bucket < 4; ++bucket) {
            const std::uint64_t reads = preadCounts[bucket].load(std::memory_order_relaxed);
            if (reads != 0) {
                std::fprintf(stderr,
                             "uuse-shim-diag: op=pread bucket=%s calls=%llu avg_us=%llu\n",
                             bucketNames[bucket],
                             static_cast<unsigned long long>(reads),
                             static_cast<unsigned long long>(
                                 preadNs[bucket].load(std::memory_order_relaxed) / reads /
                                 1000));
            }
            const std::uint64_t writes = pwriteCounts[bucket].load(std::memory_order_relaxed);
            if (writes != 0) {
                std::fprintf(stderr,
                             "uuse-shim-diag: op=pwrite bucket=%s calls=%llu avg_us=%llu\n",
                             bucketNames[bucket],
                             static_cast<unsigned long long>(writes),
                             static_cast<unsigned long long>(
                                 pwriteNs[bucket].load(std::memory_order_relaxed) / writes /
                                 1000));
            }
        }
        for (std::size_t bucket = 0; bucket < 4; ++bucket) {
            preadCounts[bucket].store(0, std::memory_order_relaxed);
            preadNs[bucket].store(0, std::memory_order_relaxed);
            pwriteCounts[bucket].store(0, std::memory_order_relaxed);
            pwriteNs[bucket].store(0, std::memory_order_relaxed);
        }
        for (ShimOpStats &stats : ops) {
            stats.count.store(0, std::memory_order_relaxed);
            stats.totalNs.store(0, std::memory_order_relaxed);
            stats.maxNs.store(0, std::memory_order_relaxed);
            stats.waitNs.store(0, std::memory_order_relaxed);
            stats.waitMaxNs.store(0, std::memory_order_relaxed);
        }
    }
};

ShimDiag &shimDiag() {
    static ShimDiag diag;
    return diag;
}

/* Starts the periodic diag reporter once; a no-op unless diag is enabled. */
void startShimDiagReporter() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!shimDiag().enabled) return;
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                shimDiag().dumpAndReset();
            }
        }).detach();
    });
}

std::uint64_t steadyNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/* Times one C ABI entry; the destructor folds it into the diag counters. */
struct ShimOpScope {
    explicit ShimOpScope(ShimOp op) noexcept
        : op_(op), startNs_(steadyNs()) {}

    void noteWait(std::uint64_t waited) noexcept { waitNs_ = waited; }

    ~ShimOpScope() { shimDiag().record(op_, steadyNs() - startNs_, waitNs_); }

    ShimOp op_;
    std::uint64_t startNs_;
    std::uint64_t waitNs_ = 0;
};

/* Locks the shared path-op client mutex and attributes the wait to the diag. */
std::unique_lock<std::mutex> lockClientWait(std::mutex &mutex, ShimOpScope &diag) {
    const std::uint64_t waitStart = steadyNs();
    std::unique_lock<std::mutex> lock(mutex);
    diag.noteWait(steadyNs() - waitStart);
    return lock;
}

/* Maps a core status to a negative errno-style code for the C ABI. */
int toErrno(uuse::Status status) {
    switch (status) {
    case uuse::Status::Ok: return 0;
    case uuse::Status::InvalidArgument: return -EINVAL;
    case uuse::Status::NotFound: return -ENOENT;
    case uuse::Status::AlreadyExists: return -EEXIST;
    case uuse::Status::NoSpace: return -ENOSPC;
    case uuse::Status::NoMemory: return -ENOMEM;
    case uuse::Status::Busy: return -EBUSY;
    case uuse::Status::TryAgain: return -EAGAIN;
    case uuse::Status::IoError: return -EIO;
    case uuse::Status::Cancelled: return -ECANCELED;
    case uuse::Status::TimedOut: return -ETIMEDOUT;
    case uuse::Status::ForkedProcess: return -ECHILD;
    case uuse::Status::Corrupt: return -EIO;
    case uuse::Status::NotSupported: return -ENOTSUP;
    case uuse::Status::InvalidState: return -EINVAL;
    }
    return -EIO;
}

/* Unwraps a nested Result<Result<T>> produced by Task<T>::wait(). */
template <typename T>
int unwrap(uuse::Result<uuse::Result<T>> &&outer, T *out) {
    if (!outer.ok()) return toErrno(outer.status());
    auto inner = std::move(outer).value();
    if (!inner.ok()) return toErrno(inner.status());
    if (out != nullptr) *out = std::move(inner).value();
    return 0;
}

/* Void specialization. */
int unwrap(uuse::Result<uuse::Result<void>> &&outer) {
    if (!outer.ok()) return toErrno(outer.status());
    auto inner = std::move(outer).value();
    return inner.ok() ? 0 : toErrno(inner.status());
}

/* Copies a diagnostic message into the caller error buffer. */
void setError(char *errbuf, size_t errlen, const char *message) {
    if (errbuf == nullptr || errlen == 0) return;
    std::snprintf(errbuf, errlen, "%s", message != nullptr ? message : "");
}

/* Copies a std::string message into the caller error buffer. */
void setError(char *errbuf, size_t errlen, const std::string &message) {
    setError(errbuf, errlen, message.c_str());
}

uuse::OpenFlags toOpenFlags(unsigned flags) {
    uuse::OpenFlags out = uuse::OpenFlags::None;
    if (flags & UUSE_MINIO_OPEN_CREATE) out = out | uuse::OpenFlags::Create;
    if (flags & UUSE_MINIO_OPEN_EXCLUSIVE) out = out | uuse::OpenFlags::Exclusive;
    if (flags & UUSE_MINIO_OPEN_TRUNCATE) out = out | uuse::OpenFlags::Truncate;
    return out;
}

void fillStat(const uuse::FileStat &source, uuse_minio_stat *out) {
    if (out == nullptr) return;
    out->inode_id = source.inodeId;
    out->generation = source.generation;
    out->link_count = source.linkCount;
    out->logical_size = source.logicalSize;
    out->mtime_ns = source.mtimeNs;
    out->ctime_ns = source.ctimeNs;
    out->directory = source.directory ? 1 : 0;
}

} // namespace

/* Reference-counted engine state shared by the engine handle and file/dir
 * handles; the engine stops when the last reference goes away. The shared
 * client is created and closed on a dedicated frontend thread owned by this
 * state (core contract: close() must run on the client's frontend thread),
 * while path ops from callers are serialized by `mutex`. */
struct UuseMinioEngineState {
    std::shared_ptr<uuse::Engine> engine;
    std::shared_ptr<uuse::Client> client;
    std::mutex mutex; /* serializes the shared path-op client */
    std::uint64_t maxInodes = 0; /* [metadata] max_inodes snapshot */
    std::uint32_t metadataBlockSize = 0; /* [storage] metadata_block_size */
    /* Bounded metadata content cache (UUSE_MINIO_META_CACHE[_MB]). */
    MetaCache metaCache{metaCacheBudgetBytes()};

    std::thread clientThread; /* frontend thread of `client` */
    std::mutex closeMutex;
    std::condition_variable closeCv;
    bool closeRequested = false;

    /* Starts the client thread and creates the shared client on it. Returns 0
     * or a negative errno. */
    int startClient() {
        std::mutex readyMutex;
        std::condition_variable readyCv;
        bool ready = false;
        int rc = 0;
        clientThread = std::thread([this, &readyMutex, &readyCv, &ready, &rc]() {
            auto created = uuse::Client::create(*engine);
            if (created.ok()) {
                client = std::move(created).value();
            } else {
                rc = toErrno(created.status());
            }
            {
                std::lock_guard<std::mutex> lock(readyMutex);
                ready = true;
                /* Notify under the lock: the opener destroys the stack condvar
                 * right after wait() returns. */
                readyCv.notify_all();
            }
            /* Stay alive so the client can be closed on its own thread. */
            std::unique_lock<std::mutex> lock(closeMutex);
            closeCv.wait(lock, [this] { return closeRequested; });
            if (client) {
                (void)unwrap(client->close().wait());
                client.reset();
            }
        });
        std::unique_lock<std::mutex> lock(readyMutex);
        readyCv.wait(lock, [&ready] { return ready; });
        return rc;
    }

    ~UuseMinioEngineState() {
        /* Frontend attribution dump (no-op unless UUSE_MINIO_SHIM_DIAG is set). */
        shimDiag().dump();
        {
            std::lock_guard<std::mutex> lock(closeMutex);
            closeRequested = true;
        }
        closeCv.notify_all();
        if (clientThread.joinable()) {
            clientThread.join();
        }
        if (engine) {
            (void)unwrap(engine->drain().wait());
            (void)unwrap(engine->stop().wait());
            engine.reset();
        }
    }
};

namespace {

/* Process-wide singleton per .conf: several MinIO drives cannot share one
 * engine in v1 (one bdev set per engine), but the registry keeps the
 * semantics well defined if the same .conf is opened twice. Entries are weak
 * so a fully released engine can be recreated later. */
std::mutex gEngineRegistryMutex;
std::unordered_map<std::string, std::weak_ptr<UuseMinioEngineState>> gEngineRegistry;

/* Recursion depth bound for remove_all (defensive; namespace paths are
 * shallow relative to object trees but trash trees can be deeper). */
constexpr int kMaxRemoveDepth = 256;

} // namespace

struct uuse_minio_engine {
    std::shared_ptr<UuseMinioEngineState> state;
};

struct uuse_minio_file {
    std::shared_ptr<UuseMinioEngineState> state;
    std::shared_ptr<uuse::File> file;
    std::string path; /* namespace path, used by the metadata cache */
};

struct uuse_minio_dir {
    std::shared_ptr<UuseMinioEngineState> state;
    std::string path;
    std::string cursor;
    bool done = false;
};

namespace {

/* Recursive delete helper; the caller must hold the state mutex. Returns 0 or
 * a negative errno. Files and empty directories are removed leaf-first. */
int removeRecursiveLocked(const std::shared_ptr<UuseMinioEngineState> &state,
                          const std::string &path, int depth) {
    if (depth > kMaxRemoveDepth) return -ELOOP;

    auto statOuter = state->client->stat(path).wait();
    if (!statOuter.ok()) return toErrno(statOuter.status());
    auto statInner = std::move(statOuter).value();
    if (!statInner.ok()) return toErrno(statInner.status());
    auto stat = std::move(statInner).value();

    if (!stat.directory) {
        return unwrap(state->client->remove(path).wait());
    }

    std::string cursor;
    std::vector<std::string> children;
    for (;;) {
        auto outer = state->client->getChildren(path, cursor, 256).wait();
        if (!outer.ok()) return toErrno(outer.status());
        auto inner = std::move(outer).value();
        if (!inner.ok()) return toErrno(inner.status());
        auto page = std::move(inner).value();
        children.clear();
        for (const auto &entry : page.entries) {
            children.push_back(entry.name);
        }
        for (const auto &name : children) {
            const std::string child = path == "/" ? "/" + name : path + "/" + name;
            const int rc = removeRecursiveLocked(state, child, depth + 1);
            if (rc != 0) return rc;
        }
        if (page.nextCursor.empty()) break;
        cursor = page.nextCursor;
    }
    return unwrap(state->client->remove(path).wait());
}

} // namespace

extern "C" {

int uuse_minio_engine_open(const char *confPath, uuse_minio_engine **out,
                           char *errbuf, size_t errlen) {
    if (confPath == nullptr || out == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    const std::string key(confPath);
    std::shared_ptr<UuseMinioEngineState> state;
    {
        std::lock_guard<std::mutex> lock(gEngineRegistryMutex);
        auto it = gEngineRegistry.find(key);
        if (it != gEngineRegistry.end()) state = it->second.lock();
        if (state == nullptr) {
            auto config = uuse::parseConfigFile(confPath);
            if (!config.ok()) {
                setError(errbuf, errlen, config.message());
                return toErrno(config.status());
            }
            auto engine = uuse::Engine::create(config.value());
            if (!engine.ok()) {
                setError(errbuf, errlen, engine.message());
                return toErrno(engine.status());
            }
            const int mountRc = unwrap(engine.value()->mount().wait());
            if (mountRc != 0) {
                setError(errbuf, errlen, "engine mount failed");
                return mountRc;
            }
            state = std::make_shared<UuseMinioEngineState>();
            state->engine = std::shared_ptr<uuse::Engine>(std::move(engine).value());
            state->maxInodes = config.value().maxInodes;
            state->metadataBlockSize = config.value().metadataBlockSize;
            const int clientRc = state->startClient();
            if (clientRc != 0) {
                setError(errbuf, errlen, "client creation failed");
                return clientRc;
            }
            gEngineRegistry[key] = state;
        }
    }
    auto *handle = new uuse_minio_engine();
    handle->state = std::move(state);
    *out = handle;
    startShimDiagReporter();
    return 0;
}

void uuse_minio_engine_close(uuse_minio_engine *engine) {
    if (engine == nullptr) return;
    /* Release the caller reference; the engine state stays alive while open
     * file handles hold a reference and stops with the last reference. */
    engine->state.reset();
    delete engine;
}

int uuse_minio_disk_stats(uuse_minio_engine *engine, uint64_t *totalBytes,
                          uint64_t *freeBytes, uint64_t *usedBytes,
                          char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    ShimOpScope diag(ShimOp::DiskStats);
    auto state = engine->state;
    auto lock = lockClientWait(state->mutex, diag);
    /* dataBlocks is in metadata-block units; size the total from the same unit
     * so it is comparable with freeDataBytes() (allocatable extents bytes). */
    if (totalBytes != nullptr) {
        std::uint64_t total = 0;
        for (const auto &geometry : state->engine->deviceGeometries()) {
            total += geometry.dataBlocks * state->metadataBlockSize;
        }
        *totalBytes = total;
    }
    if (freeBytes != nullptr) {
        *freeBytes = state->engine->freeDataBytes();
    }
    if (usedBytes != nullptr) {
        const std::uint64_t total =
            totalBytes != nullptr ? *totalBytes : state->engine->deviceCapacityBytes(0);
        const std::uint64_t free = state->engine->freeDataBytes();
        *usedBytes = total > free ? total - free : 0;
    }
    return 0;
}

int uuse_minio_inode_stats(uuse_minio_engine *engine, uint64_t *usedInodes,
                           uint64_t *maxInodes, char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    auto state = engine->state;
    ShimOpScope diag(ShimOp::InodeStats);
    auto lock = lockClientWait(state->mutex, diag);
    auto stats = state->engine->dataExtentStats();
    if (!stats.ok()) {
        setError(errbuf, errlen, stats.message());
        return toErrno(stats.status());
    }
    if (usedInodes != nullptr) *usedInodes = stats.value().inodes;
    if (maxInodes != nullptr) *maxInodes = state->maxInodes;
    return 0;
}

int uuse_minio_open(uuse_minio_engine *engine, const char *path, unsigned flags,
                    uuse_minio_file **out, char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr ||
        out == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    auto state = engine->state;
    ShimOpScope diag(ShimOp::Open);
    std::shared_ptr<uuse::File> file;
    {
        auto lock = lockClientWait(state->mutex, diag);
        const int rc = unwrap(state->client->open(path, toOpenFlags(flags)).wait(), &file);
        if (rc != 0) {
            setError(errbuf, errlen, "open failed");
            return rc;
        }
    }
    /* A create/truncate may replace the file content; any write through the
     * returned handle invalidates the cache entry again on completion. */
    if ((flags & (UUSE_MINIO_OPEN_CREATE | UUSE_MINIO_OPEN_TRUNCATE)) != 0) {
        state->metaCache.invalidate(path);
    }
    auto *handle = new uuse_minio_file();
    handle->state = std::move(state);
    handle->file = std::move(file);
    handle->path = path;
    *out = handle;
    return 0;
}

void uuse_minio_close(uuse_minio_file *file) {
    if (file == nullptr) return;
    file->file.reset();
    file->state.reset();
    delete file;
}

long long uuse_minio_pread(uuse_minio_file *file, void *buf, size_t len,
                           uint64_t offset) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    if (len == 0) return 0;
    ShimOpScope diag(ShimOp::Pread);
    auto *bytes = reinterpret_cast<std::byte *>(buf);
    if (bytes == nullptr) return -EINVAL;
    /* Metadata fast path: small reads are served from the bounded shim cache
     * when the handle's (size, mtime, generation) still match. Payload reads
     * (>= kMetaCacheMaxFileBytes) always go to the device. */
    uuse::FileStat stat{};
    bool haveStat = false;
    if (len <= kMetaCacheMaxFileBytes && file->state->metaCache.enabled()) {
        auto current = file->file->stat();
        if (current.ok()) {
            stat = current.value();
            haveStat = true;
            long long served = 0;
            if (file->state->metaCache.read(file->path, stat, offset, bytes, len,
                                            &served)) {
                shimDiag().recordIo(true, len, steadyNs() - diag.startNs_);
                return served;
            }
        }
    }
    uuse::IoVec vec{bytes, len};
    auto outer = file->file->read(std::span<uuse::IoVec>(&vec, 1), offset).wait();
    uuse::ReadResult value{};
    const int rc = unwrap(std::move(outer), &value);
    shimDiag().recordIo(true, len, steadyNs() - diag.startNs_);
    if (rc == 0 && haveStat && offset == 0 && value.bytes > 0 &&
        value.bytes <= kMetaCacheMaxFileBytes &&
        stat.logicalSize == static_cast<std::uint64_t>(value.bytes)) {
        /* A full-file read: populate the cache for the next access. */
        file->state->metaCache.insert(file->path, stat, bytes,
                                      static_cast<std::size_t>(value.bytes));
    }
    return rc != 0 ? rc : static_cast<long long>(value.bytes);
}

long long uuse_minio_pwrite(uuse_minio_file *file, const void *buf, size_t len,
                            uint64_t offset) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    if (len == 0) return 0;
    ShimOpScope diag(ShimOp::Pwrite);
    auto *bytes = reinterpret_cast<std::byte *>(const_cast<void *>(buf));
    if (bytes == nullptr) return -EINVAL;
    uuse::IoVec vec{bytes, len};
    auto outer = file->file->write(std::span<const uuse::IoVec>(&vec, 1), offset).wait();
    uuse::WriteResult value{};
    const int rc = unwrap(std::move(outer), &value);
    shimDiag().recordIo(false, len, steadyNs() - diag.startNs_);
    if (rc == 0) {
        /* The path content changed; cached metadata must not survive. */
        file->state->metaCache.invalidate(file->path);
    }
    return rc != 0 ? rc : static_cast<long long>(value.bytes);
}

int uuse_minio_fdatasync(uuse_minio_file *file) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    ShimOpScope diag(ShimOp::Fdatasync);
    return unwrap(file->file->sync().wait());
}

int uuse_minio_ftruncate(uuse_minio_file *file, uint64_t size) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    ShimOpScope diag(ShimOp::Ftruncate);
    const int rc = unwrap(file->file->truncate(size).wait());
    if (rc == 0) {
        file->state->metaCache.invalidate(file->path);
    }
    return rc;
}

int uuse_minio_fstat(uuse_minio_file *file, uuse_minio_stat *out) {
    if (file == nullptr || file->file == nullptr || out == nullptr) return -EINVAL;
    ShimOpScope diag(ShimOp::Fstat);
    auto stat = file->file->stat();
    if (!stat.ok()) return toErrno(stat.status());
    fillStat(stat.value(), out);
    return 0;
}

int uuse_minio_allocate(uuse_minio_file *file, uint64_t offset, uint64_t length) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    ShimOpScope diag(ShimOp::Allocate);
    const int rc = unwrap(file->file->allocate(offset, length).wait());
    if (rc == 0) {
        file->state->metaCache.invalidate(file->path);
    }
    return rc;
}

int uuse_minio_path_stat(uuse_minio_engine *engine, const char *path,
                         uuse_minio_stat *out, char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    auto state = engine->state;
    ShimOpScope diag(ShimOp::Stat);
    uuse::FileStat value;
    int rc = 0;
    {
        auto lock = lockClientWait(state->mutex, diag);
        rc = unwrap(state->client->stat(path).wait(), &value);
    }
    if (rc != 0) {
        if (rc != -ENOENT) setError(errbuf, errlen, "stat failed");
        return rc;
    }
    fillStat(value, out);
    return 0;
}

int uuse_minio_exists(uuse_minio_engine *engine, const char *path, int *exists,
                      char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr ||
        exists == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    auto state = engine->state;
    ShimOpScope diag(ShimOp::Exists);
    auto outer = [&] {
        auto lock = lockClientWait(state->mutex, diag);
        return state->client->stat(path).wait();
    }();
    if (!outer.ok()) return toErrno(outer.status());
    auto inner = std::move(outer).value();
    if (inner.ok()) {
        *exists = 1;
        return 0;
    }
    if (inner.status() == uuse::Status::NotFound) {
        *exists = 0;
        return 0;
    }
    return toErrno(inner.status());
}

int uuse_minio_mkdir_all(uuse_minio_engine *engine, const char *path,
                         char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    auto state = engine->state;
    ShimOpScope diag(ShimOp::MkdirAll);
    auto lock = lockClientWait(state->mutex, diag);
    const int rc = unwrap(state->client->createDirectories(path).wait());
    if (rc != 0) setError(errbuf, errlen, "mkdir failed");
    return rc;
}

int uuse_minio_remove(uuse_minio_engine *engine, const char *path,
                      uuse_minio_stat *removed, char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    auto state = engine->state;
    ShimOpScope diag(ShimOp::Remove);
    auto lock = lockClientWait(state->mutex, diag);
    state->metaCache.invalidate(path);
    if (removed != nullptr) {
        uuse::FileStat value;
        const int statRc = unwrap(state->client->stat(path).wait(), &value);
        if (statRc == 0) fillStat(value, removed);
    }
    const int rc = unwrap(state->client->remove(path).wait());
    if (rc != 0) setError(errbuf, errlen, "remove failed");
    return rc;
}

int uuse_minio_remove_all(uuse_minio_engine *engine, const char *path,
                          char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    auto state = engine->state;
    ShimOpScope diag(ShimOp::RemoveAll);
    auto lock = lockClientWait(state->mutex, diag);
    state->metaCache.invalidatePrefix(path);
    const int rc = removeRecursiveLocked(state, path, 0);
    if (rc != 0) setError(errbuf, errlen, "remove_all failed");
    return rc;
}

int uuse_minio_rename(uuse_minio_engine *engine, const char *from,
                      const char *to, char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr || from == nullptr ||
        to == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    auto state = engine->state;
    ShimOpScope diag(ShimOp::Rename);
    auto lock = lockClientWait(state->mutex, diag);
    state->metaCache.invalidatePrefix(from);
    state->metaCache.invalidatePrefix(to);
    const int rc = unwrap(state->client->rename(from, to).wait());
    if (rc != 0) setError(errbuf, errlen, "rename failed");
    return rc;
}

int uuse_minio_link(uuse_minio_engine *engine, const char *from, const char *to,
                    char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr || from == nullptr ||
        to == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    auto state = engine->state;
    ShimOpScope diag(ShimOp::Link);
    auto lock = lockClientWait(state->mutex, diag);
    const int rc = unwrap(state->client->link(from, to).wait());
    if (rc != 0) setError(errbuf, errlen, "link failed");
    return rc;
}

int uuse_minio_dir_open(uuse_minio_engine *engine, const char *path,
                        uuse_minio_dir **out, char *errbuf, size_t errlen) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr ||
        out == nullptr) {
        setError(errbuf, errlen, "invalid argument");
        return -EINVAL;
    }
    auto state = engine->state;
    {
        ShimOpScope diag(ShimOp::DirOpen);
        auto lock = lockClientWait(state->mutex, diag);
        uuse::FileStat value;
        const int rc = unwrap(state->client->stat(path).wait(), &value);
        if (rc != 0) {
            if (rc != -ENOENT) setError(errbuf, errlen, "dir_open stat failed");
            return rc;
        }
        if (!value.directory) {
            setError(errbuf, errlen, "not a directory");
            return -ENOTDIR;
        }
    }
    auto *dir = new uuse_minio_dir();
    dir->state = std::move(state);
    dir->path = path;
    *out = dir;
    return 0;
}

int uuse_minio_dir_read(uuse_minio_dir *dir, uuse_minio_dirent *entries,
                        size_t max, size_t *count, int *eof) {
    if (dir == nullptr || dir->state == nullptr || entries == nullptr ||
        count == nullptr || eof == nullptr) {
        return -EINVAL;
    }
    *count = 0;
    *eof = dir->done ? 1 : 0;
    if (dir->done || max == 0) return 0;

    auto state = dir->state;
    ShimOpScope diag(ShimOp::DirRead);
    auto lock = lockClientWait(state->mutex, diag);
    auto outer = state->client->getChildren(dir->path, dir->cursor, max).wait();
    if (!outer.ok()) return toErrno(outer.status());
    auto inner = std::move(outer).value();
    if (!inner.ok()) return toErrno(inner.status());
    auto page = std::move(inner).value();

    size_t written = 0;
    for (const auto &entry : page.entries) {
        if (written >= max) break;
        uuse_minio_dirent &out = entries[written];
        std::memset(&out, 0, sizeof(out));
        out.inode_id = entry.inodeId;
        out.generation = entry.generation;
        out.flags = entry.directory ? 1u : 0u;
        const std::size_t length =
            entry.name.size() < sizeof(out.name) ? entry.name.size() : sizeof(out.name) - 1;
        std::memcpy(out.name, entry.name.data(), length);
        out.name[length] = '\0';
        ++written;
    }
    *count = written;
    dir->cursor = page.nextCursor;
    dir->done = page.nextCursor.empty();
    *eof = dir->done ? 1 : 0;
    return 0;
}

void uuse_minio_dir_close(uuse_minio_dir *dir) {
    if (dir == nullptr) return;
    dir->state.reset();
    delete dir;
}

void uuse_minio_version(char *buf, size_t len) {
    if (buf == nullptr || len == 0) return;
    std::snprintf(buf, len, "uuse-minio-shim/0.1 core-abi-%u",
                  static_cast<unsigned>(uuse::coreAbiMajor()));
}

} // extern "C"
