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
    std::uint64_t namespaceEpoch = 0;
};

/* One bounded frontend slot keeps the immutable extent snapshot alive until
 * its completion event has been reaped by the creating thread. */
struct AsyncRequestSlot {
    bool inUse = false;
    bool lookupIndexed = false;
    std::uint64_t requestId = 0;
    CompletionToken token{};
    FileHandle file{};
    std::shared_ptr<const AsyncFileSnapshot> snapshot;

    void release() noexcept {
        inUse = false;
        lookupIndexed = false;
        requestId = 0;
        token = {};
        file = {};
        snapshot.reset();
    }
};

/* Fixed-capacity request lookup avoids allocating or scanning the slot table
 * on the normal completion path. Tombstones preserve open-addressing probes. */
struct RequestLookupEntry {
    enum class State : std::uint8_t {
        Empty,
        Occupied,
        Tombstone,
    };

    State state = State::Empty;
    std::uint64_t requestId = 0;
    std::uint32_t slotIndex = 0;
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
    std::unique_ptr<RequestLookupEntry[]> requestLookup;
    std::size_t requestLookupCapacity = 0;
    AsyncCompletionQueue completionQueue;
    bool asyncInitialized = false;

    /* Allocates every frontend-owned bounded slot before the first submit. */
    bool initializeAsync(std::size_t shardCount) noexcept {
        if (options.maxOutstanding == 0 || maxOutstandingValue != 0 ||
            asyncInitialized) {
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
        const std::size_t requiredLookupCapacity =
            static_cast<std::size_t>(maxOutstandingValue) * 2;
        if (requiredLookupCapacity < maxOutstandingValue) {
            freeSlotStack.reset();
            requestSlots.reset();
            maxOutstandingValue = 0;
            return false;
        }
        requestLookupCapacity = 1;
        while (requestLookupCapacity < requiredLookupCapacity) {
            if (requestLookupCapacity > std::numeric_limits<std::size_t>::max() / 2) {
                freeSlotStack.reset();
                requestSlots.reset();
                maxOutstandingValue = 0;
                requestLookupCapacity = 0;
                return false;
            }
            requestLookupCapacity <<= 1;
        }
        requestLookup.reset(new (std::nothrow) RequestLookupEntry[requestLookupCapacity]);
        if (requestLookup == nullptr) {
            freeSlotStack.reset();
            requestSlots.reset();
            maxOutstandingValue = 0;
            requestLookupCapacity = 0;
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
            requestLookupCapacity = 0;
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

    /* Reactor callbacks only enqueue a value; they never touch frontend slots. */
    void publish(std::uint32_t shard, const CompletionEvent &event) noexcept override {
        const auto result = completionQueue.pushWithWakeHint(shard, event);
        if (!result.accepted) {
            completionOverflow.store(true, std::memory_order_release);
            return;
        }
        if (result.notify && options.completionWake != nullptr) {
            options.completionWake(options.completionWakeContext);
        }
    }

    static std::size_t lookupHash(std::uint64_t requestId) noexcept {
        requestId ^= requestId >> 30;
        requestId *= 0xbf58476d1ce4e5b9ULL;
        requestId ^= requestId >> 27;
        requestId *= 0x94d049bb133111ebULL;
        requestId ^= requestId >> 31;
        return static_cast<std::size_t>(requestId);
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
        if (requestId == 0 || slotIndex >= maxOutstandingValue || requestLookup == nullptr ||
            requestLookupCapacity == 0) {
            return false;
        }
        const std::size_t mask = requestLookupCapacity - 1;
        const std::size_t start = lookupHash(requestId) & mask;
        std::size_t firstTombstone = requestLookupCapacity;
        for (std::size_t probe = 0; probe < requestLookupCapacity; ++probe) {
            RequestLookupEntry &entry = requestLookup[(start + probe) & mask];
            if (entry.state == RequestLookupEntry::State::Occupied) {
                if (entry.requestId == requestId) {
                    return false;
                }
                continue;
            }
            if (entry.state == RequestLookupEntry::State::Tombstone) {
                if (firstTombstone == requestLookupCapacity) {
                    firstTombstone = (start + probe) & mask;
                }
                continue;
            }
            const std::size_t target = firstTombstone == requestLookupCapacity
                                           ? (start + probe) & mask
                                           : firstTombstone;
            RequestLookupEntry &targetEntry = requestLookup[target];
            targetEntry.requestId = requestId;
            targetEntry.slotIndex = static_cast<std::uint32_t>(slotIndex);
            targetEntry.state = RequestLookupEntry::State::Occupied;
            requestSlots[slotIndex].lookupIndexed = true;
            return true;
        }
        if (firstTombstone != requestLookupCapacity) {
            RequestLookupEntry &targetEntry = requestLookup[firstTombstone];
            targetEntry.requestId = requestId;
            targetEntry.slotIndex = static_cast<std::uint32_t>(slotIndex);
            targetEntry.state = RequestLookupEntry::State::Occupied;
            requestSlots[slotIndex].lookupIndexed = true;
            return true;
        }
        return false;
    }

    void eraseRequest(std::uint64_t requestId) noexcept {
        if (requestId == 0 || requestLookup == nullptr || requestLookupCapacity == 0) {
            return;
        }
        const std::size_t mask = requestLookupCapacity - 1;
        const std::size_t start = lookupHash(requestId) & mask;
        for (std::size_t probe = 0; probe < requestLookupCapacity; ++probe) {
            RequestLookupEntry &entry = requestLookup[(start + probe) & mask];
            if (entry.state == RequestLookupEntry::State::Empty) {
                return;
            }
            if (entry.state == RequestLookupEntry::State::Occupied &&
                entry.requestId == requestId) {
                entry.state = RequestLookupEntry::State::Tombstone;
                entry.requestId = 0;
                entry.slotIndex = 0;
                return;
            }
        }
    }

    AsyncRequestSlot *findRequest(std::uint64_t requestId, std::size_t &index) noexcept {
        if (requestId != 0 && requestLookup != nullptr && requestLookupCapacity != 0) {
            const std::size_t mask = requestLookupCapacity - 1;
            const std::size_t start = lookupHash(requestId) & mask;
            for (std::size_t probe = 0; probe < requestLookupCapacity; ++probe) {
                RequestLookupEntry &entry = requestLookup[(start + probe) & mask];
                if (entry.state == RequestLookupEntry::State::Empty) {
                    break;
                }
                if (entry.state == RequestLookupEntry::State::Occupied &&
                    entry.requestId == requestId) {
                    index = entry.slotIndex;
                    if (index < maxOutstandingValue && requestSlots[index].inUse &&
                        requestSlots[index].requestId == requestId) {
                        return &requestSlots[index];
                    }
                    break;
                }
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
        bool matched = false;
        if (slot != nullptr) {
            matched = slot->token.slot == event.token.slot &&
                      slot->token.generation == event.token.generation;
            if (slot->lookupIndexed) {
                eraseRequest(event.requestId);
            }
            releaseSlot(index);
        }
        releaseCredit();
        if (!matched) {
            completionOverflow.store(true, std::memory_order_release);
        }
        return matched;
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

/* Builds one fixed request against a slot-owned extent snapshot. */
Result<IoHandle> submitAsyncRequest(ClientState &state, FileHandle handle,
                                    std::shared_ptr<const AsyncFileSnapshot> snapshot,
                                    IoVec buffer,
                                    std::uint64_t offset, CompletionToken token,
                                    bool write) {
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

    auto submitted = state.asyncService->submit(request, token, state);
    if (!submitted.ok()) {
        state.releaseSlot(slotIndex);
        return makeError<IoHandle>(submitted.status(), submitted.message());
    }
    IoHandle handleResult = std::move(submitted).value();
    slot->requestId = handleResult.requestId();
    (void)state.indexRequest(slot->requestId, slotIndex);
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

    /* Refresh metadata only when the namespace publication epoch changes. */
    Result<std::shared_ptr<const AsyncFileSnapshot>> asyncSnapshot() const {
        if (state == nullptr || state->namespaceState == nullptr) {
            return makeError<std::shared_ptr<const AsyncFileSnapshot>>(
                Status::InvalidState, "file namespace is unavailable");
        }
        const std::uint64_t currentEpoch = state->namespaceState->logicalEpochFast();
        {
            std::lock_guard<std::mutex> lock(asyncCacheMutex);
            if (asyncCache != nullptr &&
                asyncCache->namespaceEpoch == currentEpoch) {
                return asyncCache;
            }
        }

        /* A retry closes the stat/extents race with a concurrent metadata
         * mutation without holding the namespace mutex across allocations. */
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const std::uint64_t before = state->namespaceState->logicalEpochFast();
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
            const std::uint64_t after = state->namespaceState->logicalEpochFast();
            if (before != after) {
                continue;
            }
            auto refreshed = std::make_shared<AsyncFileSnapshot>();
            refreshed->stat = stat.value();
            refreshed->extentMap = std::move(map).value();
            refreshed->namespaceEpoch = after;
            std::lock_guard<std::mutex> lock(asyncCacheMutex);
            if (state->namespaceState->logicalEpochFast() != after) {
                continue;
            }
            asyncCache = std::move(refreshed);
            return asyncCache;
        }
        return makeError<std::shared_ptr<const AsyncFileSnapshot>>(
            Status::Busy, "namespace changed while building asynchronous snapshot");
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

Client::~Client() = default;

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

Result<IoHandle> Client::submitRead(const std::shared_ptr<File> &file, IoVec buffer,
                                    std::uint64_t offset, CompletionToken token) {
    auto &state = *impl_->state;
    auto valid = validateAsyncClient(state);
    if (!valid.ok()) {
        return makeError<IoHandle>(valid.status(), valid.message());
    }
    if (file == nullptr || file->impl_ == nullptr ||
        file->impl_->state.get() != impl_->state.get()) {
        return makeError<IoHandle>(Status::InvalidArgument,
                                   "file does not belong to this client");
    }
    auto snapshot = file->impl_->asyncSnapshot();
    if (!snapshot.ok()) {
        return makeError<IoHandle>(snapshot.status(), snapshot.message());
    }
    if (snapshot.value()->stat.directory) {
        return makeError<IoHandle>(Status::InvalidArgument,
                                   "asynchronous read requires a regular file");
    }
    auto submitted = submitAsyncRequest(state, file->handle(), snapshot.value(),
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
    if (file == nullptr || file->impl_ == nullptr ||
        file->impl_->state.get() != impl_->state.get()) {
        return makeError<IoHandle>(Status::InvalidArgument,
                                   "file does not belong to this client");
    }
    auto snapshot = file->impl_->asyncSnapshot();
    if (!snapshot.ok()) {
        return makeError<IoHandle>(snapshot.status(), snapshot.message());
    }
    if (snapshot.value()->stat.directory) {
        return makeError<IoHandle>(Status::InvalidArgument,
                                   "asynchronous write requires a regular file");
    }
    auto submitted = submitAsyncRequest(state, file->handle(), snapshot.value(),
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
    auto stat = state.namespaceState->stat(impl_->handle);
    if (!stat.ok()) {
        co_return makeError<ReadResult>(stat.status(), stat.message());
    }
    const std::uint64_t size = stat.value().logicalSize;
    if (offset >= size) {
        co_return ReadResult{0};
    }
    const std::uint64_t length = std::min<std::uint64_t>(total, size - offset);
    auto map = extentMapFor(state, impl_->handle);
    if (!map.ok()) {
        co_return makeError<ReadResult>(map.status(), map.message());
    }
    std::uint64_t cursor = offset;
    std::uint64_t remaining = length;
    for (const IoVec &vec : buffers) {
        if (remaining == 0) {
            break;
        }
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(vec.size, remaining));
        auto read = state.dataPlane->read(map.value(),
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

    /* Allocate missing extents, zero-initialize them, then commit. Extents are
     * invisible until COMMITTED (hfs2.0.md §9.4). */
    auto reservations = state.namespaceState->reserveExtents(impl_->handle, offset, total);
    if (!reservations.ok()) {
        co_return makeError<WriteResult>(reservations.status(), reservations.message());
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
            co_return makeError<WriteResult>(zeroed.status(), zeroed.message());
        }
        auto committed =
            state.namespaceState->commitExtents(impl_->handle, reservations.value());
        if (!committed.ok()) {
            state.namespaceState->releaseExtents(impl_->handle, reservations.value());
            co_return makeError<WriteResult>(committed.status(), committed.message());
        }
    }

    auto map = extentMapFor(state, impl_->handle);
    if (!map.ok()) {
        co_return makeError<WriteResult>(map.status(), map.message());
    }
    std::uint64_t cursor = offset;
    for (const IoVec &vec : buffers) {
        if (vec.size == 0) {
            continue;
        }
        auto written = state.dataPlane->write(
            map.value(), std::span<const std::byte>(vec.data, vec.size), cursor, vec.size);
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
    auto stat = state.namespaceState->stat(impl_->handle);
    if (!stat.ok()) {
        co_return makeError<WriteResult>(stat.status(), stat.message());
    }
    auto outer = co_await write(buffers, stat.value().logicalSize, stop);
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
