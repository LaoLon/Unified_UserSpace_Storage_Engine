#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hfs {

class OperationPool;

enum class OperationKind : std::uint8_t {
    Read,
    Write,
    Append,
    Sync,
    Other,
};

enum class OperationState : std::uint8_t {
    Created,
    Enqueued,
    WaitingDependency,
    Ready,
    Submitted,
    Completed,
    Cancelled,
    Failed,
};

/* Intrusive-lifetime operation object allocated from a bounded pool. */
class Operation {
public:
    static void setPoolCapacity(std::size_t count) noexcept;
    static Operation *create(std::uint64_t requestId = 0) noexcept;
    static std::size_t liveCount() noexcept;
    static std::size_t poolOverflow() noexcept;

    void addRef() noexcept;
    void release() noexcept; /* destroys when the last reference drops */

    /* Terminal transition; returns false when already completed/cancelled. */
    bool complete(int status) noexcept;
    bool isTerminal() const noexcept;

    OperationKind kind = OperationKind::Other;
    std::atomic<OperationState> state{OperationState::Created};
    std::uint64_t requestId = 0;
    std::uint64_t fileId = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint32_t shard = 0;
    void *userCtx = nullptr;
    void (*onComplete)(Operation *) = nullptr;
    int status = 0;
    std::uint64_t completedBytes = 0;
    std::uint32_t retryCount = 0;
    void *schedulerNode = nullptr;
    void *dmaLease = nullptr;
    void *parent = nullptr;

private:
    friend class OperationPool;

    Operation() = default;
    ~Operation() = default;

    std::atomic<std::uint32_t> refs_{1};
    std::atomic<bool> terminal_{false};
    Operation *freeNext_ = nullptr; /* pool free list link */
};

} // namespace hfs
