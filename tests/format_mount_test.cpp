#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "uuse/config.hpp"
#include "uuse/core.hpp"
#include "uuse/superblock.hpp"
#include "runtime/spdk_runtime.hpp"
#include "test_support.hpp"

namespace {

int readSuperblockSlot(uuse::Engine &engine, std::uint64_t offsetBlocks,
                       uuse::UuseSuperblock *out) {
    std::vector<std::byte> record(uuse::kSuperblockRecordSize);
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
    const std::string lockPath = "/tmp/uuse-format-" + std::to_string(::getpid()) + ".lock";
    auto parsed = uuse::parseConfig(validConfigText(lockPath), "memory");
    UUSE_TEST_CHECK(parsed.ok());

    auto created = uuse::Engine::create(parsed.value());
    UUSE_TEST_CHECK(created.ok());
    auto engine = std::move(created).value();

    /* format refuses without explicit confirmation */
    {
        auto refused = engine->format({false}).wait();
        UUSE_TEST_CHECK(refused.ok());
        UUSE_TEST_CHECK(refused.value().status() == uuse::Status::InvalidArgument);
        UUSE_TEST_CHECK(!engine->formatted());
    }

    UUSE_TEST_CHECK(engine->format({true}).wait().value().ok());
    UUSE_TEST_CHECK(engine->formatted());
    const auto uuid = engine->volumeUuid();
    UUSE_TEST_CHECK((uuid != std::array<std::uint8_t, 16>{}));

    UUSE_TEST_CHECK(engine->mount().wait().value().ok());
    const std::uint32_t sector = engine->deviceBlockSize(0);
    UUSE_TEST_CHECK(sector >= 512);

    uuse::UuseSuperblock slotA{};
    uuse::UuseSuperblock slotB{};
    UUSE_TEST_CHECK(readSuperblockSlot(*engine, 0, &slotA) == 0);
    UUSE_TEST_CHECK(readSuperblockSlot(*engine,
                                      uuse::kSuperblockSlotOffsetBytes / sector, &slotB) == 0);
    UUSE_TEST_CHECK(uuse::superblockValid(slotA));
    UUSE_TEST_CHECK(uuse::superblockValid(slotB));
    /* Mount may advance only the active slot; both replicas remain valid even
     * when their generations are adjacent during the clean-state transition. */
    UUSE_TEST_CHECK(slotA.generation > 0 && slotB.generation > 0);
    UUSE_TEST_CHECK(slotA.uuid == uuid && slotB.uuid == uuid);
    UUSE_TEST_CHECK(slotA.deviceId == 0 && slotA.replicaId == 0 && slotA.replicaCount == 1);
    UUSE_TEST_CHECK(slotA.crc32c == uuse::superblockCrc(slotA));
    UUSE_TEST_CHECK(slotA.journalBlocks > 0 && slotA.checkpointBlocks >= 2);
    UUSE_TEST_CHECK(slotA.dataBlocks > 0);

    auto geometries = engine->deviceGeometries();
    UUSE_TEST_CHECK(geometries.size() == 1);
    UUSE_TEST_CHECK(geometries[0].dataStart == slotA.dataStart);
    UUSE_TEST_CHECK(geometries[0].dataBlocks == slotA.dataBlocks);

    /* raw block write/read/flush inside the data region */
    std::vector<std::byte> pattern(sector);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<std::byte>((i * 7 + 3) & 0xFF);
    }
    const std::uint64_t dataBlock =
        slotA.dataStart * (slotA.metadataBlockSize / sector) + 1;
    UUSE_TEST_CHECK(engine->writeDeviceBlocks(0, dataBlock, pattern).ok());
    UUSE_TEST_CHECK(engine->flushDevice(0).ok());
    std::vector<std::byte> readBack(sector, std::byte{0});
    UUSE_TEST_CHECK(engine->readDeviceBlocks(0, dataBlock, readBack).ok());
    UUSE_TEST_CHECK(std::memcmp(pattern.data(), readBack.data(), sector) == 0);

    /* a corrupted slot B still mounts from slot A */
    std::vector<std::byte> garbage(sector, std::byte{0xAB});
    UUSE_TEST_CHECK(engine->writeDeviceBlocks(0, uuse::kSuperblockSlotOffsetBytes / sector,
                                             garbage).ok());
    UUSE_TEST_CHECK(engine->flushDevice(0).ok());
    UUSE_TEST_CHECK(engine->stop().wait().value().ok());

    {
        auto created2 = uuse::Engine::create(parsed.value());
        UUSE_TEST_CHECK(created2.ok());
        auto engine2 = std::move(created2).value();
        UUSE_TEST_CHECK(engine2->mount().wait().value().ok());

        /* corrupt both slots after the fallback mount; the next mount must
         * fail closed without allowing clean-stop publication to repair one. */
        UUSE_TEST_CHECK(engine2->writeDeviceBlocks(0, 0, garbage).ok());
        UUSE_TEST_CHECK(engine2->writeDeviceBlocks(0,
                                                 uuse::kSuperblockSlotOffsetBytes / sector,
                                                 garbage)
                           .ok());
        UUSE_TEST_CHECK(engine2->flushDevice(0).ok());
        auto damagedStop = engine2->stop().wait();
        UUSE_TEST_CHECK(damagedStop.ok());
        UUSE_TEST_CHECK(!damagedStop.value().ok());
        /* Engine stop preserves the publication error and therefore leaves
         * the process-wide runtime active; stop that test-owned runtime before
         * creating the next engine in this process. */
        UUSE_TEST_CHECK(uuse::SpdkRuntime::instance().stop().ok());
        engine2.reset();

        auto created3 = uuse::Engine::create(parsed.value());
        UUSE_TEST_CHECK(created3.ok());
        auto engine3 = std::move(created3).value();
        auto failed = engine3->mount().wait();
        UUSE_TEST_CHECK(failed.ok());
        UUSE_TEST_CHECK(failed.value().status() == uuse::Status::NotFound);
        UUSE_TEST_CHECK(engine3->stop().wait().value().ok());

        /* restore a valid image for the other tests */
        auto created4 = uuse::Engine::create(parsed.value());
        UUSE_TEST_CHECK(created4.ok());
        auto engine4 = std::move(created4).value();
        UUSE_TEST_CHECK(engine4->format({true}).wait().value().ok());
        UUSE_TEST_CHECK(engine4->stop().wait().value().ok());
    }

