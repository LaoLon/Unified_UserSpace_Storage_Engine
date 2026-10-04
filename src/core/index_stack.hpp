/* Bounded lock-free index stack for fixed-capacity pools. */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace uuse {

/* LIFO stack of pool indices shared by multiple producers and consumers.
 * Indices are never returned to the OS, so links can be stored in a parallel
 * array; the head packs a monotonic tag with the index so ABA is rejected.
 * Callers must own an index exclusively between pop() and push(). */
class BoundedIndexStack {
public:
    BoundedIndexStack() noexcept = default;
    BoundedIndexStack(const BoundedIndexStack &) = delete;
    BoundedIndexStack &operator=(const BoundedIndexStack &) = delete;

    /* Allocates the link array and primes the stack with [0, capacity). */
    bool initialize(std::size_t capacity) noexcept {
        if (capacity == 0 || capacity > kEmptyIndex) {
            return false;
        }
        auto links = std::unique_ptr<std::uint32_t[]>(new (std::nothrow)
                                                          std::uint32_t[capacity]);
        if (links == nullptr) {
            return false;
        }
        std::uint32_t head = kEmptyIndex;
        for (std::size_t index = capacity; index > 0; --index) {
            const auto slot = static_cast<std::uint32_t>(index - 1);
            links[slot] = head;
            head = slot;
        }
        links_ = std::move(links);
        capacity_ = static_cast<std::uint32_t>(capacity);
        head_.store(static_cast<std::uint64_t>(head), std::memory_order_release);
        return true;
    }

    bool initialized() const noexcept { return links_ != nullptr; }
    std::size_t capacity() const noexcept { return capacity_; }

    /* Returns false when the stack is empty. */
    bool pop(std::uint32_t &index) noexcept {
        std::uint64_t head = head_.load(std::memory_order_acquire);
        for (;;) {
            const auto top = static_cast<std::uint32_t>(head & 0xFFFFFFFFULL);
            if (top == kEmptyIndex) {
                return false;
            }
            const std::uint32_t next = links_[top];
            const std::uint64_t desired =
                ((head >> 32) + 1) << 32 | static_cast<std::uint64_t>(next);
            if (head_.compare_exchange_weak(head, desired, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                index = top;
                return true;
            }
        }
    }

    /* Returns false for an out-of-range index. */
    bool push(std::uint32_t index) noexcept {
        if (index >= capacity_) {
            return false;
        }
        std::uint64_t head = head_.load(std::memory_order_relaxed);
        for (;;) {
            const auto top = static_cast<std::uint32_t>(head & 0xFFFFFFFFULL);
            links_[index] = top;
            const std::uint64_t desired =
                ((head >> 32) + 1) << 32 | static_cast<std::uint64_t>(index);
            if (head_.compare_exchange_weak(head, desired, std::memory_order_release,
                                            std::memory_order_relaxed)) {
                return true;
            }
        }
    }

private:
    static constexpr std::uint32_t kEmptyIndex = 0xFFFFFFFFU;

    /* head = (tag << 32) | index; index == kEmptyIndex means empty. */
    std::atomic<std::uint64_t> head_{kEmptyIndex};
    std::unique_ptr<std::uint32_t[]> links_;
    std::uint32_t capacity_ = 0;
};

} // namespace uuse
