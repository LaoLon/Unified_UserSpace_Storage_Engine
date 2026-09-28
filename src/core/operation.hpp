#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hfs/async.hpp"
#include "hfs/types.hpp"

namespace hfs {

class OperationPool;

/* Explicit owners share one intrusive Operation reference count. */
enum class OperationRefRole : std::uint8_t {
    Legacy = 0,
    FrontendHandle,
    Actor,
    Scheduler,
    Backend,
    Parent,
    Count,
};

/* Non-owning callback hook for publishing one value completion event. */
struct CompletionSink {
    using PublishCallback = void (*)(void *context, const CompletionEvent &event);

    void *context = nullptr;
    PublishCallback publish = nullptr;
};

/* Bounded async bookkeeping storage; it does not own frontend or file objects.
 * Per-child state lives in the async data service's RequestState sidecar; only
 * aggregate counters are kept here so the operation remains small and the
 * per-request reset cost stays bounded (hfs2.0.md §5.2). */
struct AsyncIoState {
    void *schedulerNode = nullptr;
    void *parent = nullptr;
    void *backendState = nullptr;
    CompletionToken token{};
    Status status = Status::Ok;
    int backendErrno = 0;
    std::uint64_t completedBytes = 0;
    std::uint32_t childCount = 0;
    std::uint32_t submittedChildren = 0;
    std::uint32_t drainedChildren = 0;
    std::uint32_t flags = 0;
    bool completionPublished = false;

    /* Clears every operation-local sidecar field without releasing external objects. */
    void reset() noexcept { *this = AsyncIoState{}; }
};

/* Optional bounded external sidecar hook; Operation never deletes the storage. */
struct AsyncStorageHook {
    static constexpr std::size_t kMaxBytes = 4096;
    using ResetCallback = void (*)(void *storage);

    void *storage = nullptr;
    std::size_t bytes = 0;
    ResetCallback reset = nullptr;
};

enum class OperationKind : std::uint8_t {
    Read,
    Write,
    Append,
    Sync,
    Other,
};

enum class OperationState : std::uint8_t {
    Created = 0,
    Enqueued = 1,
    WaitingDependency = 2,
    Ready = 3,
    Submitted = 4,
    Completed = 5,
    Cancelled = 6,
    Failed = 7,
    Preparing = 8,
    Draining = 9,
    CompletionPublished = 10,
    Reaped = 11,
};

/* Intrusive-lifetime operation object allocated from a bounded pool. */
class Operation {
public:
    static void setPoolCapacity(std::size_t count) noexcept;
    static Operation *create(std::uint64_t requestId = 0) noexcept;
    static std::size_t liveCount() noexcept;
    static std::size_t poolOverflow() noexcept;

    /* Adds a legacy reference; role-specific callers should use acquireRef(). */
    void addRef() noexcept;
    void release() noexcept; /* destroys when the last reference drops */

    /* Acquires a role reference while the caller already keeps the Operation alive. */
    void acquireRef(OperationRefRole role) noexcept;
    /* Acquires a role reference without resurrecting a pooled Operation. */
    bool tryAcquireRef(OperationRefRole role) noexcept;
    /* Releases one role reference and returns false for an unmatched release. */
    bool releaseRef(OperationRefRole role) noexcept;
    std::uint32_t refCount() const noexcept;
    std::uint32_t refCount(OperationRefRole role) const noexcept;

    /* Creates a move-only frontend reference without owning Client or File. */
    IoHandle acquireFrontendHandle(CompletionToken token = {}) noexcept;

    /* Installs or clears the non-owning completion sink. */
    void setCompletionSink(CompletionSink sink) noexcept { completionSink_ = sink; }
    CompletionSink completionSink() const noexcept { return completionSink_; }
    void setCompletionToken(CompletionToken token) noexcept {
        completionToken_ = token;
        asyncState_.token = token;
    }
    CompletionToken completionToken() const noexcept { return completionToken_; }

    /* Accesses the fixed operation-local async state. */
    AsyncIoState &asyncState() noexcept { return asyncState_; }
    const AsyncIoState &asyncState() const noexcept { return asyncState_; }

    /* Attaches bounded external storage without transferring ownership. */
    bool attachAsyncStorage(void *storage, std::size_t bytes,
                            AsyncStorageHook::ResetCallback reset = nullptr) noexcept;
    void detachAsyncStorage() noexcept;
    const AsyncStorageHook &asyncStorage() const noexcept { return asyncStorage_; }

    /* Publishes a non-owning token exactly once after terminal completion. */
    bool publishCompletion() noexcept;
    bool isCompletionPublished() const noexcept {
        return completionPublished_.load(std::memory_order_acquire);
    }
    bool isReaped() const noexcept {
        return state.load(std::memory_order_acquire) == OperationState::Reaped;
    }

    /* Performs a non-terminal state CAS for actor/scheduler lifecycle steps. */
    bool transition(OperationState expected, OperationState desired) noexcept;
    bool beginPreparing() noexcept;
    bool beginDraining() noexcept;

    /* Terminal transition only; publication is intentionally independent. */
    bool completeTerminal(int completionStatus) noexcept;
    bool completeTerminal(Status resultStatus, int backendStatus = 0,
                          std::uint64_t bytes = 0) noexcept;

    /* Legacy terminal-and-publish operation retained for existing callers. */
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
    Status resultStatus = Status::Ok;
    int backendErrno = 0;

private:
    friend class OperationPool;

    Operation() = default;
    ~Operation() = default;

    void resetForReuse(std::uint64_t requestId) noexcept;
    bool completeTerminalState(OperationState terminalState, int legacyStatus,
                               Status resultStatus, int backendStatus,
                               std::uint64_t bytes) noexcept;
    static bool validRefRole(OperationRefRole role) noexcept;
    static std::size_t refRoleIndex(OperationRefRole role) noexcept;

    std::atomic<std::uint32_t> refs_{1};
    std::array<std::atomic<std::uint32_t>,
               static_cast<std::size_t>(OperationRefRole::Count)>
        roleRefs_{};
    std::atomic<bool> terminalClaimed_{false};
    std::atomic<bool> terminal_{false};
    std::atomic<bool> completionPublished_{false};
    CompletionToken completionToken_{};
    CompletionSink completionSink_{};
    AsyncIoState asyncState_{};
    AsyncStorageHook asyncStorage_{};
};

} // namespace hfs
