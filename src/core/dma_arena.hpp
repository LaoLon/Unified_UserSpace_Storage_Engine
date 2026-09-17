#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hfs/types.hpp"

namespace hfs {

class DmaArena;

/* RAII buffer: caller-owned memory or an arena slice (hfs2.0.md §8.1). */
class BufferLease {
public:
    BufferLease() = default;
    BufferLease(BufferLease &&other) noexcept;
    BufferLease &operator=(BufferLease &&other) noexcept;
    BufferLease(const BufferLease &) = delete;
    BufferLease &operator=(const BufferLease &) = delete;
    ~BufferLease();

    static Result<BufferLease> fromCaller(std::span<std::byte> bytes);
    static Result<BufferLease> fromArena(DmaArena &arena, std::size_t size,
                                         std::uint32_t timeoutMs);

    std::span<std::byte> span() const noexcept;
    bool fromArena() const noexcept;
    void reset() noexcept;

private:
    DmaArena *arena_ = nullptr;
    std::span<std::byte> bytes_;
};

/* Bounded on-demand arena shared by the data path (hfs2.0.md §8.2). */
class DmaArena {
public:
    using Allocator = void *(*)(std::size_t size, void *ctx);
    using Deallocator = void (*)(void *ptr, void *ctx);

    static constexpr std::size_t kClassCount = 4;
    static constexpr std::array<std::size_t, kClassCount> kClassSizes = {4096, 16384, 65536,
                                                                        1048576};

    struct Stats {
        std::size_t bytes = 0;
        std::size_t capacityBytes = 0;
        std::size_t highWatermark = 0;
        std::uint64_t acquires = 0;
        std::uint64_t releases = 0;
        std::uint64_t busy = 0;
    };

    void configure(std::size_t capacityBytes, Allocator alloc, Deallocator dealloc,
                   void *ctx) noexcept;

    /* Frees all cached blocks through the configured deallocator. */
    ~DmaArena();
    DmaArena() = default;
    DmaArena(const DmaArena &) = delete;
    DmaArena &operator=(const DmaArena &) = delete;

    /* Blocks up to timeoutMs when the cap is reached (Busy on timeout). */
    Result<std::span<std::byte>> acquire(std::size_t size, std::uint32_t timeoutMs);
    void release(std::span<std::byte> buffer) noexcept;

    /* True when the pointer is a live slice handed out by this arena; only
     * such slices are safe for direct (non-bounce) device DMA. */
    bool owns(const void *buffer) const;

    Stats stats() const;
    static std::size_t classSizeFor(std::size_t size) noexcept; /* 0 = no class */

private:
    struct SizeClass {
        std::vector<void *> free;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t capacityBytes_ = 0;
    std::size_t bytes_ = 0;
    std::size_t highWatermark_ = 0;
    std::array<SizeClass, kClassCount> classes_;
    std::unordered_map<void *, std::size_t> largeLive_;
    std::unordered_set<const void *> live_;
    Allocator allocator_ = nullptr;
    Deallocator deallocator_ = nullptr;
    void *allocatorCtx_ = nullptr;
    std::uint64_t acquires_ = 0;
    std::uint64_t releases_ = 0;
    std::uint64_t busy_ = 0;
};

} // namespace hfs
