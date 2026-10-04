#include "uuse/core.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <condition_variable>
#include <thread>
#include <unistd.h>

#include "common/log.hpp"
#include "common/test_hooks.hpp"
#include "common/stats.hpp"
#include "common/trace.hpp"
#include "core/async_data_service.hpp"
#include "core/data_plane.hpp"
#include "core/dma_arena.hpp"
#include "core/frame_pool.hpp"
#include "core/operation.hpp"
#include "core/spdk_async_backend.hpp"
#include "metadata/namespace.hpp"
#include "runtime/spdk_runtime.hpp"
#include "uuse/journal.hpp"
#include "storage/volume.hpp"

#if defined(UUSE_WITH_SPDK)
#include <spdk/env.h>
#endif

namespace uuse {

namespace {

#if defined(UUSE_WITH_SPDK)
void *dataDmaAlloc(std::size_t size, void *context) {
    (void)context;
    return spdk_dma_zmalloc(size, 4096, nullptr);
}

void dataDmaFree(void *ptr, void *context) {
    (void)context;
    spdk_dma_free(ptr);
}
#else
void *dataDmaAlloc(std::size_t, void *) { return nullptr; }
void dataDmaFree(void *, void *) {}
#endif

struct HugepageInfo {
    bool available = false;
    std::uint64_t totalPages = 0;
    std::uint64_t freePages = 0;
    std::uint64_t pageKb = 0;
};

#if defined(__linux__)
HugepageInfo readHugepageInfo() {
    HugepageInfo info;
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) {
        return info;
    }
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream stream(line);
        std::string key;
        std::uint64_t value = 0;
        std::string unit;
        stream >> key >> value >> unit;
        if (key == "HugePages_Total:") {
            info.totalPages = value;
        } else if (key == "HugePages_Free:") {
            info.freePages = value;
        } else if (key == "Hugepagesize:") {
            info.pageKb = value;
        }
    }
    info.available = info.pageKb > 0;
    return info;
}
#else
HugepageInfo readHugepageInfo() { return {}; }
#endif

/* Test-only boundary between durable data and metadata publication. */
bool testMetadataCommitFailureRequested() {
    const char *value = std::getenv("UUSE_TEST_FAIL_METADATA_COMMIT");
    return value != nullptr && std::string(value) == "1";
}

/* Process-wide fork marker: a forked child must fail closed for every API.
 * The handler only stores a lock-free flag, so it stays async-signal-safe. */
std::atomic<bool> g_forkedChild{false};

void markForkedChild() noexcept {
    g_forkedChild.store(true, std::memory_order_release);
}

const bool g_forkHandlerInstalled [[maybe_unused]] = [] {
    (void)::pthread_atfork(nullptr, nullptr, &markForkedChild);
    return true;
}();

/* getpid() is a real syscall on glibc >= 2.25; cache it and rely on the fork
 * marker above so processValid() stays a few loads. */
pid_t cachedProcessId() noexcept {
    static const pid_t pid = ::getpid();
    return pid;
}

/* Adapts the resident namespace snapshot to the journal checkpoint contract. */
class NamespaceJournalTarget final : public JournalTarget {
public:
    explicit NamespaceJournalTarget(metadata::Namespace &namespaceState)
        : namespaceState_(namespaceState) {}

    /* Applies one replayed metadata transaction record (uuse.md §9.3). */
    Result<void> apply(const JournalRecord &record) override {
        return namespaceState_.applyJournalRecord(record);
    }

    /* Saves the complete inode, directory, extent and allocator state. */
    Result<void> saveSnapshot(std::uint64_t txid, std::vector<std::byte> &out) override {
        return namespaceState_.saveSnapshot(txid, out);
    }

    /* Restores a validated checkpoint snapshot. */
    Result<void> loadSnapshot(std::span<const std::byte> data) override {
        return namespaceState_.loadSnapshot(data);
    }

    /* Reports the transaction id represented by the current snapshot. */
    std::uint64_t snapshotTxid() const noexcept override {
        return namespaceState_.snapshotTxid();
    }

private:
    metadata::Namespace &namespaceState_;
};

/* required = ceil((DMA cap + SPDK baseline reserve) * 1.15 / page size) */
std::uint64_t hugepagesRequired(const Config &config, std::uint64_t pageKb) {
    constexpr std::uint64_t kBaseReserveMb = 128;
    std::uint64_t bytes = (config.dmaArenaMb + kBaseReserveMb) << 20;
    bytes = (bytes * 115 + 99) / 100;
    const std::uint64_t pageBytes = pageKb << 10;
    return (bytes + pageBytes - 1) / pageBytes;
}

struct AsyncDataPollerContext {
    AsyncDataService *service = nullptr;
    std::uint32_t shard = 0;
};

/* Stops a mount-time trace session when mount fails before publishing. */
struct MountTraceGuard {
    bool active = false;
    ~MountTraceGuard() {
        if (active) {
            trace::setEnabled(false);
            trace::stop();
        }
    }
    void release() noexcept { active = false; }
};

/* Runs only the owning shard actor from the data-reactor poll loop. Reports
 * work so idle detection keeps polling while async operations are in flight. */
int pumpAsyncDataShard(void *context) noexcept {
    auto *poller = static_cast<AsyncDataPollerContext *>(context);
    if (poller == nullptr || poller->service == nullptr) {
        return 0;
    }
    const std::size_t pumped = poller->service->pump(poller->shard);
    if (pumped != 0) {
        return static_cast<int>(pumped);
    }
    return poller->service->outstanding() != 0 ? 1 : 0;
}

} // namespace

struct Engine::Impl {
    struct SyncBatch {
        std::uint64_t targetEpoch = 0;
        std::uint64_t frozenTargetEpoch = 0;
        Status status = Status::Ok;
        std::string message;
        bool completed = false;
        std::condition_variable condition;
    };

