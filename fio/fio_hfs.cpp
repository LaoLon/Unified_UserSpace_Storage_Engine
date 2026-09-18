/* HFS 2.0 fio external engine with bounded asynchronous completions. */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

#include "fio.h"
#include "optgroup.h"

/* fio's C helpers define min/max macros that must not leak into C++ headers. */
#undef min
#undef max

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/file.hpp"
#include "fio_completion_hub.hpp"
#include "preallocation_single_flight.hpp"

namespace {

struct FioHfsOptions {
    void *padding = nullptr; /* fio requires the first option offset to be non-zero. */
    char *config = nullptr;
    unsigned int direct = 1;
    unsigned int preallocate = 1;
    unsigned int clientDepth = 32;
    unsigned int async = 1;
    unsigned int filePerJob = 0;
};

enum class PendingState : std::uint8_t {
    Free,
    Submitted,
    Reaped,
};

/* One frontend-owned slot.  Only value tokens cross the Client boundary. */
struct FioHfsPending {
    PendingState state = PendingState::Free;
    std::uint32_t generation = 0;
    std::uint64_t requestId = 0;
    io_u *io = nullptr;
    std::shared_ptr<hfs::File> file;
    hfs::IoHandle handle;
    hfs::CompletionEvent completion{};
    std::shared_ptr<hfs::Engine> bounceOwner;
    std::byte *bounce = nullptr;
    std::size_t bounceSize = 0;
    std::size_t bounceAllocationSize = 0;
};

/* The current getevents batch stores slot identities, never core pointers. */
struct FioHfsReported {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    std::uint64_t requestId = 0;
};

struct FioHfsFile {
    std::shared_ptr<hfs::File> file;
};

struct FioHfsThread {
    std::shared_ptr<hfs::Engine> engine;
    std::shared_ptr<hfs::Client> client;
    bool direct = true;
    bool async = true;
    bool closing = false;
    bool protocolError = false;
    hfs::fio::FioCompletionHub::Endpoint *completionEndpoint = nullptr;
    std::uint32_t clientDepth = 1;
    std::size_t slotCount = 0;
    std::size_t submitted = 0;
    std::size_t reportedCount = 0;
    std::size_t reportedRemaining = 0;
    std::unique_ptr<FioHfsPending[]> pending;
    std::unique_ptr<FioHfsReported[]> reported;
    std::unique_ptr<hfs::CompletionEvent[]> polled;
};

/* FIO numjobs share one process lock while retaining one Client per worker. */
std::mutex processMutex;
std::shared_ptr<hfs::Engine> processEngine;
std::size_t processUsers = 0;
std::string processConfigPath;
std::uint64_t processEngineGeneration = 0;
std::uint64_t processDmaBytesReserved = 0;
std::atomic<std::uint64_t> processQueueQueued{0};
std::atomic<std::uint64_t> processQueueBusy{0};
std::atomic<std::uint64_t> processQueueCompletedError{0};
std::atomic<std::uint64_t> processEventsReaped{0};
std::atomic<std::uint64_t> processTokenMismatch{0};
std::atomic<std::uint64_t> processOutstanding{0};
std::atomic<std::uint64_t> processOutstandingPeak{0};
std::atomic<std::uint64_t> processPreallocationRequests{0};
std::atomic<std::uint64_t> processPreallocationOwners{0};
std::atomic<std::uint64_t> processPreallocationFailures{0};
hfs::fio::FioCompletionHub processCompletionHub;
bool processCompletionHubActive = false;

/* Shared-file preallocation is process-scoped and generation-scoped. */
hfs::fio::PreallocationSingleFlight processPreallocation(
    hfs::fio::PreallocationSingleFlight::kMaximumCapacity);

std::mutex dmaBufferMutex;
struct DmaBufferOwner {
    std::weak_ptr<hfs::Engine> engine;
    std::size_t allocationSize = 0;
    std::size_t reservedBytes = 0;
};
std::unordered_map<void *, DmaBufferOwner> dmaBufferOwners;

/* Preserve the parsed engine configuration for fio worker clones. */
int fioHfsConfigOption(void *, const char *value) {
    if (value == nullptr || value[0] == '\0') {
        return 1;
    }
    std::lock_guard<std::mutex> lock(processMutex);
    processConfigPath = value;
    return 0;
}

int fioError(hfs::Status status) {
    switch (status) {
    case hfs::Status::Busy:
    case hfs::Status::TryAgain:
        return EAGAIN;
    case hfs::Status::NoMemory:
        return ENOMEM;
    case hfs::Status::NoSpace:
        return ENOSPC;
    case hfs::Status::NotFound:
        return ENOENT;
    case hfs::Status::AlreadyExists:
        return EEXIST;
    case hfs::Status::InvalidArgument:
        return EINVAL;
    case hfs::Status::NotSupported:
        return ENOTSUP;
    case hfs::Status::Cancelled:
        return ECANCELED;
    case hfs::Status::TimedOut:
        return ETIMEDOUT;
    case hfs::Status::IoError:
        return EIO;
    default:
        return EIO;
    }
}

/* Keep diagnostics inside the module so C++ name mangling cannot affect fio symbols. */
void fioLog(const char *message) {
    std::fputs(message, stderr);
}

/* Track the process-wide effective queue depth without per-I/O logging. */
void recordQueued() noexcept {
    processQueueQueued.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t current =
        processOutstanding.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::uint64_t peak = processOutstandingPeak.load(std::memory_order_relaxed);
    while (peak < current &&
           !processOutstandingPeak.compare_exchange_weak(
               peak, current, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

/* One accepted event leaves frontend ownership exactly once. */
void recordReaped() noexcept {
    processEventsReaped.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t previous =
        processOutstanding.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0) {
        processOutstanding.store(0, std::memory_order_release);
        processTokenMismatch.fetch_add(1, std::memory_order_relaxed);
    }
}

/* Reset counters only when a fresh process-wide engine generation starts. */
void resetProcessCounters() noexcept {
    processQueueQueued.store(0, std::memory_order_relaxed);
    processQueueBusy.store(0, std::memory_order_relaxed);
    processQueueCompletedError.store(0, std::memory_order_relaxed);
    processEventsReaped.store(0, std::memory_order_relaxed);
    processTokenMismatch.store(0, std::memory_order_relaxed);
    processOutstanding.store(0, std::memory_order_relaxed);
    processOutstandingPeak.store(0, std::memory_order_relaxed);
    processPreallocationRequests.store(0, std::memory_order_relaxed);
    processPreallocationOwners.store(0, std::memory_order_relaxed);
    processPreallocationFailures.store(0, std::memory_order_relaxed);
}

/* Allocate fio's complete requested buffer from the shared HFS DMA arena. */
int fioHfsIomemAlloc(struct thread_data *td, size_t totalMem) {
    if (td == nullptr || totalMem == 0) {
        if (td != nullptr) td->orig_buffer = nullptr;
        return totalMem == 0 ? 0 : 1;
    }
    std::shared_ptr<hfs::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(processMutex);
        engine = processEngine;
    }
    if (engine == nullptr) {
        fioLog("fio-hfs: engine is not active for DMA arena allocation\n");
        return 1;
    }
    /* totalMem is fio's complete per-worker reservation.  It already includes
     * the requested io_u buffers; multiplying by iodepth double-counts memory. */
    const std::uint64_t bytes = static_cast<std::uint64_t>(totalMem);
    const std::uint64_t arenaMb = static_cast<std::uint64_t>(engine->config().dmaArenaMb);
    const std::uint64_t megabyte = 1024U * 1024U;
    const bool capOverflow = arenaMb > std::numeric_limits<std::uint64_t>::max() / megabyte;
    const std::uint64_t cap = capOverflow ? std::numeric_limits<std::uint64_t>::max()
                                          : arenaMb * megabyte;
    {
        std::lock_guard<std::mutex> lock(processMutex);
        const bool reservationOverflow =
            bytes > std::numeric_limits<std::uint64_t>::max() - processDmaBytesReserved;
        if (cap != 0 && (reservationOverflow ||
                         processDmaBytesReserved + bytes > cap)) {
            fioLog("fio-hfs: requested fio DMA arena exceeds mount cap\n");
            return 1;
        }
        processDmaBytesReserved += bytes;
    }
    auto acquired = engine->acquireDmaBuffer(totalMem, 0);
    if (!acquired.ok()) {
        std::lock_guard<std::mutex> lock(processMutex);
        processDmaBytesReserved -= bytes;
        if (acquired.status() == hfs::Status::Busy) {
            fioLog("fio-hfs: requested fio DMA arena exceeds mount cap\n");
        } else {
            fioLog("fio-hfs: complete DMA arena allocation failed\n");
        }
        return 1;
    }
    td->orig_buffer = reinterpret_cast<char *>(acquired.value().data());
    {
        std::lock_guard<std::mutex> lock(dmaBufferMutex);
        const auto inserted = dmaBufferOwners.emplace(
            td->orig_buffer, DmaBufferOwner{engine, acquired.value().size(), totalMem});
        if (inserted.second) {
            return 0;
        }
    }
    {
        engine->releaseDmaBuffer(td->orig_buffer, acquired.value().size());
        std::lock_guard<std::mutex> lock(processMutex);
        processDmaBytesReserved -= bytes;
        td->orig_buffer = nullptr;
        td->orig_buffer_size = 0;
    }
    fioLog("fio-hfs: duplicate DMA arena owner pointer\n");
    return 1;
}

/* Release only the single arena owned by fio for this worker. */
void fioHfsIomemFree(struct thread_data *td) {
    if (td == nullptr || td->orig_buffer == nullptr) {
        return;
    }
    DmaBufferOwner owner;
    {
        std::lock_guard<std::mutex> lock(dmaBufferMutex);
        auto found = dmaBufferOwners.find(td->orig_buffer);
        if (found != dmaBufferOwners.end()) {
            owner = found->second;
            dmaBufferOwners.erase(found);
        }
    }
    if (auto engine = owner.engine.lock()) {
        engine->releaseDmaBuffer(td->orig_buffer, owner.allocationSize);
    }
    if (owner.reservedBytes != 0) {
        std::lock_guard<std::mutex> lock(processMutex);
        processDmaBytesReserved = owner.reservedBytes > processDmaBytesReserved
                                       ? 0
                                       : processDmaBytesReserved - owner.reservedBytes;
    }
    td->orig_buffer = nullptr;
    td->orig_buffer_size = 0;
}

std::string fioPath(const char *name) {
    if (name == nullptr || name[0] == '\0') {
        return {};
    }
    std::string path(name);
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return path;
}

/* Translate an FIO timeout without overflowing the core duration type. */
std::chrono::microseconds fioTimeout(const struct timespec *timeout) noexcept {
    if (timeout == nullptr) {
        return std::chrono::hours(24);
    }
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0) {
        return std::chrono::microseconds(0);
    }
    constexpr std::uint64_t kMicrosPerSecond = 1000000U;
    const auto seconds = static_cast<std::uint64_t>(timeout->tv_sec);
    const auto nanos = static_cast<std::uint64_t>(timeout->tv_nsec);
    const auto maximum = static_cast<std::uint64_t>(
        std::chrono::microseconds::max().count());
    if (seconds > maximum / kMicrosPerSecond ||
        seconds * kMicrosPerSecond > maximum - nanos / 1000U) {
        return std::chrono::microseconds::max();
    }
    return std::chrono::microseconds(
        static_cast<std::int64_t>(seconds * kMicrosPerSecond + nanos / 1000U));
}

bool isQueueRetryable(hfs::Status status) noexcept {
    return status == hfs::Status::Busy || status == hfs::Status::TryAgain;
}

void setIoFailure(io_u *io, int error) noexcept {
    if (io == nullptr) {
        return;
    }
    io->error = error == 0 ? EIO : error;
    io->resid = io->xfer_buflen;
}

int backendError(const hfs::CompletionEvent &event) noexcept {
    if (event.backendErrno != 0) {
        return event.backendErrno < 0 ? -event.backendErrno : event.backendErrno;
    }
    return fioError(event.status);
}

void applyCompletion(io_u *io, const hfs::CompletionEvent &event) noexcept {
    if (io == nullptr) {
        return;
    }
    if (event.status != hfs::Status::Ok || event.backendErrno != 0) {
        setIoFailure(io, backendError(event));
        return;
    }
    const std::uint64_t bytes =
        event.completedBytes > static_cast<std::uint64_t>(io->xfer_buflen)
            ? static_cast<std::uint64_t>(io->xfer_buflen)
            : event.completedBytes;
    io->error = 0;
    io->resid = io->xfer_buflen - static_cast<unsigned int>(bytes);
}

std::uint32_t nextGeneration(FioHfsPending &slot) noexcept {
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    return slot.generation;
}

void clearPending(FioHfsPending &slot) noexcept {
    if (slot.bounceOwner != nullptr && slot.bounce != nullptr &&
        slot.bounceAllocationSize != 0) {
        slot.bounceOwner->releaseDmaBuffer(slot.bounce, slot.bounceAllocationSize);
    }
    slot.handle.reset();
    slot.file.reset();
    slot.io = nullptr;
    slot.requestId = 0;
    slot.completion = {};
    slot.bounceOwner.reset();
    slot.bounce = nullptr;
    slot.bounceSize = 0;
    slot.bounceAllocationSize = 0;
    slot.state = PendingState::Free;
}

struct PreallocationContext {
    std::shared_ptr<hfs::File> file;
};

/* The single-flight owner performs allocation and returns stat evidence. */
hfs::fio::PreallocationResult preallocationOwner(
    const hfs::fio::PreallocationKey &key, void *opaque) noexcept {
    processPreallocationOwners.fetch_add(1, std::memory_order_relaxed);
    auto *context = static_cast<PreallocationContext *>(opaque);
    if (context == nullptr || context->file == nullptr || !key.valid()) {
        return hfs::fio::PreallocationResult::makeFailure(EINVAL);
    }
    if (key.length > std::numeric_limits<std::uint64_t>::max() - key.offset) {
        return hfs::fio::PreallocationResult::makeFailure(EINVAL);
    }
    auto allocated = context->file->allocate(key.offset, key.length).wait();
    if (!allocated.ok()) {
        return hfs::fio::PreallocationResult::makeFailure(fioError(allocated.status()));
    }
    if (!allocated.value().ok()) {
        return hfs::fio::PreallocationResult::makeFailure(
            fioError(allocated.value().status()));
    }
    auto stat = context->file->stat();
    if (!stat.ok()) {
        return hfs::fio::PreallocationResult::makeFailure(fioError(stat.status()));
    }
    const std::uint64_t end = key.offset + key.length;
    if (stat.value().logicalSize < end) {
        return hfs::fio::PreallocationResult::makeFailure(EIO);
    }
    return hfs::fio::PreallocationResult::makeSuccess(
        stat.value().logicalSize, key.offset, key.length, true);
}

/* Release process-wide resources after all frontend users have drained. */
void releaseProcessUser() {
    std::lock_guard<std::mutex> lock(processMutex);
    if (processUsers != 0) {
        --processUsers;
    }
    if (processUsers != 0) {
        return;
    }
    processPreallocation.reset();
    if (processDmaBytesReserved != 0) {
        fioLog("fio-hfs: DMA reservations remain at process shutdown\n");
    }
    if (processEngine != nullptr) {
        processEngine->stop().wait();
        processEngine.reset();
    }
    processCompletionHub.shutdown();
    processCompletionHubActive = false;
    std::fprintf(stderr,
                 "fio-hfs closure: queued=%llu busy=%llu completed_error=%llu "
                 "reaped=%llu outstanding=%llu peak=%llu token_mismatch=%llu "
                 "prealloc_requests=%llu prealloc_owner=%llu prealloc_reuse=%llu "
                 "prealloc_failure=%llu\n",
                 static_cast<unsigned long long>(
                     processQueueQueued.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     processQueueBusy.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     processQueueCompletedError.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     processEventsReaped.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     processOutstanding.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     processOutstandingPeak.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     processTokenMismatch.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     processPreallocationRequests.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     processPreallocationOwners.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     processPreallocationRequests.load(std::memory_order_relaxed) -
                     processPreallocationOwners.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     processPreallocationFailures.load(std::memory_order_relaxed)));
    processDmaBytesReserved = 0;
}

/* Keep engine options statically initialized like fio's external skeleton. */
static struct fio_option options[7] = {
    {
        .name = "hfs_config",
        .lname = "HFS .conf path",
        .type = FIO_OPT_STR_STORE,
        .off1 = offsetof(FioHfsOptions, config),
        .cb = reinterpret_cast<void *>(fioHfsConfigOption),
        .help = "HFS 2.0 .conf configuration path",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "hfs_direct",
        .lname = "HFS direct mode",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(FioHfsOptions, direct),
        .help = "Use HFS direct data path",
        .def = "1",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "hfs_preallocate",
        .lname = "HFS preallocation",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(FioHfsOptions, preallocate),
        .help = "Allocate HFS extents during open_file",
        .def = "1",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "hfs_client_depth",
        .lname = "HFS client depth",
        .type = FIO_OPT_INT,
        .off1 = offsetof(FioHfsOptions, clientDepth),
        .help = "HFS client queue depth",
        .def = "32",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "hfs_async",
        .lname = "HFS asynchronous queue",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(FioHfsOptions, async),
        .help = "Use HFS QUEUED/getevents/event completion",
        .def = "1",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "hfs_file_per_job",
        .lname = "HFS file per job",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(FioHfsOptions, filePerJob),
        .help = "Use a unique HFS file for each fio worker",
        .def = "0",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = nullptr,
    },
};

int fioHfsInit(struct thread_data *td) {
    auto *options = static_cast<FioHfsOptions *>(td->eo);
    const char *configPath = options == nullptr ? nullptr : options->config;
    std::string callbackConfig;
    if (configPath == nullptr || configPath[0] == '\0') {
        std::lock_guard<std::mutex> lock(processMutex);
        callbackConfig = processConfigPath;
        configPath = callbackConfig.c_str();
    }
    if (configPath == nullptr || configPath[0] == '\0') {
        configPath = std::getenv("HFS_CONFIG");
    }
    if (configPath == nullptr || configPath[0] == '\0') {
        fioLog("fio-hfs: hfs_config is required\n");
        return 1;
    }
    auto config = hfs::parseConfigFile(configPath);
    if (!config.ok()) {
        fioLog("fio-hfs: config parse failed\n");
        return 1;
    }
    std::shared_ptr<hfs::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(processMutex);
        if (processEngine == nullptr) {
            resetProcessCounters();
            auto created = hfs::Engine::create(config.value());
            if (!created.ok()) {
                fioLog("fio-hfs: engine create failed\n");
                return 1;
            }
            engine = std::shared_ptr<hfs::Engine>(std::move(created).value());
            auto mountedTask = engine->mount().wait();
            if (!mountedTask.ok() || !mountedTask.value().ok()) {
                fioLog("fio-hfs: engine mount failed\n");
                return 1;
            }
            processEngine = engine;
            ++processEngineGeneration;
            if (processEngineGeneration == 0) {
                processEngineGeneration = 1;
            }
            processPreallocation.resetForGeneration(processEngineGeneration);
            processCompletionHubActive = processCompletionHub.initialize();
            if (!processCompletionHubActive) {
                fioLog("fio-hfs: completion hub unavailable; using per-client notifier\n");
            }
        } else {
            engine = processEngine;
        }
        ++processUsers;
    }
    const std::uint32_t requestedDepth = options == nullptr ? 32 : options->clientDepth;
    const std::uint32_t clientDepth = requestedDepth == 0 ? 1 : requestedDepth;
    const bool asyncMode = options == nullptr || options->async != 0;
    hfs::fio::FioCompletionHub::Endpoint *completionEndpoint = nullptr;
    if (asyncMode && processCompletionHubActive) {
        completionEndpoint = processCompletionHub.acquire();
    }
    const bool useCompletionHub = completionEndpoint != nullptr;
    hfs::ClientOptions clientOptions{};
    clientOptions.maxOutstanding = clientDepth;
    if (useCompletionHub) {
        clientOptions.completionWake = &hfs::fio::FioCompletionHub::wakeCallback;
        clientOptions.completionWakeContext = completionEndpoint;
        clientOptions.completionWait.prepare =
            &hfs::fio::FioCompletionHub::Endpoint::prepareWaitCallback;
        clientOptions.completionWait.epoch =
            &hfs::fio::FioCompletionHub::Endpoint::epochCallback;
        clientOptions.completionWait.waitSince =
            &hfs::fio::FioCompletionHub::Endpoint::waitSinceCallback;
        clientOptions.completionWait.context = completionEndpoint;
    }
    auto client = hfs::Client::create(*engine, clientOptions);
    if (!client.ok()) {
        fioLog("fio-hfs: client create failed\n");
        processCompletionHub.release(completionEndpoint);
        releaseProcessUser();
        return 1;
    }
    auto *state = new (std::nothrow) FioHfsThread;
    if (state == nullptr) {
        fioLog("fio-hfs: FIO state allocation failed\n");
        std::move(client).value()->close().wait();
        processCompletionHub.release(completionEndpoint);
        releaseProcessUser();
        return 1;
    }
    state->engine = std::move(engine);
    state->client = std::move(client).value();
    state->direct = options == nullptr || options->direct != 0;
    state->async = options == nullptr || options->async != 0;
    state->completionEndpoint = completionEndpoint;
    state->clientDepth = clientDepth;
    if (state->async) {
        state->slotCount = static_cast<std::size_t>(clientDepth);
        state->pending.reset(new (std::nothrow) FioHfsPending[state->slotCount]);
        state->reported.reset(new (std::nothrow) FioHfsReported[state->slotCount]);
        state->polled.reset(new (std::nothrow) hfs::CompletionEvent[state->slotCount]);
        if (state->pending == nullptr || state->reported == nullptr || state->polled == nullptr) {
            fioLog("fio-hfs: asynchronous pending table allocation failed\n");
            state->client->close().wait();
            state->client.reset();
            state->engine.reset();
            processCompletionHub.release(state->completionEndpoint);
            state->completionEndpoint = nullptr;
            delete state;
            releaseProcessUser();
            return 1;
        }
    }
    td->io_ops_data = state;
    return 0;
}

