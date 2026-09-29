#include "hfs/core.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "common/log.hpp"
#include "common/trace.hpp"
#include "core/async_completion_queue.hpp"
#include "core/async_data_service.hpp"
#include "core/data_plane.hpp"
#include "core/request_lookup.hpp"
#include "hfs/diagnostics.hpp"
#include "metadata/inode_map.hpp"
#include "metadata/namespace.hpp"

namespace hfs {

/* Counts as a metadata user for as long as a client (or any of its open
 * files) is alive, so Engine::stop() cannot tear down the namespace below
 * live handles (hfs2.0.md §14.3). */
struct MetadataLease {
    Engine *engine = nullptr;

    MetadataLease() = default;
    explicit MetadataLease(Engine &value) : engine(&value) { engine->retainMetadataUser(); }
    ~MetadataLease() {
        if (engine != nullptr) {
            engine->releaseMetadataUser();
        }
    }
    MetadataLease(const MetadataLease &) = delete;
    MetadataLease &operator=(const MetadataLease &) = delete;
};

namespace {

enum class ClientLifecycle : std::uint8_t {
    Open,
    Closing,
    Closed,
};

/* One immutable file view can be shared by every in-flight fixed I/O. */
struct AsyncFileSnapshot {
    FileStat stat{};
    metadata::InodeExtentMap extentMap;
    /* Per-inode data-map version; only this file's own size/extent changes
     * invalidate the snapshot (namespace-wide changes do not). */
    std::uint64_t dataVersion = 0;
};

/* One bounded frontend slot keeps the immutable extent snapshot alive until
 * its completion event has been reaped by the creating thread. */
struct AsyncRequestSlot {
    bool inUse = false;
    std::uint64_t requestId = 0;
    CompletionToken token{};
    FileHandle file{};
    std::shared_ptr<const AsyncFileSnapshot> snapshot;

    void release() noexcept {
        inUse = false;
        requestId = 0;
        token = {};
        file = {};
        snapshot.reset();
    }
};

/* Shared client state so open File handles keep working after Client::close
 * (the engine must still outlive them). */
struct ClientState final : AsyncDataSink {
    Engine *engine = nullptr;
    metadata::Namespace *namespaceState = nullptr;
    DataPlane *dataPlane = nullptr;
    AsyncDataService *asyncService = nullptr;
    ClientOptions options;
    std::shared_ptr<MetadataLease> lease;
    std::atomic<std::uint32_t> openFiles{0};
    std::atomic<std::size_t> outstandingCount{0};
    std::atomic<std::size_t> handleCount{0};
    std::atomic<ClientLifecycle> lifecycle{ClientLifecycle::Open};
    std::atomic<bool> completionOverflow{false};
    std::thread::id frontendThread{};
    std::uint32_t maxOutstandingValue = 0;
    std::unique_ptr<AsyncRequestSlot[]> requestSlots;
    std::unique_ptr<std::uint32_t[]> freeSlotStack;
    std::size_t freeSlotCount = 0;
    RequestLookupTable requestLookup;
    AsyncCompletionQueue completionQueue;
    bool asyncInitialized = false;

    /* Allocates every frontend-owned bounded slot before the first submit. */
    bool initializeAsync(std::size_t shardCount) noexcept {
        if (options.maxOutstanding == 0 || maxOutstandingValue != 0 ||
            asyncInitialized) {
            return false;
        }
        /* An external wait strategy is only valid together with its wake hook;
         * otherwise every blocking poll would silently run to full timeout. */
        if (options.completionWait.valid() && options.completionWake == nullptr) {
            log::error("client options require completionWake with an external wait strategy");
            return false;
        }
        maxOutstandingValue = options.maxOutstanding;
        requestSlots.reset(new (std::nothrow) AsyncRequestSlot[maxOutstandingValue]);
        if (requestSlots == nullptr) {
            maxOutstandingValue = 0;
            return false;
        }
        freeSlotStack.reset(new (std::nothrow) std::uint32_t[maxOutstandingValue]);
        if (freeSlotStack == nullptr) {
            requestSlots.reset();
            maxOutstandingValue = 0;
            return false;
        }
        if (!requestLookup.initialize(maxOutstandingValue)) {
            freeSlotStack.reset();
            requestSlots.reset();
            maxOutstandingValue = 0;
            return false;
        }
        for (std::size_t index = 0; index < maxOutstandingValue; ++index) {
            freeSlotStack[index] = static_cast<std::uint32_t>(index);
        }
        freeSlotCount = maxOutstandingValue;
        const std::size_t effectiveShardCount = shardCount == 0 ? 1 : shardCount;
        if (!completionQueue.initialize(effectiveShardCount, maxOutstandingValue,
                                        options.completionWake == nullptr)) {
            requestLookup.reset();
            freeSlotStack.reset();
            freeSlotCount = 0;
            requestSlots.reset();
            maxOutstandingValue = 0;
            return false;
        }
        frontendThread = std::this_thread::get_id();
        asyncInitialized = true;
        return true;
    }

    bool onFrontendThread() const noexcept {
        return std::this_thread::get_id() == frontendThread;
    }