    Config config;
    int lockFd = -1;
    bool mounted = false;
    bool stopped = false;
    bool formatted = false;
    std::array<std::uint8_t, 16> uuid{};
    std::vector<DeviceGeometry> geometries;
    pid_t ownerPid = 0;
    std::unique_ptr<metadata::Namespace> namespaceState;
    std::unique_ptr<NamespaceJournalTarget> metadataTarget;
    std::unique_ptr<Journal> journal;
    std::unique_ptr<DmaArena> dmaArena;
    std::unique_ptr<DataPlane> dataPlane;
    std::vector<std::unique_ptr<SpdkAsyncDataBackend>> asyncBackends;
    std::vector<AsyncDataBackend *> asyncBackendViews;
    std::unique_ptr<AsyncDataService> asyncService;
    std::vector<AsyncDataPollerContext> asyncPollers;
    std::vector<std::uint8_t> asyncPollersRegistered;
    std::atomic<std::uint32_t> metadataUsers{0};
    /* Coordinates full-snapshot durability barriers without holding the
     * namespace lock while device I/O and checkpoint writes are in flight. */
    std::mutex syncMutex;
    std::shared_ptr<SyncBatch> syncBatch;

};

Engine::Engine(Config config) : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
}

Engine::~Engine() {
    /* A destroyed-but-not-stopped engine must not leave reactor pollers running
     * against freed engine state (bug #45): stop the runtime first. stop() is
     * idempotent and bounded, and the design still requires an explicit
     * Engine::stop() before destruction. */
    if (impl_ != nullptr && SpdkRuntime::instance().active()) {
        (void)SpdkRuntime::instance().stop();
    }
    if (impl_ && impl_->lockFd >= 0) {
        ::flock(impl_->lockFd, LOCK_UN);
        ::close(impl_->lockFd);
        impl_->lockFd = -1;
    }
}

