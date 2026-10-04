/* UUSE 2.0 fio external engine with bounded asynchronous completions. */

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

#include "uuse/config.hpp"
#include "uuse/core.hpp"
#include "uuse/file.hpp"
#include "fio_completion_hub.hpp"
#include "fio_status_errno.hpp"
#include "preallocation_single_flight.hpp"

namespace {

struct FioUuseOptions {
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
struct FioUusePending {
    PendingState state = PendingState::Free;
    std::uint32_t generation = 0;
    std::uint64_t requestId = 0;
    io_u *io = nullptr;
    std::shared_ptr<uuse::File> file;
    uuse::IoHandle handle;
    uuse::CompletionEvent completion{};
    std::shared_ptr<uuse::Engine> bounceOwner;
    std::byte *bounce = nullptr;
    std::size_t bounceSize = 0;
    std::size_t bounceAllocationSize = 0;
};

/* The current getevents batch stores slot identities, never core pointers. */
struct FioUuseReported {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    std::uint64_t requestId = 0;
};

struct FioUuseFile {
    std::shared_ptr<uuse::File> file;
};

struct FioUuseThread {
    std::shared_ptr<uuse::Engine> engine;
    std::shared_ptr<uuse::Client> client;
    bool direct = true;
    bool async = true;
    bool closing = false;
    bool protocolError = false;
    uuse::fio::FioCompletionHub::Endpoint *completionEndpoint = nullptr;
    std::uint32_t clientDepth = 1;
    std::size_t slotCount = 0;
    std::size_t submitted = 0;
    std::size_t reportedCount = 0;
    std::size_t reportedRemaining = 0;
    std::unique_ptr<FioUusePending[]> pending;
    std::unique_ptr<FioUuseReported[]> reported;
    std::unique_ptr<uuse::CompletionEvent[]> polled;
    /* Free pending-slot index stack; frontend-owner private, O(1) reserve. */
    std::unique_ptr<std::uint32_t[]> freePending;
    std::size_t freePendingCount = 0;
};

/* FIO numjobs share one process lock while retaining one Client per worker. */
std::mutex processMutex;
std::shared_ptr<uuse::Engine> processEngine;
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
uuse::fio::FioCompletionHub processCompletionHub;
bool processCompletionHubActive = false;

/* Shared-file preallocation is process-scoped and generation-scoped. */
uuse::fio::PreallocationSingleFlight processPreallocation(
    uuse::fio::PreallocationSingleFlight::kMaximumCapacity);

std::mutex dmaBufferMutex;
struct DmaBufferOwner {
    std::weak_ptr<uuse::Engine> engine;
    std::size_t allocationSize = 0;
    std::size_t reservedBytes = 0;
};
std::unordered_map<void *, DmaBufferOwner> dmaBufferOwners;

void fioLog(const char *message);

/* Preserve the parsed engine configuration for fio worker clones. */
int fioUuseConfigOption(void *, const char *value) {
    if (value == nullptr || value[0] == '\0') {
        return 1;
    }
    std::lock_guard<std::mutex> lock(processMutex);
    processConfigPath = value;
    return 0;
}

/* UUSE filenames belong to the UUSE namespace; fio must not probe the host path. */
int fioUuseSetup(struct thread_data *td) {
    if (td == nullptr || td->o.use_thread == 0) {
        fioLog("fio-uuse: thread=1 is required for the process-shared engine\n");
        return 1;
    }
    return 0;
}

int fioError(uuse::Status status) {
    return uuse::fio::mapStatusToErrno(status);
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

/* Allocate fio's complete requested buffer from the shared UUSE DMA arena. */
int fioUuseIomemAlloc(struct thread_data *td, size_t totalMem) {
    if (td == nullptr || totalMem == 0) {
        if (td != nullptr) td->orig_buffer = nullptr;
        return totalMem == 0 ? 0 : 1;
    }
    std::shared_ptr<uuse::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(processMutex);
        engine = processEngine;
    }
    if (engine == nullptr) {
        fioLog("fio-uuse: engine is not active for DMA arena allocation\n");
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
            fioLog("fio-uuse: requested fio DMA arena exceeds mount cap\n");
            return 1;
        }
        processDmaBytesReserved += bytes;
    }
    auto acquired = engine->acquireDmaBuffer(totalMem, 0);
    if (!acquired.ok()) {
        std::lock_guard<std::mutex> lock(processMutex);
        processDmaBytesReserved -= bytes;
        if (acquired.status() == uuse::Status::Busy) {
            fioLog("fio-uuse: requested fio DMA arena exceeds mount cap\n");
        } else {
            fioLog("fio-uuse: complete DMA arena allocation failed\n");
        }
        return 1;
    }
    td->orig_buffer = reinterpret_cast<char *>(acquired.value().data());
    if (td->orig_buffer == nullptr || acquired.value().size() < totalMem) {
        fioLog("fio-uuse: DMA arena returned an invalid buffer\n");
        engine->releaseDmaBuffer(td->orig_buffer, acquired.value().size());
        td->orig_buffer = nullptr;
        std::lock_guard<std::mutex> lock(processMutex);
        processDmaBytesReserved -= bytes;
        return 1;
    }
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
    fioLog("fio-uuse: duplicate DMA arena owner pointer\n");
    return 1;
}

/* Release only the single arena owned by fio for this worker. */
void fioUuseIomemFree(struct thread_data *td) {
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
        /* Clamp to a deadline-safe bound: poll() adds this to a steady_clock
         * point, and microseconds::max() would overflow the nanosecond sum. */
        return std::chrono::hours(24);
    }
    return std::chrono::microseconds(
        static_cast<std::int64_t>(seconds * kMicrosPerSecond + nanos / 1000U));
}

