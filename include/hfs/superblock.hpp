#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "hfs/export.hpp"
#include "hfs/types.hpp"

namespace hfs {

/* On-disk superblock format (hfs2.0.md §9.2). All start/block fields are in
 * metadata_block_size units; device_blocks is the device capacity converted to
 * metadata blocks. Slot A lives at device offset 0, slot B at 4 MiB. */
constexpr std::uint64_t kSuperblockMagic = 0x4846533244415430ULL; /* "HFS2DAT0" */
constexpr std::uint32_t kSuperblockFormatVersion = 1;
constexpr std::uint32_t kSuperblockRecordSize = 4096;
constexpr std::uint64_t kSuperblockRegionBytes = 8ULL << 20;
constexpr std::uint64_t kSuperblockSlotOffsetBytes = kSuperblockRegionBytes / 2;

/* feature_flags bits. */
constexpr std::uint32_t kFeatureMetadataMirror = 1U << 0;

struct Hfs2Superblock {
    std::uint64_t magic;
    std::uint32_t formatVersion;
    std::uint32_t featureFlags;
    std::uint32_t deviceId;
    std::uint32_t replicaId;
    std::uint32_t replicaCount;
    std::uint32_t reserved0;
    std::array<std::uint8_t, 16> uuid;
    std::uint64_t generation;
    std::uint64_t deviceBlocks;
    std::uint32_t sectorSize;
    std::uint32_t metadataBlockSize;
    std::uint64_t journalStart;
    std::uint64_t journalBlocks;
    std::uint64_t checkpointAStart;
    std::uint64_t checkpointBStart;
    std::uint64_t checkpointBlocks;
    std::uint64_t dataStart;
    std::uint64_t dataBlocks;
    std::uint64_t checkpointGeneration;
    std::uint64_t journalHead;
    std::uint64_t journalTail;
    std::uint64_t mountEpoch;
    std::uint8_t cleanShutdown;
    std::uint8_t reserved1[3];
    std::uint32_t crc32c;
};

static_assert(sizeof(Hfs2Superblock) <= kSuperblockRecordSize,
              "superblock must fit into one 4 KiB record");

/* Per-device layout computed by format. */
struct DeviceGeometry {
    std::uint32_t deviceId = 0;
    std::uint32_t replicaId = 0;
    std::uint64_t deviceBlocks = 0; /* metadata blocks */
    std::uint64_t journalStart = 0;
    std::uint64_t journalBlocks = 0;
    std::uint64_t checkpointAStart = 0;
    std::uint64_t checkpointBStart = 0;
    std::uint64_t checkpointBlocks = 0;
    std::uint64_t dataStart = 0;
    std::uint64_t dataBlocks = 0;
};

/* Compute one device layout from its capacity; fails when the configured
 * regions do not fit with room for at least four data extents. */
HFS_API Result<DeviceGeometry> computeGeometry(std::uint64_t deviceBytes,
                                               std::uint32_t sectorSize,
                                               std::uint32_t metadataBlockSize,
                                               std::uint64_t journalMb,
                                               std::uint64_t checkpointMb,
                                               std::uint64_t extentMb);

HFS_API std::uint32_t superblockCrc(const Hfs2Superblock &superblock) noexcept;
HFS_API void superblockFinalize(Hfs2Superblock &superblock) noexcept;
HFS_API bool superblockValid(const Hfs2Superblock &superblock) noexcept;

} // namespace hfs
