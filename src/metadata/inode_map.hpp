#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/extent_map.hpp"
#include "uuse/types.hpp"
#include "metadata/allocator.hpp"

namespace uuse::metadata {

/* One logical extent range mapped to a device run (COMMITTED extents only). */
struct ExtentDescriptor {
    std::uint64_t logicalExtent = 0;  /* first logical extent index */
    std::uint64_t physicalExtent = 0; /* first device extent index */
    std::uint64_t count = 0;
    std::uint32_t deviceId = 0;
};

/* Per-inode logical-to-physical extent map (uuse.md §9.4/§13.3): a versioned
 * vector mutated by the metadata actor and read through the ExtentMap view. */
class InodeExtentMap final : public ExtentMap {
public:
    void configure(std::uint64_t extentBytes) noexcept;

    Result<void> addExtents(std::span<const ExtentDescriptor> descriptors);
    /* Removes mappings at or beyond the logical extent (truncate). */
    Result<void> removeFrom(std::uint64_t logicalExtent);
    /* Index of the first descriptor that can overlap `logicalExtent`. */
    std::size_t firstOverlapping(std::uint64_t logicalExtent) const noexcept;
    /* Descriptor by index, or nullptr; valid while the map is unchanged. */
    const ExtentDescriptor *descriptorAt(std::size_t index) const noexcept;
    /* True when inserting `descriptors` cannot overlap the current map. */
    Result<void> validateInsert(std::span<const ExtentDescriptor> descriptors) const;
    std::uint64_t mappedExtents() const noexcept;
    /* Highest mapped logical extent end (0 when empty). */
    std::uint64_t logicalExtents() const noexcept;
    bool empty() const noexcept;
    std::size_t size() const noexcept;
    std::vector<ExtentDescriptor> snapshot() const;
    Result<void> restore(std::span<const ExtentDescriptor> descriptors);

    Result<std::size_t> lookup(std::uint64_t offset, std::uint64_t length,
                               std::span<ExtentView> out) const override;

private:
    std::uint64_t extentBytes_ = 1ULL << 20;
    std::vector<ExtentDescriptor> extents_; /* sorted by logicalExtent */
};

} // namespace uuse::metadata
