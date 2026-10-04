#include <cstdio>

#include "uuse/superblock.hpp"
#include "test_support.hpp"

namespace {

constexpr std::uint32_t kSector = 512;
constexpr std::uint32_t kMeta = 4096;
constexpr std::uint64_t kGiB = 1ULL << 30;

} // namespace

int main() {
    /* A 30 GiB device with the baseline configuration layouts cleanly. */
    {
        auto geometry =
            uuse::computeGeometry(30 * kGiB, kSector, kMeta, 256, 512, 1);
        UUSE_TEST_CHECK(geometry.ok());
        const auto &layout = geometry.value();
        UUSE_TEST_CHECK(layout.deviceBlocks == (30 * kGiB) / kMeta);
        UUSE_TEST_CHECK(layout.journalStart + layout.journalBlocks ==
                       layout.checkpointAStart);
        UUSE_TEST_CHECK(layout.checkpointAStart + layout.checkpointBlocks ==
                       layout.dataStart);
        UUSE_TEST_CHECK(layout.checkpointBStart > layout.checkpointAStart);
        UUSE_TEST_CHECK(layout.checkpointBStart < layout.dataStart);
        UUSE_TEST_CHECK(layout.dataBlocks > 0);
    }

    /* Two different device sizes both fit and keep the region ordering. */
    {
        auto small = uuse::computeGeometry(8 * kGiB, kSector, kMeta, 64, 64, 1);
        auto large = uuse::computeGeometry(40 * kGiB, kSector, kMeta, 256, 512, 1);
        UUSE_TEST_CHECK(small.ok());
        UUSE_TEST_CHECK(large.ok());
        UUSE_TEST_CHECK(large.value().deviceBlocks > small.value().deviceBlocks);
        UUSE_TEST_CHECK(large.value().dataBlocks > small.value().dataBlocks);
    }

    /* Too small for the configured regions. */
    {
        auto tiny = uuse::computeGeometry(64ULL << 20, kSector, kMeta, 256, 512, 1);
        UUSE_TEST_CHECK(!tiny.ok());
        UUSE_TEST_CHECK(tiny.status() == uuse::Status::InvalidArgument);
    }

    /* Invalid metadata block size. */
    {
        auto bad = uuse::computeGeometry(30 * kGiB, kSector, 3000, 256, 512, 1);
        UUSE_TEST_CHECK(!bad.ok());
    }

    /* Superblock record CRC round trip. */
    {
        uuse::UuseSuperblock superblock{};
        superblock.magic = uuse::kSuperblockMagic;
        superblock.formatVersion = uuse::kSuperblockFormatVersion;
        superblock.deviceId = 0;
        superblock.replicaId = 0;
        superblock.replicaCount = 1;
        superblock.sectorSize = kSector;
        superblock.metadataBlockSize = kMeta;
        superblock.deviceBlocks = 1U << 20;
        superblock.journalStart = 2048;
        superblock.journalBlocks = 16;
        superblock.checkpointAStart = 2064;
        superblock.checkpointBStart = 2128;
        superblock.checkpointBlocks = 128;
        superblock.dataStart = 2192;
        superblock.dataBlocks = superblock.deviceBlocks - superblock.dataStart;
        uuse::superblockFinalize(superblock);
        UUSE_TEST_CHECK(uuse::superblockValid(superblock));

        superblock.dataBlocks -= 1;
        UUSE_TEST_CHECK(!uuse::superblockValid(superblock)); /* CRC covers the change */
        uuse::superblockFinalize(superblock);
        UUSE_TEST_CHECK(uuse::superblockValid(superblock));
    }

    std::printf("geometry_test: ok\n");
    return 0;
}