std::size_t findFreePending(const FioHfsThread &state) noexcept {
    for (std::size_t index = 0; index < state.slotCount; ++index) {
        if (state.pending[index].state == PendingState::Free) {
            return index;
        }
    }
    return state.slotCount;
}

/* Validate a completion before touching the frontend-owned io_u. */
FioHfsPending *matchCompletion(FioHfsThread &state,
                               const hfs::CompletionEvent &event) noexcept {
    if (event.token.slot >= state.slotCount) {
        fioLog("fio-hfs: completion slot is out of range\n");
        processTokenMismatch.fetch_add(1, std::memory_order_relaxed);
        state.protocolError = true;
        return nullptr;
    }
    auto &slot = state.pending[event.token.slot];
    if (slot.state != PendingState::Submitted ||
        slot.generation != event.token.generation ||
        slot.requestId != event.requestId || slot.io == nullptr) {
        fioLog("fio-hfs: completion token mismatch\n");
        processTokenMismatch.fetch_add(1, std::memory_order_relaxed);
        state.protocolError = true;
        return nullptr;
    }
    return &slot;
}

/* Turn one validated completion into a reaped or immediately drained slot. */
bool consumeCompletion(FioHfsThread &state, const hfs::CompletionEvent &event,
                       bool releaseNow) noexcept {
    auto *slot = matchCompletion(state, event);
    if (slot == nullptr) {
        return false;
    }
    applyCompletion(slot->io, event);
    if (event.status != hfs::Status::Ok || event.backendErrno != 0) {
        processQueueCompletedError.fetch_add(1, std::memory_order_relaxed);
    }
    if (event.status == hfs::Status::Ok && event.backendErrno == 0 &&
        slot->bounce != nullptr && slot->io->ddir == DDIR_READ) {
        const std::size_t bytes = std::min<std::size_t>(
            slot->bounceSize, slot->io->xfer_buflen - slot->io->resid);
        std::memcpy(slot->io->xfer_buf, slot->bounce, bytes);
    }
    slot->completion = event;
    slot->state = PendingState::Reaped;
    if (state.submitted != 0) {
        --state.submitted;
    }
    if (releaseNow) {
        clearPending(*slot);
        recordReaped();
        return true;
    }
    if (state.reportedCount >= state.slotCount) {
        fioLog("fio-hfs: completion report table overflow\n");
        state.protocolError = true;
        clearPending(*slot);
        return false;
    }
    const auto slotIndex = static_cast<std::uint32_t>(slot - state.pending.get());
    state.reported[state.reportedCount++] =
        FioHfsReported{slotIndex, slot->generation, slot->requestId};
    ++state.reportedRemaining;
    return true;
}

