#include "storage/volume.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <unistd.h>

#include "common/log.hpp"
#include "runtime/spdk_runtime.hpp"

namespace hfs {

namespace {

/* Test-only boundary before publishing a clean or dirty superblock flag. */
bool testCleanFlagFailureRequested() {
    const char *value = std::getenv("HFS_TEST_FAIL_CLEAN_FLAG");
    return value != nullptr && std::string(value) == "1";
}

/* "auto" generates a random volume UUID; otherwise parse 32 hex digits with
 * optional dashes. */
Result<std::array<std::uint8_t, 16>> resolveUuid(const std::string &text) {
    std::array<std::uint8_t, 16> uuid{};
    if (text == "auto") {
        const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return makeError<std::array<std::uint8_t, 16>>(
                Status::IoError, "cannot open /dev/urandom for volume uuid");
        }
        std::size_t readBytes = 0;
        while (readBytes < uuid.size()) {
            const ssize_t rc = ::read(fd, uuid.data() + readBytes, uuid.size() - readBytes);
            if (rc <= 0) {
                ::close(fd);
                return makeError<std::array<std::uint8_t, 16>>(
                    Status::IoError, "cannot read /dev/urandom for volume uuid");
            }
            readBytes += static_cast<std::size_t>(rc);
        }
        ::close(fd);
        if (std::all_of(uuid.begin(), uuid.end(), [](std::uint8_t byte) { return byte == 0; })) {
            uuid[0] = 1; /* never emit the all-zero uuid */
        }
        return uuid;
    }

    std::string hex;
    for (char character : text) {
        if (character == '-' || character == ':') {
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(character)) == 0) {
            return makeError<std::array<std::uint8_t, 16>>(
                Status::InvalidArgument, "volume_uuid must be 'auto' or 32 hex digits");
        }
        hex.push_back(character);
    }
    if (hex.size() != 32) {
        return makeError<std::array<std::uint8_t, 16>>(
            Status::InvalidArgument, "volume_uuid must be 'auto' or 32 hex digits");
    }
    auto hexValue = [](char character) -> std::uint8_t {
        if (character >= '0' && character <= '9') {
            return static_cast<std::uint8_t>(character - '0');
        }
        return static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(character)) -
                                         'a' + 10);
    };
    for (std::size_t i = 0; i < uuid.size(); ++i) {
        uuid[i] = static_cast<std::uint8_t>((hexValue(hex[i * 2]) << 4) |
                                            hexValue(hex[i * 2 + 1]));
    }
    return uuid;
}

std::vector<std::byte> buildSuperblockRecord(const Hfs2Superblock &superblock,
                                             std::uint32_t metadataBlockSize) {
    std::vector<std::byte> record(metadataBlockSize, std::byte{0});
    std::memcpy(record.data(), &superblock, sizeof(superblock));
    return record;
}

Result<bool> readSuperblock(SpdkRuntime &runtime, std::size_t deviceIndex,
                            std::uint64_t byteOffset, std::uint32_t metadataBlockSize,
                            std::uint32_t sectorSize, Hfs2Superblock *out) {
    std::vector<std::byte> record(metadataBlockSize);
    const std::uint64_t offsetBlocks = byteOffset / sectorSize;
    auto read = runtime.controlRead(deviceIndex, offsetBlocks, record);
    if (!read.ok()) {
        return makeError<bool>(read.status(), read.message());
    }
    Hfs2Superblock candidate{};
    std::memcpy(&candidate, record.data(), sizeof(candidate));
    if (!superblockValid(candidate)) {
        return false;
    }
    *out = candidate;
    return true;
}