Result<std::unique_ptr<Engine>> Engine::create(const Config &config) {
    auto valid = validateConfig(config);
    if (!valid.ok()) {
        return makeError<std::unique_ptr<Engine>>(valid.status(), valid.message());
    }

    auto engine = std::unique_ptr<Engine>(new Engine(config));
    auto &impl = *engine->impl_;
    impl.ownerPid = ::getpid();

    /* Bounded pools sized by the config; must be applied before first use. */
    FramePool::instance().setCapacity(
        static_cast<std::size_t>(config.coroutineFramePool) * 1024);
    Operation::setPoolCapacity(config.operationPool);

    impl.lockFd = ::open(config.processLockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (impl.lockFd < 0) {
        return makeError<std::unique_ptr<Engine>>(
            Status::IoError, "cannot open process lock '" + config.processLockPath +
                                 "': " + std::strerror(errno));
    }
    if (::flock(impl.lockFd, LOCK_EX | LOCK_NB) != 0) {
        ::close(impl.lockFd);
        impl.lockFd = -1;
        return makeError<std::unique_ptr<Engine>>(
            Status::Busy, "another engine already holds '" + config.processLockPath + "'");
    }

    const HugepageInfo pages = readHugepageInfo();
    if (!pages.available) {
        log::info("hugepage preflight skipped (no hugepage info available)");
    } else {
        const std::uint64_t required = hugepagesRequired(config, pages.pageKb);
        if (required > pages.freePages) {
            return makeError<std::unique_ptr<Engine>>(
                Status::NoMemory, "hugepage preflight failed: required " +
                                      std::to_string(required) + " pages, available " +
                                      std::to_string(pages.freePages) + " pages of " +
                                      std::to_string(pages.pageKb) + " KiB");
        }
        log::info("hugepage preflight ok: required %llu, available %llu of %llu pages",
                  static_cast<unsigned long long>(required),
                  static_cast<unsigned long long>(pages.freePages),
                  static_cast<unsigned long long>(pages.totalPages));
    }

    log::info("engine created: instance='%s' abi=%u", config.instanceName.c_str(),
              coreAbiMajor());
    return std::unique_ptr<Engine>(engine.release());
}

Task<Result<void>> Engine::format(const FormatOptions &options) {
    auto &impl = *impl_;
    if (!processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (impl.stopped) {
        co_return Result<void>(Status::InvalidState, "engine already stopped");
    }
    if (impl.mounted) {
        co_return Result<void>(Status::InvalidState, "engine is mounted; stop it before format");
    }

    auto &runtime = SpdkRuntime::instance();
    const bool startedHere = !runtime.active();
    if (startedHere) {
        auto started = runtime.start(impl.config);
        if (!started.ok()) {
            co_return started;
        }
    }
    auto volume = formatVolume(runtime, impl.config, options.confirm);
    if (volume.ok()) {
        impl.formatted = true;
        impl.uuid = volume.value().uuid;
        impl.geometries = volume.value().devices;
    }
    if (startedHere) {
        runtime.stop();
    }
    if (!volume.ok()) {
        co_return makeError(volume.status(), volume.message());
    }
    log::info("engine format complete");
    co_return Result<void>(Status::Ok);
}

Task<Result<void>> Engine::mount() {
    auto &impl = *impl_;
    if (!processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (impl.stopped) {
        co_return Result<void>(Status::InvalidState, "engine already stopped");
    }
    if (impl.mounted) {
        co_return Result<void>(Status::InvalidState, "engine already mounted");
    }
    /* Start the observability session before the runtime and journal replay so
     * mount/recovery trace points land in this session (uuse.md §16.5). */
    stats::setEnabled(impl.config.stats);
    trace::setSample(impl.config.traceSample);
    trace::stop();
    if (impl.config.trace && !trace::start(impl.config.traceDir.c_str())) {
        log::info("trace session unavailable; tracing disabled");
    }
    trace::setEnabled(impl.config.trace && trace::sessionActive());
    MountTraceGuard traceGuard;
    traceGuard.active = impl.config.trace && trace::sessionActive();
    auto &runtime = SpdkRuntime::instance();
    auto started = runtime.start(impl.config);
    if (!started.ok()) {
        co_return started;
    }
    auto volume = mountVolume(runtime, impl.config);
    if (!volume.ok()) {
        runtime.stop();
        co_return makeError(volume.status(), volume.message());
    }
    /* Mark the volume dirty before exposing any mounted handles. */
    auto markedDirty = updateCleanShutdown(runtime, impl.config, false);
    if (!markedDirty.ok()) {
        runtime.stop();
        co_return markedDirty;
    }
    impl.geometries = volume.value().devices;

    /* Build the resident namespace before replay so checkpoints can restore it. */
    metadata::NamespaceConfig namespaceConfig;
    namespaceConfig.maxInodes = impl.config.maxInodes;
    namespaceConfig.maxNamespaceBytes = impl.config.maxNamespaceBytes;
    namespaceConfig.extentBytes = impl.config.extentMb << 20;
    namespaceConfig.placement = impl.config.placement == "round_robin"
                                    ? metadata::PlacementPolicy::RoundRobin
                                    : metadata::PlacementPolicy::CapacityWeighted;
    namespaceConfig.maxReserveExtents = std::max<std::uint64_t>(1, impl.config.preallocExtents);
    if (namespaceConfig.maxReserveExtents > 64) {
        namespaceConfig.maxReserveExtents = 64; /* uuse.md §9.4 per-transaction cap */
    }
    const std::uint32_t metaBlock = impl.config.metadataBlockSize;
    if (metaBlock == 0 || impl.config.extentMb == 0 ||
        namespaceConfig.extentBytes % metaBlock != 0) {
        runtime.stop();
        co_return makeError(Status::InvalidArgument,
                            "extent size is not a multiple of the metadata block size");
    }
    const std::uint64_t blocksPerExtent = namespaceConfig.extentBytes / metaBlock;
    for (std::size_t i = 0;
         i < impl.config.devices.size() && i < volume.value().devices.size(); ++i) {
        const DeviceConfig &device = impl.config.devices[i];
        const bool isData =
            std::find(impl.config.dataDevices.begin(), impl.config.dataDevices.end(),
                      device.id) != impl.config.dataDevices.end();
        if (!isData) {
            continue;
        }
        const DeviceGeometry &geometry = volume.value().devices[i];
        if ((geometry.dataStart * metaBlock) % namespaceConfig.extentBytes != 0) {
            runtime.stop();
            co_return makeError(Status::Corrupt, "data region is not extent aligned");
        }
        const std::uint64_t extents = geometry.dataBlocks / blocksPerExtent;
        if (extents == 0) {
            runtime.stop();
            co_return makeError(Status::InvalidArgument, "device has no data extents");
        }
        namespaceConfig.deviceExtents.push_back(extents);
        namespaceConfig.deviceStartExtents.push_back(geometry.dataStart * metaBlock /
                                                     namespaceConfig.extentBytes);
    }
    auto namespaceState = metadata::Namespace::create(namespaceConfig);
    if (!namespaceState.ok()) {
        runtime.stop();
        co_return makeError(namespaceState.status(), namespaceState.message());
    }
    impl.namespaceState = std::move(namespaceState).value();
    /* P2-2(a): keep checkpoint-freeze attribution in the engine so the
     * metadata layer stays free of stats/trace dependencies. */
    impl.namespaceState->setCheckpointFreezeObserver(
        [](const metadata::Namespace::CheckpointFreezeStats &freeze) {
            stats::counters().syncCheckpointFreeze.record(freeze.freezeNs);
            trace::event(trace::EventId::ChkptFreeze, freeze.txid, freeze.inodes,
                         freeze.entries);
        });
    /* §7.3: install the namespace lock-wait hook only while tracing is active
     * so the metadata fast path stays a single null check otherwise. */
    if (impl.config.trace) {
        impl.namespaceState->setMetaLockWaitObserver(
            [](void *, std::uint64_t kind, std::uint64_t waitNs) {
                trace::event(trace::EventId::MetaLockWait, kind, waitNs);
            },
            nullptr);
    }

    /* Mount the metadata journal against the resident checkpoint target. */
    impl.metadataTarget = std::make_unique<NamespaceJournalTarget>(*impl.namespaceState);
    const auto metadataDevice = std::find_if(
        impl.config.devices.begin(), impl.config.devices.end(),
        [](const DeviceConfig &device) { return device.metadata; });
    if (metadataDevice == impl.config.devices.end()) {
        impl.metadataTarget.reset();
        impl.namespaceState.reset();
        runtime.stop();
        co_return makeError(Status::Corrupt, "configured metadata device set is empty");
    }
    const std::size_t metadataIndex =
        static_cast<std::size_t>(metadataDevice - impl.config.devices.begin());
    auto journal = Journal::open(*this, impl.metadataTarget.get(), metadataIndex);
    if (!journal.ok()) {
        impl.metadataTarget.reset();
        impl.namespaceState.reset();
        runtime.stop();
        co_return makeError(journal.status(), "metadata journal open failed: " +
                                               journal.message());
    }
    impl.journal = std::move(journal).value();
    auto replay = impl.journal->replay();
    if (!replay.ok()) {
        impl.journal.reset();
        impl.metadataTarget.reset();
        impl.namespaceState.reset();
        runtime.stop();
        co_return makeError(replay.status(), "metadata journal replay failed: " +
                                                replay.message());
    }
    /* Everything replayed from the checkpoint + journal is durable. */
    auto replayedEpoch =
        impl.namespaceState->publishDurableEpoch(impl.namespaceState->logicalEpoch());
    if (!replayedEpoch.ok()) {
        impl.journal.reset();
        impl.metadataTarget.reset();
        impl.namespaceState.reset();
        runtime.stop();
        co_return makeError(replayedEpoch.status(),
                            "replayed durable epoch publish failed: " +
                                replayedEpoch.message());
    }

    /* Shared DMA arena and data plane (bounded by [limits] dma_arena_mb). */
    impl.dmaArena = std::make_unique<DmaArena>();
    impl.dmaArena->configure(impl.config.dmaArenaMb << 20, dataDmaAlloc, dataDmaFree,
                             nullptr);
    IoPlanner::Config plannerConfig;
    plannerConfig.blockSize = metaBlock;
    plannerConfig.maxChildBytes = 1U << 20;
    plannerConfig.maxChildren = 64;
    /* Use the native-sector planner only when every mounted device has the
     * same geometry; the baseline is one 512-byte-sector NVMe. Writes planned
     * at sector granularity avoid read-modify-write for aligned partial writes
     * (for example InnoDB redo blocks). */
    std::uint32_t nativeBlockSize = 0;
    for (std::size_t index = 0; index < runtime.deviceCount(); ++index) {
        const std::uint32_t deviceBlockSize = runtime.deviceBlockSize(index);
        if (index == 0) {
            nativeBlockSize = deviceBlockSize;
        } else if (deviceBlockSize != nativeBlockSize) {
            nativeBlockSize = 0;
            break;
        }
    }
    DataPath::Config pathConfig;
    pathConfig.reactorIndex = 0;
    pathConfig.timeoutMs = static_cast<std::uint32_t>(impl.config.metadataWaitTimeoutMs);
    pathConfig.nativeBlockSize = nativeBlockSize;
    impl.dataPlane = std::make_unique<DataPlane>(runtime, *impl.dmaArena, plannerConfig,
                                                 pathConfig);

    /* Build the bounded actor-local asynchronous data path after the shared
     * arena and resident namespace are ready. */
    const std::uint32_t shardCount = runtime.dataReactorCount();
    if (shardCount == 0) {
        impl.dataPlane.reset();
        impl.dmaArena.reset();
        impl.journal.reset();
        impl.metadataTarget.reset();
        impl.namespaceState.reset();
        runtime.stop();
        co_return makeError(Status::InvalidState,
                            "SPDK runtime has no data reactors for async data service");
    }

    AsyncDataServiceConfig asyncConfig;
    asyncConfig.shardCount = shardCount;
    asyncConfig.ringDepth = std::max<std::size_t>(1, impl.config.ringDepth);
    asyncConfig.maxOperations = std::max<std::size_t>(1, impl.config.operationPool);
    const std::uint64_t configuredInodes = std::min<std::uint64_t>(
        impl.config.maxInodes, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()));
    const std::uint64_t inodeShardCeiling = configuredInodes == 0
                                                ? 0
                                                : 1 + (configuredInodes - 1) /
                                                          static_cast<std::uint64_t>(shardCount);
    asyncConfig.maxInodesPerShard = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::min<std::uint64_t>(
            inodeShardCeiling, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))));
    asyncConfig.maxPreparingPerShard = asyncConfig.maxOperations;
    asyncConfig.nativeReadBlockSize = nativeBlockSize;
    asyncConfig.planner = plannerConfig;

    /* Keep the backend credit bounded while allowing each shard to use its
     * proportional share of the mount-level operation budget. */
    const std::size_t maxInFlight = std::max<std::size_t>(
        1, (asyncConfig.maxOperations + static_cast<std::size_t>(shardCount) - 1) /
               static_cast<std::size_t>(shardCount));
    impl.asyncBackends.clear();
    impl.asyncBackendViews.clear();
    impl.asyncPollers.clear();
    impl.asyncPollersRegistered.clear();
    impl.asyncBackends.reserve(shardCount);
    impl.asyncBackendViews.reserve(shardCount);
    impl.asyncPollers.resize(shardCount);
    impl.asyncPollersRegistered.assign(shardCount, 0);

    auto rollbackAsyncMount = [&]() {
        /* Poller contexts must remain alive until every registration is gone. */
        bool pollerClearFailed = false;
        for (std::size_t index = impl.asyncPollersRegistered.size(); index > 0; --index) {
            const std::size_t pollerIndex = index - 1;
            if (impl.asyncPollersRegistered[pollerIndex] == 0) {
                continue;
            }
            auto cleared = runtime.clearDataReactorPoller(
                static_cast<std::uint32_t>(pollerIndex), &impl.asyncPollers[pollerIndex]);
            if (!cleared.ok()) {
                pollerClearFailed = true;
            }
            impl.asyncPollersRegistered[pollerIndex] = 0;
        }
        /* A failed detach is conservatively synchronized by stopping the
         * runtime before releasing any callback-visible context. */
        if (pollerClearFailed) {
            (void)runtime.stop();
        }
        impl.asyncService.reset();
        impl.asyncBackendViews.clear();
        impl.asyncBackends.clear();
        impl.asyncPollers.clear();
        impl.asyncPollersRegistered.clear();
        impl.dataPlane.reset();
        impl.dmaArena.reset();
        impl.journal.reset();
        impl.metadataTarget.reset();
        impl.namespaceState.reset();
        if (!pollerClearFailed) {
            (void)runtime.stop();
        }
    };

    for (std::uint32_t index = 0; index < shardCount; ++index) {
        auto backend = std::unique_ptr<SpdkAsyncDataBackend>(new (std::nothrow)
            SpdkAsyncDataBackend(runtime, index, metaBlock, maxInFlight));
        if (backend == nullptr || !backend->valid()) {
            rollbackAsyncMount();
            co_return makeError(backend == nullptr ? Status::NoMemory : Status::InvalidState,
                                backend == nullptr ? "cannot allocate async data backend"
                                                    : "cannot initialize async data backend");
        }
        impl.asyncBackendViews.push_back(backend.get());
        impl.asyncBackends.push_back(std::move(backend));
    }

    impl.asyncService = std::unique_ptr<AsyncDataService>(
        new (std::nothrow) AsyncDataService(asyncConfig, *impl.dmaArena,
                                            std::span<AsyncDataBackend *>(
                                                impl.asyncBackendViews.data(),
                                                impl.asyncBackendViews.size())));
    if (impl.asyncService == nullptr || !impl.asyncService->valid()) {
        rollbackAsyncMount();
        co_return makeError(Status::NoMemory,
                            "cannot initialize async data service");
    }

    for (std::uint32_t index = 0; index < shardCount; ++index) {
        impl.asyncPollers[index] = AsyncDataPollerContext{impl.asyncService.get(), index};
        auto registered = runtime.setDataReactorPoller(index, &pumpAsyncDataShard,
                                                       &impl.asyncPollers[index]);
        if (!registered.ok()) {
            rollbackAsyncMount();
            co_return makeError(registered.status(),
                                "cannot register async data reactor poller: " +
                                    registered.message());
        }
        impl.asyncPollersRegistered[index] = 1;
    }

    impl.formatted = true;
    impl.uuid = volume.value().uuid;
    impl.mounted = true;
    traceGuard.release();
    log::info("engine mounted: generation=%llu clean=%d",
              static_cast<unsigned long long>(volume.value().generation),
              volume.value().cleanShutdown ? 1 : 0);
    co_return Result<void>(Status::Ok);
}