/* Release a report only after its slot identity is checked again. */
io_u *releaseReported(FioHfsThread &state, std::size_t reportIndex) noexcept {
    if (reportIndex >= state.reportedCount) {
        return nullptr;
    }
    const auto report = state.reported[reportIndex];
    if (report.slot >= state.slotCount) {
        state.protocolError = true;
        return nullptr;
    }
    auto &slot = state.pending[report.slot];
    if (slot.state != PendingState::Reaped || slot.generation != report.generation ||
        slot.requestId != report.requestId) {
        fioLog("fio-hfs: reported slot identity mismatch\n");
        state.protocolError = true;
        return nullptr;
    }
    io_u *io = slot.io;
    clearPending(slot);
    recordReaped();
    state.reported[reportIndex] = {};
    if (state.reportedRemaining != 0) {
        --state.reportedRemaining;
    }
    if (state.reportedRemaining == 0) {
        state.reportedCount = 0;
    }
    return io;
}

/* Cleanup consumes reports internally because fio will not call event(). */
void releaseAllReports(FioHfsThread &state) noexcept {
    for (std::size_t index = 0; index < state.reportedCount; ++index) {
        if (state.reported[index].generation != 0) {
            (void)releaseReported(state, index);
        }
    }
    if (state.reportedRemaining == 0) {
        state.reportedCount = 0;
    }
}

