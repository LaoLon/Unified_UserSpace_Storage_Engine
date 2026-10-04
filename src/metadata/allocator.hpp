#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include "uuse/types.hpp"

namespace uuse::metadata {

/* Extent lifecycle: FREE -> RESERVED -> INITIALIZED -> COMMITTED (uuse.md §9.4). */
enum class ExtentState : std::uint8_t {
    Free = 0,
    Reserved = 1,
    Initialized = 2,
    Committed = 3,
};

/* One contiguous run of device extents. */
struct ExtentRun {
    std::uint64_t start = 0; /* device extent index */
    std::uint64_t count = 0;
    std::uint32_t deviceId = 0;
};

struct AllocatorConfig {
    std::uint32_t deviceId = 0;
    std::uint64_t totalExtents = 0;    /* data region capacity in extents */
    std::uint64_t capacityExtents = 0; /* 0 = totalExtents (test override) */
    std::uint64_t maxReserveExtents = 64;
};

/* Multi-device placement policy (uuse.md §9.5). */
enum class PlacementPolicy : std::uint8_t {
    CapacityWeighted = 0,
    RoundRobin = 1,
};

/* Picks the next device for an allocation; pure policy, no state. */
class PlacementPicker {
public:
    PlacementPicker(PlacementPolicy policy, std::uint64_t deviceCount) noexcept;
    std::uint64_t pick(std::span<const std::uint64_t> freeExtents) noexcept;

private:
    PlacementPolicy policy_;
    std::uint64_t deviceCount_;
    std::uint64_t cursor_ = 0;
};

/* Bounded per-device extent allocator: two-level bitmap plus a free-run index
 * with next-fit search and neighbour merging. Bitmap state is only made
 * durable through checkpoint snapshots (uuse.md §9.4). */
class Allocator {
public:
    /* Snapshot framing used by checkpoint snapshots and tests. */
    static constexpr std::size_t kSnapshotHeaderBytes = 64;
    static constexpr std::size_t kSnapshotTrailerBytes = 4;

    struct Stats {
        std::uint64_t total = 0;
        std::uint64_t capacity = 0;
        std::uint64_t free = 0;
        std::uint64_t reserved = 0;
        std::uint64_t initialized = 0;
        std::uint64_t committed = 0;
        std::uint64_t freeRuns = 0;
    };

    static Result<std::unique_ptr<Allocator>> create(const AllocatorConfig &config);
    ~Allocator();
    Allocator(const Allocator &) = delete;
    Allocator &operator=(const Allocator &) = delete;

    /* Contiguous reservation; count must fit the per-reserve limit. */
    Result<ExtentRun> reserve(std::uint64_t count);
    /* Replay semantics: marks an exact range reserved (FREE -> RESERVED). */
    Result<void> markReserved(std::uint64_t start, std::uint64_t count);
    /* Scattered reservation: returns runs totalling count, each <= the limit;
     * returns NoSpace when fewer than count extents are free. */
    Result<std::vector<ExtentRun>> reserveScattered(std::uint64_t count);
    Result<void> markInitialized(std::uint64_t start, std::uint64_t count);
    Result<void> commit(std::uint64_t start, std::uint64_t count);
    Result<void> release(std::uint64_t start, std::uint64_t count);
    /* Replay aid: RESERVED runs were never committed, so reclaim them. */
    Result<void> reclaimReserved();

    ExtentState stateAt(std::uint64_t extent) const;
    std::uint64_t freeExtents() const noexcept { return freeExtents_; }
    /* Extents that may still be handed out (capacity minus used). */
    std::uint64_t allocatableExtents() const noexcept {
        return capacityExtents_ - (totalExtents() - freeExtents_);
    }
    std::uint64_t capacityExtents() const noexcept { return capacityExtents_; }
    std::uint64_t totalExtents() const noexcept {
        return static_cast<std::uint64_t>(states_.size());
    }
    std::uint32_t deviceId() const noexcept { return config_.deviceId; }
    Stats stats() const;
    /* Cross-checks bitmap, state array and free-run index. */
    bool verify() const;

    std::vector<std::byte> serialize() const;
    Result<void> deserialize(std::span<const std::byte> data);

private:
    explicit Allocator(const AllocatorConfig &config);

    bool isUsed(std::uint64_t extent) const noexcept;
    void setUsed(std::uint64_t extent, bool used) noexcept;
    void insertRun(std::uint64_t start, std::uint64_t count);
    /* Removes [start, start+count) from the free-run index (splitting runs). */
    void splitRunForRange(std::uint64_t start, std::uint64_t count);
    Result<std::uint64_t> takeRun(std::uint64_t count);

    AllocatorConfig config_;
    std::uint64_t capacityExtents_ = 0;
    std::uint64_t freeExtents_ = 0;
    std::uint64_t searchCursor_ = 0;
    std::vector<std::uint8_t> states_; /* one ExtentState per extent */
    std::vector<std::uint64_t> l2_;    /* one bit per extent: used */
    std::vector<std::uint64_t> l1_;    /* one bit per l2 word: any used bit */
    std::map<std::uint64_t, std::uint64_t> freeRuns_; /* start -> count */
};

} // namespace uuse::metadata
