#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/superblock.hpp"
#include "test_support.hpp"

namespace {

int readSuperblockSlot(hfs::Engine &engine, std::uint64_t offsetBlocks,
                       hfs::Hfs2Superblock *out) {
    std::vector<std::byte> record(hfs::kSuperblockRecordSize);
    auto read = engine.readDeviceBlocks(0, offsetBlocks, record);
    if (!read.ok()) {
        std::fprintf(stderr, "FAIL readDeviceBlocks: %s\n", read.message().c_str());
        return 1;
    }
    std::memcpy(out, record.data(), sizeof(*out));
    return 0;
}

} // namespace

int main() {
    const std::string lockPath = "/tmp/hfs2-format-" + std::to_string(::getpid()) + ".lock";
    auto parsed = hfs::parseConfig(validConfigText(lockPath), "memory");
    HFS_TEST_CHECK(parsed.ok());

    auto created = hfs::Engine::create(parsed.value());
    HFS_TEST_CHECK(created.ok());
    auto engine = std::move(created).value();

    /* format refuses without explicit confirmation */
    {
        auto refused = engine->format({false}).wait();
        HFS_TEST_CHECK(refused.ok());
        HFS_TEST_CHECK(refused.value().status() == hfs::Status::InvalidArgument);
        HFS_TEST_CHECK(!engine->formatted());
    }

    HFS_TEST_CHECK(engine->format({true}).wait().value().ok());
    HFS_TEST_CHECK(engine->formatted());
    const auto uuid = engine->volumeUuid();
    HFS_TEST_CHECK((uuid != std::array<std::uint8_t, 16>{}));

    HFS_TEST_CHECK(engine->mount().wait().value().ok());
    const std::uint32_t sector = engine->deviceBlockSize(0);
    HFS_TEST_CHECK(sector >= 512);

    hfs::Hfs2Superblock slotA{};
    hfs::Hfs2Superblock slotB{};
    HFS_TEST_CHECK(readSuperblockSlot(*engine, 0, &slotA) == 0);
    HFS_TEST_CHECK(readSuperblockSlot(*engine,
                                      hfs::kSuperblockSlotOffsetBytes / sector, &slotB) == 0);
    HFS_TEST_CHECK(hfs::superblockValid(slotA));
    HFS_TEST_CHECK(hfs::superblockValid(slotB));
    HFS_TEST_CHECK(slotA.generation == slotB.generation);
    HFS_TEST_CHECK(slotA.uuid == uuid && slotB.uuid == uuid);
    HFS_TEST_CHECK(slotA.deviceId == 0 && slotA.replicaId == 0 && slotA.replicaCount == 1);
    HFS_TEST_CHECK(slotA.crc32c == hfs::superblockCrc(slotA));
    HFS_TEST_CHECK(slotA.journalBlocks > 0 && slotA.checkpointBlocks >= 2);
    HFS_TEST_CHECK(slotA.dataBlocks > 0);

    auto geometries = engine->deviceGeometries();
    HFS_TEST_CHECK(geometries.size() == 1);
    HFS_TEST_CHECK(geometries[0].dataStart == slotA.dataStart);
    HFS_TEST_CHECK(geometries[0].dataBlocks == slotA.dataBlocks);

    /* raw block write/read/flush inside the data region */
    std::vector<std::byte> pattern(sector);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<std::byte>((i * 7 + 3) & 0xFF);
    }
    const std::uint64_t dataBlock =
        slotA.dataStart * (slotA.metadataBlockSize / sector) + 1;
    HFS_TEST_CHECK(engine->writeDeviceBlocks(0, dataBlock, pattern).ok());
    HFS_TEST_CHECK(engine->flushDevice(0).ok());
    std::vector<std::byte> readBack(sector, std::byte{0});
    HFS_TEST_CHECK(engine->readDeviceBlocks(0, dataBlock, readBack).ok());
    HFS_TEST_CHECK(std::memcmp(pattern.data(), readBack.data(), sector) == 0);

    /* a corrupted slot B still mounts from slot A */
    std::vector<std::byte> garbage(sector, std::byte{0xAB});
    HFS_TEST_CHECK(engine->writeDeviceBlocks(0, hfs::kSuperblockSlotOffsetBytes / sector,
                                             garbage).ok());
    HFS_TEST_CHECK(engine->flushDevice(0).ok());
    HFS_TEST_CHECK(engine->stop().wait().value().ok());

    {
        auto created2 = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(created2.ok());
        auto engine2 = std::move(created2).value();
        HFS_TEST_CHECK(engine2->mount().wait().value().ok());

        /* both slots corrupted: mount must fail closed */
        HFS_TEST_CHECK(engine2->writeDeviceBlocks(0, 0, garbage).ok());
        HFS_TEST_CHECK(engine2->flushDevice(0).ok());
        HFS_TEST_CHECK(engine2->stop().wait().value().ok());

        auto created3 = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(created3.ok());
        auto engine3 = std::move(created3).value();
        auto failed = engine3->mount().wait();
        HFS_TEST_CHECK(failed.ok());
        HFS_TEST_CHECK(failed.value().status() == hfs::Status::NotFound);
        HFS_TEST_CHECK(engine3->stop().wait().value().ok());

        /* restore a valid image for the other tests */
        auto created4 = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(created4.ok());
        auto engine4 = std::move(created4).value();
        HFS_TEST_CHECK(engine4->format({true}).wait().value().ok());
        HFS_TEST_CHECK(engine4->stop().wait().value().ok());
    }

    /* explicit volume_uuid mismatch fails closed */
    {
        std::string uuidText = validConfigText(lockPath);
        HFS_TEST_CHECK(replaceAll(uuidText, "volume_uuid = auto",
                                  "volume_uuid = 00112233445566778899aabbccddeeff"));
        auto uuidParsed = hfs::parseConfig(uuidText, "uuid-config");
        HFS_TEST_CHECK(uuidParsed.ok());

        auto uuidEngine = hfs::Engine::create(uuidParsed.value());
        HFS_TEST_CHECK(uuidEngine.ok());
        HFS_TEST_CHECK(uuidEngine.value()->format({true}).wait().value().ok());
        HFS_TEST_CHECK(uuidEngine.value()->stop().wait().value().ok());

        /* the same explicit uuid mounts */
        auto sameEngine = hfs::Engine::create(uuidParsed.value());
        HFS_TEST_CHECK(sameEngine.ok());
        HFS_TEST_CHECK(sameEngine.value()->mount().wait().value().ok());
        HFS_TEST_CHECK(sameEngine.value()->stop().wait().value().ok());

        /* a different explicit uuid is rejected */
        std::string otherText = uuidText;
        HFS_TEST_CHECK(replaceAll(otherText, "volume_uuid = 00112233445566778899aabbccddeeff",
                                  "volume_uuid = ffeeddccbbaa99887766554433221100"));
        auto otherParsed = hfs::parseConfig(otherText, "other-uuid");
        HFS_TEST_CHECK(otherParsed.ok());
        auto otherEngine = hfs::Engine::create(otherParsed.value());
        HFS_TEST_CHECK(otherEngine.ok());
        auto wrongUuid = otherEngine.value()->mount().wait();
        HFS_TEST_CHECK(wrongUuid.ok());
        HFS_TEST_CHECK(!wrongUuid.value().ok());
        HFS_TEST_CHECK(wrongUuid.value().status() == hfs::Status::Corrupt);
        HFS_TEST_CHECK(otherEngine.value()->stop().wait().value().ok());
    }

    /* stored geometry mismatch fails closed */
    {
        auto created5 = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(created5.ok());
        auto engine5 = std::move(created5).value();
        HFS_TEST_CHECK(engine5->format({true}).wait().value().ok());
        HFS_TEST_CHECK(engine5->mount().wait().value().ok());

        auto slots = engine5->readSuperblockSlots(0);
        HFS_TEST_CHECK(slots.ok());
        HFS_TEST_CHECK(slots.value().validA);
        hfs::Hfs2Superblock tampered = slots.value().slotA;
        tampered.dataBlocks -= 1; /* keeps ranges valid but breaks the geometry match */
        hfs::superblockFinalize(tampered);
        std::vector<std::byte> record(hfs::kSuperblockRecordSize, std::byte{0});
        std::memcpy(record.data(), &tampered, sizeof(tampered));
        HFS_TEST_CHECK(engine5->writeDeviceBlocks(0, 0, record).ok());
        HFS_TEST_CHECK(engine5->flushDevice(0).ok());
        HFS_TEST_CHECK(engine5->stop().wait().value().ok());

        auto created6 = hfs::Engine::create(parsed.value());
        HFS_TEST_CHECK(created6.ok());
        auto engine6 = std::move(created6).value();
        auto badGeometry = engine6->mount().wait();
        HFS_TEST_CHECK(badGeometry.ok());
        HFS_TEST_CHECK(!badGeometry.value().ok());
        HFS_TEST_CHECK(badGeometry.value().status() == hfs::Status::Corrupt);
        HFS_TEST_CHECK(engine6->stop().wait().value().ok());
    }

    /* a configured but missing bdev is rejected (bounded wait) */
    {
        std::string missingText = validConfigText(lockPath);
        HFS_TEST_CHECK(replaceAll(missingText, "bdev_name = Nvme0n1",
                                  "bdev_name = Nvme9n1"));
        auto missingParsed = hfs::parseConfig(missingText, "missing-device");
        HFS_TEST_CHECK(missingParsed.ok());
        auto missingEngine = hfs::Engine::create(missingParsed.value());
        HFS_TEST_CHECK(missingEngine.ok());
        auto armed = missingEngine.value()->mount().wait();
        HFS_TEST_CHECK(armed.ok());
        HFS_TEST_CHECK(!armed.value().ok());
        HFS_TEST_CHECK(missingEngine.value()->stop().wait().value().ok());
    }

    ::unlink(lockPath.c_str());
    std::printf("format_mount_test: ok\n");
    return 0;
}