/* Uses the Client-owned drain, spin, rescan, and wait sequence. The FIO
 * endpoint is injected through ClientOptions instead of duplicating polling. */
hfs::PollResult pollClient(FioHfsThread &state,
                           std::span<hfs::CompletionEvent> output,
                           std::size_t min, std::size_t max,
                           std::chrono::microseconds timeout) {
    return state.client->poll(output, min, max, timeout);
}

/* Drain accepted operations before releasing Client, File, or fio memory. */
void drainAsync(FioHfsThread &state) noexcept {
    if (!state.async || state.client == nullptr) {
        return;
    }
    state.closing = true;
    const auto drainStart = std::chrono::steady_clock::now();
    bool warned = false;
    for (;;) {
        releaseAllReports(state);
        const std::size_t coreOutstanding = state.client->outstanding();
        if (state.submitted == 0 && state.reportedRemaining == 0 && coreOutstanding == 0) {
            return;
        }
        auto result = pollClient(
            state,
            std::span<hfs::CompletionEvent>(state.polled.get(), state.slotCount),
            state.submitted == 0 ? 0 : 1, state.slotCount,
            std::chrono::milliseconds(100));
        if (result.status != hfs::Status::Ok && result.status != hfs::Status::TimedOut) {
            fioLog("fio-hfs: Client poll failed while draining\n");
        }
        const std::size_t count = result.count > state.slotCount ? state.slotCount : result.count;
        for (std::size_t index = 0; index < count; ++index) {
            (void)consumeCompletion(state, state.polled[index], true);
        }
        if (count == 0) {
            std::this_thread::yield();
        }
        if (!warned && std::chrono::steady_clock::now() - drainStart >=
                             std::chrono::seconds(5)) {
            fioLog("fio-hfs: async cleanup is still draining; retaining all resources\n");
            warned = true;
        }
    }
}