DeviceGeometry geometryFromSuperblock(const Hfs2Superblock &superblock) {
    DeviceGeometry geometry;
    geometry.deviceId = superblock.deviceId;
    geometry.replicaId = superblock.replicaId;
    geometry.deviceBlocks = superblock.deviceBlocks;
    geometry.journalStart = superblock.journalStart;
    geometry.journalBlocks = superblock.journalBlocks;
    geometry.checkpointAStart = superblock.checkpointAStart;
    geometry.checkpointBStart = superblock.checkpointBStart;
    geometry.checkpointBlocks = superblock.checkpointBlocks;
    geometry.dataStart = superblock.dataStart;
    geometry.dataBlocks = superblock.dataBlocks;
    return geometry;
}

bool sameGeometry(const DeviceGeometry &left, const DeviceGeometry &right) {
    return left.deviceBlocks == right.deviceBlocks && left.journalStart == right.journalStart &&
           left.journalBlocks == right.journalBlocks &&
           left.checkpointAStart == right.checkpointAStart &&
           left.checkpointBStart == right.checkpointBStart &&
           left.checkpointBlocks == right.checkpointBlocks &&
           left.dataStart == right.dataStart && left.dataBlocks == right.dataBlocks;
}

Result<DeviceGeometry> expectedGeometry(SpdkRuntime &runtime, const Config &config,
                                        std::size_t deviceIndex) {
    const std::uint64_t deviceBytes = runtime.deviceCapacityBytes(deviceIndex);
    const std::uint32_t sectorSize = runtime.deviceBlockSize(deviceIndex);
    if (deviceBytes == 0 || sectorSize == 0) {
        return makeError<DeviceGeometry>(Status::IoError,
                                         "device geometry is not available");
    }
    return computeGeometry(deviceBytes, sectorSize, config.metadataBlockSize,
                           config.journalMb, config.checkpointMb, config.extentMb);
}

} // namespace