Task<Result<void>> Engine::drain() {
    if (!processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (impl_->asyncService != nullptr && impl_->asyncService->outstanding() != 0) {
        co_return makeError(Status::Busy,
                            "async data operations are still outstanding");
    }
    co_return Result<void>(Status::Ok);
}

Task<Result<void>> Engine::stop() {
    auto &impl = *impl_;
    if (!processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (impl.stopped) {
        co_return Result<void>(Status::Ok);
    }
    /* Active clients/open files keep the namespace alive; callers must close
     * them first (uuse.md §14.3). */
    if (impl.metadataUsers.load(std::memory_order_acquire) != 0) {
        co_return Result<void>(Status::Busy, "clients or open files are still active");
    }
    if (impl.mounted) {
        if (impl.asyncService != nullptr && impl.asyncService->outstanding() != 0) {
            co_return makeError(Status::Busy,
                                "async data operations are still outstanding");
        }
        /* No active clients remain here, so publish a clean lifecycle state
         * before releasing the runtime that owns the control path. */
        auto markedClean = updateCleanShutdown(SpdkRuntime::instance(), impl.config, true);
        if (!markedClean.ok()) {
            co_return markedClean;
        }

        /* Pollers must be detached before their service and backend contexts
         * are destroyed; a failed detach leaves the mount intact for retry. */
        auto &runtime = SpdkRuntime::instance();
        for (std::size_t index = impl.asyncPollersRegistered.size(); index > 0; --index) {
            const std::size_t pollerIndex = index - 1;
            if (impl.asyncPollersRegistered[pollerIndex] == 0) {
                continue;
            }
            auto cleared = runtime.clearDataReactorPoller(
                static_cast<std::uint32_t>(pollerIndex), &impl.asyncPollers[pollerIndex]);
            if (!cleared.ok()) {
                co_return makeError(cleared.status(),
                                    "cannot clear async data reactor poller: " +
                                        cleared.message());
            }
            impl.asyncPollersRegistered[pollerIndex] = 0;
        }
        if (impl.asyncService != nullptr) {
            const auto asyncStats = impl.asyncService->stats();
            log::info(
                "async closure: accepted=%llu terminal=%llu published=%llu "
                "dropped=%llu scheduler_wait=%llu child_submit=%llu child_complete=%llu "
                "child_rejected=%llu plan_fail=%llu dma_fail=%llu preparing=%llu",
                static_cast<unsigned long long>(asyncStats.accepted),
                static_cast<unsigned long long>(asyncStats.terminal),
                static_cast<unsigned long long>(asyncStats.completionPublished),
                static_cast<unsigned long long>(asyncStats.completionDropped),
                static_cast<unsigned long long>(asyncStats.schedulerWaits),
                static_cast<unsigned long long>(asyncStats.childSubmits),
                static_cast<unsigned long long>(asyncStats.childCompletes),
                static_cast<unsigned long long>(asyncStats.childSubmitRejected),
                static_cast<unsigned long long>(asyncStats.planFailures),
                static_cast<unsigned long long>(asyncStats.dmaFailures),
                static_cast<unsigned long long>(asyncStats.preparing));
        }
        impl.asyncService.reset();
        impl.asyncBackendViews.clear();
        impl.asyncBackends.clear();
        impl.asyncPollers.clear();
        impl.asyncPollersRegistered.clear();
        if (impl.journal != nullptr) {
            auto flushed = impl.journal->flush();
            if (!flushed.ok()) {
                log::error("journal flush on stop failed: %s", flushed.message().c_str());
            }
        }
        impl.journal.reset();
        impl.metadataTarget.reset();
        impl.dataPlane.reset();
        impl.dmaArena.reset();
        auto stopped = SpdkRuntime::instance().stop();
        if (!stopped.ok()) {
            co_return stopped;
        }
        impl.mounted = false;
    }
    impl.namespaceState.reset();
    impl.dataPlane.reset();
    impl.dmaArena.reset();
    if (impl.lockFd >= 0) {
        ::flock(impl.lockFd, LOCK_UN);
        ::close(impl.lockFd);
        impl.lockFd = -1;
    }
    impl.stopped = true;
    trace::setEnabled(false);
    trace::stop();
    stats::setEnabled(false);
    /* Diagnostic dump used to verify the fsync coverage contract. */
    if (const char *flag = std::getenv("UUSE_DIAG_SYNC_STATS"); flag != nullptr && *flag != '\0') {
        const auto &counters = stats::counters();
        log::info("sync stats: calls=%llu early=%llu joiners=%llu barriers=%llu flushes=%llu "
                  "flush_skips=%llu checkpoints=%llu journal_txns=%llu",
                  static_cast<unsigned long long>(counters.syncCalls.load()),
                  static_cast<unsigned long long>(counters.syncEarlyReturns.load()),
                  static_cast<unsigned long long>(counters.syncJoiners.load()),
                  static_cast<unsigned long long>(counters.syncBarriers.load()),
                  static_cast<unsigned long long>(counters.deviceFlushes.load()),
                  static_cast<unsigned long long>(counters.deviceFlushSkips.load()),
                  static_cast<unsigned long long>(counters.journalCheckpoints.load()),
                  static_cast<unsigned long long>(counters.journalTransactions.load()));
        const auto averageUs = [](const stats::PhaseTimer &timer) {
            const std::uint64_t count = timer.count.load();
            return std::to_string(count == 0 ? 0 : timer.totalNs.load() / count / 1000);
        };
        const auto maxUs = [](const stats::PhaseTimer &timer) {
            return std::to_string(timer.maxNs.load() / 1000);
        };
        log::info("sync phases us avg/max: barrier=%s/%s device_flush=%s/%s ckpt_freeze=%s/%s "
                  "ckpt_snapshot=%s/%s meta_write=%s/%s ckpt_flush=%s/%s",
                  averageUs(counters.syncBarrier).c_str(), maxUs(counters.syncBarrier).c_str(),
                  averageUs(counters.syncDeviceFlush).c_str(),
                  maxUs(counters.syncDeviceFlush).c_str(),
                  averageUs(counters.syncCheckpointFreeze).c_str(),
                  maxUs(counters.syncCheckpointFreeze).c_str(),
                  averageUs(counters.syncCheckpointSnapshot).c_str(),
                  maxUs(counters.syncCheckpointSnapshot).c_str(),
                  averageUs(counters.syncCheckpointWrite).c_str(),
                  maxUs(counters.syncCheckpointWrite).c_str(),
                  averageUs(counters.syncCheckpointFlush).c_str(),
                  maxUs(counters.syncCheckpointFlush).c_str());
    }
    log::info("engine stopped");
    co_return Result<void>(Status::Ok);
}

/* Implements the durable ordering: data flush, staged metadata snapshot,
 * checkpoint/superblock flush, and finally in-memory epoch publication. */
Result<void> Engine::syncMetadata(std::uint64_t epoch) {
    if (!processValid()) {
        return makeError(Status::ForkedProcess, "engine belongs to another process");
    }
    if (!impl_->mounted || impl_->namespaceState == nullptr || impl_->journal == nullptr) {
        return makeError(Status::InvalidState, "engine durability path is not mounted");
    }
    if (epoch > impl_->namespaceState->logicalEpoch()) {
        return makeError(Status::InvalidArgument, "sync epoch is ahead of logical epoch");
    }
    stats::counters().syncCalls.fetch_add(1, std::memory_order_relaxed);
    while (true) {
        std::shared_ptr<Impl::SyncBatch> batch;
        bool leader = false;
        {
            std::unique_lock<std::mutex> syncLock(impl_->syncMutex);
            /* A repeated barrier needs no checkpoint when the epoch is already
             * published, and no device flush when no completed data write is
             * pending (uuse.md 10.2/20.3). */
            if (epoch <= impl_->namespaceState->durableEpoch() &&
                !SpdkRuntime::instance().dataFlushPending()) {
                stats::counters().syncEarlyReturns.fetch_add(1, std::memory_order_relaxed);
                trace::event(trace::EventId::FsyncEnd, epoch,
                             static_cast<std::uint64_t>(Status::Ok), 1ULL);
                return Result<void>(Status::Ok);
            }
            if (impl_->syncBatch != nullptr && !impl_->syncBatch->completed) {
                batch = impl_->syncBatch;
                batch->targetEpoch = std::max(batch->targetEpoch, epoch);
            } else {
                batch = std::make_shared<Impl::SyncBatch>();
                batch->targetEpoch = epoch;
                impl_->syncBatch = batch;
                leader = true;
            }
        }

        if (!leader) {
            std::unique_lock<std::mutex> syncLock(impl_->syncMutex);
            batch->condition.wait(syncLock, [&batch] { return batch->completed; });
            stats::counters().syncJoiners.fetch_add(1, std::memory_order_relaxed);
            if (epoch <= batch->frozenTargetEpoch &&
                !SpdkRuntime::instance().dataFlushPending()) {
                trace::event(trace::EventId::FsyncEnd, epoch,
                             static_cast<std::uint64_t>(batch->status), 2ULL);
                return Result<void>(batch->status, batch->message);
            }
            /* This waiter arrived after the batch target was frozen, or a data
             * write completed after the batch flush. It must become the next
             * leader rather than being reported durable early. */
            continue;
        }

        /* Let concurrent sync callers join the same full-snapshot transaction.
         * This is a coalescing window, never a durability timeout. */
        if (impl_->config.maxGroupCommitWaitUs != 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(
                impl_->config.maxGroupCommitWaitUs));
        }
        std::uint64_t targetEpoch = epoch;
        {
            std::lock_guard<std::mutex> syncLock(impl_->syncMutex);
            targetEpoch = batch->targetEpoch;
            batch->frozenTargetEpoch = targetEpoch;
        }

        Result<void> result(Status::Ok);
        stats::counters().syncBarriers.fetch_add(1, std::memory_order_relaxed);
        const auto barrierStart = std::chrono::steady_clock::now();
        trace::event(trace::EventId::FsyncBegin, targetEpoch,
                     static_cast<std::uint64_t>(epoch));
        const bool checkpointNeeded = targetEpoch > impl_->namespaceState->durableEpoch();
        bool prepared = false;
        /* Phase 1 (metadata, uuse.md §9.3): namespace-shape changes and
         * journal compaction write the full snapshot; journaled mutations are
         * appended after the data flush so a failed flush cannot leave durable
         * metadata ahead of non-durable data. */
        const bool checkpointPath =
            checkpointNeeded && (impl_->namespaceState->checkpointRequired() ||
                                 impl_->journal->needsCheckpoint());
        const auto checkpointPhase = [&]() {
            /* Namespace shape changes cannot be replayed from the journal record
             * format, so they force a full base; journal-backed deltas only
             * cover windows without shape changes (uuse.md §10.4 b). */
            const bool shapeChanged = impl_->namespaceState->checkpointRequired();
            if (shapeChanged) {
                impl_->journal->requireFullCheckpoint();
            }
            const bool fullCheckpoint =
                shapeChanged || impl_->journal->nextCheckpointWillBeFull();
            auto staged = impl_->namespaceState->takeStagedRecords();
            impl_->namespaceState->clearPendingRecords();
            if (!fullCheckpoint && !staged.records.empty()) {
                auto appended = impl_->journal->append(staged.records,
                                                       /*flushDevice=*/false,
                                                       /*applyToTarget=*/false);
                if (!appended.ok()) {
                    impl_->namespaceState->requireCheckpoint();
                    result = makeError(appended.status(), appended.message());
                    return;
                }
                stats::counters().journalTransactions.fetch_add(1,
                                                                std::memory_order_relaxed);
            }
            auto durable = impl_->namespaceState->prepareDurableEpoch(targetEpoch);
            if (!durable.ok()) {
                impl_->namespaceState->requireCheckpoint();
                result = durable;
                return;
            }
            prepared = true;
            auto checkpoint = impl_->journal->checkpoint(/*deferFlush=*/true);
            stats::counters().journalCheckpoints.fetch_add(1, std::memory_order_relaxed);
            if (!checkpoint.ok()) {
                impl_->namespaceState->rollbackPreparedDurableEpoch();
                prepared = false;
                /* Staged records were consumed by this attempt; the next
                 * barrier must retry the snapshot. */
                impl_->namespaceState->requireCheckpoint();
                result = checkpoint;
            }
        };
        if (checkpointPath) {
            checkpointPhase();
        }
        /* Phase 2 (journal): append the staged metadata records; the single
         * device flush below makes data, journal slots and checkpoint records
         * durable together, so the barrier still needs one round trip. */
        if (result.ok() && checkpointNeeded && !checkpointPath) {
            auto batch = impl_->namespaceState->takeStagedRecords();
            if (!batch.records.empty()) {
                auto appended = impl_->journal->append(batch.records,
                                                       /*flushDevice=*/false,
                                                       /*applyToTarget=*/false);
                if (!appended.ok()) {
                    /* The staged records are gone; the next barrier snapshots. */
                    impl_->namespaceState->requireCheckpoint();
                    result = makeError(appended.status(), appended.message());
                } else {
                    stats::counters().journalTransactions.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }
        /* Phase 3 (durability): one device flush covers every write completed
         * before it was submitted - user data, journal slots, the checkpoint
         * record and the superblock (uuse.md 10.2/20.3). */
        if (result.ok()) {
            const auto deviceFlushStart = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < impl_->config.devices.size(); ++i) {
                result = flushDevice(i);
                if (!result.ok()) {
                    break;
                }
            }
            stats::counters().syncDeviceFlush.record(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - deviceFlushStart)
                        .count()));
        }
        if (result.ok()) {
            test_hooks::crashIfRequested("metadata_commit");
            if (testMetadataCommitFailureRequested()) {
                result = makeError(Status::IoError,
                                   "test-injected metadata commit boundary failure");
            }
        }
        if (result.ok() && checkpointNeeded) {
            result = impl_->namespaceState->publishDurableEpoch(targetEpoch);
        }
        if (!result.ok() && prepared) {
            /* A failed barrier must not leave staged durable sizes behind. */
            impl_->namespaceState->rollbackPreparedDurableEpoch();
        }

        if (!result.ok()) {
            log::error("sync barrier failed: status=%u message=%s",
                       static_cast<unsigned>(result.status()), result.message().c_str());
        }
        stats::counters().syncBarrier.record(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - barrierStart)
                                           .count()));
        {
            std::lock_guard<std::mutex> syncLock(impl_->syncMutex);
            batch->status = result.status();
            batch->message = result.message();
            batch->completed = true;
        }
        batch->condition.notify_all();
        trace::event(trace::EventId::FsyncEnd, targetEpoch,
                     static_cast<std::uint64_t>(result.status()), 3ULL);
        return result;
    }
}

