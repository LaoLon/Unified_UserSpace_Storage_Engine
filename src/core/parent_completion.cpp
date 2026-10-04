#include "core/parent_completion.hpp"

#include "common/trace.hpp"

namespace uuse {

void ParentCompletion::init(std::uint32_t childCount, DoneCallback callback,
                            void *ctx) noexcept {
    pending_.store(childCount, std::memory_order_release);
    completedBytes_.store(0, std::memory_order_release);
    firstError_.store(0, std::memory_order_release);
    fired_.store(false, std::memory_order_release);
    callback_ = callback;
    ctx_ = ctx;
}

void ParentCompletion::childDone(int status, std::uint32_t bytes) noexcept {
    completedBytes_.fetch_add(bytes, std::memory_order_acq_rel);
    if (status != 0) {
        int expected = 0;
        firstError_.compare_exchange_strong(expected, status, std::memory_order_acq_rel);
    }
    if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (!fired_.exchange(true, std::memory_order_acq_rel)) {
            const int error = firstError_.load(std::memory_order_acquire);
            const std::uint64_t bytes = completedBytes_.load(std::memory_order_acquire);
            trace::event(trace::EventId::ParentComplete, bytes,
                         static_cast<std::uint64_t>(error));
            if (callback_ != nullptr) {
                callback_(ctx_, error, bytes);
            }
        }
    }
}

bool ParentCompletion::done() const noexcept { return pending_.load() == 0; }

int ParentCompletion::firstError() const noexcept {
    return firstError_.load(std::memory_order_acquire);
}

std::uint64_t ParentCompletion::completedBytes() const noexcept {
    return completedBytes_.load(std::memory_order_acquire);
}

std::uint32_t ParentCompletion::pending() const noexcept {
    return pending_.load(std::memory_order_acquire);
}

} // namespace uuse