Result<VolumeInfo> formatVolume(SpdkRuntime &runtime, const Config &config, bool confirm) {
    if (!confirm) {
        return makeError<VolumeInfo>(
            Status::InvalidArgument,
            "format destroys all data on the configured device set; confirmation is required");
    }
    auto uuid = resolveUuid(config.volumeUuid);
    if (!uuid.ok()) {
        return makeError<VolumeInfo>(uuid.status(), uuid.message());
    }

    VolumeInfo volume;
    volume.uuid = uuid.value();
    volume.generation = 1;
    volume.metadataBlockSize = config.metadataBlockSize;
    volume.replicaCount = config.metadataReplication;
    volume.cleanShutdown = true;

    std::uint32_t nextReplicaId = 0;
    for (std::size_t i = 0; i < config.devices.size(); ++i) {
        auto geometry = expectedGeometry(runtime, config, i);
        if (!geometry.ok()) {
            return makeError<VolumeInfo>(geometry.status(), geometry.message());
        }
        DeviceGeometry layout = geometry.value();
        layout.deviceId = static_cast<std::uint32_t>(i);
        layout.replicaId = config.devices[i].metadata ? nextReplicaId++ : 0;

        Hfs2Superblock superblock{};
        superblock.magic = kSuperblockMagic;
        superblock.formatVersion = kSuperblockFormatVersion;
        superblock.featureFlags =
            volume.replicaCount > 1 ? kFeatureMetadataMirror : 0U;
        superblock.deviceId = layout.deviceId;
        superblock.replicaId = layout.replicaId;
        superblock.replicaCount = volume.replicaCount;
        superblock.uuid = volume.uuid;
        superblock.generation = volume.generation;
        superblock.deviceBlocks = layout.deviceBlocks;
        superblock.sectorSize = runtime.deviceBlockSize(i);
        superblock.metadataBlockSize = config.metadataBlockSize;
        superblock.journalStart = layout.journalStart;
        superblock.journalBlocks = layout.journalBlocks;
        superblock.checkpointAStart = layout.checkpointAStart;
        superblock.checkpointBStart = layout.checkpointBStart;
        superblock.checkpointBlocks = layout.checkpointBlocks;
        superblock.dataStart = layout.dataStart;
        superblock.dataBlocks = layout.dataBlocks;
        superblock.checkpointGeneration = 0;
        superblock.journalHead = 0;
        superblock.journalTail = 0;
        superblock.mountEpoch = 0;
        superblock.cleanShutdown = 1;
        superblockFinalize(superblock);

        const std::uint32_t sectorSize = runtime.deviceBlockSize(i);
        const std::uint64_t blockFactor = config.metadataBlockSize / sectorSize;
        std::vector<std::byte> record =
            buildSuperblockRecord(superblock, config.metadataBlockSize);
        auto writeA = runtime.controlWrite(i, 0, record);
        if (!writeA.ok()) {
            return makeError<VolumeInfo>(writeA.status(),
                                         "superblock A write failed: " + writeA.message());
        }
        auto writeB = runtime.controlWrite(i, kSuperblockSlotOffsetBytes / sectorSize, record);
        if (!writeB.ok()) {
            return makeError<VolumeInfo>(writeB.status(),
                                         "superblock B write failed: " + writeB.message());
        }
        /* Invalidate any older journal/checkpoint generation so a fresh image
         * never replays stale ring content. */
        const std::vector<std::byte> emptyBlock(config.metadataBlockSize, std::byte{0});
        const std::uint64_t journalHeadBlock = layout.journalStart;
        auto clearJournal = runtime.controlWrite(i, journalHeadBlock * blockFactor, emptyBlock);
        if (!clearJournal.ok()) {
            return makeError<VolumeInfo>(clearJournal.status(),
                                         "journal reset failed: " + clearJournal.message());
        }
        for (const std::uint64_t slotStart : {layout.checkpointAStart, layout.checkpointBStart}) {
            auto clearSlot = runtime.controlWrite(i, slotStart * blockFactor, emptyBlock);
            if (!clearSlot.ok()) {
                return makeError<VolumeInfo>(clearSlot.status(),
                                             "checkpoint reset failed: " + clearSlot.message());
            }
        }
        auto flush = runtime.controlFlush(i);
        if (!flush.ok()) {
            return makeError<VolumeInfo>(flush.status(), flush.message());
        }
        log::info("formatted device %zu: blocks=%llu data_start=%llu data_blocks=%llu",
                  i, static_cast<unsigned long long>(layout.deviceBlocks),
                  static_cast<unsigned long long>(layout.dataStart),
                  static_cast<unsigned long long>(layout.dataBlocks));
        volume.devices.push_back(layout);
    }
    return volume;
}

