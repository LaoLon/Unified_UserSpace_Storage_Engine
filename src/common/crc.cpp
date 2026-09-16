#include "hfs/crc.hpp"

#include <array>

namespace hfs {

namespace {

constexpr std::array<std::uint32_t, 256> makeCrc32cTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0x82F63B78U & (0U - (crc & 1U)));
        }
        table[i] = crc;
    }
    return table;
}

constexpr auto kCrc32cTable = makeCrc32cTable();

} // namespace

std::uint32_t crc32c(const void *data, std::size_t length, std::uint32_t seed) noexcept {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::uint32_t crc = ~seed;
    for (std::size_t i = 0; i < length; ++i) {
        crc = kCrc32cTable[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

} // namespace hfs
