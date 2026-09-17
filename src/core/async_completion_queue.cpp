#include "core/async_completion_queue.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <limits>
#include <new>
#include <thread>

#if defined(__linux__)
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace hfs {

namespace {

/* Converts a positive duration to poll milliseconds without early timeout. */
#if defined(__linux__)
int timeoutMilliseconds(std::chrono::microseconds timeout) noexcept {
    if (timeout.count() <= 0) {
        return 0;
    }
    const auto milliseconds = timeout.count() / std::chrono::microseconds(1000).count() +
                              (timeout.count() % std::chrono::microseconds(1000).count() != 0);
    if (milliseconds >= static_cast<long long>(INT_MAX)) {
        return INT_MAX;
    }
    return static_cast<int>(milliseconds);
}
#endif

} // namespace

CompletionNotifier::CompletionNotifier() noexcept = default;

CompletionNotifier::~CompletionNotifier() { closeEventFd(); }

bool CompletionNotifier::initialize() noexcept {
#if defined(__linux__)
    if (eventFd_ >= 0) {
        return true;
    }
    eventFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    /* A host without eventfd still has the producer-lock-free epoch path. */
#endif
    return true;
}

void CompletionNotifier::closeEventFd() noexcept {
#if defined(__linux__)
    if (eventFd_ >= 0) {
        ::close(eventFd_);
        eventFd_ = -1;
    }
#endif
}

bool CompletionNotifier::notify() noexcept {
    epoch_.fetch_add(1, std::memory_order_release);
    notifications_.fetch_add(1, std::memory_order_relaxed);
#if defined(__linux__)
    if (eventFd_ >= 0) {
        const std::uint64_t increment = 1;
        const ssize_t written = ::write(eventFd_, &increment, sizeof(increment));
        if (written == static_cast<ssize_t>(sizeof(increment))) {
            return true;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* A saturated eventfd already represents a pending wake. */
            saturated_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (written < 0 && errno == EINTR) {
            /* The ring remains authoritative; a later transition will retry. */
            errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
#endif
    return true;
}

bool CompletionNotifier::wait(std::chrono::microseconds timeout) noexcept {
    return waitSince(epoch(), timeout);
}

bool CompletionNotifier::waitSince(std::uint64_t observed,
                                   std::chrono::microseconds timeout) noexcept {
    if (timeout.count() <= 0) {
        return false;
    }
#if defined(__linux__)
    if (eventFd_ >= 0) {
        pollfd descriptor{};
        descriptor.fd = eventFd_;
        descriptor.events = POLLIN;
        const int result = ::poll(&descriptor, 1, timeoutMilliseconds(timeout));
        if (result <= 0) {
            return false;
        }
        if ((descriptor.revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            std::uint64_t value = 0;
            const ssize_t readCount = ::read(eventFd_, &value, sizeof(value));
            if (readCount < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                errors_.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        return false;
    }
#endif

    /* The fallback deliberately uses only atomics on the producer path. */
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (epoch_.load(std::memory_order_acquire) != observed) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining = deadline - now;
        const auto oneMillisecond =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(1));
        const auto slice = remaining < oneMillisecond ? remaining : oneMillisecond;
        std::this_thread::sleep_for(slice);
    }
}

bool CompletionNotifier::usingEventFd() const noexcept {
#if defined(__linux__)
    return eventFd_ >= 0;
#else
    return false;
#endif
}

std::uint64_t CompletionNotifier::epoch() const noexcept {
    return epoch_.load(std::memory_order_acquire);
}

CompletionNotifier::Diagnostics CompletionNotifier::diagnostics() const noexcept {
    Diagnostics result;
    result.notifications = notifications_.load(std::memory_order_relaxed);
    result.saturated = saturated_.load(std::memory_order_relaxed);
    result.errors = errors_.load(std::memory_order_relaxed);
    result.eventFd = usingEventFd();
    return result;
}

AsyncCompletionRing::AsyncCompletionRing(std::size_t capacity) noexcept {
    initialize(capacity);
}

bool AsyncCompletionRing::initialize(std::size_t capacity) noexcept {
    if (capacity == 0 || storage_ != nullptr) {
        return false;
    }
    auto *storage = new (std::nothrow) CompletionEvent[capacity]();
    if (storage == nullptr) {
        return false;
    }
    storage_.reset(storage);
    capacity_ = capacity;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    wakeArmed_.store(false, std::memory_order_relaxed);
    pushed_.store(0, std::memory_order_relaxed);
    drained_.store(0, std::memory_order_relaxed);
    overflow_.store(0, std::memory_order_relaxed);
    highWatermark_.store(0, std::memory_order_relaxed);
    wakeTransitions_.store(0, std::memory_order_relaxed);
    return true;
}

bool AsyncCompletionRing::valid() const noexcept {
    return storage_ != nullptr && capacity_ != 0;
}

std::size_t AsyncCompletionRing::capacity() const noexcept { return capacity_; }

std::size_t AsyncCompletionRing::size() const noexcept {
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    const std::uint64_t used = head - tail;
    return used > static_cast<std::uint64_t>(capacity_) ? capacity_
                                                        : static_cast<std::size_t>(used);
}

bool AsyncCompletionRing::empty() const noexcept {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
}

AsyncCompletionRing::PushResult
AsyncCompletionRing::pushWithWakeHint(const CompletionEvent &event) noexcept {
    PushResult result;
    if (!valid()) {
        overflow_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= static_cast<std::uint64_t>(capacity_)) {
        overflow_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    storage_[static_cast<std::size_t>(head % capacity_)] = event;
    head_.store(head + 1, std::memory_order_release);
    pushed_.fetch_add(1, std::memory_order_relaxed);

    const std::uint64_t usedCount = (head + 1) - tail;
    const std::size_t used = static_cast<std::size_t>(std::min<std::uint64_t>(
        usedCount, static_cast<std::uint64_t>(capacity_)));
    std::size_t high = highWatermark_.load(std::memory_order_relaxed);
    while (used > high &&
           !highWatermark_.compare_exchange_weak(high, used, std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
    }

    bool expected = false;
    result.notify = wakeArmed_.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire);
    if (result.notify) {
        wakeTransitions_.fetch_add(1, std::memory_order_relaxed);
    }
    result.accepted = true;
    return result;
}

bool AsyncCompletionRing::push(const CompletionEvent &event) noexcept {
    return pushWithWakeHint(event).accepted;
}

bool AsyncCompletionRing::pop(CompletionEvent &event) noexcept {
    if (!valid()) {
        return false;
    }
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {
        return false;
    }
    event = storage_[static_cast<std::size_t>(tail % capacity_)];
    tail_.store(tail + 1, std::memory_order_release);
    drained_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::size_t AsyncCompletionRing::drain(std::span<CompletionEvent> output) noexcept {
    if (!valid() || output.empty()) {
        return 0;
    }
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    const std::uint64_t available = head - tail;
    const std::size_t count = std::min<std::size_t>(
        output.size(), static_cast<std::size_t>(std::min<std::uint64_t>(
            available, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))));
    for (std::size_t index = 0; index < count; ++index) {
        output[index] = storage_[static_cast<std::size_t>((tail + index) % capacity_)];
    }
    if (count != 0) {
        tail_.store(tail + count, std::memory_order_release);
        drained_.fetch_add(count, std::memory_order_relaxed);
    }
    return count;
}

void AsyncCompletionRing::prepareWait() noexcept {
    wakeArmed_.store(false, std::memory_order_release);
}

bool AsyncCompletionRing::rearmIfNonEmpty() noexcept {
    if (empty()) {
        wakeArmed_.store(false, std::memory_order_release);
        return false;
    }
    wakeArmed_.store(true, std::memory_order_release);
    return true;
}

AsyncCompletionRing::Diagnostics AsyncCompletionRing::diagnostics() const noexcept {
    Diagnostics result;
    result.capacity = capacity_;
    result.size = size();
    result.highWatermark = highWatermark_.load(std::memory_order_relaxed);
    result.pushed = pushed_.load(std::memory_order_relaxed);
    result.drained = drained_.load(std::memory_order_relaxed);
    result.overflow = overflow_.load(std::memory_order_relaxed);
    result.wakeTransitions = wakeTransitions_.load(std::memory_order_relaxed);
    return result;
}

AsyncCompletionQueue::AsyncCompletionQueue(std::size_t shardCount,
                                           std::size_t perShardCapacity) noexcept {
    initialize(shardCount, perShardCapacity);
}

bool AsyncCompletionQueue::initialize(std::size_t shardCount,
                                      std::size_t perShardCapacity) noexcept {
    if (shardCount == 0 || perShardCapacity == 0 || rings_ != nullptr) {
        return false;
    }
    auto rings = new (std::nothrow) AsyncCompletionRing[shardCount]();
    if (rings == nullptr) {
        return false;
    }
    rings_.reset(rings);
    for (std::size_t index = 0; index < shardCount; ++index) {
        if (!rings_[index].initialize(perShardCapacity)) {
            rings_.reset();
            return false;
        }
    }
    if (!notifier_.initialize()) {
        rings_.reset();
        return false;
    }
    shardCount_ = shardCount;
    perShardCapacity_ = perShardCapacity;
    cursor_ = 0;
    invalidShard_.store(0, std::memory_order_relaxed);
    return true;
}

bool AsyncCompletionQueue::valid() const noexcept {
    return rings_ != nullptr && shardCount_ != 0 && perShardCapacity_ != 0;
}

std::size_t AsyncCompletionQueue::shardCount() const noexcept { return shardCount_; }

std::size_t AsyncCompletionQueue::perShardCapacity() const noexcept {
    return perShardCapacity_;
}

std::size_t AsyncCompletionQueue::totalCapacity() const noexcept {
    if (perShardCapacity_ != 0 &&
        shardCount_ > std::numeric_limits<std::size_t>::max() / perShardCapacity_) {
        return std::numeric_limits<std::size_t>::max();
    }
    return shardCount_ * perShardCapacity_;
}

bool AsyncCompletionQueue::push(std::size_t shard, const CompletionEvent &event) noexcept {
    if (!valid() || shard >= shardCount_) {
        invalidShard_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const AsyncCompletionRing::PushResult result = rings_[shard].pushWithWakeHint(event);
    if (result.accepted && result.notify) {
        notifier_.notify();
    }
    return result.accepted;
}

std::size_t AsyncCompletionQueue::drainAvailable(std::span<CompletionEvent> output,
                                                 std::size_t max) noexcept {
    if (!valid() || max == 0 || output.empty()) {
        return 0;
    }
    const std::size_t limit = std::min(max, output.size());
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < shardCount_ && count < limit; ++offset) {
        const std::size_t shard = (cursor_ + offset) % shardCount_;
        const std::size_t remaining = limit - count;
        count += rings_[shard].drain(output.subspan(count, remaining));
    }
    cursor_ = (cursor_ + 1) % shardCount_;
    return count;
}

std::size_t AsyncCompletionQueue::drain(std::span<CompletionEvent> output,
                                        std::size_t max) noexcept {
    return drainAvailable(output, max);
}

bool AsyncCompletionQueue::rearmAndRescan(std::span<CompletionEvent> output,
                                          std::size_t &count, std::size_t max) noexcept {
    bool raced = false;
    for (std::size_t index = 0; index < shardCount_; ++index) {
        raced = rings_[index].rearmIfNonEmpty() || raced;
    }
    if (raced && count < max) {
        count += drainAvailable(output.subspan(count, max - count), max - count);
    }
    return raced;
}

PollResult AsyncCompletionQueue::poll(std::span<CompletionEvent> output, std::size_t min,
                                      std::size_t max,
                                      std::chrono::microseconds timeout) noexcept {
    PollResult result;
    if (!valid()) {
        result.status = Status::InvalidState;
        return result;
    }
    if (min > max) {
        result.status = Status::InvalidArgument;
        return result;
    }

    const std::size_t limit = std::min(max, output.size());
    if (min > limit) {
        result.status = Status::InvalidArgument;
        return result;
    }
    if (limit == 0) {
        return result;
    }

    const auto boundedTimeout = timeout.count() > 0 ? timeout : std::chrono::microseconds(0);
    const auto deadline = std::chrono::steady_clock::now() + boundedTimeout;
    for (;;) {
        result.count += drainAvailable(output.subspan(result.count, limit - result.count),
                                       limit - result.count);
        if (result.count >= min || result.count == limit || boundedTimeout.count() == 0) {
            return result;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return result;
        }

        /* Clear wake arms, rescan, and only then sleep to close the race with push. */
        const std::uint64_t observedEpoch = notifier_.epoch();
        for (std::size_t index = 0; index < shardCount_; ++index) {
            rings_[index].prepareWait();
        }
        result.count += drainAvailable(output.subspan(result.count, limit - result.count),
                                       limit - result.count);
        if (result.count >= min || result.count == limit) {
            return result;
        }
        if (rearmAndRescan(output, result.count, limit) && result.count >= min) {
            return result;
        }

        const auto waitNow = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline -
                                                                                       waitNow);
        if (remaining.count() <= 0) {
            return result;
        }
        notifier_.waitSince(observedEpoch, remaining);
    }
}

AsyncCompletionRing::Diagnostics
AsyncCompletionQueue::shardDiagnostics(std::size_t shard) const noexcept {
    if (!valid() || shard >= shardCount_) {
        return AsyncCompletionRing::Diagnostics{};
    }
    return rings_[shard].diagnostics();
}

AsyncCompletionQueue::Diagnostics AsyncCompletionQueue::diagnostics() const noexcept {
    Diagnostics result;
    result.shardCount = shardCount_;
    result.perShardCapacity = perShardCapacity_;
    result.totalCapacity = totalCapacity();
    result.invalidShard = invalidShard_.load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < shardCount_; ++index) {
        const auto shard = rings_[index].diagnostics();
        result.highWatermark = std::max(result.highWatermark, shard.highWatermark);
        result.pushed += shard.pushed;
        result.drained += shard.drained;
        result.overflow += shard.overflow;
    }
    result.notifier = notifier_.diagnostics();
    return result;
}

CompletionNotifier &AsyncCompletionQueue::notifier() noexcept { return notifier_; }

const CompletionNotifier &AsyncCompletionQueue::notifier() const noexcept { return notifier_; }

} // namespace hfs