void fioHfsCleanup(struct thread_data *td) {
    auto *state = static_cast<FioHfsThread *>(td->io_ops_data);
    if (state == nullptr) {
        return;
    }
    drainAsync(*state);
    if (state->client != nullptr) {
        auto closed = state->client->close().wait();
        if (!closed.ok() || !closed.value().ok()) {
            fioLog("fio-hfs: Client close did not reach Closed\n");
        }
    }
    state->client.reset();
    processCompletionHub.release(state->completionEndpoint);
    state->completionEndpoint = nullptr;
    state->engine.reset();
    releaseProcessUser();
    delete state;
    td->io_ops_data = nullptr;
}

int fioHfsOpen(struct thread_data *td, struct fio_file *fioFile) {
    auto *state = static_cast<FioHfsThread *>(td->io_ops_data);
    if (state == nullptr || state->client == nullptr) {
        return EINVAL;
    }
    auto *options = static_cast<FioHfsOptions *>(td->eo);
    std::string path = fioPath(fioFile->file_name);
    if (options != nullptr && options->filePerJob != 0) {
        /* Preserve shared-file behavior by default; opt in to per-worker files. */
        path += ".job-" + std::to_string(td->thread_number);
    }
    auto opened = state->client->open(path, hfs::OpenFlags::Create).wait();
    if (!opened.ok()) {
        return fioError(opened.status());
    }
    auto *file = new (std::nothrow) FioHfsFile;
    if (file == nullptr) {
        return ENOMEM;
    }
    auto openedResult = std::move(opened).value();
    if (!openedResult.ok()) {
        delete file;
        return fioError(openedResult.status());
    }
    file->file = std::move(openedResult).value();
    auto stat = file->file->stat();
    if (!stat.ok()) {
        delete file;
        return fioError(stat.status());
    }
    const std::uint64_t requestedSize = static_cast<std::uint64_t>(td->o.size);
    if ((options == nullptr || options->preallocate != 0) && requestedSize != 0) {
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(processMutex);
            generation = processEngineGeneration;
        }
        hfs::fio::PreallocationKey key{
            generation, file->file->handle(), 0, requestedSize};
        PreallocationContext context{file->file};
        processPreallocationRequests.fetch_add(1, std::memory_order_relaxed);
        const auto allocation = processPreallocation.getOrAllocate(
            key, preallocationOwner, &context);
        if (!allocation.ok()) {
            processPreallocationFailures.fetch_add(1, std::memory_order_relaxed);
            delete file;
            return allocation.backendErrno == 0 ? EIO : allocation.backendErrno;
        }
        stat.value().logicalSize = allocation.logicalSize;
    }
    fioFile->engine_data = file;
    fioFile->real_file_size = stat.value().logicalSize;
    fio_file_set_size_known(fioFile);
    return 0;
}

