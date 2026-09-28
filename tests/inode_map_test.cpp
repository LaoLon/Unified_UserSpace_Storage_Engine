/* InodeExtentMap lookup correctness plus a targeted lookup microbenchmark.
 * Correctness checks are the hard gate; the benchmark only prints ns/lookup so
 * the linear-scan -> binary-search change can be compared across builds. */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "metadata/inode_map.hpp"
#include "test_support.hpp"

namespace {

using hfs::ExtentView;
using hfs::metadata::ExtentDescriptor;
using hfs::metadata::InodeExtentMap;

constexpr std::uint64_t kExtentBytes = 1ULL << 20; /* test geometry: 1 MiB */

/* Maps `count` logical MiB extents with a fixed gap between them. */
bool buildMap(InodeExtentMap &map, std::size_t count, std::uint64_t strideExtents) {
    map.configure(kExtentBytes);
    std::vector<ExtentDescriptor> descriptors;
    descriptors.reserve(count);
    std::uint64_t logical = 0;
    for (std::size_t index = 0; index < count; ++index) {
        descriptors.push_back(ExtentDescriptor{logical, logical + 1000, 1, 0});
        logical += strideExtents;
    }
    return map.restore(descriptors).ok();
}

} // namespace

int main() {
    std::array<ExtentView, 8> out{};

    /* Empty map: no mappings, not an error. */
    {
        InodeExtentMap map;
        map.configure(kExtentBytes);
        auto result = map.lookup(0, kExtentBytes, out);
        HFS_TEST_CHECK(result.ok());
        HFS_TEST_CHECK(result.value() == 0);
    }

    /* Dense map: first, boundary-crossing, tail, past-end and capacity cases. */
    {
        InodeExtentMap map;
        HFS_TEST_CHECK(buildMap(map, 4, 1));

        auto first = map.lookup(0, 4096, out);
        HFS_TEST_CHECK(first.ok() && first.value() == 1);
        HFS_TEST_CHECK(out[0].logicalOffset == 0 && out[0].length == 4096);
        HFS_TEST_CHECK(out[0].physicalByte == 1000ULL * kExtentBytes);
        HFS_TEST_CHECK(out[0].deviceId == 0);

        /* Crossing one extent boundary yields two clipped views. */
        auto crossing = map.lookup(kExtentBytes - 2048, 4096, out);
        HFS_TEST_CHECK(crossing.ok() && crossing.value() == 2);
        HFS_TEST_CHECK(out[0].logicalOffset == kExtentBytes - 2048);
        HFS_TEST_CHECK(out[0].length == 2048);
        HFS_TEST_CHECK(out[1].logicalOffset == kExtentBytes && out[1].length == 2048);

        /* Starting inside the last mapped extent keeps its physical offset. */
        auto tail = map.lookup(3 * kExtentBytes + 100, 512, out);
        HFS_TEST_CHECK(tail.ok() && tail.value() == 1);
        HFS_TEST_CHECK(out[0].logicalOffset == 3 * kExtentBytes + 100);
        HFS_TEST_CHECK(out[0].physicalByte == 1003ULL * kExtentBytes + 100);

        /* Past the last mapping. */
        auto past = map.lookup(4 * kExtentBytes, 4096, out);
        HFS_TEST_CHECK(past.ok() && past.value() == 0);

        /* Output span too small for a multi-extent result. */
        auto small = map.lookup(0, 3 * kExtentBytes, std::span<ExtentView>(out.data(), 2));
        HFS_TEST_CHECK(!small.ok() && small.status() == hfs::Status::NoMemory);
    }

    /* Sparse map (logical MiB 0, 2, 4, 6): ranges in a hole return nothing,
     * adjacent mappings are clipped, and multi-extent ranges keep order. */
    {
        InodeExtentMap map;
        HFS_TEST_CHECK(buildMap(map, 4, 2));

        auto toNextMapping = map.lookup(kExtentBytes + 512, kExtentBytes, out);
        HFS_TEST_CHECK(toNextMapping.ok() && toNextMapping.value() == 1);
        HFS_TEST_CHECK(out[0].logicalOffset == 2 * kExtentBytes);
        HFS_TEST_CHECK(out[0].length == 512);

        auto insideHole = map.lookup(kExtentBytes + 512, 1024, out);
        HFS_TEST_CHECK(insideHole.ok() && insideHole.value() == 0);

        auto throughHole = map.lookup(kExtentBytes / 2, 2 * kExtentBytes, out);
        HFS_TEST_CHECK(throughHole.ok() && throughHole.value() == 2);
        HFS_TEST_CHECK(out[0].logicalOffset == kExtentBytes / 2);
        HFS_TEST_CHECK(out[0].length == kExtentBytes / 2);
        HFS_TEST_CHECK(out[1].logicalOffset == 2 * kExtentBytes);
        HFS_TEST_CHECK(out[1].length == kExtentBytes / 2);
    }

    /* Large map: correctness at both ends must not depend on scan order. */
    {
        InodeExtentMap map;
        constexpr std::size_t kCount = 4096;
        HFS_TEST_CHECK(buildMap(map, kCount, 1));
        auto last = map.lookup((kCount - 1) * kExtentBytes + 42, 100, out);
        HFS_TEST_CHECK(last.ok() && last.value() == 1);
        HFS_TEST_CHECK(out[0].logicalOffset == (kCount - 1) * kExtentBytes + 42);
        auto first = map.lookup(0, 1, out);
        HFS_TEST_CHECK(first.ok() && first.value() == 1);
        HFS_TEST_CHECK(out[0].logicalOffset == 0 && out[0].length == 1);
    }

    /* Targeted microbenchmark: one million lookups spread over a 4096-extent
     * map. The previous linear scan visited up to ~2048 descriptors per
     * lookup; the binary search must stay in the tens of nanoseconds. */
    {
        InodeExtentMap map;
        constexpr std::size_t kCount = 4096;
        constexpr std::size_t kLookups = 1'000'000;
        HFS_TEST_CHECK(buildMap(map, kCount, 1));
        std::uint64_t checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < kLookups; ++index) {
            const std::uint64_t offset =
                ((index * 2654435761ULL) % kCount) * kExtentBytes + (index % 4096);
            auto result = map.lookup(offset, 4096, out);
            checksum += result.ok() ? result.value() : 0;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        HFS_TEST_CHECK(checksum != 0);
        std::printf("inode_map_test: lookups=%zu extents=%zu total_ns=%lld "
                    "ns_per_lookup=%.1f\n",
                    kLookups, kCount, static_cast<long long>(elapsed),
                    static_cast<double>(elapsed) / static_cast<double>(kLookups));
    }

    std::printf("inode_map_test: ok\n");
    return 0;
}