    /* The service owns credit acquisition; this is its bounded sink hook. */
    bool tryAcquireCredit() noexcept override {
        if (lifecycle.load(std::memory_order_acquire) != ClientLifecycle::Open ||
            maxOutstandingValue == 0) {
            return false;
        }
        std::size_t current = outstandingCount.load(std::memory_order_relaxed);
        while (current < maxOutstandingValue) {
            if (outstandingCount.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void releaseCredit() noexcept override {
        std::size_t current = outstandingCount.load(std::memory_order_relaxed);
        while (current != 0) {
            if (outstandingCount.compare_exchange_weak(
                    current, current - 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return;
            }
        }
    }

    /* Reactor callbacks only enqueue a value; they never touch frontend slots.
     * Returns false on overflow so the core never claims a published closure. */
    bool publish(std::uint32_t shard, const CompletionEvent &event) noexcept override {
        const auto result = completionQueue.pushWithWakeHint(shard, event);
        if (!result.accepted) {
            completionOverflow.store(true, std::memory_order_release);
            return false;
        }
        if (result.notify && options.completionWake != nullptr) {
            options.completionWake(options.completionWakeContext);
        }
        return true;
    }

    /* One ring publish and one wake callback for the whole batch. */
    bool publishBatch(std::uint32_t shard, const CompletionEvent *events,
                      std::size_t count) noexcept override {
        if (count == 0 || events == nullptr) {
            return true;
        }
        const auto result = completionQueue.pushBatch(shard, events, count);
        if (!result.accepted) {
            /* Ring lacked capacity: preserve the per-event overflow semantics. */
            for (std::size_t index = 0; index < count; ++index) {
                if (!publish(shard, events[index])) {
                    return false;
                }
            }
            return true;
        }
        if (result.notify && options.completionWake != nullptr) {
            options.completionWake(options.completionWakeContext);
        }
        return true;
    }

    void releaseSlot(std::size_t index) noexcept {
        if (index >= maxOutstandingValue || requestSlots == nullptr ||
            freeSlotStack == nullptr) {
            return;
        }
        requestSlots[index].release();
        if (freeSlotCount < maxOutstandingValue) {
            freeSlotStack[freeSlotCount++] = static_cast<std::uint32_t>(index);
        }
    }

    bool indexRequest(std::uint64_t requestId, std::size_t slotIndex) noexcept {
        return requestId != 0 && slotIndex < maxOutstandingValue &&
               requestLookup.insert(requestId, static_cast<std::uint32_t>(slotIndex));
    }

    AsyncRequestSlot *findRequest(std::uint64_t requestId, std::size_t &index) noexcept {
        std::uint32_t mappedIndex = 0;
        if (requestLookup.find(requestId, mappedIndex)) {
            index = mappedIndex;
            if (index < maxOutstandingValue && requestSlots[index].inUse &&
                requestSlots[index].requestId == requestId) {
                return &requestSlots[index];
            }
        }
        /* Keep an integrity-preserving fallback for an impossible full-table
         * insertion or a corrupted lookup entry; normal completions are O(1). */
        for (std::size_t candidate = 0; candidate < maxOutstandingValue; ++candidate) {
            AsyncRequestSlot &slot = requestSlots[candidate];
            if (slot.inUse && slot.requestId == requestId) {
                index = candidate;
                return &slot;
            }
        }
        return nullptr;
    }

    AsyncRequestSlot *reserveSlot(const CompletionToken &token,
                                  std::size_t &index) noexcept {
        if (!asyncInitialized || token.generation == 0 || requestSlots == nullptr) {
            return nullptr;
        }
        if (freeSlotCount == 0 || freeSlotStack == nullptr) {
            return nullptr;
        }
        index = freeSlotStack[--freeSlotCount];
        AsyncRequestSlot &slot = requestSlots[index];
        if (slot.inUse) {
            freeSlotStack[freeSlotCount++] = static_cast<std::uint32_t>(index);
            return nullptr;
        }
        slot.inUse = true;
        slot.requestId = 0;
        slot.token = token;
        slot.file = {};
        return &slot;
    }

    bool reapEvent(const CompletionEvent &event) noexcept {
        std::size_t index = 0;
        AsyncRequestSlot *slot = findRequest(event.requestId, index);
        if (slot == nullptr || slot->token.slot != event.token.slot ||
            slot->token.generation != event.token.generation) {
            completionOverflow.store(true, std::memory_order_release);
            return false;
        }

        const bool erased = requestLookup.erase(event.requestId);
        releaseSlot(index);
        releaseCredit();
        if (!erased) {
            completionOverflow.store(true, std::memory_order_release);
        }
        return erased;
    }
};

/* IoHandle invokes this exactly once when its frontend reference is released. */
void releaseClientHandle(void *context) noexcept {
    auto *state = static_cast<ClientState *>(context);
    if (state == nullptr) {
        return;
    }
    std::size_t current = state->handleCount.load(std::memory_order_relaxed);
    while (current != 0 &&
           !state->handleCount.compare_exchange_weak(
               current, current - 1, std::memory_order_acq_rel,
               std::memory_order_relaxed)) {
    }
}

/* Rebuilds the per-inode extent map view for one File handle. */
Result<metadata::InodeExtentMap> extentMapFor(const ClientState &state, FileHandle handle) {
    auto descriptors = state.namespaceState->extents(handle);
    if (!descriptors.ok()) {
        return makeError<metadata::InodeExtentMap>(descriptors.status(),
                                                   descriptors.message());
    }
    metadata::InodeExtentMap map;
    map.configure(state.namespaceState->extentBytes());
    auto restored = map.restore(descriptors.value());
    if (!restored.ok()) {
        return makeError<metadata::InodeExtentMap>(restored.status(), restored.message());
    }
    return map;
}

Result<void> validateClient(const ClientState &state) {
    if (state.engine == nullptr || !state.engine->processValid()) {
        return makeError(Status::ForkedProcess, "engine belongs to another process");
    }
    if (state.lifecycle.load(std::memory_order_acquire) != ClientLifecycle::Open) {
        return makeError(Status::InvalidState, "client is closing or closed");
    }
    if (!state.engine->mounted() || state.namespaceState == nullptr) {
        return makeError(Status::InvalidState, "engine is not mounted");
    }
    return Result<void>(Status::Ok);
}

/* Counts queued values without consuming them; this is used by close(). */
std::size_t queuedCompletions(const ClientState &state) noexcept {
    if (!state.asyncInitialized) {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t shard = 0; shard < state.completionQueue.shardCount(); ++shard) {
        count += state.completionQueue.shardDiagnostics(shard).size;
    }
    return count;
}

Result<void> validateAsyncClient(const ClientState &state) {
    auto valid = validateClient(state);
    if (!valid.ok()) {
        return valid;
    }
    if (!state.onFrontendThread()) {
        return makeError(Status::InvalidState,
                         "asynchronous Client API called from a non-frontend thread");
    }
    if (!state.asyncInitialized || state.asyncService == nullptr ||
        !state.asyncService->valid()) {
        return makeError(Status::InvalidState, "asynchronous data service is unavailable");
    }
    if (state.completionOverflow.load(std::memory_order_acquire)) {
        return makeError(Status::Corrupt, "completion queue integrity failure");
    }
    return Result<void>(Status::Ok);
}

/* Builds one fixed request against a slot-owned extent snapshot. The owner
 * reference keeps the completion sink alive until the terminal publish. */
Result<IoHandle> submitAsyncRequest(const std::shared_ptr<ClientState> &stateOwner,
                                    FileHandle handle,
                                    std::shared_ptr<const AsyncFileSnapshot> snapshot,
                                    IoVec buffer,
                                    std::uint64_t offset, CompletionToken token,
                                    bool write) {
    ClientState &state = *stateOwner;
    if (snapshot == nullptr) {
        return makeError<IoHandle>(Status::InvalidState,
                                   "asynchronous file snapshot is unavailable");
    }
    const FileStat &stat = snapshot->stat;
    if (!token.valid()) {
        return makeError<IoHandle>(Status::InvalidArgument,
                                   "completion token generation is zero");
    }
    if (!handle.valid()) {
        return makeError<IoHandle>(Status::InvalidArgument, "invalid file handle");
    }
    if (buffer.size == 0 || buffer.data == nullptr) {
        return makeError<IoHandle>(Status::InvalidArgument,
                                   "asynchronous I/O requires one non-empty buffer");
    }

    std::uint64_t length = static_cast<std::uint64_t>(buffer.size);
    if (write) {
        if (offset > std::numeric_limits<std::uint64_t>::max() - length) {
            return makeError<IoHandle>(Status::InvalidArgument,
                                       "write range overflows");
        }
        if (offset > stat.logicalSize || offset + length > stat.logicalSize) {
            return makeError<IoHandle>(
                Status::InvalidArgument,
                "asynchronous write must stay within the preallocated logical size");
        }
    } else {
        if (offset >= stat.logicalSize) {
            return makeError<IoHandle>(Status::InvalidArgument,
                                       "read starts at or beyond logical EOF");
        }
        length = std::min<std::uint64_t>(length, stat.logicalSize - offset);
    }

    if (state.outstandingCount.load(std::memory_order_acquire) >=
        state.maxOutstandingValue) {
        return makeError<IoHandle>(Status::Busy, "client outstanding credit exhausted");
    }

    std::size_t slotIndex = 0;
    AsyncRequestSlot *slot = state.reserveSlot(token, slotIndex);
    if (slot == nullptr) {
        return makeError<IoHandle>(Status::Busy, "client frontend slot table is full");
    }

    slot->file = handle;
    slot->snapshot = std::move(snapshot);

    AsyncDataRequest request;
    request.fileId = handle.inodeId;
    request.offset = offset;
    request.length = length;
    request.buffer = std::span<std::byte>(buffer.data, static_cast<std::size_t>(length));
    request.extentMap = &slot->snapshot->extentMap;
    request.logicalSize = stat.logicalSize;
    request.write = write;
    request.fixedOverwrite = write;

    auto submitted = state.asyncService->submit(request, token, state, stateOwner);
    if (!submitted.ok()) {
        state.releaseSlot(slotIndex);
        return makeError<IoHandle>(submitted.status(), submitted.message());
    }
    IoHandle handleResult = std::move(submitted).value();
    slot->requestId = handleResult.requestId();
    if (!state.indexRequest(slot->requestId, slotIndex)) {
        state.completionOverflow.store(true, std::memory_order_release);
    }
    return Result<IoHandle>(std::move(handleResult));
}

} // namespace

struct Client::Impl {
    std::shared_ptr<ClientState> state;
};

struct File::Impl {
    std::shared_ptr<ClientState> state;
    FileHandle handle;
    mutable std::mutex asyncCacheMutex;
    mutable std::shared_ptr<const AsyncFileSnapshot> asyncCache;

    /* Returns the cached snapshot when this inode's data version is current.
     * Records the hit counters; returns null on a miss or unavailable state. */
    std::shared_ptr<const AsyncFileSnapshot> cachedSnapshot() const {
        if (state == nullptr || state->namespaceState == nullptr) {
            return nullptr;
        }
        const std::uint64_t currentVersion =
            state->namespaceState->dataVersionFast(handle);
        std::lock_guard<std::mutex> lock(asyncCacheMutex);
        if (asyncCache != nullptr && asyncCache->dataVersion == currentVersion) {
            diagnostics::recordSnapshotHit();
            trace::event(trace::EventId::SnapHit, handle.inodeId, currentVersion);
            return asyncCache;
        }
        return nullptr;
    }

    /* Refresh metadata only when this inode's data-map version changes. */
    Result<std::shared_ptr<const AsyncFileSnapshot>> asyncSnapshot() const {
        if (state == nullptr || state->namespaceState == nullptr) {
            return makeError<std::shared_ptr<const AsyncFileSnapshot>>(
                Status::InvalidState, "file namespace is unavailable");
        }
        if (auto cached = cachedSnapshot(); cached != nullptr) {
            return cached;
        }

        /* A retry closes the stat/extents race with a concurrent metadata
         * mutation without holding the namespace mutex across allocations. */
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const std::uint64_t before = state->namespaceState->dataVersionFast(handle);
            auto stat = state->namespaceState->stat(handle);
            if (!stat.ok()) {
                return makeError<std::shared_ptr<const AsyncFileSnapshot>>(
                    stat.status(), stat.message());
            }
            auto map = extentMapFor(*state, handle);
            if (!map.ok()) {
                return makeError<std::shared_ptr<const AsyncFileSnapshot>>(
                    map.status(), map.message());
            }
            const std::uint64_t after = state->namespaceState->dataVersionFast(handle);
            if (before != after) {
                continue;
            }
            auto refreshed = std::make_shared<AsyncFileSnapshot>();
            refreshed->stat = stat.value();
            refreshed->extentMap = std::move(map).value();
            refreshed->dataVersion = after;
            std::lock_guard<std::mutex> lock(asyncCacheMutex);
            if (state->namespaceState->dataVersionFast(handle) != after) {
                continue;
            }
            asyncCache = std::move(refreshed);
            diagnostics::recordSnapshotBuild();
            trace::event(trace::EventId::SnapBuild, handle.inodeId, after,
                         static_cast<std::uint64_t>(asyncCache->extentMap.size()));
            return asyncCache;
        }
        return makeError<std::shared_ptr<const AsyncFileSnapshot>>(
            Status::Busy, "namespace changed while building asynchronous snapshot");
    }

    /* Builds one snapshot directly from the namespace without touching the
     * cache; used when concurrent churn outruns the retry loop above. */
    Result<std::shared_ptr<const AsyncFileSnapshot>> buildSnapshot() const {
        if (state == nullptr || state->namespaceState == nullptr) {
            return makeError<std::shared_ptr<const AsyncFileSnapshot>>(
                Status::InvalidState, "file namespace is unavailable");
        }
        auto stat = state->namespaceState->stat(handle);
        if (!stat.ok()) {
            return makeError<std::shared_ptr<const AsyncFileSnapshot>>(
                stat.status(), stat.message());
        }
        auto map = extentMapFor(*state, handle);
        if (!map.ok()) {
            return makeError<std::shared_ptr<const AsyncFileSnapshot>>(
                map.status(), map.message());
        }
        auto built = std::make_shared<AsyncFileSnapshot>();
        built->stat = stat.value();
        built->extentMap = std::move(map).value();
        built->dataVersion = state->namespaceState->dataVersionFast(handle);
        return std::shared_ptr<const AsyncFileSnapshot>(std::move(built));
    }

    /* Snapshot accessor for the synchronous paths: cached while the per-inode
     * data version is current, with a direct build fallback so they keep the
     * always-succeed behavior of the previous stat + extentMapFor path. */
    Result<std::shared_ptr<const AsyncFileSnapshot>> syncSnapshot() const {
        auto snapshot = asyncSnapshot();
        if (snapshot.ok()) {
            return snapshot;
        }
        return buildSnapshot();
    }

    ~Impl() {
        if (state != nullptr) {
            if (state->namespaceState != nullptr && handle.valid()) {
                state->namespaceState->close(handle);
            }
            state->openFiles.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
};

Client::Client(Engine &engine, const ClientOptions &options) : impl_(std::make_unique<Impl>()) {
    impl_->state = std::make_shared<ClientState>();
    impl_->state->engine = &engine;
    impl_->state->namespaceState = engine.metadataNamespace();
    impl_->state->dataPlane = engine.dataPlane();
    impl_->state->asyncService = engine.asyncDataService();
    impl_->state->options = options;
    impl_->state->lease = std::make_shared<MetadataLease>(engine);
    (void)impl_->state->initializeAsync(engine.dataReactorCount());
}

Client::~Client() {
    if (impl_ == nullptr || impl_->state == nullptr) {
        return;
    }
    const auto lifecycle = impl_->state->lifecycle.load(std::memory_order_acquire);
    const std::size_t outstanding =
        impl_->state->outstandingCount.load(std::memory_order_acquire);
    const std::size_t handles = impl_->state->handleCount.load(std::memory_order_acquire);
    const std::uint32_t openFiles = impl_->state->openFiles.load(std::memory_order_acquire);
    if (outstanding != 0 || handles != 0 || openFiles != 0) {
        /* In-flight work at facade destruction is a real contract violation;
         * a fully drained client that skipped close() is only informational. */
        log::error("client destroyed with live work: lifecycle=%u outstanding=%llu "
                   "handles=%llu open_files=%u",
                   static_cast<unsigned>(lifecycle),
                   static_cast<unsigned long long>(outstanding),
                   static_cast<unsigned long long>(handles), openFiles);
    } else if (lifecycle != ClientLifecycle::Closed) {
        log::info("client destroyed without close after drain");
    }
}

Result<std::shared_ptr<Client>> Client::create(Engine &engine, const ClientOptions &options) {
    if (!engine.processValid()) {
        return makeError<std::shared_ptr<Client>>(Status::ForkedProcess,
                                                  "engine belongs to another process");
    }
    if (!engine.mounted() || engine.metadataNamespace() == nullptr) {
        return makeError<std::shared_ptr<Client>>(Status::InvalidState,
                                                  "engine is not mounted");
    }
    if (options.maxOutstanding == 0) {
        return makeError<std::shared_ptr<Client>>(Status::InvalidArgument,
                                                  "maxOutstanding must be non-zero");
    }
    auto client = std::shared_ptr<Client>(new Client(engine, options));
    if (!client->impl_->state->asyncInitialized) {
        return makeError<std::shared_ptr<Client>>(Status::NoMemory,
                                                  "client async resources could not be initialized");
    }
    return client;
}

Task<Result<void>> Client::close() {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (!state.onFrontendThread()) {
        co_return Result<void>(Status::InvalidState,
                               "client close called from a non-frontend thread");
    }
    ClientLifecycle expected = ClientLifecycle::Open;
    if (!state.lifecycle.compare_exchange_strong(expected, ClientLifecycle::Closing,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire) &&
        expected == ClientLifecycle::Closed) {
        co_return Result<void>(Status::Ok);
    }

    const std::size_t outstanding =
        state.outstandingCount.load(std::memory_order_acquire);
    const std::size_t queued = queuedCompletions(state);
    const std::size_t handles = state.handleCount.load(std::memory_order_acquire);
    const std::uint32_t openFiles = state.openFiles.load(std::memory_order_acquire);
    if (outstanding != 0 || queued != 0 || handles != 0 || openFiles != 0) {
        log::info("client closing: open files=%u outstanding=%llu queued=%llu handles=%llu",
                  openFiles,
                  static_cast<unsigned long long>(outstanding),
                  static_cast<unsigned long long>(queued),
                  static_cast<unsigned long long>(handles));
        co_return Result<void>(Status::Busy, "client resources are still active");
    }
    for (std::size_t index = 0; index < state.maxOutstandingValue; ++index) {
        if (state.requestSlots[index].inUse) {
            co_return Result<void>(Status::Busy, "client frontend handles are still active");
        }
    }
    if (state.completionOverflow.load(std::memory_order_acquire)) {
        co_return Result<void>(Status::Corrupt, "completion queue integrity failure");
    }
    state.lease.reset();
    state.lifecycle.store(ClientLifecycle::Closed, std::memory_order_release);
    log::info("client closed: open files=%u", openFiles);
    co_return Result<void>(Status::Ok);
}

Result<void> Client::bindFrontendThread() {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        return valid;
    }
    if (!state.asyncInitialized) {
        return makeError(Status::InvalidState, "client async resources are unavailable");
    }
    if (state.outstandingCount.load(std::memory_order_acquire) != 0 ||
        state.handleCount.load(std::memory_order_acquire) != 0 ||
        queuedCompletions(state) != 0) {
        return makeError(Status::Busy,
                         "client frontend ownership cannot move with active requests");
    }
    state.frontendThread = std::this_thread::get_id();
    return Result<void>(Status::Ok);
}

/* Design §20.4 keeps File handles on the process-level owner client while
 * per-thread clients submit I/O, so a file only has to belong to the same
 * engine; the submitting client still owns slots, credits and completions.
 * Client is a friend of File, so the private impl_ is reachable here. */
bool Client::fileBelongsToEngine(Engine &engine, const std::shared_ptr<File> &file) {
    return file != nullptr && file->impl_ != nullptr && file->impl_->state != nullptr &&
           file->impl_->state->engine == &engine;
}

Result<IoHandle> Client::submitRead(const std::shared_ptr<File> &file, IoVec buffer,
                                    std::uint64_t offset, CompletionToken token) {
    auto &state = *impl_->state;
    auto valid = validateAsyncClient(state);
    if (!valid.ok()) {
        return makeError<IoHandle>(valid.status(), valid.message());
    }
    if (!fileBelongsToEngine(*state.engine, file)) {
        return makeError<IoHandle>(Status::InvalidArgument,
                                   "file does not belong to this engine");
    }
    auto snapshot = file->impl_->asyncSnapshot();
    if (!snapshot.ok()) {
        return makeError<IoHandle>(snapshot.status(), snapshot.message());
    }
    if (snapshot.value()->stat.directory) {
        return makeError<IoHandle>(Status::InvalidArgument,
                                   "asynchronous read requires a regular file");
    }
    auto submitted = submitAsyncRequest(impl_->state, file->handle(), snapshot.value(),
                                        buffer, offset, token, false);
    if (!submitted.ok()) {
        return submitted;
    }
    IoHandle handle = std::move(submitted).value();
    state.handleCount.fetch_add(1, std::memory_order_acq_rel);
    handle.attachOwner(std::static_pointer_cast<void>(impl_->state), impl_->state.get(),
                       &releaseClientHandle);
    trace::event(trace::EventId::FeSubmit, handle.requestId(), offset, buffer.size, 0);
    return Result<IoHandle>(std::move(handle));
}

Result<IoHandle> Client::submitWrite(const std::shared_ptr<File> &file, IoVec buffer,
                                     std::uint64_t offset, CompletionToken token) {
    auto &state = *impl_->state;
    auto valid = validateAsyncClient(state);
    if (!valid.ok()) {
        return makeError<IoHandle>(valid.status(), valid.message());
    }
    if (!fileBelongsToEngine(*state.engine, file)) {
        return makeError<IoHandle>(Status::InvalidArgument,
                                   "file does not belong to this engine");
    }
    auto snapshot = file->impl_->asyncSnapshot();
    if (!snapshot.ok()) {
        return makeError<IoHandle>(snapshot.status(), snapshot.message());
    }
    if (snapshot.value()->stat.directory) {
        return makeError<IoHandle>(Status::InvalidArgument,
                                   "asynchronous write requires a regular file");
    }
    auto submitted = submitAsyncRequest(impl_->state, file->handle(), snapshot.value(),
                                        buffer, offset, token, true);
    if (!submitted.ok()) {
        return submitted;
    }
    IoHandle handle = std::move(submitted).value();
    state.handleCount.fetch_add(1, std::memory_order_acq_rel);
    handle.attachOwner(std::static_pointer_cast<void>(impl_->state), impl_->state.get(),
                       &releaseClientHandle);
    trace::event(trace::EventId::FeSubmit, handle.requestId(), offset, buffer.size, 1);
    return Result<IoHandle>(std::move(handle));
}

PollResult Client::poll(std::span<CompletionEvent> events, std::size_t min,
                        std::size_t max, std::chrono::microseconds timeout) {
    PollResult result;
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        result.status = Status::ForkedProcess;
        return result;
    }
    if (!state.onFrontendThread()) {
        result.status = Status::InvalidState;
        return result;
    }
    if (!state.asyncInitialized) {
        result.status = Status::InvalidState;
        return result;
    }
    /* A dropped or mismatched completion is fatal: never block the frontend on
     * a queue that can no longer satisfy its outstanding credits. */
    if (state.completionOverflow.load(std::memory_order_acquire)) {
        result.status = Status::Corrupt;
        return result;
    }
    result = state.completionQueue.poll(events, min, max, timeout,
                                        &state.options.completionWait);
    trace::event(trace::EventId::PollReturn, result.count,
                 static_cast<std::uint64_t>(timeout.count()),
                 static_cast<std::uint64_t>(min), static_cast<std::uint64_t>(max));
    if (result.status != Status::Ok) {
        return result;
    }
    for (std::size_t index = 0; index < result.count; ++index) {
        trace::event(trace::EventId::FeReap, events[index].requestId,
                     static_cast<std::uint64_t>(events[index].status),
                     events[index].completedBytes,
                     static_cast<std::uint64_t>(events[index].backendErrno));
        if (!state.reapEvent(events[index])) {
            result.status = Status::Corrupt;
        }
    }
    return result;
}

std::size_t Client::outstanding() const noexcept {
    if (impl_ == nullptr || impl_->state == nullptr) {
        return 0;
    }
    return impl_->state->outstandingCount.load(std::memory_order_acquire);
}

std::size_t Client::maxOutstanding() const noexcept {
    if (impl_ == nullptr || impl_->state == nullptr) {
        return 0;
    }
    return impl_->state->maxOutstandingValue;
}

std::size_t Client::openFileCount() const noexcept {
    if (impl_ == nullptr || impl_->state == nullptr) {
        return 0;
    }
    return impl_->state->openFiles.load(std::memory_order_acquire);
}

Task<Result<std::shared_ptr<File>>> Client::open(std::string_view path, OpenFlags flags) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return makeError<std::shared_ptr<File>>(valid.status(), valid.message());
    }
    Result<FileHandle> handle =
        hasFlag(flags, OpenFlags::Create)
            ? state.namespaceState->create(path, hasFlag(flags, OpenFlags::Exclusive),
                                           hasFlag(flags, OpenFlags::Truncate),
                                           hasFlag(flags, OpenFlags::Directory))
            : state.namespaceState->open(path);
    if (!handle.ok()) {
        co_return makeError<std::shared_ptr<File>>(handle.status(), handle.message());
    }
    const bool wantDirectory = hasFlag(flags, OpenFlags::Directory);
    auto stat = state.namespaceState->stat(handle.value());
    if (!stat.ok() || stat.value().directory != wantDirectory) {
        state.namespaceState->close(handle.value());
        co_return makeError<std::shared_ptr<File>>(Status::InvalidArgument,
                                                   "open flags do not match the path type");
    }
    const std::uint32_t maxFiles = state.options.maxOpenFiles;
    if (maxFiles != 0 && state.openFiles.load(std::memory_order_relaxed) >= maxFiles) {
        state.namespaceState->close(handle.value());
        co_return makeError<std::shared_ptr<File>>(Status::NoMemory,
                                                   "client open file limit reached");
    }
    state.openFiles.fetch_add(1, std::memory_order_acq_rel);
    auto fileImpl = std::make_unique<File::Impl>();
    fileImpl->state = impl_->state;
    fileImpl->handle = handle.value();
    co_return std::shared_ptr<File>(new File(std::move(fileImpl)));
}

Task<Result<void>> Client::remove(std::string_view path) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return makeError(valid.status(), valid.message());
    }
    co_return state.namespaceState->remove(path);
}