int fioHfsClose(struct thread_data *, struct fio_file *fioFile) {
    auto *file = static_cast<FioHfsFile *>(fioFile->engine_data);
    delete file;
    fioFile->engine_data = nullptr;
    return 0;
}

/* HFS owns cache coherency; fio has no host file descriptor to invalidate. */
int fioHfsInvalidate(struct thread_data *, struct fio_file *) { return 0; }

/* Fio needs a logical extent before it schedules the first request. */
int fioHfsGetFileSize(struct thread_data *td, struct fio_file *fioFile) {
    const std::uint64_t size = static_cast<std::uint64_t>(td->o.size);
    fioFile->real_file_size = size == 0 ? 4096 : size;
    return 0;
}

/* Executes one FIO request without crossing the caller's queue callback. */
void executeFioIo(io_u *io, const std::shared_ptr<hfs::File> &file, bool direct) {
    std::unique_ptr<std::byte[]> bounce;
    std::byte *data = static_cast<std::byte *>(io->xfer_buf);
    if (!direct) {
        /* The synchronous fallback keeps a bounded per-request bounce buffer. */
        bounce.reset(new (std::nothrow) std::byte[io->xfer_buflen]);
        if (bounce == nullptr) {
            setIoFailure(io, ENOMEM);
            return;
        }
        data = bounce.get();
        if (io->ddir == DDIR_WRITE) {
            std::memcpy(data, io->xfer_buf, io->xfer_buflen);
        }
    }
    hfs::IoVec buffer{data, io->xfer_buflen};
    if (io->ddir == DDIR_READ) {
        auto taskResult = file->read(std::span<hfs::IoVec>(&buffer, 1), io->offset).wait();
        if (!taskResult.ok() || !taskResult.value().ok()) {
            setIoFailure(io, taskResult.ok() ? fioError(taskResult.value().status()) : EIO);
        } else {
            io->error = 0;
            const std::size_t bytes = taskResult.value().value().bytes > io->xfer_buflen
                                          ? io->xfer_buflen
                                          : taskResult.value().value().bytes;
            io->resid = io->xfer_buflen - bytes;
            if (!direct) {
                std::memcpy(io->xfer_buf, data, bytes);
            }
        }
    } else if (io->ddir == DDIR_WRITE) {
        auto taskResult =
            file->write(std::span<const hfs::IoVec>(&buffer, 1), io->offset).wait();
        if (!taskResult.ok() || !taskResult.value().ok()) {
            setIoFailure(io, taskResult.ok() ? fioError(taskResult.value().status()) : EIO);
        } else {
            io->error = 0;
            const std::size_t bytes = taskResult.value().value().bytes > io->xfer_buflen
                                          ? io->xfer_buflen
                                          : taskResult.value().value().bytes;
            io->resid = io->xfer_buflen - bytes;
        }
    } else {
        setIoFailure(io, EOPNOTSUPP);
    }
}