Result<std::vector<MetadataJournalScan>> Engine::scanMetadataJournals() {
    if (!processValid()) {
        return makeError<std::vector<MetadataJournalScan>>(
            Status::ForkedProcess, "engine belongs to another process");
    }
    if (impl_->stopped || impl_->mounted) {
        return makeError<std::vector<MetadataJournalScan>>(
            Status::InvalidState, "scan-only journal check requires a fresh engine");
    }

    auto &runtime = SpdkRuntime::instance();
    auto started = runtime.start(impl_->config);
    if (!started.ok()) {
        return makeError<std::vector<MetadataJournalScan>>(started.status(), started.message());
    }
    auto volume = mountVolume(runtime, impl_->config);
    if (!volume.ok()) {
        runtime.stop();
        return makeError<std::vector<MetadataJournalScan>>(volume.status(), volume.message());
    }
    impl_->geometries = volume.value().devices;
    impl_->uuid = volume.value().uuid;
    impl_->formatted = true;

    std::vector<MetadataJournalScan> scans;
    for (std::size_t i = 0; i < impl_->config.devices.size(); ++i) {
        if (!impl_->config.devices[i].metadata) continue;
        MetadataJournalScan scan;
        scan.deviceIndex = i;
        auto journal = Journal::open(*this, nullptr, i);
        if (!journal.ok()) {
            scan.error = journal.message();
            scans.push_back(std::move(scan));
            continue;
        }
        auto replay = journal.value()->replay();
        scan.stats = journal.value()->stats();
        scan.usageRatio = journal.value()->usageRatio();
        if (!replay.ok()) {
            scan.error = replay.message();
        }
        scans.push_back(std::move(scan));
    }
    runtime.stop();
    return scans;
}

