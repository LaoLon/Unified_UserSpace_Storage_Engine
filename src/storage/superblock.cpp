#include "hfs/superblock.hpp"

#include <cstring>

#include "hfs/crc.hpp"

namespace hfs {

namespace {

bool isPowerOfTwo(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

std::uint64_t bytesToBlocks(std::uint64_t bytes, std::uint64_t blockSize) {
    return (bytes + blockSize - 1) / blockSize;
}

} // namespace

Result<DeviceGeometry> computeGeometry(std::uint64_t deviceBytes,
                                       std::uint32_t sectorSize,
                                       std::uint32_t metadataBlockSize,
                                       std::uint64_t journalMb,
                                       std::uint64_t checkpointMb,
                                       std::uint64_t extentMb) {
    auto invalid = [](const char *message) {
        return makeError<DeviceGeometry>(Status::InvalidArgument, message);
    };
    if (sectorSize == 0 || !isPowerOfTwo(sectorSize)) {
        return invalid("sector_size must be a power of two");
    }
    if (metadataBlockSize < sectorSize || !isPowerOfTwo(metadataBlockSize) ||
        metadataBlockSize % sectorSize != 0) {
        return invalid("metadata_block_size must be a power of two multiple of sector_size");
    }
    if (journalMb == 0 || checkpointMb == 0 || extentMb == 0) {
        return invalid("journal/checkpoint/extent sizes must be non-zero");
    }

    const std::uint64_t metaBytes = metadataBlockSize;
    const std::uint64_t deviceBlocks = deviceBytes / metaBytes;
    const std::uint64_t superblockBlocks = bytesToBlocks(kSuperblockRegionBytes, metaBytes);
    const std::uint64_t journalBlocks = bytesToBlocks(journalMb << 20, metaBytes);
    const std::uint64_t checkpointBlocks = bytesToBlocks(checkpointMb << 20, metaBytes);
    if (checkpointBlocks < 2) {
        return invalid("checkpoint region must hold two slots");
    }
    const std::uint64_t extentBlocks = bytesToBlocks(extentMb << 20, metaBytes);

    DeviceGeometry geometry;
    geometry.deviceBlocks = deviceBlocks;
    geometry.journalStart = superblockBlocks;
    geometry.journalBlocks = journalBlocks;
    geometry.checkpointAStart = geometry.journalStart + journalBlocks;
    geometry.checkpointBStart = geometry.checkpointAStart + checkpointBlocks / 2;
    geometry.checkpointBlocks = checkpointBlocks;
    geometry.dataStart = geometry.checkpointAStart + checkpointBlocks;
    if (geometry.dataStart >= deviceBlocks) {
        return invalid("device is too small for the configured metadata regions");
    }
    geometry.dataBlocks = deviceBlocks - geometry.dataStart;
    if (geometry.dataBlocks < 4 * extentBlocks) {
        return invalid("device has less than four data extents after metadata regions");
    }
    return geometry;
}

std::uint32_t superblockCrc(const Hfs2Superblock &superblock) noexcept {
    return crc32c(&superblock, offsetof(Hfs2Superblock, crc32c));
}

void superblockFinalize(Hfs2Superblock &superblock) noexcept {
    superblock.crc32c = superblockCrc(superblock);
}

bool superblockValid(const Hfs2Superblock &superblock) noexcept {
    if (superblock.magic != kSuperblockMagic ||
        superblock.formatVersion != kSuperblockFormatVersion) {
        return false;
    }
    if (superblock.crc32c != superblockCrc(superblock)) {
        return false;
    }
    if (superblock.sectorSize == 0 || superblock.metadataBlockSize < superblock.sectorSize ||
        superblock.metadataBlockSize % superblock.sectorSize != 0) {
        return false;
    }
    if (superblock.replicaCount == 0 || superblock.replicaId >= superblock.replicaCount) {
        return false;
    }
    if (superblock.journalStart < 1 ||
        superblock.journalStart + superblock.journalBlocks > superblock.checkpointAStart) {
        return false;
    }
    if (superblock.checkpointAStart + superblock.checkpointBlocks > superblock.dataStart) {
        return false;
    }
    if (superblock.dataStart + superblock.dataBlocks > superblock.deviceBlocks) {
        return false;
    }
    if (superblock.dataBlocks == 0) {
        return false;
    }
    return true;
}

} // namespace hfs