Result<VolumeInfo> mountVolume(SpdkRuntime &runtime, const Config &config) {
    VolumeInfo volume;
    volume.metadataBlockSize = config.metadataBlockSize;
    volume.replicaCount = config.metadataReplication;
    volume.cleanShutdown = true;
    /* Track every configured metadata replica so missing or duplicated roles
     * fail closed before any journal or namespace state is opened. */
    std::vector<bool> metadataReplicasSeen(config.metadataReplication, false);
    std::size_t metadataReplicaCount = 0;
    Hfs2Superblock metadataReference{};
    bool haveMetadataReference = false;

    for (std::size_t i = 0; i < config.devices.size(); ++i) {
        const std::uint64_t deviceBytes = runtime.deviceCapacityBytes(i);
        const std::uint32_t sectorSize = runtime.deviceBlockSize(i);
        if (deviceBytes == 0 || sectorSize == 0) {
            return makeError<VolumeInfo>(Status::IoError, "device geometry is not available");
        }

        Hfs2Superblock slotA{};
        Hfs2Superblock slotB{};
        auto readA = readSuperblock(runtime, i, 0, config.metadataBlockSize, sectorSize, &slotA);
        if (!readA.ok()) {
            return makeError<VolumeInfo>(readA.status(), readA.message());
        }
        auto readB = readSuperblock(runtime, i, kSuperblockSlotOffsetBytes,
                                    config.metadataBlockSize, sectorSize, &slotB);
        if (!readB.ok()) {
            return makeError<VolumeInfo>(readB.status(), readB.message());
        }

        const bool hasA = readA.value();
        const bool hasB = readB.value();
        if (!hasA && !hasB) {
            return makeError<VolumeInfo>(
                Status::NotFound, "device '" + config.devices[i].bdevName +
                                      "' is not formatted as HFS 2.0 (or was erased)");
        }
        const Hfs2Superblock &chosen =
            (hasA && hasB) ? (slotB.generation > slotA.generation ? slotB : slotA)
                           : (hasA ? slotA : slotB);

        if (chosen.metadataBlockSize != config.metadataBlockSize) {
            return makeError<VolumeInfo>(Status::Corrupt,
                                         "metadata_block_size mismatch on device '" +
                                             config.devices[i].bdevName + "'");
        }
        if (chosen.replicaCount != config.metadataReplication) {
            return makeError<VolumeInfo>(Status::Corrupt,
                                         "metadata_replication mismatch on device '" +
                                             config.devices[i].bdevName + "'");
        }
        const bool isMetadataDevice = config.devices[i].metadata;
        if (isMetadataDevice) {
            if (chosen.replicaId >= metadataReplicasSeen.size()) {
                return makeError<VolumeInfo>(
                    Status::Corrupt, "metadata replica_id is outside the configured set on device '" +
                                         config.devices[i].bdevName + "'");
            }
            if (metadataReplicasSeen[chosen.replicaId]) {
                return makeError<VolumeInfo>(
                    Status::Corrupt, "duplicate metadata replica_id on device '" +
                                         config.devices[i].bdevName + "'");
            }
            metadataReplicasSeen[chosen.replicaId] = true;
            ++metadataReplicaCount;
            const bool mirrored = (chosen.featureFlags & kFeatureMetadataMirror) != 0;
            if (mirrored != (config.metadataReplication > 1)) {
                return makeError<VolumeInfo>(
                    Status::Corrupt, "metadata mirror feature mismatch on device '" +
                                         config.devices[i].bdevName + "'");
            }
            if (!haveMetadataReference) {
                metadataReference = chosen;
                haveMetadataReference = true;
            } else if (chosen.generation != metadataReference.generation ||
                       chosen.checkpointGeneration != metadataReference.checkpointGeneration ||
                       chosen.journalHead != metadataReference.journalHead ||
                       chosen.journalTail != metadataReference.journalTail ||
                       chosen.mountEpoch != metadataReference.mountEpoch ||
                       chosen.cleanShutdown != metadataReference.cleanShutdown) {
                return makeError<VolumeInfo>(
                    Status::Corrupt, "metadata replica state is inconsistent on device '" +
                                         config.devices[i].bdevName + "'");
            }
        }
        if (chosen.deviceId != i) {
            return makeError<VolumeInfo>(Status::Corrupt,
                                         "device_id mismatch on device '" +
                                             config.devices[i].bdevName + "'");
        }
        auto expected = expectedGeometry(runtime, config, i);
        if (!expected.ok()) {
            return makeError<VolumeInfo>(expected.status(), expected.message());
        }
        DeviceGeometry stored = geometryFromSuperblock(chosen);
        if (!sameGeometry(stored, expected.value())) {
            return makeError<VolumeInfo>(Status::Corrupt,
                                         "stored geometry does not match device '" +
                                             config.devices[i].bdevName + "'");
        }
        if (i == 0) {
            volume.uuid = chosen.uuid;
            volume.generation = chosen.generation;
        } else if (chosen.uuid != volume.uuid) {
            return makeError<VolumeInfo>(Status::Corrupt,
                                         "volume uuid mismatch on device '" +
                                             config.devices[i].bdevName + "'");
        }
        if (chosen.cleanShutdown == 0) {
            volume.cleanShutdown = false;
        }
        volume.devices.push_back(stored);
    }

    if (metadataReplicaCount != config.metadataReplication ||
        std::find(metadataReplicasSeen.begin(), metadataReplicasSeen.end(), false) !=
            metadataReplicasSeen.end()) {
        return makeError<VolumeInfo>(Status::Corrupt,
                                     "metadata replica set is incomplete");
    }

    if (config.volumeUuid != "auto") {
        auto expected = resolveUuid(config.volumeUuid);
        if (!expected.ok()) {
            return makeError<VolumeInfo>(expected.status(), expected.message());
        }
        if (volume.uuid != expected.value()) {
            return makeError<VolumeInfo>(
                Status::Corrupt, "volume uuid does not match the configured volume_uuid");
        }
    }

    log::info("volume mounted: generation=%llu devices=%zu clean=%d",
              static_cast<unsigned long long>(volume.generation), volume.devices.size(),
              volume.cleanShutdown ? 1 : 0);
    return volume;
}