const Config &Engine::config() const noexcept { return impl_->config; }

bool Engine::mounted() const noexcept { return impl_->mounted; }

bool Engine::processValid() const noexcept {
    return impl_ != nullptr && impl_->ownerPid != 0 &&
           !g_forkedChild.load(std::memory_order_acquire) &&
           impl_->ownerPid == cachedProcessId();
}

bool Engine::formatted() const noexcept { return impl_->formatted; }

metadata::Namespace *Engine::metadataNamespace() const noexcept {
    return impl_->namespaceState.get();
}

DataPlane *Engine::dataPlane() const noexcept { return impl_->dataPlane.get(); }

AsyncDataService *Engine::asyncDataService() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->asyncService.get();
}

void Engine::retainMetadataUser() noexcept {
    impl_->metadataUsers.fetch_add(1, std::memory_order_acq_rel);
}

void Engine::releaseMetadataUser() noexcept {
    impl_->metadataUsers.fetch_sub(1, std::memory_order_acq_rel);
}

std::array<std::uint8_t, 16> Engine::volumeUuid() const noexcept { return impl_->uuid; }

std::vector<DeviceGeometry> Engine::deviceGeometries() const { return impl_->geometries; }

/* Exposes allocator-backed free data capacity to frontend adapters. */
std::uint64_t Engine::freeDataBytes() const noexcept {
    if (impl_ == nullptr || impl_->namespaceState == nullptr) {
        return 0;
    }
    return impl_->namespaceState->freeDataBytes();
}

