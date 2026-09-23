#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "core/io_planner.hpp"
#include "core/operation.hpp"
#include "hfs/types.hpp"

namespace hfs {

class DmaArena;
class ExtentMap;

/* Actor-local asynchronous backend contract. */
class AsyncDataBackend {
public:
    struct Request {
        /* Stable parent request id used by offline trace correlation. */
        std::uint64_t requestId = 0;
        bool write = false;
        bool rmw = false;
        std::uint64_t physicalBlock = 0;
        std::uint64_t physicalByte = 0;
        bool physicalByteValid = false;
        std::uint32_t deviceId = 0;
        std::uint32_t bytes = 0;
        std::uint32_t spanBytes = 0;
        std::uint32_t spanOffset = 0;
        std::uint32_t childIndex = 0;
        std::byte *buffer = nullptr;
        std::size_t bufferBytes = 0;
    };

    enum class SubmitDisposition : std::uint8_t {
        Accepted,
        WaitingForCredit,
        Rejected,
    };

    struct SubmitResult {
        SubmitDisposition disposition = SubmitDisposition::Rejected;
        int backendErrno = 0;
    };

    using CompletionCallback = void (*)(void *context, std::uint32_t childIndex,
                                        Status status, int backendErrno,
                                        std::uint32_t completedBytes) noexcept;

    virtual ~AsyncDataBackend() = default;

    /* Called only by the owning shard actor. WaitingForCredit represents the
     * backend's same-reactor io-wait registration and is already accepted. */
    virtual SubmitResult submitRead(const Request &request, CompletionCallback callback,
                                    void *context) noexcept = 0;
    virtual SubmitResult submitWrite(const Request &request, CompletionCallback callback,
                                     void *context) noexcept = 0;

    /* Actor-local retry pump for backends that accept requests while their
     * internal credit is exhausted (SubmitDisposition::WaitingForCredit). */
    virtual void pump() noexcept {}

    /* Reactor wake hint after a ring publish armed the shard doorbell. Called
     * from the publishing thread; must be nonblocking and coalescing. */
    virtual void wake() noexcept {}
};

/* Credit and completion sink owned by the frontend client. */
class AsyncDataSink {
public:
    virtual ~AsyncDataSink() = default;

    /* Returns false only when a request may be rejected before ring publish. */
    virtual bool tryAcquireCredit() noexcept = 0;

    /* Rolls back a credit for a request rejected before ring publish. */
    virtual void releaseCredit() noexcept = 0;

    /* Receives exactly one value completion for every accepted request.
     * Returns false when the event could not be delivered (queue overflow or
     * invalid shard); the sink must then fail closed with its own fatal state. */
    virtual bool publish(std::uint32_t shard, const CompletionEvent &event) noexcept = 0;

    /* Batched delivery for one shard; default keeps per-event semantics. */
    virtual bool publishBatch(std::uint32_t shard, const CompletionEvent *events,
                              std::size_t count) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            if (!publish(shard, events[index])) {
                return false;
            }
        }
        return true;
    }
};

/* One contiguous read or fixed-size preallocated overwrite request. */
struct AsyncDataRequest {
    FileId fileId = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::span<std::byte> buffer{};
    /* Borrowed only during submit; accepted requests use a fixed sidecar snapshot. */
    const ExtentMap *extentMap = nullptr;
    std::uint64_t logicalSize = 0;
    bool write = false;
    bool fixedOverwrite = true;
};

/* Bounded service resources are allocated during construction. */
struct AsyncDataServiceConfig {
    std::uint32_t shardCount = 1;
    std::size_t ringDepth = 64;
    std::size_t maxOperations = 64;
    std::size_t maxInodesPerShard = 64;
    std::size_t maxPreparingPerShard = 0;
    /* Native device sector used only by the read planner. */
    std::uint32_t nativeReadBlockSize = 0;
    IoPlanner::Config planner{};
};

/* Engine-owned operation path shared by all frontend clients. */
class AsyncDataService {
public:
    AsyncDataService(const AsyncDataServiceConfig &config, DmaArena &arena,
                     std::span<AsyncDataBackend *> backends) noexcept;
    ~AsyncDataService();

    AsyncDataService(const AsyncDataService &) = delete;
    AsyncDataService &operator=(const AsyncDataService &) = delete;

    bool valid() const noexcept;

    /* Publishes only after all pre-publish resources and references exist.
     * sinkOwner keeps the sink (and the client state behind it) alive until
     * the terminal completion has been published; it must not be null. */
    Result<IoHandle> submit(const AsyncDataRequest &request, CompletionToken token,
                            AsyncDataSink &sink, std::shared_ptr<void> sinkOwner);

    /* Runs one shard actor and its deferred preparation/completion work. */
    std::size_t pump(std::uint32_t shard) noexcept;
    std::size_t pumpAll() noexcept;

    struct Stats {
        std::uint64_t accepted = 0;
        std::uint64_t busy = 0;
        std::uint64_t terminal = 0;
        std::uint64_t completionPublished = 0;
        std::uint64_t completionDropped = 0;
        std::uint64_t planFailures = 0;
        std::uint64_t dmaFailures = 0;
        std::uint64_t preparing = 0;
        std::uint64_t schedulerWaits = 0;
        /* Children with at least one accepted backend submission. */
        std::uint64_t childSubmits = 0;
        /* Terminal children that had been submitted. */
        std::uint64_t childCompletes = 0;
        /* Children rejected before any accepted submission. */
        std::uint64_t childSubmitRejected = 0;
        std::uint64_t tokenEvents = 0;
    };

    Stats stats() const noexcept;
    std::size_t outstanding() const noexcept;
    std::size_t shardCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hfs