Task<Result<void>> Client::rename(std::string_view from, std::string_view to) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return makeError(valid.status(), valid.message());
    }
    co_return state.namespaceState->rename(from, to);
}

/* Creates a namespace hard link while preserving one shared inode and extent map. */
Task<Result<void>> Client::link(std::string_view from, std::string_view to) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return Result<void>(valid.status(), valid.message());
    }
    co_return state.namespaceState->link(from, to);
}

Task<Result<DirectoryPage>> Client::getChildren(std::string_view path,
                                                std::string_view cursor,
                                                std::size_t maxEntries) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return makeError<DirectoryPage>(valid.status(), valid.message());
    }
    co_return state.namespaceState->getChildren(path, cursor, maxEntries);
}

/* Resolves metadata for a namespace path without keeping an open handle. */
Task<Result<FileStat>> Client::stat(std::string_view path) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return makeError<FileStat>(valid.status(), valid.message());
    }
    if (path.size() == 1 && path.front() == '/') {
        /* The root inode has no parent entry; stat it through a short-lived
         * open handle so root and non-root paths share one code path. */
        auto handle = state.namespaceState->open(path);
        if (!handle.ok()) {
            co_return makeError<FileStat>(handle.status(), handle.message());
        }
        auto result = state.namespaceState->stat(handle.value());
        state.namespaceState->close(handle.value());
        co_return result;
    }
    co_return state.namespaceState->stat(path);
}