/* Reports namespace and per-device allocator counters for extent diagnostics. */
Result<Engine::DataExtentStats> Engine::dataExtentStats() const {
    if (impl_ == nullptr || !impl_->mounted || impl_->namespaceState == nullptr) {
        return makeError<DataExtentStats>(Status::InvalidState,
                                          "engine durability path is not mounted");
    }
    const metadata::Namespace::Stats ns = impl_->namespaceState->stats();
    const metadata::Namespace::ExtentPoolStats pool =
        impl_->namespaceState->extentPoolStats();
    DataExtentStats stats;
    stats.inodes = ns.inodes;
    stats.orphanInodes = ns.orphanInodes;
    stats.allocatedExtents = ns.allocatedExtents;
    stats.freedExtents = ns.freedExtents;
    stats.totalExtents = pool.total;
    stats.freeExtents = pool.free;
    stats.reservedExtents = pool.reserved;
    stats.initializedExtents = pool.initialized;
    stats.committedExtents = pool.committed;
    return stats;
}

/* Acquires one contiguous buffer from the mount-level DMA arena. */
Result<std::span<std::byte>> Engine::acquireDmaBuffer(std::size_t size,
                                                      std::uint32_t timeoutMs) {
    if (impl_ == nullptr || impl_->dmaArena == nullptr) {
        return makeError<std::span<std::byte>>(Status::InvalidState, "DMA arena is not active");
    }
    return impl_->dmaArena->acquire(size, timeoutMs);
}

