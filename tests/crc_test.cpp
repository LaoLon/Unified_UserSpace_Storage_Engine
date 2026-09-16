#include <cstdio>
#include <cstring>
#include <string>

#include "hfs/crc.hpp"
#include "test_support.hpp"

int main() {
    /* Standard CRC32C check vector. */
    const char *check = "123456789";
    HFS_TEST_CHECK(hfs::crc32c(check, std::strlen(check)) == 0xE3069283U);
    HFS_TEST_CHECK(hfs::crc32c("", 0) == 0U);

    /* Incremental usage matches a single pass. */
    const char *whole = "superblock-record-payload";
    const std::size_t split = 10;
    const std::uint32_t full = hfs::crc32c(whole, std::strlen(whole));
    const std::uint32_t first = hfs::crc32c(whole, split);
    const std::uint32_t second = hfs::crc32c(whole + split, std::strlen(whole) - split, first);
    HFS_TEST_CHECK(second == full);

    std::printf("crc_test: ok\n");
    return 0;
}
