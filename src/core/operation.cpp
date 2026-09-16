#include "core/operation.hpp"

#include <atomic>
#include <cstddef>
#include <mutex>

#include "hfs/types.hpp"

namespace hfs {

/* Bounded pool for Operation objects; see operation.hpp. */
class OperationPool {
public:
    static OperationPool &instance() noexcept {
        static OperationPool pool;
        return pool;
    }

    void setCapacity(std::size_t count) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count != 0 && live_ == 0) {
            capacity_ = count > freeCount_ ? count : freeCount_;
        }
    }

    Operation *create(std::uint64_t requestId) noexcept {
        Operation *operation = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (free_ != nullptr) {
                operation = free_;
                free_ = operation->freeNext_;
                operation->freeNext_ = nullptr;
                --freeCount_;
            } else if (live_ < capacity_) {
                operation = new Operation();
            } else {
                overflow_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            ++live_;
            if (live_ > highWatermark_) {
                highWatermark_ = live_;
            }
        }
        operation->refs_.store(1, std::memory_order_release);
        operation->terminal_.store(false, std::memory_order_release);
        operation->state.store(OperationState::Created, std::memory_order_release);
        operation->requestId = requestId;
        operation->kind = OperationKind::Other;
        operation->fileId = 0;
        operation->offset = 0;
        operation->length = 0;
        operation->shard = 0;
        operation->userCtx = nullptr;
        operation->onComplete = nullptr;
        operation->status = 0;
        operation->completedBytes = 0;
        operation->retryCount = 0;
        operation->schedulerNode = nullptr;
        operation->dmaLease = nullptr;
        operation->parent = nullptr;
        return operation;
    }

    void destroy(Operation *operation) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        operation->freeNext_ = free_;
        free_ = operation;
        ++freeCount_;
        --live_;
    }

    std::size_t liveCount() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return live_;
    }

    std::size_t overflow() const noexcept {
        return static_cast<std::size_t>(overflow_.load(std::memory_order_relaxed));
    }

    std::size_t highWatermark() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return highWatermark_;
    }

private:
    OperationPool() = default;

    mutable std::mutex mutex_;
    Operation *free_ = nullptr;
    std::size_t capacity_ = 4096;
    std::size_t live_ = 0;
    std::size_t freeCount_ = 0;
    std::size_t highWatermark_ = 0;
    std::atomic<std::uint64_t> overflow_{0};
};

void Operation::setPoolCapacity(std::size_t count) noexcept {
    OperationPool::instance().setCapacity(count);
}

Operation *Operation::create(std::uint64_t requestId) noexcept {
    return OperationPool::instance().create(requestId);
}

void Operation::addRef() noexcept { refs_.fetch_add(1, std::memory_order_relaxed); }

void Operation::release() noexcept {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    OperationPool::instance().destroy(this);
}

bool Operation::complete(int completionStatus) noexcept {
    if (terminal_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    status = completionStatus;
    state.store(completionStatus == 0 ? OperationState::Completed : OperationState::Failed,
                std::memory_order_release);
    if (onComplete != nullptr) {
        onComplete(this);
    }
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
