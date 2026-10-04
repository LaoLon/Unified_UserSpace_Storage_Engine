#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "uuse/journal.hpp"
#include "uuse/superblock.hpp"
#include "metadata/group_commit.hpp"
#include "test_support.hpp"

namespace {

constexpr std::uint32_t kMeta = 4096;
constexpr std::uint64_t kDeviceBytes = 64ULL << 20;

class MemoryDevice final : public uuse::JournalDevice {
public:
    explicit MemoryDevice(std::uint64_t deviceBytes)
        : data_(deviceBytes, std::byte{0}) {}

    std::uint32_t blockSize() const noexcept override { return kMeta; }
    std::size_t size() const noexcept { return data_.size(); }

    std::byte *raw() noexcept { return data_.data(); }
    void failWriteAt(std::size_t writeIndex) noexcept {
        writeCount_ = 0;
        failWriteAt_ = writeIndex;
    }
    void failFlush(bool enabled) noexcept { failFlush_ = enabled; }
    std::byte *slot(std::uint64_t sequence, std::uint64_t journalStart,
                    std::uint64_t journalBlocks) noexcept {
        return data_.data() + (journalStart + (sequence % journalBlocks)) * kMeta;
    }

    uuse::Result<void> read(std::uint64_t offsetBlocks,
                           std::span<std::byte> buffer) override {
        const std::uint64_t bytes = offsetBlocks * kMeta;
        if (bytes + buffer.size() > data_.size()) {
            return fail();
        }
        std::memcpy(buffer.data(), data_.data() + bytes, buffer.size());
        return uuse::Result<void>(uuse::Status::Ok);
    }

    uuse::Result<void> write(std::uint64_t offsetBlocks,
                            std::span<const std::byte> buffer) override {
        if (writeCount_++ == failWriteAt_) {
            return uuse::makeError(uuse::Status::InvalidArgument,
                                  "injected journal write failure");
        }
        const std::uint64_t bytes = offsetBlocks * kMeta;
        if (bytes + buffer.size() > data_.size()) {
            return fail();
        }
        std::memcpy(data_.data() + bytes, buffer.data(), buffer.size());
        return uuse::Result<void>(uuse::Status::Ok);
    }

    uuse::Result<void> flush() override {
        if (failFlush_) {
            return uuse::makeError(uuse::Status::IoError, "injected journal flush failure");
        }
        return uuse::Result<void>(uuse::Status::Ok);
    }

private:
    uuse::Result<void> fail() {
        return uuse::makeError(uuse::Status::InvalidArgument, "memory device out of range");
    }
    std::vector<std::byte> data_;
    std::size_t writeCount_ = 0;
    std::size_t failWriteAt_ = std::numeric_limits<std::size_t>::max();
    bool failFlush_ = false;
};

class MemoryTarget final : public uuse::JournalTarget {
public:
    uuse::Result<void> apply(const uuse::JournalRecord &record) override {
        if (applied >= failAfter) {
            return uuse::makeError(uuse::Status::Corrupt, "injected target failure");
        }
        if (record.type == uuse::JournalRecordType::SetSize) {
            values[record.objectId] = record.arg0;
        } else if (record.type == uuse::JournalRecordType::Create) {
            values[record.objectId] = 1;
        }
        ++applied;
        return ok();
    }

    uuse::Result<void> saveSnapshot(std::uint64_t txid,
                                   std::vector<std::byte> &out) override {
        txid_ = txid;
        out.clear();
        for (const auto &[key, value] : values) {
            appendU64(out, key);
            appendU64(out, value);
        }
        return ok();
    }

    uuse::Result<void> loadSnapshot(std::span<const std::byte> data) override {
        values.clear();
        for (std::size_t i = 0; i + 16 <= data.size(); i += 16) {
            std::uint64_t key = 0;
            std::uint64_t value = 0;
            std::memcpy(&key, data.data() + i, sizeof(key));
            std::memcpy(&value, data.data() + i + 8, sizeof(value));
            values[key] = value;
        }
        return ok();
    }