    /* explicit volume_uuid mismatch fails closed */
    {
        std::string uuidText = validConfigText(lockPath);
        UUSE_TEST_CHECK(replaceAll(uuidText, "volume_uuid = auto",
                                  "volume_uuid = 00112233445566778899aabbccddeeff"));
        auto uuidParsed = uuse::parseConfig(uuidText, "uuid-config");
        UUSE_TEST_CHECK(uuidParsed.ok());

        auto uuidEngine = uuse::Engine::create(uuidParsed.value());
        UUSE_TEST_CHECK(uuidEngine.ok());
        UUSE_TEST_CHECK(uuidEngine.value()->format({true}).wait().value().ok());
        UUSE_TEST_CHECK(uuidEngine.value()->stop().wait().value().ok());

        /* the same explicit uuid mounts */
        auto sameEngine = uuse::Engine::create(uuidParsed.value());
        UUSE_TEST_CHECK(sameEngine.ok());
        UUSE_TEST_CHECK(sameEngine.value()->mount().wait().value().ok());
        UUSE_TEST_CHECK(sameEngine.value()->stop().wait().value().ok());

        /* a different explicit uuid is rejected */
        std::string otherText = uuidText;
        UUSE_TEST_CHECK(replaceAll(otherText, "volume_uuid = 00112233445566778899aabbccddeeff",
                                  "volume_uuid = ffeeddccbbaa99887766554433221100"));
        auto otherParsed = uuse::parseConfig(otherText, "other-uuid");
        UUSE_TEST_CHECK(otherParsed.ok());
        auto otherEngine = uuse::Engine::create(otherParsed.value());
        UUSE_TEST_CHECK(otherEngine.ok());
        auto wrongUuid = otherEngine.value()->mount().wait();
        UUSE_TEST_CHECK(wrongUuid.ok());
        UUSE_TEST_CHECK(!wrongUuid.value().ok());
        UUSE_TEST_CHECK(wrongUuid.value().status() == uuse::Status::Corrupt);
        UUSE_TEST_CHECK(otherEngine.value()->stop().wait().value().ok());
    }

    /* stored geometry mismatch fails closed */
    {
        auto created5 = uuse::Engine::create(parsed.value());
        UUSE_TEST_CHECK(created5.ok());
        auto engine5 = std::move(created5).value();
        UUSE_TEST_CHECK(engine5->format({true}).wait().value().ok());
        UUSE_TEST_CHECK(engine5->mount().wait().value().ok());

        auto slots = engine5->readSuperblockSlots(0);
        UUSE_TEST_CHECK(slots.ok());
        UUSE_TEST_CHECK(slots.value().validA);
        const bool tamperB = slots.value().validB &&
                             slots.value().slotB.generation > slots.value().slotA.generation;
        uuse::UuseSuperblock tampered = tamperB ? slots.value().slotB : slots.value().slotA;
        tampered.dataBlocks -= 1; /* keeps ranges valid but breaks the geometry match */
        uuse::superblockFinalize(tampered);
        std::vector<std::byte> record(uuse::kSuperblockRecordSize, std::byte{0});
        std::memcpy(record.data(), &tampered, sizeof(tampered));
        const std::uint64_t tamperOffset =
            tamperB ? uuse::kSuperblockSlotOffsetBytes / sector : 0;
        UUSE_TEST_CHECK(engine5->writeDeviceBlocks(0, tamperOffset, record).ok());
        UUSE_TEST_CHECK(engine5->flushDevice(0).ok());
        UUSE_TEST_CHECK(engine5->stop().wait().value().ok());

        auto created6 = uuse::Engine::create(parsed.value());
        UUSE_TEST_CHECK(created6.ok());
        auto engine6 = std::move(created6).value();
        auto badGeometry = engine6->mount().wait();
        UUSE_TEST_CHECK(badGeometry.ok());
        UUSE_TEST_CHECK(!badGeometry.value().ok());
        UUSE_TEST_CHECK(badGeometry.value().status() == uuse::Status::Corrupt);
        UUSE_TEST_CHECK(engine6->stop().wait().value().ok());
    }

    /* a configured but missing bdev is rejected (bounded wait) */
    {
        std::string missingText = validConfigText(lockPath);
        UUSE_TEST_CHECK(replaceAll(missingText, "bdev_name = Nvme0n1",
                                  "bdev_name = Nvme9n1"));
        auto missingParsed = uuse::parseConfig(missingText, "missing-device");
        UUSE_TEST_CHECK(missingParsed.ok());
        auto missingEngine = uuse::Engine::create(missingParsed.value());
        UUSE_TEST_CHECK(missingEngine.ok());
        auto armed = missingEngine.value()->mount().wait();
        UUSE_TEST_CHECK(armed.ok());
        UUSE_TEST_CHECK(!armed.value().ok());
        UUSE_TEST_CHECK(missingEngine.value()->stop().wait().value().ok());
    }

    ::unlink(lockPath.c_str());
    std::printf("format_mount_test: ok\n");
    return 0;
}