/* Returns a buffer to the mount-level DMA arena. */
void Engine::releaseDmaBuffer(void *buffer, std::size_t size) noexcept {
    if (impl_ == nullptr || impl_->dmaArena == nullptr || buffer == nullptr || size == 0) {
        return;
    }
    impl_->dmaArena->release(std::span<std::byte>(static_cast<std::byte *>(buffer), size));
}

Result<void> Engine::readDeviceBlocks(std::size_t deviceIndex, std::uint64_t offsetBlocks,
                                      std::span<std::byte> buffer) {
    if (!processValid()) {
        return makeError(Status::ForkedProcess, "engine belongs to another process");
    }
    if (!SpdkRuntime::instance().active()) {
        return makeError(Status::InvalidState, "engine is not mounted");
    }
    return SpdkRuntime::instance().controlRead(deviceIndex, offsetBlocks, buffer);
}

Result<void> Engine::writeDeviceBlocks(std::size_t deviceIndex, std::uint64_t offsetBlocks,
                                       std::span<const std::byte> buffer) {
    if (!processValid()) {
        return makeError(Status::ForkedProcess, "engine belongs to another process");
    }
    if (!SpdkRuntime::instance().active()) {
        return makeError(Status::InvalidState, "engine is not mounted");
    }
    return SpdkRuntime::instance().controlWrite(deviceIndex, offsetBlocks, buffer);
}

Result<void> Engine::flushDevice(std::size_t deviceIndex) {
    if (!processValid()) {
        return makeError(Status::ForkedProcess, "engine belongs to another process");
    }
    if (!SpdkRuntime::instance().active()) {
        return makeError(Status::InvalidState, "engine is not mounted");
    }
    return SpdkRuntime::instance().controlFlush(deviceIndex);
}

std::uint32_t Engine::deviceBlockSize(std::size_t deviceIndex) const noexcept {
    return SpdkRuntime::instance().deviceBlockSize(deviceIndex);
}

std::uint64_t Engine::deviceCapacityBytes(std::size_t deviceIndex) const noexcept {
    return SpdkRuntime::instance().deviceCapacityBytes(deviceIndex);
}

Result<SuperblockSlots> Engine::readSuperblockSlots(std::size_t deviceIndex) const {
    if (!processValid()) {
        return makeError<SuperblockSlots>(Status::ForkedProcess,
                                          "engine belongs to another process");
    }
    auto &runtime = SpdkRuntime::instance();
    if (!runtime.active()) {
        return makeError<SuperblockSlots>(Status::InvalidState, "engine is not mounted");
    }
    const std::uint32_t sectorSize = runtime.deviceBlockSize(deviceIndex);
    if (sectorSize == 0 || impl_->config.metadataBlockSize == 0) {
        return makeError<SuperblockSlots>(Status::InvalidArgument,
                                          "device index out of range");
    }
    std::vector<std::byte> record(impl_->config.metadataBlockSize, std::byte{0});
    auto readA = runtime.controlRead(deviceIndex, 0, record);
    if (!readA.ok()) {
        return makeError<SuperblockSlots>(readA.status(), readA.message());
    }
    SuperblockSlots slots;
    std::memcpy(&slots.slotA, record.data(), sizeof(slots.slotA));
    slots.validA = superblockValid(slots.slotA);
    const std::uint64_t slotBOffset = kSuperblockSlotOffsetBytes / sectorSize;
    auto readB = runtime.controlRead(deviceIndex, slotBOffset, record);
    if (!readB.ok()) {
        return makeError<SuperblockSlots>(readB.status(), readB.message());
    }
    std::memcpy(&slots.slotB, record.data(), sizeof(slots.slotB));
    slots.validB = superblockValid(slots.slotB);
    return slots;
}

std::uint32_t Engine::dataReactorCount() const noexcept {
    return SpdkRuntime::instance().dataReactorCount();
}

std::size_t Engine::channelCount() const noexcept {
    return SpdkRuntime::instance().channelCount();
}

Result<void> Engine::postToDataReactor(std::uint32_t index, std::function<void()> fn) {
    if (!processValid()) {
        return makeError(Status::ForkedProcess, "engine belongs to another process");
    }
    return SpdkRuntime::instance().postToDataReactor(index, std::move(fn));
}

std::uint64_t Engine::dataReactorThreadId(std::uint32_t index) const {
    return SpdkRuntime::instance().dataReactorThreadId(index);
}

std::uint32_t coreAbiMajor() noexcept {
#ifdef UUSE_CORE_ABI_MAJOR
    return UUSE_CORE_ABI_MAJOR;
#else
    return 0;
#endif
}

} // namespace uuse
