#include "core/shard_ring.hpp"

#include <thread>

#include "common/stats.hpp"
#include "common/trace.hpp"
#include "core/operation.hpp"

namespace hfs {

std::size_t ShardRing::roundUpPowerOfTwo(std::size_t value) {
    std::size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

ShardRing::ShardRing(std::size_t depth) {
    const std::size_t capacity = roundUpPowerOfTwo(depth < 2 ? 2 : depth);
    depth_ = capacity;
    mask_ = capacity - 1;
    slots_ = std::make_unique<Slot[]>(capacity);
    for (std::size_t i = 0; i < capacity; ++i) {
        slots_[i].sequence.store(i, std::memory_order_relaxed);
    }
}

ShardRing::Reservation ShardRing::reserve() noexcept {
    for (;;) {
        std::uint64_t head = head_.load(std::memory_order_relaxed);
        Slot &slot = slots_[head & mask_];
        const std::uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
        if (sequence == head) {
            if (head_.compare_exchange_weak(head, head + 1, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
                reservations_.fetch_add(1, std::memory_order_relaxed);
                trace::event(trace::EventId::RingReserve, head);
                return Reservation{&slot.value, head};
            }
            continue;
        }
        if (sequence < head) {
            /* full: this slot still holds data from the previous lap */
            busy_.fetch_add(1, std::memory_order_relaxed);
            if (stats::enabled()) {
                stats::counters().busyReject.fetch_add(1, std::memory_order_relaxed);
            }
            trace::event(trace::EventId::RingBusy, head);
            return Reservation{};
        }
        std::this_thread::yield(); /* another producer is publishing */
    }
}

bool ShardRing::publish(Reservation reservation, Operation *operation) noexcept {
    if (!reservation.valid()) {
        return false;
    }
    *reservation.slot = operation;
    Slot &slot = slots_[reservation.sequence & mask_];
    slot.sequence.store(reservation.sequence + 1, std::memory_order_release);
    published_.fetch_add(1, std::memory_order_relaxed);

    const std::uint64_t used =
        inFlight_.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::size_t usedSize = static_cast<std::size_t>(used);
    std::size_t high = highWatermark_.load(std::memory_order_relaxed);
    while (usedSize > high &&
           !highWatermark_.compare_exchange_weak(high, usedSize, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
    }

    if (!armed_.exchange(true, std::memory_order_acq_rel)) {
        doorbells_.fetch_add(1, std::memory_order_relaxed);
        trace::event(trace::EventId::RingDoorbell, reservation.sequence);
        return true;
    }
    return false;
}

std::size_t ShardRing::drain(Operation **out, std::size_t max) noexcept {
    std::size_t count = 0;
    while (count < max) {
        Slot &slot = slots_[tail_ & mask_];
        if (slot.sequence.load(std::memory_order_acquire) != tail_ + 1) {
            break;
        }
        out[count] = slot.value;
        slot.value = nullptr;
        slot.sequence.store(tail_ + mask_ + 1, std::memory_order_release);
        ++tail_;
        ++count;
    }
    if (count > 0) {
        consumed_.fetch_add(count, std::memory_order_relaxed);
        inFlight_.fetch_sub(count, std::memory_order_relaxed);
    }
    return count;
}

bool ShardRing::empty() const noexcept {
    return tail_ == head_.load(std::memory_order_acquire);
}

bool ShardRing::doorbellArmed() const noexcept {
    return armed_.load(std::memory_order_acquire);
}

void ShardRing::clearDoorbell() noexcept {
    armed_.store(false, std::memory_order_release);
}

void ShardRing::armDoorbell() noexcept {
    armed_.store(true, std::memory_order_release);
}

std::size_t ShardRing::depth() const noexcept { return depth_; }

ShardRing::Stats ShardRing::stats() const noexcept {
    Stats stats;
    stats.reservations = reservations_.load(std::memory_order_relaxed);
    stats.published = published_.load(std::memory_order_relaxed);
    stats.consumed = consumed_.load(std::memory_order_relaxed);
    stats.busy = busy_.load(std::memory_order_relaxed);
    stats.doorbells = doorbells_.load(std::memory_order_relaxed);
    stats.depth = depth_;
    stats.highWatermark = highWatermark_.load(std::memory_order_relaxed);
    return stats;
}

} // namespace hfs