enum fio_q_status fioHfsQueue(struct thread_data *td, struct io_u *io) {
    auto *state = static_cast<FioHfsThread *>(td->io_ops_data);
    auto *file = io->file == nullptr ? nullptr
                                     : static_cast<FioHfsFile *>(io->file->engine_data);
    if (state == nullptr || file == nullptr || file->file == nullptr) {
        io->error = EINVAL;
        return FIO_Q_COMPLETED;
    }
    if (!state->async) {
        executeFioIo(io, file->file, state->direct);
        return FIO_Q_COMPLETED;
    }
    if (state->closing || state->protocolError || state->client == nullptr) {
        setIoFailure(io, EIO);
        processQueueCompletedError.fetch_add(1, std::memory_order_relaxed);
        return FIO_Q_COMPLETED;
    }
    if (io->ddir != DDIR_READ && io->ddir != DDIR_WRITE) {
        setIoFailure(io, EOPNOTSUPP);
        processQueueCompletedError.fetch_add(1, std::memory_order_relaxed);
        return FIO_Q_COMPLETED;
    }

    const std::size_t slotIndex = findFreePending(*state);
    if (slotIndex == state->slotCount) {
        processQueueBusy.fetch_add(1, std::memory_order_relaxed);
        return FIO_Q_BUSY;
    }
    FioHfsPending &slot = state->pending[slotIndex];
    const std::uint32_t generation = nextGeneration(slot);
    slot.state = PendingState::Submitted;
    slot.io = io;
    slot.file = file->file;
    slot.completion = {};

