#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace hfs {

class Operation;

/* Bounded MPSC ring: producers reserve slots, one actor consumes in batches.
 * Depth is rounded up to a power of two. */
class ShardRing {
public:
    struct Stats {
        std::uint64_t reservations = 0;
        std::uint64_t published = 0;
        std::uint64_t consumed = 0;
        std::uint64_t busy = 0;
        std::uint64_t doorbells = 0;
        std::size_t depth = 0;
        std::size_t highWatermark = 0;
    };

    struct Reservation {
        Operation **slot = nullptr;
        std::uint64_t sequence = 0;
        bool valid() const noexcept { return slot != nullptr; }
    };

    explicit ShardRing(std::size_t depth);

    /* Reserve a producer slot; invalid when the ring is full (Busy). */
    Reservation reserve() noexcept;
    /* Publishes one operation; returns true when this publish armed the
     * doorbell (ring transitioned empty->non-empty). */
    bool publish(Reservation reservation, Operation *operation) noexcept;

    /* Actor side: move up to `max` published operations out of the ring. */
    std::size_t drain(Operation **out, std::size_t max) noexcept;

    bool empty() const noexcept;
    bool doorbellArmed() const noexcept;
    void clearDoorbell() noexcept; /* actor: clear, then re-check empty */
    void armDoorbell() noexcept;   /* actor: re-arm after clear + non-empty */

    std::size_t depth() const noexcept;
    Stats stats() const noexcept;

private:
    struct Slot {
        std::atomic<std::uint64_t> sequence{0};
        Operation *value = nullptr;
    };

    static std::size_t roundUpPowerOfTwo(std::size_t value);

    std::unique_ptr<Slot[]> slots_;
    std::size_t depth_ = 0;
    std::size_t mask_ = 0;
    std::atomic<std::uint64_t> head_{0}; /* producers reserve here */
    std::uint64_t tail_ = 0;             /* consumer only */
    std::atomic<bool> armed_{false};
    std::atomic<std::uint64_t> reservations_{0};
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> consumed_{0};
    std::atomic<std::uint64_t> busy_{0};
    std::atomic<std::uint64_t> doorbells_{0};
    std::atomic<std::size_t> inFlight_{0};
    std::atomic<std::size_t> highWatermark_{0};
};

} // namespace hfs
