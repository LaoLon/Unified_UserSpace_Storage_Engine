#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/extent_map.hpp"
#include "hfs/types.hpp"
#include "metadata/allocator.hpp"

namespace hfs::metadata {

/* One logical extent range mapped to a device run (COMMITTED extents only). */
struct ExtentDescriptor {
    std::uint64_t logicalExtent = 0;  /* first logical extent index */
    std::uint64_t physicalExtent = 0; /* first device extent index */
    std::uint64_t count = 0;
    std::uint32_t deviceId = 0;
};

/* Per-inode logical-to-physical extent map (hfs2.0.md §9.4/§13.3): a versioned
 * vector mutated by the metadata actor and read through the ExtentMap view. */
class InodeExtentMap final : public ExtentMap {
public:
    void configure(std::uint64_t extentBytes) noexcept;

    Result<void> addExtents(std::span<const ExtentDescriptor> descriptors);
    /* Removes mappings at or beyond the logical extent (truncate). */
    Result<void> removeFrom(std::uint64_t logicalExtent);
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

} // namespace hfs::metadata