/* Creates missing directory components one by one; existing directories are
 * reused and a non-directory component fails without creating anything after
 * it. Each step is atomic inside the namespace lock of create(). */
Task<Result<void>> Client::createDirectories(std::string_view path) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return makeError(valid.status(), valid.message());
    }
    if (path.empty() || path.front() != '/') {
        co_return makeError(Status::InvalidArgument, "path must be absolute");
    }
    if (path.size() == 1) {
        co_return Result<void>(Status::Ok); /* root always exists */
    }
    if (path.back() == '/') {
        co_return makeError(Status::InvalidArgument, "trailing slash is not allowed");
    }
    std::size_t start = 1;
    while (start < path.size()) {
        std::size_t slash = path.find('/', start);
        if (slash == std::string_view::npos) {
            slash = path.size();
        }
        const std::string_view prefix = path.substr(0, slash);
        Result<FileHandle> handle =
            state.namespaceState->create(prefix, false, false, true);
        if (!handle.ok()) {
            co_return makeError(handle.status(), handle.message());
        }
        auto closed = state.namespaceState->close(handle.value());
        if (!closed.ok()) {
            co_return closed;
        }
        start = slash + 1;
    }
    co_return Result<void>(Status::Ok);
}

File::File(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

File::~File() = default;

FileHandle File::handle() const noexcept { return impl_->handle; }

Result<FileStat> File::stat() const {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        return makeError<FileStat>(Status::ForkedProcess,
                                   "engine belongs to another process");
    }
    if (state.namespaceState == nullptr) {
        return makeError<FileStat>(Status::InvalidState, "engine is not mounted");
    }
    return state.namespaceState->stat(impl_->handle);
}

