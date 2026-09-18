#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "hfs/async.hpp"

namespace hfs {

/* Nonblocking producer notification used only as a wake hint. */
class CompletionNotifier {
public:
    struct Diagnostics {
        std::uint64_t notifications = 0;
        std::uint64_t saturated = 0;
        std::uint64_t errors = 0;
        bool eventFd = false;
    };

    CompletionNotifier() noexcept;
    ~CompletionNotifier();

    CompletionNotifier(const CompletionNotifier &) = delete;
    CompletionNotifier &operator=(const CompletionNotifier &) = delete;

    /* Initializes an eventfd when available and otherwise selects the atomic fallback. */
    bool initialize() noexcept;

    /* Never waits and never allocates; EAGAIN still counts as a successful wake hint. */
    bool notify() noexcept;

    /* Waits only on the consumer side and returns true for a wake or false for timeout. */
    bool wait(std::chrono::microseconds timeout) noexcept;

    /* Waits for a producer epoch newer than observed; used to close fallback races. */
    bool waitSince(std::uint64_t observed,
                   std::chrono::microseconds timeout) noexcept;

    bool usingEventFd() const noexcept;
    std::uint64_t epoch() const noexcept;
    Diagnostics diagnostics() const noexcept;

private:
    void closeEventFd() noexcept;

    int eventFd_ = -1;
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<std::uint64_t> notifications_{0};
    std::atomic<std::uint64_t> saturated_{0};
    std::atomic<std::uint64_t> errors_{0};
};

/* One bounded SPSC ring: one shard actor publishes, one frontend consumes. */
class AsyncCompletionRing {
public:
    struct Diagnostics {
        std::size_t capacity = 0;
        std::size_t size = 0;
        std::size_t highWatermark = 0;
        std::uint64_t pushed = 0;
        std::uint64_t drained = 0;
        std::uint64_t overflow = 0;
        std::uint64_t wakeTransitions = 0;
    };

    struct PushResult {
        bool accepted = false;
        bool notify = false;
    };

    AsyncCompletionRing() noexcept = default;
    explicit AsyncCompletionRing(std::size_t capacity) noexcept;
    ~AsyncCompletionRing() = default;

    AsyncCompletionRing(const AsyncCompletionRing &) = delete;
    AsyncCompletionRing &operator=(const AsyncCompletionRing &) = delete;

    /* Initializes once; allocation happens here, never in push. */
    bool initialize(std::size_t capacity) noexcept;

    bool valid() const noexcept;
    std::size_t capacity() const noexcept;
    std::size_t size() const noexcept;
    bool empty() const noexcept;

    /* The simple overload is useful for direct ring tests. */
    bool push(const CompletionEvent &event) noexcept;

    /* Returns whether this push changed the producer wake-arm state. */
    PushResult pushWithWakeHint(const CompletionEvent &event) noexcept;

    bool pop(CompletionEvent &event) noexcept;
    std::size_t drain(std::span<CompletionEvent> output) noexcept;

    /* Consumer-side handshake closes the empty-to-nonempty lost-wake window. */
    void prepareWait() noexcept;
    bool rearmIfNonEmpty() noexcept;

    Diagnostics diagnostics() const noexcept;

private:
    std::unique_ptr<CompletionEvent[]> storage_;
    std::size_t capacity_ = 0;
    std::atomic<std::uint64_t> head_{0}; /* producer-owned publication index */
    std::atomic<std::uint64_t> tail_{0}; /* consumer-owned release index */
    std::atomic<bool> wakeArmed_{false};
    std::atomic<std::uint64_t> pushed_{0};
    std::atomic<std::uint64_t> drained_{0};
    std::atomic<std::uint64_t> overflow_{0};
    std::atomic<std::size_t> highWatermark_{0};
    std::atomic<std::uint64_t> wakeTransitions_{0};
};

/* Bounded set of per-shard SPSC rings with one frontend consumer. */
class AsyncCompletionQueue {
public:
    struct Diagnostics {
        std::size_t shardCount = 0;
        std::size_t perShardCapacity = 0;
        std::size_t totalCapacity = 0;
        std::size_t highWatermark = 0;
        std::uint64_t pushed = 0;
        std::uint64_t drained = 0;
        std::uint64_t overflow = 0;
        std::uint64_t invalidShard = 0;
        CompletionNotifier::Diagnostics notifier{};
    };

    AsyncCompletionQueue() noexcept = default;
    AsyncCompletionQueue(std::size_t shardCount, std::size_t perShardCapacity) noexcept;
    ~AsyncCompletionQueue() = default;

    AsyncCompletionQueue(const AsyncCompletionQueue &) = delete;
    AsyncCompletionQueue &operator=(const AsyncCompletionQueue &) = delete;

    /* Creates all rings with the same worst-case per-shard capacity. */
    bool initialize(std::size_t shardCount, std::size_t perShardCapacity) noexcept;

    bool valid() const noexcept;
    std::size_t shardCount() const noexcept;
    std::size_t perShardCapacity() const noexcept;
    std::size_t totalCapacity() const noexcept;

    /* Producer entry point; only the owning shard may call push for a shard. */
    bool push(std::size_t shard, const CompletionEvent &event) noexcept;

    /* Nonblocking drain across all shards. */
    std::size_t drain(std::span<CompletionEvent> output, std::size_t max) noexcept;

    /* Drains available completions and waits only when min is not met. */
    PollResult poll(std::span<CompletionEvent> output, std::size_t min,
                    std::size_t max, std::chrono::microseconds timeout) noexcept;

    AsyncCompletionRing::Diagnostics shardDiagnostics(std::size_t shard) const noexcept;
    Diagnostics diagnostics() const noexcept;
    CompletionNotifier &notifier() noexcept;
    const CompletionNotifier &notifier() const noexcept;

private:
    std::size_t drainAvailable(std::span<CompletionEvent> output, std::size_t max) noexcept;
    bool rearmAndRescan(std::span<CompletionEvent> output, std::size_t &count,
                        std::size_t max) noexcept;

    std::unique_ptr<AsyncCompletionRing[]> rings_;
    std::size_t shardCount_ = 0;
    std::size_t perShardCapacity_ = 0;
    std::size_t cursor_ = 0; /* consumer-only round-robin start */
    std::uint32_t activePollSpins_ = 128; /* consumer-only adaptive spin budget */
    std::atomic<std::uint64_t> invalidShard_{0};
    CompletionNotifier notifier_;
};

using ShardCompletionRing = AsyncCompletionRing;

} // namespace hfs
