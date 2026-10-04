#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "uuse/types.hpp"

namespace uuse {

/* One logical-to-physical extent mapping (physical in device bytes). */
struct ExtentView {
    std::uint64_t logicalOffset = 0; /* bytes */
    std::uint64_t length = 0;        /* bytes */
    std::uint64_t physicalByte = 0;  /* device byte offset */
    std::uint32_t deviceId = 0;
};

/* Maps logical byte ranges to extents (synthetic for Shard-75; the real map
 * comes with the namespace shard). */
class ExtentMap {
public:
    virtual ~ExtentMap() = default;
    /* Fill `out` with extents overlapping [offset, offset+length), clipped.
     * Returns the number of entries written. */
    virtual Result<std::size_t> lookup(std::uint64_t offset, std::uint64_t length,
                                       std::span<ExtentView> out) const = 0;
};

/* Ordered synthetic extent list used by tests and the early data path. */
class SyntheticExtentMap final : public ExtentMap {
public:
    void add(std::uint64_t logicalOffset, std::uint64_t length, std::uint64_t physicalByte,
             std::uint32_t deviceId = 0);
    Result<std::size_t> lookup(std::uint64_t offset, std::uint64_t length,
                               std::span<ExtentView> out) const override;
    std::size_t size() const noexcept;
    void clear() noexcept;

private:
    std::vector<ExtentView> extents_;
};

} // namespace uuse