/* Sums committed extent bytes so frontends can verify allocation coverage. */
Result<std::uint64_t> File::allocatedBytes() const {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        return makeError<std::uint64_t>(Status::ForkedProcess,
                                        "engine belongs to another process");
    }
    if (state.namespaceState == nullptr) {
        return makeError<std::uint64_t>(Status::InvalidState, "engine is not mounted");
    }
    auto descriptors = state.namespaceState->extents(impl_->handle);
    if (!descriptors.ok()) {
        return makeError<std::uint64_t>(descriptors.status(), descriptors.message());
    }
    const std::uint64_t extentBytes = state.namespaceState->extentBytes();
    if (extentBytes == 0) {
        return makeError<std::uint64_t>(Status::InvalidState, "invalid extent size");
    }
    std::uint64_t bytes = 0;
    for (const metadata::ExtentDescriptor &descriptor : descriptors.value()) {
        if (descriptor.count >
            (std::numeric_limits<std::uint64_t>::max() - bytes) / extentBytes) {
            return makeError<std::uint64_t>(Status::InvalidArgument,
                                            "extent byte count overflows");
        }
        bytes += descriptor.count * extentBytes;
    }
    return Result<std::uint64_t>(bytes);
}

Task<Result<ReadResult>> File::read(std::span<IoVec> buffers, std::uint64_t offset,
                                    std::stop_token stop) {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<ReadResult>(Status::ForkedProcess,
                                     "engine belongs to another process");
    }
    if (state.namespaceState == nullptr || state.dataPlane == nullptr) {
        co_return Result<ReadResult>(Status::InvalidState, "engine is not mounted");
    }
    /* Honor cancellation before inspecting or submitting any data operation.
     * In-flight synchronous requests use the async AbortIO contract instead. */
    if (stop.stop_requested()) {
        co_return makeError<ReadResult>(Status::Cancelled, "read cancelled before submission");
    }
    std::size_t total = 0;
    for (const IoVec &vec : buffers) {
        if (vec.size != 0 && vec.data == nullptr) {
            co_return makeError<ReadResult>(Status::InvalidArgument, "null iovec buffer");
        }
        total += vec.size;
    }
    if (total == 0) {
        co_return ReadResult{0};
    }
    auto snapshot = impl_->syncSnapshot();
    if (!snapshot.ok()) {
        co_return makeError<ReadResult>(snapshot.status(), snapshot.message());
    }
    const std::uint64_t size = snapshot.value()->stat.logicalSize;
    if (offset >= size) {
        co_return ReadResult{0};
    }
    const std::uint64_t length = std::min<std::uint64_t>(total, size - offset);
    std::uint64_t cursor = offset;
    std::uint64_t remaining = length;
    for (const IoVec &vec : buffers) {
        if (remaining == 0) {
            break;
        }
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(vec.size, remaining));
        auto read = state.dataPlane->read(snapshot.value()->extentMap,
                                          std::span<std::byte>(vec.data, take), cursor,
                                          take);
        if (!read.ok()) {
            co_return makeError<ReadResult>(read.status(), read.message());
        }
        cursor += take;
        remaining -= take;
    }
    co_return ReadResult{length};
}