    std::uint64_t snapshotTxid() const noexcept override { return txid_; }

    std::map<std::uint64_t, std::uint64_t> values;
    std::uint64_t applied = 0;
    std::uint64_t failAfter = std::numeric_limits<std::uint64_t>::max();

private:
    static uuse::Result<void> ok() { return uuse::Result<void>(uuse::Status::Ok); }
    static void appendU64(std::vector<std::byte> &out, std::uint64_t value) {
        const auto *raw = reinterpret_cast<const std::byte *>(&value);
        out.insert(out.end(), raw, raw + sizeof(value));
    }
    std::uint64_t txid_ = 0;
};

struct Fixture {
    std::unique_ptr<MemoryDevice> device;
    uuse::DeviceGeometry geometry;

    Fixture() : device(std::make_unique<MemoryDevice>(kDeviceBytes)) {
        auto computed = uuse::computeGeometry(kDeviceBytes, 512, kMeta, 1, 1, 1);
        if (computed.ok()) {
            geometry = computed.value();
        }
    }

    bool valid() const { return geometry.deviceBlocks != 0; }

    int format() {
        uuse::UuseSuperblock superblock{};
        superblock.magic = uuse::kSuperblockMagic;
        superblock.formatVersion = uuse::kSuperblockFormatVersion;
        superblock.deviceId = 0;
        superblock.replicaId = 0;
        superblock.replicaCount = 1;
        for (std::size_t i = 0; i < superblock.uuid.size(); ++i) {
            superblock.uuid[i] = static_cast<std::uint8_t>(i + 1);
        }
        superblock.generation = 1;
        superblock.deviceBlocks = geometry.deviceBlocks;
        superblock.sectorSize = 512;
        superblock.metadataBlockSize = kMeta;
        superblock.journalStart = geometry.journalStart;
        superblock.journalBlocks = geometry.journalBlocks;
        superblock.checkpointAStart = geometry.checkpointAStart;
        superblock.checkpointBStart = geometry.checkpointBStart;
        superblock.checkpointBlocks = geometry.checkpointBlocks;
        superblock.dataStart = geometry.dataStart;
        superblock.dataBlocks = geometry.dataBlocks;
        uuse::superblockFinalize(superblock);
        std::vector<std::byte> record(kMeta, std::byte{0});
        std::memcpy(record.data(), &superblock, sizeof(superblock));
        if (!device->write(0, record).ok()) {
            return 1;
        }
        if (!device->write(uuse::kSuperblockSlotOffsetBytes / kMeta, record).ok()) {
            return 1;
        }
        return 0;
    }

    std::unique_ptr<uuse::Journal> open(uuse::JournalTarget *target) {
        auto journal = uuse::Journal::open(*device, geometry.journalStart,
                                          geometry.journalBlocks, geometry.checkpointAStart,
                                          geometry.checkpointBStart,
                                          geometry.checkpointBlocks, 50, 80, target);
        return journal.ok() ? std::move(journal).value() : nullptr;
    }
};

uuse::JournalRecord setSize(std::uint64_t objectId, std::uint64_t size) {
    uuse::JournalRecord record;
    record.type = uuse::JournalRecordType::SetSize;
    record.objectId = objectId;
    record.arg0 = size;
    return record;
}

} // namespace