bool isQueueRetryable(uuse::Status status) noexcept {
    return status == uuse::Status::Busy || status == uuse::Status::TryAgain;
}

void setIoFailure(io_u *io, int error) noexcept {
    if (io == nullptr) {
        return;
    }
    io->error = error == 0 ? EIO : error;
    io->resid = io->xfer_buflen;
}

int backendError(const uuse::CompletionEvent &event) noexcept {
    if (event.backendErrno != 0) {
        return event.backendErrno < 0 ? -event.backendErrno : event.backendErrno;
    }
    return fioError(event.status);
}

void applyCompletion(io_u *io, const uuse::CompletionEvent &event) noexcept {
    if (io == nullptr) {
        return;
    }
    if (event.status != uuse::Status::Ok || event.backendErrno != 0) {
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

std::uint32_t nextGeneration(FioUusePending &slot) noexcept {
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    return slot.generation;
}

void clearPending(FioUuseThread &state, FioUusePending &slot) noexcept {
    if (slot.state == PendingState::Free) {
        return; /* already released: never link the same slot twice */
    }
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
    const auto index = static_cast<std::uint32_t>(&slot - state.pending.get());
    if (state.freePendingCount < state.slotCount) {
        state.freePending[state.freePendingCount++] = index;
    }
}

struct PreallocationContext {
    std::shared_ptr<uuse::File> file;
};

/* The single-flight owner performs allocation and returns stat evidence. */
uuse::fio::PreallocationResult preallocationOwner(
    const uuse::fio::PreallocationKey &key, void *opaque) noexcept {
    processPreallocationOwners.fetch_add(1, std::memory_order_relaxed);
    auto *context = static_cast<PreallocationContext *>(opaque);
    if (context == nullptr || context->file == nullptr || !key.valid()) {
        return uuse::fio::PreallocationResult::makeFailure(EINVAL);
    }
    if (key.length > std::numeric_limits<std::uint64_t>::max() - key.offset) {
        return uuse::fio::PreallocationResult::makeFailure(EINVAL);
    }
    auto allocated = context->file->allocate(key.offset, key.length).wait();
    if (!allocated.ok()) {
        return uuse::fio::PreallocationResult::makeFailure(fioError(allocated.status()));
    }
    if (!allocated.value().ok()) {
        return uuse::fio::PreallocationResult::makeFailure(
            fioError(allocated.value().status()));
    }
    auto stat = context->file->stat();
    if (!stat.ok()) {
        return uuse::fio::PreallocationResult::makeFailure(fioError(stat.status()));
    }
    const std::uint64_t end = key.offset + key.length;
    if (stat.value().logicalSize < end) {
        return uuse::fio::PreallocationResult::makeFailure(EIO);
    }
    /* Coverage must be real committed extents, not just a logical size. */
    auto coverage = context->file->allocatedBytes();
    if (!coverage.ok() || coverage.value() < end) {
        return uuse::fio::PreallocationResult::makeFailure(EIO);
    }
    return uuse::fio::PreallocationResult::makeSuccess(
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
        fioLog("fio-uuse: DMA reservations remain at process shutdown\n");
    }
    if (processEngine != nullptr) {
        /* Sibling cleanup threads may still hold metadata leases, and a
         * failed stop means the runtime is alive: never free it here. */
        bool stopped = false;
        std::string lastError;
        for (int attempt = 0; attempt < 200; ++attempt) {
            auto result = processEngine->stop().wait();
            if (result.ok() && result.value().ok()) {
                stopped = true;
                break;
            }
            lastError = result.ok() ? result.value().message() : result.message();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (stopped) {
            if (!lastError.empty()) {
                std::fprintf(stderr, "fio-uuse: engine stop retried after: %s\n",
                             lastError.c_str());
            }
            processEngine.reset();
        } else {
            std::fprintf(stderr, "fio-uuse: engine stop failed (%s); retaining engine\n",
                         lastError.c_str());
        }
    }
    processCompletionHub.shutdown();
    processCompletionHubActive = false;
    std::fprintf(stderr,
                 "fio-uuse closure: queued=%llu busy=%llu completed_error=%llu "
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
        .name = "uuse_config",
        .lname = "UUSE .conf path",
        .type = FIO_OPT_STR_STORE,
        .off1 = offsetof(FioUuseOptions, config),
        .cb = reinterpret_cast<void *>(fioUuseConfigOption),
        .help = "UUSE 2.0 .conf configuration path",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "uuse_direct",
        .lname = "UUSE direct mode",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(FioUuseOptions, direct),
        .help = "Use UUSE direct data path",
        .def = "1",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "uuse_preallocate",
        .lname = "UUSE preallocation",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(FioUuseOptions, preallocate),
        .help = "Allocate UUSE extents during open_file",
        .def = "1",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "uuse_client_depth",
        .lname = "UUSE client depth",
        .type = FIO_OPT_INT,
        .off1 = offsetof(FioUuseOptions, clientDepth),
        .help = "UUSE client queue depth",
        .def = "32",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "uuse_async",
        .lname = "UUSE asynchronous queue",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(FioUuseOptions, async),
        .help = "Use UUSE QUEUED/getevents/event completion",
        .def = "1",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "uuse_file_per_job",
        .lname = "UUSE file per job",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(FioUuseOptions, filePerJob),
        .help = "Use a unique UUSE file for each fio worker",
        .def = "0",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = nullptr,
    },
};

int fioUuseInit(struct thread_data *td) {
    /* Forked jobs cannot share one Engine (exclusive process lock); fail with
     * a concrete message instead of a per-child engine create error. */
    if (td->o.use_thread == 0 && td->o.numjobs > 1) {
        fioLog("fio-uuse: numjobs>1 requires thread=1 (forked jobs cannot share "
               "one UUSE engine)\n");
        return 1;
    }
    auto *options = static_cast<FioUuseOptions *>(td->eo);
    const char *configPath = options == nullptr ? nullptr : options->config;
    std::string callbackConfig;
    if (configPath == nullptr || configPath[0] == '\0') {
        std::lock_guard<std::mutex> lock(processMutex);
        callbackConfig = processConfigPath;
        configPath = callbackConfig.c_str();
    }
    if (configPath == nullptr || configPath[0] == '\0') {
        configPath = std::getenv("UUSE_CONFIG");
    }
    if (configPath == nullptr || configPath[0] == '\0') {
        fioLog("fio-uuse: uuse_config is required\n");
        return 1;
    }
    auto config = uuse::parseConfigFile(configPath);
    if (!config.ok()) {
        fioLog("fio-uuse: config parse failed\n");
        return 1;
    }
    std::shared_ptr<uuse::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(processMutex);
        if (processEngine == nullptr) {
            resetProcessCounters();
            auto created = uuse::Engine::create(config.value());
            if (!created.ok()) {
                fioLog("fio-uuse: engine create failed\n");
                return 1;
            }
            engine = std::shared_ptr<uuse::Engine>(std::move(created).value());
            auto mountedTask = engine->mount().wait();
            if (!mountedTask.ok() || !mountedTask.value().ok()) {
                fioLog("fio-uuse: engine mount failed\n");
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
                fioLog("fio-uuse: completion hub unavailable; using per-client notifier\n");
            }
        } else {
            engine = processEngine;
        }
        ++processUsers;
    }
    const std::uint32_t requestedDepth = options == nullptr ? 32 : options->clientDepth;
    const std::uint32_t clientDepth = requestedDepth == 0 ? 1 : requestedDepth;
    const bool asyncMode = options == nullptr || options->async != 0;
    uuse::fio::FioCompletionHub::Endpoint *completionEndpoint = nullptr;
    if (asyncMode && processCompletionHubActive) {
        completionEndpoint = processCompletionHub.acquire();
    }
    const bool useCompletionHub = completionEndpoint != nullptr;
    uuse::ClientOptions clientOptions{};
    clientOptions.maxOutstanding = clientDepth;
    if (useCompletionHub) {
        clientOptions.completionWake = &uuse::fio::FioCompletionHub::wakeCallback;
        clientOptions.completionWakeContext = completionEndpoint;
        clientOptions.completionWait.prepare =
            &uuse::fio::FioCompletionHub::Endpoint::prepareWaitCallback;
        clientOptions.completionWait.epoch =
            &uuse::fio::FioCompletionHub::Endpoint::epochCallback;
        clientOptions.completionWait.waitSince =
            &uuse::fio::FioCompletionHub::Endpoint::waitSinceCallback;
        clientOptions.completionWait.context = completionEndpoint;
    }
    auto client = uuse::Client::create(*engine, clientOptions);
    if (!client.ok()) {
        fioLog("fio-uuse: client create failed\n");
        processCompletionHub.release(completionEndpoint);
        releaseProcessUser();
        return 1;
    }
    auto *state = new (std::nothrow) FioUuseThread;
    if (state == nullptr) {
        fioLog("fio-uuse: FIO state allocation failed\n");
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
        state->pending.reset(new (std::nothrow) FioUusePending[state->slotCount]);
        state->reported.reset(new (std::nothrow) FioUuseReported[state->slotCount]);
        state->polled.reset(new (std::nothrow) uuse::CompletionEvent[state->slotCount]);
        state->freePending.reset(new (std::nothrow) std::uint32_t[state->slotCount]);
        if (state->pending == nullptr || state->reported == nullptr ||
            state->polled == nullptr || state->freePending == nullptr) {
            fioLog("fio-uuse: asynchronous pending table allocation failed\n");
            state->client->close().wait();
            state->client.reset();
            state->engine.reset();
            processCompletionHub.release(state->completionEndpoint);
            state->completionEndpoint = nullptr;
            delete state;
            releaseProcessUser();
            return 1;
        }
        for (std::size_t index = 0; index < state->slotCount; ++index) {
            state->freePending[index] = static_cast<std::uint32_t>(index);
        }
        state->freePendingCount = state->slotCount;
    }
    td->io_ops_data = state;
    return 0;
}

std::size_t findFreePending(FioUuseThread &state) noexcept {
    if (state.freePendingCount == 0) {
        return state.slotCount;
    }
    return state.freePending[--state.freePendingCount];
}

/* Validate a completion before touching the frontend-owned io_u. */
FioUusePending *matchCompletion(FioUuseThread &state,
                               const uuse::CompletionEvent &event) noexcept {
    if (event.token.slot >= state.slotCount) {
        fioLog("fio-uuse: completion slot is out of range\n");
        processTokenMismatch.fetch_add(1, std::memory_order_relaxed);
        state.protocolError = true;
        return nullptr;
    }
    auto &slot = state.pending[event.token.slot];
    if (slot.state != PendingState::Submitted ||
        slot.generation != event.token.generation ||
        slot.requestId != event.requestId || slot.io == nullptr) {
        fioLog("fio-uuse: completion token mismatch\n");
        processTokenMismatch.fetch_add(1, std::memory_order_relaxed);
        state.protocolError = true;
        return nullptr;
    }
    return &slot;
}

/* Turn one validated completion into a reaped or immediately drained slot. */
bool consumeCompletion(FioUuseThread &state, const uuse::CompletionEvent &event,
                       bool releaseNow) noexcept {
    auto *slot = matchCompletion(state, event);
    if (slot == nullptr) {
        return false;
    }
    applyCompletion(slot->io, event);
    if (event.status != uuse::Status::Ok || event.backendErrno != 0) {
        processQueueCompletedError.fetch_add(1, std::memory_order_relaxed);
    }
    if (event.status == uuse::Status::Ok && event.backendErrno == 0 &&
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
        clearPending(state, *slot);
        recordReaped();
        return true;
    }
    if (state.reportedCount >= state.slotCount) {
        fioLog("fio-uuse: completion report table overflow\n");
        state.protocolError = true;
        clearPending(state, *slot);
        return false;
    }
    const auto slotIndex = static_cast<std::uint32_t>(slot - state.pending.get());
    state.reported[state.reportedCount++] =
        FioUuseReported{slotIndex, slot->generation, slot->requestId};
    ++state.reportedRemaining;
    return true;
}

/* Release a report only after its slot identity is checked again. */
io_u *releaseReported(FioUuseThread &state, std::size_t reportIndex) noexcept {
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
        fioLog("fio-uuse: reported slot identity mismatch\n");
        state.protocolError = true;
        return nullptr;
    }
    io_u *io = slot.io;
    clearPending(state, slot);
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
void releaseAllReports(FioUuseThread &state) noexcept {
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
uuse::PollResult pollClient(FioUuseThread &state,
                           std::span<uuse::CompletionEvent> output,
                           std::size_t min, std::size_t max,
                           std::chrono::microseconds timeout) {
    return state.client->poll(output, min, max, timeout);
}

/* Drain accepted operations before releasing Client, File, or fio memory. */
void drainAsync(FioUuseThread &state) noexcept {
    if (!state.async || state.client == nullptr) {
        return;
    }
    state.closing = true;
    const auto drainStart = std::chrono::steady_clock::now();
    /* Bounded drain: a protocol error or a wedged device must fail the run
     * instead of spinning forever in cleanup. */
    const auto drainDeadline = drainStart + std::chrono::seconds(30);
    bool warned = false;
    for (;;) {
        releaseAllReports(state);
        const std::size_t coreOutstanding = state.client->outstanding();
        if (state.submitted == 0 && state.reportedRemaining == 0 && coreOutstanding == 0) {
            return;
        }
        auto result = pollClient(
            state,
            std::span<uuse::CompletionEvent>(state.polled.get(), state.slotCount),
            state.submitted == 0 ? 0 : 1, state.slotCount,
            std::chrono::milliseconds(100));
        if (result.status != uuse::Status::Ok && result.status != uuse::Status::TimedOut) {
            fioLog("fio-uuse: Client poll failed while draining; aborting drain\n");
            return;
        }
        const std::size_t count = result.count > state.slotCount ? state.slotCount : result.count;
        for (std::size_t index = 0; index < count; ++index) {
            if (!consumeCompletion(state, state.polled[index], true)) {
                fioLog("fio-uuse: completion protocol error while draining; aborting drain\n");
                return;
            }
        }
        if (count == 0) {
            std::this_thread::yield();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= drainDeadline) {
            fioLog("fio-uuse: async cleanup drain deadline exceeded; failing cleanup\n");
            return;
        }
        if (!warned && now - drainStart >= std::chrono::seconds(5)) {
            fioLog("fio-uuse: async cleanup is still draining; retaining all resources\n");
            warned = true;
        }
    }
}

void fioUuseCleanup(struct thread_data *td) {
    auto *state = static_cast<FioUuseThread *>(td->io_ops_data);
    if (state == nullptr) {
        return;
    }
    drainAsync(*state);
    if (state->client != nullptr) {
        auto closed = state->client->close().wait();
        if (!closed.ok() || !closed.value().ok()) {
            fioLog("fio-uuse: Client close did not reach Closed\n");
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

/* One path helper so get_file_size and open_file agree on per-job names. */
std::string fioUuseFilePath(struct thread_data *td, struct fio_file *fioFile) {
    std::string path = fioPath(fioFile->file_name);
    auto *options = static_cast<FioUuseOptions *>(td->eo);
    if (options != nullptr && options->filePerJob != 0) {
        /* Preserve shared-file behavior by default; opt in to per-worker files. */
        path += ".job-" + std::to_string(td->thread_number);
    }
    return path;
}

int fioUuseOpen(struct thread_data *td, struct fio_file *fioFile) {
    auto *state = static_cast<FioUuseThread *>(td->io_ops_data);
    if (state == nullptr || state->client == nullptr) {
        errno = EINVAL;
        return EINVAL;
    }
    auto *options = static_cast<FioUuseOptions *>(td->eo);
    auto opened =
        state->client->open(fioUuseFilePath(td, fioFile), uuse::OpenFlags::Create).wait();
    if (!opened.ok()) {
        /* fio reads errno, not the return value, when reporting open errors. */
        return uuse::fio::setErrnoFromStatus(opened.status());
    }
    auto *file = new (std::nothrow) FioUuseFile;
    if (file == nullptr) {
        errno = ENOMEM;
        return ENOMEM;
    }
    auto openedResult = std::move(opened).value();
    if (!openedResult.ok()) {
        delete file;
        return uuse::fio::setErrnoFromStatus(openedResult.status());
    }
    file->file = std::move(openedResult).value();
    auto stat = file->file->stat();
    if (!stat.ok()) {
        delete file;
        return uuse::fio::setErrnoFromStatus(stat.status());
    }
    const std::uint64_t requestedSize = static_cast<std::uint64_t>(td->o.size);
    if ((options == nullptr || options->preallocate != 0) && requestedSize != 0) {
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(processMutex);
            generation = processEngineGeneration;
        }
        uuse::fio::PreallocationKey key{
            generation, file->file->handle(), 0, requestedSize};
        PreallocationContext context{file->file};
        processPreallocationRequests.fetch_add(1, std::memory_order_relaxed);
        const auto allocation = processPreallocation.getOrAllocate(
            key, preallocationOwner, &context);
        if (!allocation.ok()) {
            processPreallocationFailures.fetch_add(1, std::memory_order_relaxed);
            delete file;
            const int error = allocation.backendErrno == 0 ? EIO : allocation.backendErrno;
            errno = error;
            return error;
        }
        stat.value().logicalSize = allocation.logicalSize;
    }
    fioFile->engine_data = file;
    fioFile->real_file_size = stat.value().logicalSize;
    fio_file_set_size_known(fioFile);
    return 0;
}

int fioUuseClose(struct thread_data *, struct fio_file *fioFile) {
    auto *file = static_cast<FioUuseFile *>(fioFile->engine_data);
    delete file;
    fioFile->engine_data = nullptr;
    return 0;
}

/* UUSE owns cache coherency; fio has no host file descriptor to invalidate. */
int fioUuseInvalidate(struct thread_data *, struct fio_file *) { return 0; }

/* Fio calls this before open_file, so report the real logical size when the
 * file already exists; otherwise expose the requested size (0 stays 0 so fio
 * reports the missing size instead of receiving a fabricated layout).
 * Note: with create_serialize=1 (fio default) the main process calls this
 * before the engine is initialized and only the explicit size is available;
 * size-less workloads need create_serialize=0 so the per-thread setup can
 * stat the existing file. */
int fioUuseGetFileSize(struct thread_data *td, struct fio_file *fioFile) {
    const std::uint64_t requestedSize = static_cast<std::uint64_t>(td->o.size);
    auto *state = static_cast<FioUuseThread *>(td->io_ops_data);
    if (state != nullptr && state->client != nullptr && fioFile->file_name != nullptr) {
        auto opened =
            state->client->open(fioUuseFilePath(td, fioFile), uuse::OpenFlags::None).wait();
        if (opened.ok()) {
            auto openedResult = std::move(opened).value();
            if (openedResult.ok()) {
                auto file = std::move(openedResult).value();
                auto stat = file->stat();
                if (stat.ok()) {
                    fioFile->real_file_size = stat.value().logicalSize;
                    return 0;
                }
                fioLog("fio-uuse: get_file_size stat failed\n");
            } else {
                fioLog("fio-uuse: get_file_size open result failed\n");
            }
        } else {
            fioLog("fio-uuse: get_file_size open failed\n");
        }
    }
    fioFile->real_file_size = requestedSize;
    return 0;
}

/* Maps fio's DDIR_SYNC onto the UUSE namespace durability barrier. */
void executeSyncIo(io_u *io, const std::shared_ptr<uuse::File> &file) {
    auto synced = file->sync().wait();
    if (!synced.ok() || !synced.value().ok()) {
        setIoFailure(io, synced.ok() ? fioError(synced.value().status()) : EIO);
        return;
    }
    io->error = 0;
    io->resid = 0;
}

/* Releases a synchronous bounce lease on every return path. */
struct DmaBufferGuard {
    std::shared_ptr<uuse::Engine> engine;
    void *pointer = nullptr;
    std::size_t size = 0;

    ~DmaBufferGuard() {
        if (engine != nullptr && pointer != nullptr) {
            engine->releaseDmaBuffer(pointer, size);
        }
    }
};

/* Executes one FIO request without crossing the caller's queue callback. */
void executeFioIo(io_u *io, const std::shared_ptr<uuse::File> &file, bool direct,
                  const std::shared_ptr<uuse::Engine> &engine) {
    if (io->ddir == DDIR_SYNC) {
        executeSyncIo(io, file);
        return;
    }
    DmaBufferGuard bounceGuard;
    std::byte *data = static_cast<std::byte *>(io->xfer_buf);
    if (!direct) {
        /* The synchronous fallback uses the bounded engine DMA arena too. */
        if (engine == nullptr) {
            setIoFailure(io, EIO);
            return;
        }
        auto acquired = engine->acquireDmaBuffer(io->xfer_buflen, 0);
        if (!acquired.ok()) {
            setIoFailure(io, fioError(acquired.status()));
            return;
        }
        bounceGuard.engine = engine;
        bounceGuard.pointer = acquired.value().data();
        bounceGuard.size = acquired.value().size();
        data = acquired.value().data();
        if (io->ddir == DDIR_WRITE) {
            std::memcpy(data, io->xfer_buf, io->xfer_buflen);
        }
    }
    uuse::IoVec buffer{data, io->xfer_buflen};
    if (io->ddir == DDIR_READ) {
        auto taskResult = file->read(std::span<uuse::IoVec>(&buffer, 1), io->offset).wait();
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
            file->write(std::span<const uuse::IoVec>(&buffer, 1), io->offset).wait();
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

enum fio_q_status fioUuseQueue(struct thread_data *td, struct io_u *io) {
    auto *state = static_cast<FioUuseThread *>(td->io_ops_data);
    auto *file = io->file == nullptr ? nullptr
                                     : static_cast<FioUuseFile *>(io->file->engine_data);
    if (state == nullptr || file == nullptr || file->file == nullptr) {
        io->error = EINVAL;
        return FIO_Q_COMPLETED;
    }
    if (io->ddir == DDIR_SYNC) {
        executeSyncIo(io, file->file);
        return FIO_Q_COMPLETED;
    }
    if (!state->async) {
        executeFioIo(io, file->file, state->direct, state->engine);
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
    FioUusePending &slot = state->pending[slotIndex];
    const std::uint32_t generation = nextGeneration(slot);
    slot.state = PendingState::Submitted;
    slot.io = io;
    slot.file = file->file;
    slot.completion = {};

    std::byte *data = static_cast<std::byte *>(io->xfer_buf);
    if (!state->direct) {
        auto acquired = state->engine->acquireDmaBuffer(io->xfer_buflen, 0);
        if (!acquired.ok()) {
            clearPending(*state, slot);
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

    const uuse::CompletionToken token{static_cast<std::uint32_t>(slotIndex), generation};
    const uuse::IoVec buffer{data, io->xfer_buflen};
    uuse::Result<uuse::IoHandle> submitted =
        io->ddir == DDIR_READ
            ? state->client->submitRead(file->file, buffer, io->offset, token)
            : state->client->submitWrite(file->file, buffer, io->offset, token);
    if (!submitted.ok()) {
        const uuse::Status status = submitted.status();
        clearPending(*state, slot);
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

int fioUuseGetevents(struct thread_data *td, unsigned int min, unsigned int max,
                    const struct timespec *timeout) {
    auto *state = static_cast<FioUuseThread *>(td->io_ops_data);
    if (state == nullptr || max == 0 || !state->async) {
        return 0;
    }
    if (state->client == nullptr || state->protocolError) {
        return -EIO;
    }
    if (state->reportedRemaining != 0) {
        fioLog("fio-uuse: getevents called before the prior event batch was consumed\n");
        state->protocolError = true;
        return -EIO;
    }

    const std::size_t limit = std::min<std::size_t>(max, state->slotCount);
    const std::size_t minimum = std::min<std::size_t>(min, limit);
    state->reportedCount = 0;
    state->reportedRemaining = 0;
    auto result = pollClient(
        *state,
        std::span<uuse::CompletionEvent>(state->polled.get(), limit), minimum, limit,
        fioTimeout(timeout));
    const std::size_t count = std::min(result.count, limit);
    for (std::size_t index = 0; index < count; ++index) {
        (void)consumeCompletion(*state, state->polled[index], false);
    }
    if (state->protocolError || result.status == uuse::Status::Corrupt) {
        return state->reportedCount == 0 ? -EIO
                                         : static_cast<int>(state->reportedCount);
    }
    if (result.status != uuse::Status::Ok && result.status != uuse::Status::TimedOut) {
        return state->reportedCount == 0 ? -fioError(result.status)
                                         : static_cast<int>(state->reportedCount);
    }
    return static_cast<int>(state->reportedCount);
}

struct io_u *fioUuseEvent(struct thread_data *td, int event) {
    auto *state = static_cast<FioUuseThread *>(td->io_ops_data);
    if (state == nullptr || event < 0 ||
        static_cast<std::size_t>(event) >= state->reportedCount) {
        return nullptr;
    }
    return releaseReported(*state, static_cast<std::size_t>(event));
}

} // namespace

extern "C" struct ioengine_ops ioengine = {
    .name = "uuse",
    .version = FIO_IOOPS_VERSION,
    .flags = FIO_RAWIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_MEMALIGN | FIO_DISKLESSIO,
    .setup = fioUuseSetup,
    .init = fioUuseInit,
    .queue = fioUuseQueue,
    .getevents = fioUuseGetevents,
    .event = fioUuseEvent,
    .cleanup = fioUuseCleanup,
    .open_file = fioUuseOpen,
    .close_file = fioUuseClose,
    .invalidate = fioUuseInvalidate,
    .get_file_size = fioUuseGetFileSize,
    .iomem_alloc = fioUuseIomemAlloc,
    .iomem_free = fioUuseIomemFree,
    .option_struct_size = sizeof(FioUuseOptions),
    .options = options,
};