Task<Result<WriteResult>> File::write(std::span<const IoVec> buffers, std::uint64_t offset,
                                      std::stop_token stop) {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<WriteResult>(Status::ForkedProcess,
                                      "engine belongs to another process");
    }
    if (state.namespaceState == nullptr || state.dataPlane == nullptr) {
        co_return Result<WriteResult>(Status::InvalidState, "engine is not mounted");
    }
    /* Avoid reserving extents or mutating namespace state for a pre-cancelled
     * write; in-flight cancellation remains the async AbortIO contract. */
    if (stop.stop_requested()) {
        co_return makeError<WriteResult>(Status::Cancelled,
                                         "write cancelled before submission");
    }
    std::size_t total = 0;
    for (const IoVec &vec : buffers) {
        if (vec.size != 0 && vec.data == nullptr) {
            co_return makeError<WriteResult>(Status::InvalidArgument, "null iovec buffer");
        }
        if (vec.size > std::numeric_limits<std::size_t>::max() - total) {
            co_return makeError<WriteResult>(Status::InvalidArgument,
                                             "iovec byte count overflows");
        }
        total += vec.size;
    }
    if (total == 0) {
        co_return WriteResult{0};
    }
    if (offset > std::numeric_limits<std::uint64_t>::max() - total) {
        co_return makeError<WriteResult>(Status::InvalidArgument,
                                         "write range overflows");
    }

    /* The pre-write logical size marks the append frontier: device blocks at
     * or beyond it hold no committed bytes, so the data path can skip the RMW
     * refresh read for them. Reuse the cached snapshot on a hit; a miss only
     * needs the cheap stat, because an append rebuilds the map after commit. */
    std::uint64_t knownEof = 0;
    if (auto cached = impl_->cachedSnapshot(); cached != nullptr) {
        knownEof = cached->stat.logicalSize;
    } else {
        auto beforeWrite = state.namespaceState->stat(impl_->handle);
        if (!beforeWrite.ok()) {
            co_return makeError<WriteResult>(beforeWrite.status(),
                                             beforeWrite.message());
        }
        knownEof = beforeWrite.value().logicalSize;
    }

    /* Allocate missing extents, zero-initialize them, then commit. Extents are
     * invisible until COMMITTED (hfs2.0.md §9.4). */
    auto reservations = state.namespaceState->reserveExtents(impl_->handle, offset, total);
    if (!reservations.ok()) {
        co_return makeError<WriteResult>(reservations.status(), reservations.message());
    }
    if (!reservations.value().empty()) {
        /* Zero only the parts of the new extents that this write does not
         * cover; fully covered extents need no zero fill at all. */
        const std::uint64_t extentBytes = state.namespaceState->extentBytes();
        for (const metadata::ExtentAllocation &allocation : reservations.value()) {
            const std::uint32_t deviceId =
                static_cast<std::uint32_t>(allocation.run.deviceId);
            const std::uint64_t physicalStart =
                state.namespaceState->deviceStartExtent(deviceId) + allocation.run.start;
            for (std::uint64_t i = 0; i < allocation.run.count; ++i) {
                const std::uint64_t extentStart =
                    (allocation.logicalExtent + i) * extentBytes;
                const std::uint64_t extentEnd = extentStart + extentBytes;
                const std::uint64_t coverStart = std::max(offset, extentStart);
                const std::uint64_t coverEnd = std::min(offset + total, extentEnd);
                if (coverStart > extentStart) {
                    auto zeroed = state.dataPlane->zeroRange(
                        deviceId, physicalStart + i, 0, coverStart - extentStart,
                        extentBytes);
                    if (!zeroed.ok()) {
                        state.namespaceState->releaseExtents(impl_->handle,
                                                             reservations.value());
                        co_return makeError<WriteResult>(zeroed.status(),
                                                         zeroed.message());
                    }
                }
                if (coverEnd < extentEnd) {
                    auto zeroed = state.dataPlane->zeroRange(
                        deviceId, physicalStart + i, coverEnd - extentStart,
                        extentEnd - coverEnd, extentBytes);
                    if (!zeroed.ok()) {
                        state.namespaceState->releaseExtents(impl_->handle,
                                                             reservations.value());
                        co_return makeError<WriteResult>(zeroed.status(),
                                                         zeroed.message());
                    }
                }
            }
        }
        auto committed =
            state.namespaceState->commitExtents(impl_->handle, reservations.value());
        if (!committed.ok()) {
            state.namespaceState->releaseExtents(impl_->handle, reservations.value());
            co_return makeError<WriteResult>(committed.status(), committed.message());
        }
    }

    auto snapshot = impl_->syncSnapshot();
    if (!snapshot.ok()) {
        co_return makeError<WriteResult>(snapshot.status(), snapshot.message());
    }
    std::uint64_t cursor = offset;
    for (const IoVec &vec : buffers) {
        if (vec.size == 0) {
            continue;
        }
        auto written = state.dataPlane->write(
            snapshot.value()->extentMap,
            std::span<const std::byte>(vec.data, vec.size), cursor, vec.size,
            knownEof);
        if (!written.ok()) {
            co_return makeError<WriteResult>(written.status(), written.message());
        }
        cursor += vec.size;
    }
    auto resized = state.namespaceState->setSize(impl_->handle, cursor);
    if (!resized.ok()) {
        co_return makeError<WriteResult>(resized.status(), resized.message());
    }
    co_return WriteResult{total};
}

