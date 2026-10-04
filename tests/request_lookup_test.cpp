#include <array>
#include <cstdint>
#include <cstdio>

#include "core/request_lookup.hpp"
#include "test_support.hpp"

int main() {
    uuse::RequestLookupTable table;
    UUSE_TEST_CHECK(table.initialize(32));
    UUSE_TEST_CHECK(table.valid());
    UUSE_TEST_CHECK(!table.initialize(32));
    UUSE_TEST_CHECK(!table.insert(0, 0));

    std::array<std::uint64_t, 32> active{};
    for (std::size_t index = 0; index < active.size(); ++index) {
        active[index] = 0x100000000ULL + index * 17 + 1;
        UUSE_TEST_CHECK(table.insert(active[index], static_cast<std::uint32_t>(index)));
    }
    UUSE_TEST_CHECK(!table.insert(active[0], 99));

    for (std::size_t index = 0; index < active.size(); ++index) {
        std::uint32_t slot = 0;
        UUSE_TEST_CHECK(table.find(active[index], slot));
        UUSE_TEST_CHECK(slot == index);
    }

    /* Repeated replacement keeps probe chains valid without tombstones. */
    for (std::uint64_t cycle = 0; cycle < 10000; ++cycle) {
        const std::size_t index = static_cast<std::size_t>(cycle % active.size());
        UUSE_TEST_CHECK(table.erase(active[index]));
        std::uint32_t missingSlot = 0;
        UUSE_TEST_CHECK(!table.find(active[index], missingSlot));
        active[index] = 0x200000000ULL + cycle * 256 + index + 1;
        UUSE_TEST_CHECK(table.insert(active[index], static_cast<std::uint32_t>(index)));
        std::uint32_t slot = 0;
        UUSE_TEST_CHECK(table.find(active[index], slot));
        UUSE_TEST_CHECK(slot == index);
    }

    UUSE_TEST_CHECK(!table.erase(0xdeadbeefULL));
    table.reset();
    UUSE_TEST_CHECK(!table.valid());
    std::printf("request_lookup_test: ok\n");
    return 0;
}