Result<void> updateCleanShutdown(SpdkRuntime &runtime, const Config &config, bool clean) {
    std::vector<Hfs2Superblock> current(config.devices.size());
    std::vector<std::uint32_t> sectorSizes(config.devices.size(), 0);
    std::vector<std::uint64_t> targetOffsets(config.devices.size(), 0);
    /* Read every replica before publishing one shared lifecycle version. */
    for (std::size_t deviceIndex = 0; deviceIndex < config.devices.size(); ++deviceIndex) {
        const std::uint32_t sectorSize = runtime.deviceBlockSize(deviceIndex);
        if (sectorSize == 0 || config.metadataBlockSize == 0) {
            return makeError(Status::IoError, "device geometry is not available");
        }
        Hfs2Superblock slotA{};
        Hfs2Superblock slotB{};
        auto readA = readSuperblock(runtime, deviceIndex, 0, config.metadataBlockSize,
                                    sectorSize, &slotA);
        if (!readA.ok()) return makeError(readA.status(), readA.message());
        auto readB = readSuperblock(runtime, deviceIndex, kSuperblockSlotOffsetBytes,
                                    config.metadataBlockSize, sectorSize, &slotB);
        if (!readB.ok()) return makeError(readB.status(), readB.message());
        if (!readA.value() && !readB.value()) {
            return makeError(Status::NotFound, "no valid superblock while updating clean flag");
        }
        const bool useB = readB.value() && (!readA.value() || slotB.generation > slotA.generation);
        current[deviceIndex] = useB ? slotB : slotA;
        sectorSizes[deviceIndex] = sectorSize;
        targetOffsets[deviceIndex] = useB ? 0 : kSuperblockSlotOffsetBytes;
    }
    const Hfs2Superblock &reference = current.front();
    if (reference.generation == std::numeric_limits<std::uint64_t>::max() ||
        reference.mountEpoch == std::numeric_limits<std::uint64_t>::max()) {
        return makeError(Status::Corrupt, "superblock lifecycle counter overflow");
    }
    const std::uint64_t nextGeneration = reference.generation + 1;
    const std::uint64_t nextMountEpoch = reference.mountEpoch + 1;
    if (testCleanFlagFailureRequested()) {
        return makeError(Status::IoError,
                         "test-injected clean-flag publish failure");
    }
    /* Publish identical generation and epoch values to all configured devices. */
    for (std::size_t deviceIndex = 0; deviceIndex < config.devices.size(); ++deviceIndex) {
        Hfs2Superblock updated = current[deviceIndex];
        updated.generation = nextGeneration;
        updated.mountEpoch = nextMountEpoch;
        updated.cleanShutdown = clean ? 1 : 0;
        superblockFinalize(updated);
        auto written = runtime.controlWrite(
            deviceIndex, targetOffsets[deviceIndex] / sectorSizes[deviceIndex],
            buildSuperblockRecord(updated, config.metadataBlockSize));
        if (!written.ok()) return written;
        auto flushed = runtime.controlFlush(deviceIndex);
        if (!flushed.ok()) return flushed;
    }
    return Result<void>(Status::Ok);
}

} // namespace hfs
