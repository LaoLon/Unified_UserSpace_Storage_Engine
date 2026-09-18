#include "fio_completion_hub.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <thread>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace hfs::fio {

void FioCompletionHub::Endpoint::reset() noexcept {
    epoch_.store(0, std::memory_order_relaxed);
    waiters_.store(0, std::memory_order_relaxed);
    armed_.store(false, std::memory_order_relaxed);
}

void FioCompletionHub::Endpoint::notify() noexcept {
    epoch_.fetch_add(1, std::memory_order_release);
#if defined(__linux__)
    if (waiters_.load(std::memory_order_acquire) == 0) {
        return;
    }
    bool expected = false;
    if (armed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
        (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&epoch_),
                        FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    }
#endif
}

void FioCompletionHub::Endpoint::prepareWait() noexcept {
    armed_.store(false, std::memory_order_release);
}

std::uint32_t FioCompletionHub::Endpoint::epoch() const noexcept {
    return epoch_.load(std::memory_order_acquire);
}

bool FioCompletionHub::Endpoint::waitSince(
    std::uint32_t observed, std::chrono::microseconds timeout) noexcept {
    if (timeout.count() <= 0 || epoch() != observed) {
        return epoch() != observed;
    }
#if defined(__linux__)
    timespec duration{};
    duration.tv_sec = static_cast<time_t>(timeout.count() / 1000000);
    duration.tv_nsec = static_cast<long>((timeout.count() % 1000000) * 1000);
    waiters_.fetch_add(1, std::memory_order_acq_rel);
    if (epoch() != observed) {
        waiters_.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }
    const int result = static_cast<int>(::syscall(
        SYS_futex, reinterpret_cast<std::uint32_t *>(&epoch_), FUTEX_WAIT_PRIVATE,
        observed, &duration, nullptr, 0));
    const int error = errno;
    waiters_.fetch_sub(1, std::memory_order_acq_rel);
    if (result == 0 || error == EAGAIN || error == EINTR) {
        return true;
    }
    return error != ETIMEDOUT;
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (epoch() == observed) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining = deadline - now;
        const auto slice = std::min(remaining, std::chrono::milliseconds(1));
        std::this_thread::sleep_for(slice);
    }
    return true;
#endif
}

bool FioCompletionHub::initialize() noexcept {
    for (Endpoint &endpoint : endpoints_) {
        endpoint.reset();
        endpoint.inUse_.store(false, std::memory_order_relaxed);
    }
    active_.store(true, std::memory_order_release);
    return true;
}

void FioCompletionHub::shutdown() noexcept {
    active_.store(false, std::memory_order_release);
    for (Endpoint &endpoint : endpoints_) {
        endpoint.prepareWait();
        endpoint.inUse_.store(false, std::memory_order_release);
    }
}

FioCompletionHub::Endpoint *FioCompletionHub::acquire() noexcept {
    if (!active_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    for (Endpoint &endpoint : endpoints_) {
        bool expected = false;
        if (endpoint.inUse_.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
            endpoint.reset();
            return &endpoint;
        }
    }
    return nullptr;
}

void FioCompletionHub::release(Endpoint *endpoint) noexcept {
    if (endpoint != nullptr) {
        endpoint->prepareWait();
        endpoint->inUse_.store(false, std::memory_order_release);
    }
}

void FioCompletionHub::wakeCallback(void *context) noexcept {
    if (context != nullptr) {
        static_cast<Endpoint *>(context)->notify();
    }
}

} // namespace hfs::fio
