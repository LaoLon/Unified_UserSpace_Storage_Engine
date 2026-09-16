#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace hfs {

/* Bounded size-class allocator for coroutine frames. Pool memory is retained
 * on free; allocations beyond the cap fall back to the system allocator and
 * are counted as overflow so tests can assert the pool is actually used. */
class FramePool {
public:
    struct Stats {
        std::uint64_t allocations = 0;
        std::uint64_t deallocations = 0;
        std::uint64_t overflowAllocations = 0;
        std::size_t poolBytes = 0;   /* pool footprint */
        std::size_t capacityBytes = 0;
        std::array<std::size_t, 7> freeBlocks{};
    };

    static FramePool &instance() noexcept;

    /* Set the byte cap; call before use (startup only). */
    void setCapacity(std::size_t bytes) noexcept;

    void *allocate(std::size_t size) noexcept;
    void deallocate(void *ptr, std::size_t size) noexcept;

    /* True when the pool can serve one more frame of `size` without overflow. */
    bool hasBudget(std::size_t size) const noexcept;

    Stats stats() const noexcept;

private:
    FramePool() = default;

    static constexpr std::array<std::size_t, 7> kClassSizes = {128, 256, 512, 1024,
                                                               2048, 4096, 8192};
    struct FreeNode {
        FreeNode *next = nullptr;
    };
    struct SizeClass {
        mutable std::mutex mutex;
        FreeNode *free = nullptr;
        std::size_t count = 0;
    };

    std::size_t classIndexFor(std::size_t size) const noexcept; /* npos when too big */

    mutable std::array<SizeClass, kClassSizes.size()> classes_;
    mutable std::mutex mutex_;
    std::size_t capacityBytes_ = 8U << 20;
    std::size_t poolBytes_ = 0;
    std::atomic<std::uint64_t> allocations_{0};
    std::atomic<std::uint64_t> deallocations_{0};
    std::atomic<std::uint64_t> overflowAllocations_{0};
};

} // namespace hfs
