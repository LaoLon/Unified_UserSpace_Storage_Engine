#pragma once

#include <array>
#include <cstdint>

#include "core/extent_map.hpp"
#include "hfs/types.hpp"

namespace hfs {

/* Splits a logical I/O into device-local children (hfs2.0.md §7.3):
 * aligned direct children (<= maxChildBytes) plus head/tail RMW children. */
class IoPlanner {
public:
    struct Config {
        std::uint32_t blockSize = 4096;
        std::uint32_t maxChildBytes = 1U << 20;
        std::uint32_t maxChildren = 64;
    };

    struct Child {
        enum class Kind : std::uint8_t { Direct, HeadRmw, TailRmw };
        Kind kind = Kind::Direct;
        std::uint64_t logicalOffset = 0; /* user offset of the child */
        std::uint64_t physicalBlock = 0; /* device block of the aligned span */
        std::uint32_t deviceId = 0;
        std::uint32_t bytes = 0;        /* user bytes covered by the child */
        std::uint32_t spanBytes = 0;    /* aligned device bytes to move */
        std::uint32_t bufferOffset = 0; /* offset inside the user buffer */
        std::uint32_t spanOffset = 0;   /* user bytes inside the aligned span */
    };

    struct Plan {
        static constexpr std::size_t kMaxChildren = 64;
        std::array<Child, kMaxChildren> children{};
        std::uint32_t count = 0;
        std::uint64_t coveredBytes = 0;
    };

    explicit IoPlanner(Config config);

    Result<Plan> plan(const ExtentMap &map, std::uint64_t offset, std::uint64_t length,
                      bool write) const;

private:
    Config config_;
};

} // namespace hfs
