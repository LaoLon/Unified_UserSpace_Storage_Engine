#pragma once

#include <cstddef>
#include <cstdint>

#include "hfs/export.hpp"

namespace hfs {

/* CRC32C (Castagnoli) over a byte range; seed 0 matches the on-disk format. */
HFS_API std::uint32_t crc32c(const void *data, std::size_t length,
                             std::uint32_t seed = 0) noexcept;

} // namespace hfs
