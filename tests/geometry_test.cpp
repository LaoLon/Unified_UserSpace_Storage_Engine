#include <cstdio>

#include "hfs/superblock.hpp"
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
            hfs::computeGeometry(30 * kGiB, kSector, kMeta, 256, 512, 1);
        HFS_TEST_CHECK(geometry.ok());
        const auto &layout = geometry.value();
        HFS_TEST_CHECK(layout.deviceBlocks == (30 * kGiB) / kMeta);
        HFS_TEST_CHECK(layout.journalStart + layout.journalBlocks ==
                       layout.checkpointAStart);
        HFS_TEST_CHECK(layout.checkpointAStart + layout.checkpointBlocks ==
                       layout.dataStart);
        HFS_TEST_CHECK(layout.checkpointBStart > layout.checkpointAStart);
        HFS_TEST_CHECK(layout.checkpointBStart < layout.dataStart);
        HFS_TEST_CHECK(layout.dataBlocks > 0);
    }

    /* Two different device sizes both fit and keep the region ordering. */
    {
        auto small = hfs::computeGeometry(8 * kGiB, kSector, kMeta, 64, 64, 1);
        auto large = hfs::computeGeometry(40 * kGiB, kSector, kMeta, 256, 512, 1);
        HFS_TEST_CHECK(small.ok());
        HFS_TEST_CHECK(large.ok());
        HFS_TEST_CHECK(large.value().deviceBlocks > small.value().deviceBlocks);
        HFS_TEST_CHECK(large.value().dataBlocks > small.value().dataBlocks);
    }

    /* Too small for the configured regions. */
    {
        auto tiny = hfs::computeGeometry(64ULL << 20, kSector, kMeta, 256, 512, 1);
        HFS_TEST_CHECK(!tiny.ok());
        HFS_TEST_CHECK(tiny.status() == hfs::Status::InvalidArgument);
    }

    /* Invalid metadata block size. */
    {
        auto bad = hfs::computeGeometry(30 * kGiB, kSector, 3000, 256, 512, 1);
        HFS_TEST_CHECK(!bad.ok());
    }

    /* Superblock record CRC round trip. */
    {
        hfs::Hfs2Superblock superblock{};
        superblock.magic = hfs::kSuperblockMagic;
        superblock.formatVersion = hfs::kSuperblockFormatVersion;
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
        hfs::superblockFinalize(superblock);
        HFS_TEST_CHECK(hfs::superblockValid(superblock));

        superblock.dataBlocks -= 1;
        HFS_TEST_CHECK(!hfs::superblockValid(superblock)); /* CRC covers the change */
        hfs::superblockFinalize(superblock);
        HFS_TEST_CHECK(hfs::superblockValid(superblock));
    }

    std::printf("geometry_test: ok\n");
    return 0;
}