int main() {
    /* 0. checkpoint generation selects the newest slot when txid/sequence tie. */
    {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);
        MemoryTarget writer;
        auto journal = fixture.open(&writer);
        UUSE_TEST_CHECK(journal != nullptr);
        writer.values[7] = 100;
        UUSE_TEST_CHECK(journal->checkpoint().ok());
        /* Journal-backed delta: the second checkpoint keeps the first full
         * snapshot as its base and retains the journal for the difference, so
         * the change must be journaled (the mock applies it via append). */
        UUSE_TEST_CHECK(journal->append({setSize(7, 200)}, true).ok());
        UUSE_TEST_CHECK(journal->checkpoint().ok());
        MemoryTarget reader;
        auto reopened = fixture.open(&reader);
        UUSE_TEST_CHECK(reopened != nullptr);
        UUSE_TEST_CHECK(reopened->replay().ok());
        UUSE_TEST_CHECK(reader.values[7] == 200);
    }

    /* 0. Multiple metadata requests share one journal transaction and flush. */
    {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);
        MemoryTarget target;
        auto journal = fixture.open(&target);
        UUSE_TEST_CHECK(journal != nullptr);
        uuse::metadata::GroupCommitCoordinator coordinator(*journal);
        UUSE_TEST_CHECK(coordinator.enqueue({setSize(1, 100)}, 1).ok());
        UUSE_TEST_CHECK(coordinator.enqueue({setSize(1, 200)}, 2).ok());
        UUSE_TEST_CHECK(coordinator.sync(2).ok());
        UUSE_TEST_CHECK(coordinator.durableEpoch() == 2);
        UUSE_TEST_CHECK(coordinator.stats().transactions == 1);
        UUSE_TEST_CHECK(coordinator.stats().flushes == 1);
        UUSE_TEST_CHECK(coordinator.stats().lastBatchRequests == 2);
        UUSE_TEST_CHECK(coordinator.stats().lastBatchRecords == 2);
        UUSE_TEST_CHECK(target.values[1] == 200);
    }

    /* 1. append, replay into a fresh target, checkpoint resume */
    {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);

        MemoryTarget writer;
        auto journal = fixture.open(&writer);
        UUSE_TEST_CHECK(journal != nullptr);
        UUSE_TEST_CHECK(journal->stats().tail == 0);

        for (std::uint64_t i = 1; i <= 4; ++i) {
            auto appended = journal->append({setSize(7, i * 100)}, true);
            UUSE_TEST_CHECK(appended.ok());
            UUSE_TEST_CHECK(appended.value() == i);
        }
        UUSE_TEST_CHECK(journal->stats().lastTxid == 4);

        /* checkpoint at txid 4, then two more transactions */
        UUSE_TEST_CHECK(journal->checkpoint().ok());
        UUSE_TEST_CHECK(journal->stats().lastCheckpointTxid == 4);
        UUSE_TEST_CHECK(journal->stats().checkpointAValid);
        UUSE_TEST_CHECK(!journal->stats().checkpointBValid);
        UUSE_TEST_CHECK(journal->checkpoint().ok());
        UUSE_TEST_CHECK(journal->stats().checkpointAValid);
        UUSE_TEST_CHECK(journal->stats().checkpointBValid);
        for (std::uint64_t i = 5; i <= 6; ++i) {
            UUSE_TEST_CHECK(journal->append({setSize(7, i * 100)}, true).ok());
        }
        UUSE_TEST_CHECK(journal->flush().ok());

        MemoryTarget reader;
        auto reopened = fixture.open(&reader);
        UUSE_TEST_CHECK(reopened != nullptr);
        UUSE_TEST_CHECK(reopened->replay().ok());
        UUSE_TEST_CHECK(reader.values[7] == 600);
        UUSE_TEST_CHECK(reopened->stats().lastTxid == 6);
        UUSE_TEST_CHECK(reopened->stats().validTransactions == 2);
        UUSE_TEST_CHECK(reopened->stats().corruption.empty());
        UUSE_TEST_CHECK(reopened->stats().lastCheckpointTxid == 4);

        /* torn last commit: zeroed slot is a clean incomplete tail */
        std::memset(fixture.device->slot(17, fixture.geometry.journalStart,
                                         fixture.geometry.journalBlocks),
                    0, kMeta);
        MemoryTarget torn;
        auto tornJournal = fixture.open(&torn);
        UUSE_TEST_CHECK(tornJournal != nullptr);
        UUSE_TEST_CHECK(tornJournal->replay().ok());
        UUSE_TEST_CHECK(tornJournal->stats().lastTxid == 5);
        UUSE_TEST_CHECK(tornJournal->stats().corruption.empty());

        /* garbage in a mid-ring commit slot is reported as corruption */
        std::memset(fixture.device->slot(14, fixture.geometry.journalStart,
                                         fixture.geometry.journalBlocks),
                    0xAB, kMeta);
        MemoryTarget corrupt;
        auto corruptJournal = fixture.open(&corrupt);
        UUSE_TEST_CHECK(corruptJournal != nullptr);
        UUSE_TEST_CHECK(corruptJournal->replay().status() == uuse::Status::Corrupt);
        UUSE_TEST_CHECK(corruptJournal->stats().lastTxid == 4);
        UUSE_TEST_CHECK(!corruptJournal->stats().corruption.empty());
        UUSE_TEST_CHECK(corrupt.values[7] == 400);
    }

    /* 2. A target failure rolls back the whole in-memory after-image. */
    {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);
        MemoryTarget target;
        target.failAfter = 1;
        auto journal = fixture.open(&target);
        UUSE_TEST_CHECK(journal != nullptr);
        auto appended = journal->append({setSize(11, 100), setSize(11, 200)}, true);
        UUSE_TEST_CHECK(appended.status() == uuse::Status::Corrupt);
        UUSE_TEST_CHECK(target.values.empty());
    }

    /* 3. force threshold triggers an automatic checkpoint instead of NoSpace */
    {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);
        MemoryTarget target;
        auto journal = fixture.open(&target);
        UUSE_TEST_CHECK(journal != nullptr);
        for (std::uint64_t i = 1; i <= 100; ++i) {
            auto appended = journal->append({setSize(9, i)}, true);
            UUSE_TEST_CHECK(appended.ok());
        }
        UUSE_TEST_CHECK(journal->stats().lastCheckpointTxid > 0);
        UUSE_TEST_CHECK(journal->usageRatio() < 0.8);

        MemoryTarget reader;
        auto reopened = fixture.open(&reader);
        UUSE_TEST_CHECK(reopened != nullptr);
        UUSE_TEST_CHECK(reopened->replay().ok());
        UUSE_TEST_CHECK(reader.values[9] == 100);
        UUSE_TEST_CHECK(reopened->stats().lastTxid == 100);
        UUSE_TEST_CHECK(reopened->stats().corruption.empty());
    }

    /* 4. orphan record at the ring tail is corruption evidence */
    {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);
        MemoryTarget target;
        auto journal = fixture.open(&target);
        UUSE_TEST_CHECK(journal != nullptr);
        UUSE_TEST_CHECK(journal->append({setSize(3, 42)}, true).ok());
        /* Flip a byte inside the BEGIN slot header that the slot CRC covers
         * (txid field), so the slot is non-empty but structurally invalid. */
        auto *begin = fixture.device->slot(0, fixture.geometry.journalStart,
                                           fixture.geometry.journalBlocks);
        begin[16] ^= std::byte{0xFF};
        MemoryTarget reader;
        auto reopened = fixture.open(&reader);
        UUSE_TEST_CHECK(reopened != nullptr);
        UUSE_TEST_CHECK(reopened->replay().status() == uuse::Status::Corrupt);
        UUSE_TEST_CHECK(reopened->stats().lastTxid == 0);
        UUSE_TEST_CHECK(!reopened->stats().corruption.empty());
    }

    /* 5. A failed transaction write leaves an ignorable tail. BEGIN, records
     * and COMMIT are one batched write, so a single injected failure covers it. */
    {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);
        MemoryTarget writer;
        auto journal = fixture.open(&writer);
        UUSE_TEST_CHECK(journal != nullptr);
        fixture.device->failWriteAt(0);
        UUSE_TEST_CHECK(!journal->append({setSize(21, 42)}, true).ok());
        fixture.device->failWriteAt(std::numeric_limits<std::size_t>::max());
        MemoryTarget reader;
        auto reopened = fixture.open(&reader);
        UUSE_TEST_CHECK(reopened != nullptr);
        UUSE_TEST_CHECK(reopened->replay().ok());
        UUSE_TEST_CHECK(reader.values.empty());
        UUSE_TEST_CHECK(reopened->stats().lastTxid == 0);
        UUSE_TEST_CHECK(reopened->stats().corruption.empty());
    }

    /* 5b. A batched transaction whose COMMIT slot never landed (torn write) is
     * an ignorable tail: replay stops before the transaction is published. */
    {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);
        MemoryTarget writer;
        auto journal = fixture.open(&writer);
        UUSE_TEST_CHECK(journal != nullptr);
        UUSE_TEST_CHECK(journal->append({setSize(21, 42)}, true).ok());
        std::memset(fixture.device->slot(2, fixture.geometry.journalStart,
                                         fixture.geometry.journalBlocks),
                    0, fixture.device->blockSize());
        MemoryTarget reader;
        auto reopened = fixture.open(&reader);
        UUSE_TEST_CHECK(reopened != nullptr);
        UUSE_TEST_CHECK(reopened->replay().ok());
        UUSE_TEST_CHECK(reader.values.empty());
        UUSE_TEST_CHECK(reopened->stats().lastTxid == 0);
        UUSE_TEST_CHECK(reopened->stats().corruption.empty());
    }

    /* 6. A COMMIT write followed by flush failure is not published in memory. */
    {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);
        MemoryTarget target;
        auto journal = fixture.open(&target);
        UUSE_TEST_CHECK(journal != nullptr);
        fixture.device->failFlush(true);
        auto appended = journal->append({setSize(22, 84)}, true);
        UUSE_TEST_CHECK(appended.status() == uuse::Status::IoError);
        UUSE_TEST_CHECK(target.values.empty());
    }

    /* 7. Checkpoint slot and superblock publication failures are retryable. */
    for (std::size_t failAt : {0U, 1U}) {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);
        MemoryTarget target;
        auto journal = fixture.open(&target);
        UUSE_TEST_CHECK(journal != nullptr);
        UUSE_TEST_CHECK(journal->append({setSize(31, 99)}, true).ok());
        fixture.device->failWriteAt(failAt);
        UUSE_TEST_CHECK(!journal->checkpoint().ok());
        UUSE_TEST_CHECK(target.values[31] == 99);
        fixture.device->failWriteAt(std::numeric_limits<std::size_t>::max());
        UUSE_TEST_CHECK(journal->checkpoint().ok());
        MemoryTarget reader;
        auto reopened = fixture.open(&reader);
        UUSE_TEST_CHECK(reopened != nullptr);
        UUSE_TEST_CHECK(reopened->replay().ok());
        UUSE_TEST_CHECK(reader.values[31] == 99);
    }

    /* 8. A checkpoint flush failure does not mutate the target. */
    {
        Fixture fixture;
        UUSE_TEST_CHECK(fixture.valid());
        UUSE_TEST_CHECK(fixture.format() == 0);
        MemoryTarget target;
        auto journal = fixture.open(&target);
        UUSE_TEST_CHECK(journal != nullptr);
        UUSE_TEST_CHECK(journal->append({setSize(32, 101)}, true).ok());
        fixture.device->failFlush(true);
        UUSE_TEST_CHECK(!journal->checkpoint().ok());
        UUSE_TEST_CHECK(target.values[32] == 101);
        fixture.device->failFlush(false);
        UUSE_TEST_CHECK(journal->checkpoint().ok());
    }

    std::printf("journal_test: ok\n");
    return 0;
}