Task<Result<WriteResult>> File::append(std::span<const IoVec> buffers,
                                       std::stop_token stop) {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<WriteResult>(Status::ForkedProcess,
                                      "engine belongs to another process");
    }
    if (state.namespaceState == nullptr) {
        co_return Result<WriteResult>(Status::InvalidState, "engine is not mounted");
    }
    auto snapshot = impl_->syncSnapshot();
    if (!snapshot.ok()) {
        co_return makeError<WriteResult>(snapshot.status(), snapshot.message());
    }
    auto outer = co_await write(buffers, snapshot.value()->stat.logicalSize, stop);
    if (!outer.ok()) {
        co_return makeError<WriteResult>(outer.status(), outer.message());
    }
    co_return outer.value();
}

Task<Result<void>> File::allocate(std::uint64_t offset, std::uint64_t length) {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (state.namespaceState == nullptr || state.dataPlane == nullptr) {
        co_return Result<void>(Status::InvalidState, "engine is not mounted");
    }
    if (length == 0) {
        co_return Result<void>(Status::Ok);
    }
    if (offset > std::numeric_limits<std::uint64_t>::max() - length) {
        co_return Result<void>(Status::InvalidArgument, "allocation range overflows");
    }
    auto reservations = state.namespaceState->reserveExtents(impl_->handle, offset, length);
    if (!reservations.ok()) {
        co_return makeError(reservations.status(), reservations.message());
    }
    if (!reservations.value().empty()) {
        std::vector<metadata::ExtentDescriptor> descriptors;
        descriptors.reserve(reservations.value().size());
        for (const metadata::ExtentAllocation &allocation : reservations.value()) {
            metadata::ExtentDescriptor descriptor;
            descriptor.logicalExtent = allocation.logicalExtent;
            descriptor.physicalExtent =
                state.namespaceState->deviceStartExtent(
                    static_cast<std::uint32_t>(allocation.run.deviceId)) +
                allocation.run.start;
            descriptor.count = allocation.run.count;
            descriptor.deviceId = static_cast<std::uint32_t>(allocation.run.deviceId);
            descriptors.push_back(descriptor);
        }
        auto zeroed = state.dataPlane->zeroExtents(descriptors,
                                                   state.namespaceState->extentBytes());
        if (!zeroed.ok()) {
            state.namespaceState->releaseExtents(impl_->handle, reservations.value());
            co_return makeError(zeroed.status(), zeroed.message());
        }
        auto committed =
            state.namespaceState->commitExtents(impl_->handle, reservations.value());
        if (!committed.ok()) {
            state.namespaceState->releaseExtents(impl_->handle, reservations.value());
            co_return makeError(committed.status(), committed.message());
        }
    }
    co_return state.namespaceState->setSize(impl_->handle, offset + length);
}

Task<Result<void>> File::truncate(std::uint64_t size) {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (state.namespaceState == nullptr) {
        co_return Result<void>(Status::InvalidState, "engine is not mounted");
    }
    co_return state.namespaceState->truncateFile(impl_->handle, size);
}

Task<Result<void>> File::datasync() {
    if (impl_->state->engine == nullptr || !impl_->state->engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    /* Durability is a namespace-wide barrier; an inode epoch may be stale. */
    co_return impl_->state->engine->syncMetadata(
        impl_->state->namespaceState->logicalEpoch());
}

Task<Result<void>> File::sync() {
    if (impl_->state->engine == nullptr || !impl_->state->engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    /* Durability is a namespace-wide barrier; an inode epoch may be stale. */
    co_return impl_->state->engine->syncMetadata(
        impl_->state->namespaceState->logicalEpoch());
}

} // namespace hfs
