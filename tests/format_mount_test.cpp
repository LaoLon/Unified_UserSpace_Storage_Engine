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

    ::unlink(lockPath.c_str());
    std::printf("format_mount_test: ok\n");
    return 0;
}
