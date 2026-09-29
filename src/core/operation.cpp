#include "core/operation.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <mutex>
#include <new>

#include "common/trace.hpp"
#include "core/index_stack.hpp"
#include "hfs/types.hpp"

namespace hfs {

/* Bounded pool for Operation objects; see operation.hpp. Storage is
 * preallocated once, so create/destroy never allocate and the free list is a
 * bounded lock-free index stack shared by frontend and reactor threads. */
class OperationPool {
public:
    static OperationPool &instance() noexcept {
        static OperationPool pool;
        return pool;
    }

    /* Control-plane sizing; refuses to resize while any operation is live. */
    void setCapacity(std::size_t count) noexcept {
        if (count == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (live_.load(std::memory_order_acquire) != 0) {
            return;
        }
        capacity_ = count;
        if (storageCapacity_ < count) {
            growLocked(count);
        }
    }

    Operation *create(std::uint64_t requestId) noexcept {
        ensureStorage();
        /* Review §7.3: time the lock-free acquire only while tracing. */
        const bool measure = trace::enabled();
        const auto start = measure ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        const auto emit = [&](std::uint64_t contended) {
            if (!measure) {
                return;
            }
            trace::event(trace::EventId::OpPoolAcquire, requestId, contended,
                         static_cast<std::uint64_t>(
                             std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count()));
        };
        const std::size_t live = live_.fetch_add(1, std::memory_order_acq_rel);
        if (live >= capacity_) {
            live_.fetch_sub(1, std::memory_order_acq_rel);
            overflow_.fetch_add(1, std::memory_order_relaxed);
            emit(1);
            return nullptr;
        }
        std::uint32_t index = 0;
        if (!freeStack_.pop(index)) {
            live_.fetch_sub(1, std::memory_order_acq_rel);
            overflow_.fetch_add(1, std::memory_order_relaxed);
            emit(1);
            return nullptr;
        }
        Operation *operation = storage_ + index;
        operation->resetForReuse(requestId);
        std::size_t high = highWatermark_.load(std::memory_order_relaxed);
        while (live + 1 > high &&
               !highWatermark_.compare_exchange_weak(high, live + 1,
                                                     std::memory_order_relaxed)) {
        }
        emit(0);
        return operation;
    }

    void destroy(Operation *operation) noexcept {
        if (operation == nullptr || storage_ == nullptr) {
            return;
        }
        const auto index = static_cast<std::uint32_t>(operation - storage_);
        (void)freeStack_.push(index);
        live_.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::size_t liveCount() const noexcept { return live_.load(std::memory_order_acquire); }

    std::size_t overflow() const noexcept {
        return static_cast<std::size_t>(overflow_.load(std::memory_order_relaxed));
    }

    std::size_t highWatermark() const noexcept {
        return highWatermark_.load(std::memory_order_relaxed);
    }

private:
    OperationPool() = default;
    ~OperationPool() { delete[] storage_; }

    void ensureStorage() noexcept {
        if (storage_ != nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (storage_ == nullptr) {
            growLocked(capacity_);
        }
    }

    void growLocked(std::size_t count) noexcept {
        if (count == 0) {
            return;
        }
        Operation *storage = new (std::nothrow) Operation[count];
        if (storage == nullptr) {
            return;
        }
        if (!freeStack_.initialize(count)) {
            delete[] storage;
            return;
        }
        /* Only reachable while no operation is live (control-plane resize). */
        delete[] storage_;
        storage_ = storage;
        storageCapacity_ = count;
    }

    mutable std::mutex controlMutex_;
    Operation *storage_ = nullptr;
    std::size_t storageCapacity_ = 0;
    std::size_t capacity_ = 4096;
    std::atomic<std::size_t> live_{0};
    std::atomic<std::size_t> highWatermark_{0};
    std::atomic<std::uint64_t> overflow_{0};
    BoundedIndexStack freeStack_;
};

void Operation::setPoolCapacity(std::size_t count) noexcept {
    OperationPool::instance().setCapacity(count);
}

Operation *Operation::create(std::uint64_t requestId) noexcept {
    return OperationPool::instance().create(requestId);
}

void Operation::resetForReuse(std::uint64_t newRequestId) noexcept {
    if (asyncStorage_.reset != nullptr && asyncStorage_.storage != nullptr) {
        asyncStorage_.reset(asyncStorage_.storage);
    }

    refs_.store(1, std::memory_order_release);
    for (auto &roleRef : roleRefs_) {
        roleRef.store(0, std::memory_order_release);
    }
    roleRefs_[refRoleIndex(OperationRefRole::Legacy)].store(1,
                                                              std::memory_order_release);
    terminalClaimed_.store(false, std::memory_order_release);
    terminal_.store(false, std::memory_order_release);
    completionPublished_.store(false, std::memory_order_release);

    state.store(OperationState::Created, std::memory_order_release);
    kind = OperationKind::Other;
    requestId = newRequestId;
    fileId = 0;
    offset = 0;
    length = 0;
    shard = 0;
    userCtx = nullptr;
    onComplete = nullptr;
    status = 0;
    resultStatus = Status::Ok;
    backendErrno = 0;
    completedBytes = 0;
    retryCount = 0;
    schedulerNode = nullptr;
    dmaLease = nullptr;
    parent = nullptr;
    completionToken_ = {};
    completionSink_ = {};
    asyncState_.reset();
    asyncStorage_ = {};
}

bool Operation::validRefRole(OperationRefRole role) noexcept {
    return static_cast<std::size_t>(role) <
           static_cast<std::size_t>(OperationRefRole::Count);
}

std::size_t Operation::refRoleIndex(OperationRefRole role) noexcept {
    return static_cast<std::size_t>(role);
}

void Operation::addRef() noexcept { acquireRef(OperationRefRole::Legacy); }

void Operation::release() noexcept { (void)releaseRef(OperationRefRole::Legacy); }

void Operation::acquireRef(OperationRefRole role) noexcept {
    if (!validRefRole(role)) {
        return;
    }
    refs_.fetch_add(1, std::memory_order_relaxed);
    roleRefs_[refRoleIndex(role)].fetch_add(1, std::memory_order_relaxed);
}

bool Operation::tryAcquireRef(OperationRefRole role) noexcept {
    if (!validRefRole(role)) {
        return false;
    }

    auto expected = refs_.load(std::memory_order_acquire);
    for (;;) {
        if (expected == 0 || expected == std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        if (refs_.compare_exchange_weak(expected, expected + 1,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            roleRefs_[refRoleIndex(role)].fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
}

bool Operation::releaseRef(OperationRefRole role) noexcept {
    if (!validRefRole(role)) {
        return false;
    }

    auto &roleRef = roleRefs_[refRoleIndex(role)];
    auto expectedRole = roleRef.load(std::memory_order_acquire);
    for (;;) {
        if (expectedRole == 0) {
            return false;
        }
        if (roleRef.compare_exchange_weak(expectedRole, expectedRole - 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            break;
        }
    }

    if (refs_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return true;
    }

    state.store(OperationState::Reaped, std::memory_order_release);
    if (asyncStorage_.reset != nullptr && asyncStorage_.storage != nullptr) {
        asyncStorage_.reset(asyncStorage_.storage);
    }
    asyncStorage_ = {};
    OperationPool::instance().destroy(this);
    return true;
}

std::uint32_t Operation::refCount() const noexcept {
    return refs_.load(std::memory_order_acquire);
}

std::uint32_t Operation::refCount(OperationRefRole role) const noexcept {
    if (!validRefRole(role)) {
        return 0;
    }
    return roleRefs_[refRoleIndex(role)].load(std::memory_order_acquire);
}

IoHandle Operation::acquireFrontendHandle(CompletionToken token) noexcept {
    if (!tryAcquireRef(OperationRefRole::FrontendHandle)) {
        return {};
    }
    completionToken_ = token;
    asyncState_.token = token;
    return IoHandle(this, requestId, token);
}

IoHandle::IoHandle(IoHandle &&other) noexcept
    : operation_(other.operation_), requestId_(other.requestId_), token_(other.token_),
      owner_(std::move(other.owner_)), ownerContext_(other.ownerContext_),
      ownerRelease_(other.ownerRelease_) {
    other.operation_ = nullptr;
    other.requestId_ = 0;
    other.token_ = {};
    other.ownerContext_ = nullptr;
    other.ownerRelease_ = nullptr;
}

IoHandle &IoHandle::operator=(IoHandle &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    operation_ = other.operation_;
    requestId_ = other.requestId_;
    token_ = other.token_;
    owner_ = std::move(other.owner_);
    ownerContext_ = other.ownerContext_;
    ownerRelease_ = other.ownerRelease_;
    other.operation_ = nullptr;
    other.requestId_ = 0;
    other.token_ = {};
    other.ownerContext_ = nullptr;
    other.ownerRelease_ = nullptr;
    return *this;
}

IoHandle::~IoHandle() { reset(); }

void IoHandle::reset() noexcept {
    Operation *operation = operation_;
    void *ownerContext = ownerContext_;
    void (*ownerRelease)(void *) = ownerRelease_;
    operation_ = nullptr;
    requestId_ = 0;
    token_ = {};
    ownerContext_ = nullptr;
    ownerRelease_ = nullptr;
    if (operation != nullptr) {
        (void)operation->releaseRef(OperationRefRole::FrontendHandle);
    }
    if (ownerRelease != nullptr) {
        ownerRelease(ownerContext);
    }
    owner_.reset();
}

bool Operation::attachAsyncStorage(void *storage, std::size_t bytes,
                                    AsyncStorageHook::ResetCallback reset) noexcept {
    if (storage == nullptr || bytes == 0 || bytes > AsyncStorageHook::kMaxBytes ||
        asyncStorage_.storage != nullptr) {
        return false;
    }
    asyncStorage_.storage = storage;
    asyncStorage_.bytes = bytes;
    asyncStorage_.reset = reset;
    return true;
}

void Operation::detachAsyncStorage() noexcept {
    if (asyncStorage_.reset != nullptr && asyncStorage_.storage != nullptr) {
        asyncStorage_.reset(asyncStorage_.storage);
    }
    asyncStorage_ = {};
}

bool Operation::transition(OperationState expected, OperationState desired) noexcept {
    switch (desired) {
    case OperationState::Created:
    case OperationState::Enqueued:
    case OperationState::WaitingDependency:
    case OperationState::Ready:
    case OperationState::Submitted:
    case OperationState::Preparing:
    case OperationState::Draining:
        break;
    case OperationState::Completed:
    case OperationState::Cancelled:
    case OperationState::Failed:
    case OperationState::CompletionPublished:
    case OperationState::Reaped:
        return false;
    }
    if (terminalClaimed_.load(std::memory_order_acquire)) {
        return false;
    }
    return state.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
}

bool Operation::beginPreparing() noexcept {
    return transition(OperationState::Enqueued, OperationState::Preparing);
}

bool Operation::beginDraining() noexcept {
    auto current = state.load(std::memory_order_acquire);
    for (;;) {
        if (current == OperationState::Draining) {
            return true;
        }
        if (current == OperationState::Completed || current == OperationState::Cancelled ||
            current == OperationState::Failed ||
            current == OperationState::CompletionPublished || current == OperationState::Reaped) {
            return false;
        }
        if (state.compare_exchange_weak(current, OperationState::Draining,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            return true;
        }
    }
}

bool Operation::completeTerminalState(OperationState terminalState, int legacyStatus,
                                      Status newResultStatus, int backendStatus,
                                      std::uint64_t bytes) noexcept {
    bool expected = false;
    if (!terminalClaimed_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
        return false;
    }
    status = legacyStatus;
    resultStatus = newResultStatus;
    backendErrno = backendStatus;
    completedBytes = bytes;
    state.store(terminalState, std::memory_order_release);
    terminal_.store(true, std::memory_order_release);
    return true;
}

bool Operation::completeTerminal(int completionStatus) noexcept {
    const Status newResultStatus = completionStatus == 0 ? Status::Ok : Status::IoError;
    const int backendStatus = completionStatus < 0 ? completionStatus : 0;
    const OperationState terminalState = completionStatus == 0 ? OperationState::Completed
                                                                : OperationState::Failed;
    return completeTerminalState(terminalState, completionStatus, newResultStatus,
                                 backendStatus, 0);
}

bool Operation::completeTerminal(Status newResultStatus, int backendStatus,
                                 std::uint64_t bytes) noexcept {
    const OperationState terminalState = newResultStatus == Status::Ok
                                             ? OperationState::Completed
                                             : (newResultStatus == Status::Cancelled
                                                    ? OperationState::Cancelled
                                                    : OperationState::Failed);
    const int legacyStatus = backendStatus != 0
                                 ? backendStatus
                                 : (newResultStatus == Status::Ok ? 0 : -1);
    return completeTerminalState(terminalState, legacyStatus, newResultStatus,
                                 backendStatus, bytes);
}

bool Operation::publishCompletion() noexcept {
    if (!terminal_.load(std::memory_order_acquire)) {
        return false;
    }
    bool expected = false;
    if (!completionPublished_.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
        return false;
    }

    CompletionEvent event;
    event.requestId = requestId;
    event.token = completionToken_;
    event.status = resultStatus;
    event.backendErrno = backendErrno;
    event.completedBytes = completedBytes;
    asyncState_.completionPublished = true;
    state.store(OperationState::CompletionPublished, std::memory_order_release);

    if (completionSink_.publish != nullptr) {
        completionSink_.publish(completionSink_.context, event);
    }
    if (onComplete != nullptr) {
        onComplete(this);
    }
    return true;
}

bool Operation::complete(int completionStatus) noexcept {
    if (!completeTerminal(completionStatus)) {
        return false;
    }
    (void)publishCompletion();
    return true;
}

bool Operation::isTerminal() const noexcept {
    return terminal_.load(std::memory_order_acquire);
}

std::size_t Operation::liveCount() noexcept {
    return OperationPool::instance().liveCount();
}

std::size_t Operation::poolOverflow() noexcept {
    return OperationPool::instance().overflow();
}

} // namespace hfs
