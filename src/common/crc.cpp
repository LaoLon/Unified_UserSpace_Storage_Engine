#include "hfs/crc.hpp"

#include <array>
#include <cstring>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define HFS_CRC32C_X86 1
#endif

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

std::uint32_t crc32cTable(const std::uint8_t *bytes, std::size_t length,
                          std::uint32_t seed) noexcept {
    std::uint32_t crc = ~seed;
    for (std::size_t i = 0; i < length; ++i) {
        crc = kCrc32cTable[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

#if HFS_CRC32C_X86
/* SSE4.2 CRC32 instruction; same polynomial and seed convention as the table
 * path, so on-disk and journal values are unchanged. */
__attribute__((target("sse4.2"))) std::uint32_t crc32cSse42(const std::uint8_t *bytes,
                                                             std::size_t length,
                                                             std::uint32_t seed) noexcept {
    std::uint32_t crc = ~seed;
    std::size_t i = 0;
    for (; i + 8 <= length; i += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, bytes + i, sizeof(word));
        crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, word));
    }
    for (; i < length; ++i) {
        crc = _mm_crc32_u8(crc, bytes[i]);
    }
    return ~crc;
}
#endif

} // namespace

std::uint32_t crc32c(const void *data, std::size_t length, std::uint32_t seed) noexcept {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
#if HFS_CRC32C_X86
    static const bool useSse42 = __builtin_cpu_supports("sse4.2");
    if (useSse42 && length != 0) {
        return crc32cSse42(bytes, length, seed);
    }
#endif
    return crc32cTable(bytes, length, seed);
}

} // namespace hfs