    std::byte *data = static_cast<std::byte *>(io->xfer_buf);
    if (!state->direct) {
        auto acquired = state->engine->acquireDmaBuffer(io->xfer_buflen, 0);
        if (!acquired.ok()) {
            clearPending(slot);
            if (isQueueRetryable(acquired.status())) {
                processQueueBusy.fetch_add(1, std::memory_order_relaxed);
                io->error = 0;
                io->resid = 0;
                return FIO_Q_BUSY;
            }
            setIoFailure(io, fioError(acquired.status()));
            processQueueCompletedError.fetch_add(1, std::memory_order_relaxed);
            return FIO_Q_COMPLETED;
        }
        slot.bounceOwner = state->engine;
        slot.bounce = acquired.value().data();
        slot.bounceSize = io->xfer_buflen;
        slot.bounceAllocationSize = acquired.value().size();
        data = slot.bounce;
        if (io->ddir == DDIR_WRITE) {
            std::memcpy(data, io->xfer_buf, io->xfer_buflen);
        }
    }

    const hfs::CompletionToken token{static_cast<std::uint32_t>(slotIndex), generation};
    const hfs::IoVec buffer{data, io->xfer_buflen};
    hfs::Result<hfs::IoHandle> submitted =
        io->ddir == DDIR_READ
            ? state->client->submitRead(file->file, buffer, io->offset, token)
            : state->client->submitWrite(file->file, buffer, io->offset, token);
    if (!submitted.ok()) {
        const hfs::Status status = submitted.status();
        clearPending(slot);
        if (isQueueRetryable(status)) {
            processQueueBusy.fetch_add(1, std::memory_order_relaxed);
            io->error = 0;
            io->resid = 0;
            return FIO_Q_BUSY;
        }
        setIoFailure(io, fioError(status));
        processQueueCompletedError.fetch_add(1, std::memory_order_relaxed);
        return FIO_Q_COMPLETED;
    }

    slot.handle = std::move(submitted).value();
    slot.requestId = slot.handle.requestId();
    ++state->submitted;
    recordQueued();
    io->error = 0;
    io->resid = 0;
    return FIO_Q_QUEUED;
}

int fioHfsGetevents(struct thread_data *td, unsigned int min, unsigned int max,
                    const struct timespec *timeout) {
    auto *state = static_cast<FioHfsThread *>(td->io_ops_data);
    if (state == nullptr || max == 0 || !state->async) {
        return 0;
    }
    if (state->client == nullptr || state->protocolError) {
        return -EIO;
    }
    if (state->reportedRemaining != 0) {
        fioLog("fio-hfs: getevents called before the prior event batch was consumed\n");
        state->protocolError = true;
        return -EIO;
    }

    const std::size_t limit = std::min<std::size_t>(max, state->slotCount);
    const std::size_t minimum = std::min<std::size_t>(min, limit);
    state->reportedCount = 0;
    state->reportedRemaining = 0;
    auto result = pollClient(
        *state,
        std::span<hfs::CompletionEvent>(state->polled.get(), limit), minimum, limit,
        fioTimeout(timeout));
    const std::size_t count = std::min(result.count, limit);
    for (std::size_t index = 0; index < count; ++index) {
        (void)consumeCompletion(*state, state->polled[index], false);
    }
    if (state->protocolError || result.status == hfs::Status::Corrupt) {
        return state->reportedCount == 0 ? -EIO
                                         : static_cast<int>(state->reportedCount);
    }
    if (result.status != hfs::Status::Ok && result.status != hfs::Status::TimedOut) {
        return state->reportedCount == 0 ? -fioError(result.status)
                                         : static_cast<int>(state->reportedCount);
    }
    return static_cast<int>(state->reportedCount);
}

struct io_u *fioHfsEvent(struct thread_data *td, int event) {
    auto *state = static_cast<FioHfsThread *>(td->io_ops_data);
    if (state == nullptr || event < 0 ||
        static_cast<std::size_t>(event) >= state->reportedCount) {
        return nullptr;
    }
    return releaseReported(*state, static_cast<std::size_t>(event));
}

} // namespace

extern "C" struct ioengine_ops ioengine = {
    .name = "hfs",
    .version = FIO_IOOPS_VERSION,
    .flags = FIO_NOEXTEND | FIO_MEMALIGN,
    .init = fioHfsInit,
    .queue = fioHfsQueue,
    .getevents = fioHfsGetevents,
    .event = fioHfsEvent,
    .cleanup = fioHfsCleanup,
    .open_file = fioHfsOpen,
    .close_file = fioHfsClose,
    .invalidate = fioHfsInvalidate,
    .get_file_size = fioHfsGetFileSize,
    .iomem_alloc = fioHfsIomemAlloc,
    .iomem_free = fioHfsIomemFree,
    .option_struct_size = sizeof(FioHfsOptions),
    .options = options,
};
