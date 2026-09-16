#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/trace.hpp"

namespace hfs {

/* Bounded SPSC completion queue: the shard actor pushes, the frontend reaps. */
class CompletionQueue {
public:
    struct Event {
        std::uint64_t requestId = 0;
        std::uint64_t fileId = 0;
        int status = 0;
        std::uint64_t completedBytes = 0;
        void *userCtx = nullptr;
    };

    explicit CompletionQueue(std::size_t depth)
        : mask_(roundUpPowerOfTwo(depth < 2 ? 2 : depth) - 1), buffer_(mask_ + 1) {}

    bool push(const Event &event) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= buffer_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buffer_[head & mask_] = event;
        head_.store(head + 1, std::memory_order_release);
        pushed_.fetch_add(1, std::memory_order_relaxed);
        trace::event(trace::EventId::CqPush, event.requestId);
        return true;
    }

    std::size_t reap(Event *out, std::size_t max) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        std::size_t count = 0;
        while (count < max && tail + count < head) {
            out[count] = buffer_[(tail + count) & mask_];
            ++count;
        }
        if (count > 0) {
            tail_.store(tail + count, std::memory_order_release);
            reaped_.fetch_add(count, std::memory_order_relaxed);
            trace::event(trace::EventId::CqReap, count);
        }
        return count;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    std::size_t depth() const noexcept { return buffer_.size(); }
    std::uint64_t pushed() const noexcept {
        return pushed_.load(std::memory_order_relaxed);
    }
    std::uint64_t reaped() const noexcept {
        return reaped_.load(std::memory_order_relaxed);
    }
    std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    static std::size_t roundUpPowerOfTwo(std::size_t value) {
        std::size_t result = 1;
        while (result < value) {
            result <<= 1;
        }
        return result;
    }

    std::size_t mask_ = 0;
    std::vector<Event> buffer_;
    std::atomic<std::uint64_t> head_{0}; /* actor */
    std::atomic<std::uint64_t> tail_{0}; /* frontend */
    std::atomic<std::uint64_t> pushed_{0};
    std::atomic<std::uint64_t> reaped_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

} // namespace hfs
