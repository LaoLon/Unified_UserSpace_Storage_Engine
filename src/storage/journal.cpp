#include "hfs/journal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "common/log.hpp"
#include "common/stats.hpp"
#include "common/test_hooks.hpp"
#include "hfs/core.hpp"
#include "hfs/crc.hpp"
#include "hfs/superblock.hpp"
#include "runtime/spdk_runtime.hpp"

namespace hfs {

namespace {

/* Nanoseconds elapsed since a phase start, for barrier diagnostics. */
std::uint64_t elapsedNs(std::chrono::steady_clock::time_point start) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start)
            .count());
}

constexpr std::uint64_t kJournalSlotMagic = 0x484653324a524e4cULL; /* "HFS2JRNL" */
constexpr std::uint32_t kJournalFormatVersion = 1;
constexpr std::uint64_t kCheckpointMagic = 0x4846533243505431ULL; /* "HFS2CPT1" */
constexpr std::uint32_t kCheckpointFormatVersion = 1;
constexpr std::size_t kControlIoLimit = 1U << 20;

/* One journal slot; every slot is a metadata block. */
struct SlotHeader {
    std::uint64_t magic;
    std::uint32_t formatVersion;
    std::uint16_t type;
    std::uint16_t reserved0;
    std::uint64_t txid;
    std::uint64_t sequence;
    std::array<std::uint8_t, 16> uuid; /* volume uuid; stale generations differ */
    std::uint32_t blockSize;
    std::uint32_t payloadSize;
    std::uint32_t recordCount; /* BEGIN only */
    std::uint64_t txCrc;       /* COMMIT only */
    std::uint32_t crc;         /* over bytes before this field + payload */
};

/* Fixed-size record payload carried by RECORD slots. */
struct RecordPayload {
    std::uint16_t type;
    std::uint16_t reserved0;
    std::uint32_t reserved1;
    std::uint64_t objectId;
    std::uint64_t arg0;
    std::uint64_t arg1;
};

/* Checkpoint slot header; payload follows and spans blocks. */
struct CheckpointHeader {
    std::uint64_t magic;
    std::uint32_t formatVersion;
    std::uint32_t payloadSize;
    std::uint64_t txid;
    std::uint64_t sequence; /* ring head at checkpoint time */
    std::uint64_t generation;
    std::array<std::uint8_t, 16> uuid;
    std::uint32_t crc; /* over bytes before this field + payload */
    std::uint32_t reserved;
};

std::uint64_t bytesToBlocks(std::uint64_t bytes, std::uint64_t blockSize) {
    return (bytes + blockSize - 1) / blockSize;
}

/* Test-only checkpoint barrier fault; production behavior is unchanged unless
 * the caller explicitly requests this environment-variable injection. */
bool testCheckpointFlushFailureRequested() {
    const char *value = std::getenv("HFS_TEST_FAIL_CHECKPOINT_FLUSH");
    return value != nullptr && std::string(value) == "1";
}

bool slotHeaderValid(const SlotHeader &header, std::uint32_t blockSize,
                     std::span<const std::byte> slot) {
    if (header.magic != kJournalSlotMagic ||
        header.formatVersion != kJournalFormatVersion) {
        return false;
    }
    if (header.blockSize != blockSize || header.payloadSize > blockSize ||
        sizeof(SlotHeader) + header.payloadSize > blockSize) {
        return false;
    }
    const std::uint32_t expected =
        crc32c(slot.data(), offsetof(SlotHeader, crc));
    std::uint32_t expectedWithPayload = crc32c(
        slot.data() + sizeof(SlotHeader), header.payloadSize, expected);
    return header.crc == expectedWithPayload;
}

bool slotIsEmpty(std::span<const std::byte> slot) {
    for (const auto byte : slot) {
        if (byte != std::byte{0}) {
            return false;
        }
    }
    return true;
}

/* Classification of a ring slot during replay. */
enum class SlotState { Valid, Empty, Stale, Invalid };

} // namespace

struct Journal::Impl {
    std::unique_ptr<JournalDevice> ownedDevice;
    JournalDevice *device = nullptr;
    JournalTarget *target = nullptr;

    std::uint64_t journalStart = 0;
    std::uint64_t journalBlocks = 0;
    std::uint64_t checkpointAStart = 0;
    std::uint64_t checkpointBStart = 0;
    std::uint64_t checkpointBlocks = 0;
    std::uint32_t triggerPercent = 50;
    std::uint32_t forcePercent = 80;
    std::uint32_t blockSize = 0;

    Hfs2Superblock superblock{};
    std::uint32_t sbSlot = 0; /* slot holding the newest superblock */
    bool sbValid = false;

    std::uint64_t head = 0;
    std::uint64_t tail = 0;
    std::uint64_t lastTxid = 0;
    std::uint32_t validTransactions = 0;
    bool checkpointAValid = false;
    bool checkpointBValid = false;
    std::uint64_t checkpointATxid = 0;
    std::uint64_t checkpointBTxid = 0;
    std::uint64_t checkpointASequence = 0;
    std::uint64_t checkpointBSequence = 0;
    std::uint64_t checkpointAGeneration = 0;
    std::uint64_t checkpointBGeneration = 0;
    std::string corruption;
    Stats statsCache;

    Result<void> readBlock(std::uint64_t offsetBlocks, std::span<std::byte> buffer) {
        return device->read(offsetBlocks, buffer);
    }

    Result<void> writeBlock(std::uint64_t offsetBlocks, std::span<const std::byte> buffer) {
        return device->write(offsetBlocks, buffer);
    }

    Result<void> readSlot(std::uint64_t sequence, std::vector<std::byte> &slot,
                          SlotHeader *header) {
        slot.assign(blockSize, std::byte{0});
        const std::uint64_t offset = journalStart + (sequence % journalBlocks);
        auto read = readBlock(offset, slot);
        if (!read.ok()) {
            return read;
        }
        SlotHeader candidate{};
        std::memcpy(&candidate, slot.data(), sizeof(candidate));
        if (!slotHeaderValid(candidate, blockSize, slot)) {
            return makeError(Status::InvalidArgument, "invalid journal slot");
        }
        *header = candidate;
        return Result<void>(Status::Ok);
    }

    /* Classify a slot: valid, never written, stale (previous format/wrap) or
     * corrupt. Stale content ends the ring scan cleanly. */
    SlotState readSlotState(std::uint64_t sequence, std::vector<std::byte> &slot,
                            SlotHeader *header, Result<void> *error) {
        slot.assign(blockSize, std::byte{0});
        const std::uint64_t offset = journalStart + (sequence % journalBlocks);
        auto read = readBlock(offset, slot);
        if (!read.ok()) {
            *error = read;
            return SlotState::Invalid;
        }
        if (slotIsEmpty(slot)) {
            return SlotState::Empty;
        }
        SlotHeader candidate{};
        std::memcpy(&candidate, slot.data(), sizeof(candidate));
        if (candidate.magic != kJournalSlotMagic ||
            candidate.formatVersion != kJournalFormatVersion ||
            candidate.uuid != superblock.uuid) {
            return SlotState::Stale;
        }
        if (!slotHeaderValid(candidate, blockSize, slot)) {
            return SlotState::Invalid;
        }
        *header = candidate;
        return SlotState::Valid;
    }

    Result<void> writeSlot(std::uint64_t sequence, std::uint16_t type, std::uint64_t txid,
                           std::uint32_t recordCount, std::uint64_t txCrc,
                           std::span<const std::byte> payload) {
        std::vector<std::byte> slot(blockSize, std::byte{0});
        SlotHeader header{};
        header.magic = kJournalSlotMagic;
        header.formatVersion = kJournalFormatVersion;
        header.type = type;
        header.txid = txid;
        header.sequence = sequence;
        header.uuid = superblock.uuid;
        header.blockSize = blockSize;
        header.payloadSize = static_cast<std::uint32_t>(payload.size());
        header.recordCount = recordCount;
        header.txCrc = txCrc;
        std::memcpy(slot.data(), &header, sizeof(header));
        if (!payload.empty()) {
            std::memcpy(slot.data() + sizeof(header), payload.data(), payload.size());
        }
        auto *headerView = reinterpret_cast<SlotHeader *>(slot.data());
        headerView->crc = crc32c(slot.data() + sizeof(header), payload.size(),
                                 crc32c(slot.data(), offsetof(SlotHeader, crc)));
        const std::uint64_t offset = journalStart + (sequence % journalBlocks);
        return writeBlock(offset, slot);
    }

    Result<void> readSuperblocks() {
        std::vector<std::byte> record(kSuperblockRecordSize);
        Hfs2Superblock slotA{};
        Hfs2Superblock slotB{};
        auto readA = readBlock(0, record);
        if (!readA.ok()) {
            return readA;
        }
        std::memcpy(&slotA, record.data(), sizeof(slotA));
        const std::uint64_t slotBBlock = kSuperblockSlotOffsetBytes / blockSize;
        auto readB = readBlock(slotBBlock, record);
        if (!readB.ok()) {
            return readB;
        }
        std::memcpy(&slotB, record.data(), sizeof(slotB));

        const bool validA = superblockValid(slotA);
        const bool validB = superblockValid(slotB);
        if (!validA && !validB) {
            return makeError(Status::NotFound, "no valid HFS 2.0 superblock on device");
        }
        if (validA && validB && slotB.generation > slotA.generation) {
            superblock = slotB;
            sbSlot = 1;
        } else {
            superblock = validA ? slotA : slotB;
            sbSlot = validA ? 0 : 1;
        }
        sbValid = true;
        return Result<void>(Status::Ok);
    }

    /* One checkpoint record ready to be written. */
    struct CheckpointRecord {
        std::uint64_t offsetBlocks = 0;
        std::vector<std::byte> record;
        bool writeA = false;
        std::uint64_t txid = 0;
        std::uint64_t sequence = 0;
        std::uint64_t generation = 0;
    };

    /* One superblock record ready to be written. */
    struct SuperblockRecord {
        std::uint64_t offsetBlocks = 0;
        std::vector<std::byte> record;
        std::uint32_t slot = 0;
    };

    Result<SuperblockRecord> buildSuperblockRecord() {
        superblock.generation += 1;
        superblock.checkpointGeneration = lastTxid;
        superblock.journalHead = head;
        superblock.journalTail = tail;
        superblockFinalize(superblock);
        SuperblockRecord built;
        built.record.assign(kSuperblockRecordSize, std::byte{0});
        std::memcpy(built.record.data(), &superblock, sizeof(superblock));
        const std::uint32_t targetSlot = 1 - sbSlot;
        built.offsetBlocks = targetSlot == 0 ? 0 : kSuperblockSlotOffsetBytes / blockSize;
        built.slot = targetSlot;
        return built;
    }

    Result<void> readCheckpoint(std::uint64_t slotStart, bool *valid, std::uint64_t *txid,
                                std::uint64_t *sequence, std::uint64_t *generation,
                                std::vector<std::byte> *payload) {
        const std::uint64_t slotBlocks = checkpointBlocks / 2;
        std::vector<std::byte> first(blockSize, std::byte{0});
        auto read = readBlock(slotStart, first);
        if (!read.ok()) {
            return read;
        }
        CheckpointHeader header{};
        std::memcpy(&header, first.data(), sizeof(header));
        *valid = false;
        if (header.magic != kCheckpointMagic ||
            header.formatVersion != kCheckpointFormatVersion ||
            header.uuid != superblock.uuid) {
            return Result<void>(Status::Ok);
        }
        const std::uint64_t totalBytes = sizeof(CheckpointHeader) + header.payloadSize;
        if (bytesToBlocks(totalBytes, blockSize) > slotBlocks) {
            return Result<void>(Status::Ok);
        }
        std::vector<std::byte> record(totalBytes);
        const std::uint64_t recordBlocks = bytesToBlocks(totalBytes, blockSize);
        // Read the full record block-aligned; the header was already read.
        std::vector<std::byte> raw(recordBlocks * blockSize, std::byte{0});
        auto rawRead = readBlock(slotStart, raw);
        if (!rawRead.ok()) {
            return rawRead;
        }
        std::memcpy(record.data(), raw.data(), totalBytes);
        const std::uint32_t expected =
            crc32c(record.data(), offsetof(CheckpointHeader, crc));
        const std::uint32_t expectedWithPayload =
            crc32c(record.data() + sizeof(CheckpointHeader), header.payloadSize, expected);
        if (header.crc != expectedWithPayload) {
            return Result<void>(Status::Ok);
        }
        *valid = true;
        *txid = header.txid;
        *sequence = header.sequence;
        *generation = header.generation;
        payload->assign(record.begin() + sizeof(CheckpointHeader), record.end());
        return Result<void>(Status::Ok);
    }

    Result<CheckpointRecord> buildCheckpointRecord(std::span<const std::byte> snapshot) {
        const std::uint64_t slotBlocks = checkpointBlocks / 2;
        const std::uint64_t totalBytes = sizeof(CheckpointHeader) + snapshot.size();
        if (bytesToBlocks(totalBytes, blockSize) > slotBlocks) {
            return makeError<CheckpointRecord>(Status::NoSpace,
                                               "checkpoint snapshot does not fit the slot");
        }
        CheckpointRecord built;
        built.record.assign(bytesToBlocks(totalBytes, blockSize) * blockSize, std::byte{0});
        CheckpointHeader header{};
        header.magic = kCheckpointMagic;
        header.formatVersion = kCheckpointFormatVersion;
        header.payloadSize = static_cast<std::uint32_t>(snapshot.size());
        header.txid = lastTxid;
        header.sequence = head;
        header.generation = superblock.generation;
        header.uuid = superblock.uuid;
        std::memcpy(built.record.data(), &header, sizeof(header));
        std::memcpy(built.record.data() + sizeof(header), snapshot.data(), snapshot.size());
        auto *headerView = reinterpret_cast<CheckpointHeader *>(built.record.data());
        headerView->crc = crc32c(built.record.data() + sizeof(CheckpointHeader), snapshot.size(),
                                 crc32c(built.record.data(), offsetof(CheckpointHeader, crc)));

        /* Rotate to the inactive slot; once both are valid, replace the older
         * sequence so one complete checkpoint remains available on failure. */
        built.writeA = !checkpointAValid ||
                       (checkpointBValid && checkpointASequence < checkpointBSequence);
        built.offsetBlocks = built.writeA ? checkpointAStart : checkpointBStart;
        built.txid = lastTxid;
        built.sequence = head;
        built.generation = superblock.generation;
        return built;
    }

    /* Writes the checkpoint record and the superblock as one batch, so the
     * device overlaps both round trips, then applies the in-memory state. */
    Result<void> writeCheckpointAndSuperblock(const CheckpointRecord &checkpoint,
                                              const SuperblockRecord &superblockRecord,
                                              bool deferFlush) {
        JournalDevice::WriteRange ranges[2] = {
            {checkpoint.offsetBlocks, checkpoint.record},
            {superblockRecord.offsetBlocks, superblockRecord.record},
        };
        const auto writeStart = std::chrono::steady_clock::now();
        auto write = device->writeBatch(ranges);
        stats::counters().syncCheckpointWrite.record(elapsedNs(writeStart));
        if (!write.ok()) {
            return write;
        }
        test_hooks::crashIfRequested("checkpoint_flush");
        if (testCheckpointFlushFailureRequested()) {
            return makeError(Status::IoError, "test-injected checkpoint flush failure");
        }
        if (!deferFlush) {
            const auto flushStart = std::chrono::steady_clock::now();
            auto flush = device->flush();
            stats::counters().syncCheckpointFlush.record(elapsedNs(flushStart));
            if (!flush.ok()) {
                return flush;
            }
        }
        if (checkpoint.writeA) {
            checkpointAValid = true;
            checkpointATxid = checkpoint.txid;
            checkpointASequence = checkpoint.sequence;
            checkpointAGeneration = checkpoint.generation;
        } else {
            checkpointBValid = true;
            checkpointBTxid = checkpoint.txid;
            checkpointBSequence = checkpoint.sequence;
            checkpointBGeneration = checkpoint.generation;
        }
        sbSlot = superblockRecord.slot;
        log::info("checkpoint written: slot=%c txid=%llu sequence=%llu bytes=%zu",
                  checkpoint.writeA ? 'A' : 'B',
                  static_cast<unsigned long long>(checkpoint.txid),
                  static_cast<unsigned long long>(checkpoint.sequence),
                  checkpoint.record.size());
        return Result<void>(Status::Ok);
    }

    void refreshStats() {
        statsCache.head = head;
        statsCache.tail = tail;
        statsCache.lastTxid = lastTxid;
        statsCache.lastCheckpointTxid =
            std::max(checkpointAValid ? checkpointATxid : 0,
                     checkpointBValid ? checkpointBTxid : 0);
        statsCache.checkpointATxid = checkpointATxid;
        statsCache.checkpointBTxid = checkpointBTxid;
        statsCache.checkpointASequence = checkpointASequence;
        statsCache.checkpointBSequence = checkpointBSequence;
        statsCache.checkpointAValid = checkpointAValid;
        statsCache.checkpointBValid = checkpointBValid;
        statsCache.validTransactions = validTransactions;
        statsCache.corruption = corruption;
    }
};

Journal::Journal() : impl_(std::make_unique<Impl>()) {}
Journal::~Journal() = default;

Result<std::unique_ptr<Journal>> Journal::create(std::unique_ptr<JournalDevice> ownedDevice,
                                                 JournalDevice *device,
                                                 std::uint64_t journalStart,
                                                 std::uint64_t journalBlocks,
                                                 std::uint64_t checkpointAStart,
                                                 std::uint64_t checkpointBStart,
                                                 std::uint64_t checkpointBlocks,
                                                 std::uint32_t triggerPercent,
                                                 std::uint32_t forcePercent,
                                                 JournalTarget *target) {
    if (device == nullptr || journalBlocks < 4 || checkpointBlocks < 2) {
        return makeError<std::unique_ptr<Journal>>(Status::InvalidArgument,
                                                   "invalid journal geometry");
    }
    auto journal = std::unique_ptr<Journal>(new Journal());
    auto &impl = *journal->impl_;
    impl.ownedDevice = std::move(ownedDevice);
    impl.device = device;
    impl.target = target;
    impl.journalStart = journalStart;
    impl.journalBlocks = journalBlocks;
    impl.checkpointAStart = checkpointAStart;
    impl.checkpointBStart = checkpointBStart;
    impl.checkpointBlocks = checkpointBlocks;
    impl.triggerPercent = triggerPercent == 0 ? 50 : triggerPercent;
    impl.forcePercent = forcePercent == 0 ? 80 : forcePercent;
    impl.blockSize = device->blockSize();
    if (impl.blockSize < 512 || (impl.blockSize & (impl.blockSize - 1)) != 0) {
        return makeError<std::unique_ptr<Journal>>(Status::InvalidArgument,
                                                   "invalid metadata block size");
    }

    auto superblocks = impl.readSuperblocks();
    if (!superblocks.ok()) {
        return makeError<std::unique_ptr<Journal>>(superblocks.status(), superblocks.message());
    }
    impl.head = impl.superblock.journalHead;
    impl.tail = impl.superblock.journalTail;
    impl.lastTxid = 0;

    /* Newest valid checkpoint wins; its sequence resumes the ring scan. */
    std::vector<std::byte> payloadA;
    std::vector<std::byte> payloadB;
    std::uint64_t sequenceA = 0;
    std::uint64_t sequenceB = 0;
    std::uint64_t txidA = 0;
    std::uint64_t txidB = 0;
    std::uint64_t generationA = 0;
    std::uint64_t generationB = 0;
    auto readA = impl.readCheckpoint(impl.checkpointAStart, &impl.checkpointAValid, &txidA,
                                     &sequenceA, &generationA, &payloadA);
    if (!readA.ok()) {
        return makeError<std::unique_ptr<Journal>>(readA.status(), readA.message());
    }
    auto readB = impl.readCheckpoint(impl.checkpointBStart, &impl.checkpointBValid, &txidB,
                                     &sequenceB, &generationB, &payloadB);
    if (!readB.ok()) {
        return makeError<std::unique_ptr<Journal>>(readB.status(), readB.message());
    }
    impl.checkpointATxid = txidA;
    impl.checkpointBTxid = txidB;
    impl.checkpointASequence = sequenceA;
    impl.checkpointBSequence = sequenceB;
    impl.checkpointAGeneration = generationA;
    impl.checkpointBGeneration = generationB;

    const bool useA = impl.checkpointAValid &&
                      (!impl.checkpointBValid ||
                       std::tuple{impl.checkpointAGeneration, impl.checkpointATxid,
                                  impl.checkpointASequence} >=
                           std::tuple{impl.checkpointBGeneration, impl.checkpointBTxid,
                                      impl.checkpointBSequence});
    const bool useB = impl.checkpointBValid && !useA;
    const std::uint64_t checkpointSequence = useA ? sequenceA : sequenceB;
    const std::uint64_t checkpointTxid = useA ? impl.checkpointATxid : impl.checkpointBTxid;
    const std::vector<std::byte> &payload = useA ? payloadA : payloadB;
    if ((useA || useB) && checkpointSequence >= impl.tail) {
        impl.tail = checkpointSequence;
        impl.lastTxid = checkpointTxid;
        if (impl.target != nullptr) {
            auto loaded = impl.target->loadSnapshot(payload);
            if (!loaded.ok()) {
                return makeError<std::unique_ptr<Journal>>(loaded.status(),
                                                           "checkpoint snapshot rejected: " +
                                                               loaded.message());
            }
        }
    }
    if (impl.head < impl.tail) {
        impl.head = impl.tail;
    }
    impl.refreshStats();
    return journal;
}

/* Engine-backed journal device: control-path I/O on the metadata reactor. */
namespace {

class RuntimeJournalDevice final : public JournalDevice {
public:
    RuntimeJournalDevice(SpdkRuntime &runtime, std::size_t index,
                         std::uint32_t metadataBlockSize, std::uint32_t sectorSize)
        : runtime_(runtime),
          index_(index),
          metadataBlockSize_(metadataBlockSize),
          blockFactor_(metadataBlockSize / sectorSize) {}

    std::uint32_t blockSize() const noexcept override { return metadataBlockSize_; }

    Result<void> read(std::uint64_t offsetBlocks, std::span<std::byte> buffer) override {
        return transferRead(offsetBlocks, buffer);
    }

    Result<void> write(std::uint64_t offsetBlocks,
                       std::span<const std::byte> buffer) override {
        return transferWrite(offsetBlocks, buffer);
    }

    Result<void> flush() override { return runtime_.controlFlush(index_); }

    Result<void> writeBatch(std::span<const WriteRange> ranges) override {
        std::vector<SpdkRuntime::ControlWriteRange> batch;
        batch.reserve(ranges.size());
        for (const WriteRange &range : ranges) {
            std::size_t done = 0;
            while (done < range.buffer.size()) {
                const std::size_t bytes = std::min(kControlIoLimit, range.buffer.size() - done);
                const std::uint64_t deviceOffset =
                    (range.offsetBlocks * blockFactor_) +
                    (done / metadataBlockSize_) * blockFactor_;
                batch.push_back({deviceOffset, range.buffer.subspan(done, bytes)});
                done += bytes;
            }
        }
        return runtime_.controlWriteBatch(index_, batch);
    }

private:
    Result<void> transferRead(std::uint64_t offsetBlocks, std::span<std::byte> buffer) {
        std::size_t done = 0;
        while (done < buffer.size()) {
            const std::size_t bytes = std::min(kControlIoLimit, buffer.size() - done);
            const std::uint64_t deviceOffset =
                (offsetBlocks * blockFactor_) + (done / metadataBlockSize_) * blockFactor_;
            auto rc = runtime_.controlRead(index_, deviceOffset, buffer.subspan(done, bytes));
            if (!rc.ok()) {
                return rc;
            }
            done += bytes;
        }
        return Result<void>(Status::Ok);
    }

    Result<void> transferWrite(std::uint64_t offsetBlocks,
                               std::span<const std::byte> buffer) {
        std::size_t done = 0;
        while (done < buffer.size()) {
            const std::size_t bytes = std::min(kControlIoLimit, buffer.size() - done);
            const std::uint64_t deviceOffset =
                (offsetBlocks * blockFactor_) + (done / metadataBlockSize_) * blockFactor_;
            auto rc = runtime_.controlWrite(index_, deviceOffset, buffer.subspan(done, bytes));
            if (!rc.ok()) {
                return rc;
            }
            done += bytes;
        }
        return Result<void>(Status::Ok);
    }

    SpdkRuntime &runtime_;
    std::size_t index_;
    std::uint32_t metadataBlockSize_;
    std::uint32_t blockFactor_;
};

/* Mirrors journal and checkpoint I/O to every configured metadata device. */
class MirroredRuntimeJournalDevice final : public JournalDevice {
public:
    MirroredRuntimeJournalDevice(SpdkRuntime &runtime, const Config &config,
                                 const std::vector<DeviceGeometry> &geometries,
                                 std::size_t primaryIndex)
        : runtime_(runtime), config_(config), geometries_(geometries), primaryIndex_(primaryIndex) {
        for (std::size_t i = 0; i < config_.devices.size(); ++i) {
            if (!config_.devices[i].metadata) continue;
            const std::uint32_t sectorSize = runtime_.deviceBlockSize(i);
            devices_.push_back(std::make_pair(
                i, std::make_unique<RuntimeJournalDevice>(runtime_, i,
                                                           config_.metadataBlockSize,
                                                           sectorSize)));
        }
    }

    std::uint32_t blockSize() const noexcept override { return config_.metadataBlockSize; }

    Result<void> read(std::uint64_t offsetBlocks, std::span<std::byte> buffer) override {
        if (!isMirroredRegion(offsetBlocks)) {
            return devices_.front().second->read(offsetBlocks, buffer);
        }
        std::vector<std::byte> reference(buffer.size());
        auto first = devices_.front().second->read(translate(offsetBlocks, devices_.front().first),
                                                   reference);
        if (!first.ok()) return first;
        for (std::size_t i = 1; i < devices_.size(); ++i) {
            std::vector<std::byte> candidate(buffer.size());
            auto readResult = devices_[i].second->read(translate(offsetBlocks, devices_[i].first),
                                                       candidate);
            if (!readResult.ok()) return readResult;
            if (candidate != reference) {
                const auto mismatch = std::mismatch(reference.begin(), reference.end(),
                                                    candidate.begin());
                const std::size_t byteOffset =
                    static_cast<std::size_t>(mismatch.first - reference.begin());
                return makeError(Status::Corrupt,
                                 "metadata replica bytes diverged at block " +
                                     std::to_string(offsetBlocks) + " byte " +
                                     std::to_string(byteOffset) + " on device " +
                                     std::to_string(devices_[i].first));
            }
        }
        std::copy(reference.begin(), reference.end(), buffer.begin());
        return Result<void>(Status::Ok);
    }

    Result<void> write(std::uint64_t offsetBlocks,
                       std::span<const std::byte> buffer) override {
        for (auto &entry : devices_) {
            std::vector<std::byte> replica(buffer.begin(), buffer.end());
            if (isSuperblock(offsetBlocks) && replica.size() >= sizeof(Hfs2Superblock)) {
                Hfs2Superblock superblock{};
                std::memcpy(&superblock, replica.data(), sizeof(superblock));
                const DeviceGeometry &geometry = geometries_[entry.first];
                superblock.deviceId = static_cast<std::uint32_t>(entry.first);
                superblock.replicaId = geometry.replicaId;
                superblock.deviceBlocks = geometry.deviceBlocks;
                superblock.journalStart = geometry.journalStart;
                superblock.journalBlocks = geometry.journalBlocks;
                superblock.checkpointAStart = geometry.checkpointAStart;
                superblock.checkpointBStart = geometry.checkpointBStart;
                superblock.checkpointBlocks = geometry.checkpointBlocks;
                superblock.dataStart = geometry.dataStart;
                superblock.dataBlocks = geometry.dataBlocks;
                superblockFinalize(superblock);
                std::memcpy(replica.data(), &superblock, sizeof(superblock));
            }
            auto written = entry.second->write(translate(offsetBlocks, entry.first), replica);
            if (!written.ok()) return written;
        }
        return Result<void>(Status::Ok);
    }

    Result<void> writeBatch(std::span<const WriteRange> ranges) override {
        std::vector<std::vector<std::byte>> replicas;
        std::vector<WriteRange> batch;
        replicas.reserve(ranges.size());
        batch.reserve(ranges.size());
        for (auto &entry : devices_) {
            replicas.clear();
            batch.clear();
            for (const WriteRange &range : ranges) {
                replicas.emplace_back(range.buffer.begin(), range.buffer.end());
                std::vector<std::byte> &replica = replicas.back();
                if (isSuperblock(range.offsetBlocks) &&
                    replica.size() >= sizeof(Hfs2Superblock)) {
                    Hfs2Superblock superblock{};
                    std::memcpy(&superblock, replica.data(), sizeof(superblock));
                    const DeviceGeometry &geometry = geometries_[entry.first];
                    superblock.deviceId = static_cast<std::uint32_t>(entry.first);
                    superblock.replicaId = geometry.replicaId;
                    superblock.deviceBlocks = geometry.deviceBlocks;
                    superblock.journalStart = geometry.journalStart;
                    superblock.journalBlocks = geometry.journalBlocks;
                    superblock.checkpointAStart = geometry.checkpointAStart;
                    superblock.checkpointBStart = geometry.checkpointBStart;
                    superblock.checkpointBlocks = geometry.checkpointBlocks;
                    superblock.dataStart = geometry.dataStart;
                    superblock.dataBlocks = geometry.dataBlocks;
                    superblockFinalize(superblock);
                    std::memcpy(replica.data(), &superblock, sizeof(superblock));
                }
                batch.push_back({translate(range.offsetBlocks, entry.first), replica});
            }
            auto written = entry.second->writeBatch(batch);
            if (!written.ok()) return written;
        }
        return Result<void>(Status::Ok);
    }

    Result<void> flush() override {
        for (auto &entry : devices_) {
            auto flushed = entry.second->flush();
            if (!flushed.ok()) return flushed;
        }
        return Result<void>(Status::Ok);
    }

private:
    bool isSuperblock(std::uint64_t offsetBlocks) const noexcept {
        return offsetBlocks == 0 ||
               offsetBlocks == kSuperblockSlotOffsetBytes / config_.metadataBlockSize;
    }

    bool isMirroredRegion(std::uint64_t offsetBlocks) const noexcept {
        const DeviceGeometry &primary = geometries_[primaryIndex_];
        return (offsetBlocks >= primary.journalStart &&
                offsetBlocks < primary.journalStart + primary.journalBlocks) ||
               (offsetBlocks >= primary.checkpointAStart &&
                offsetBlocks < primary.checkpointAStart + primary.checkpointBlocks);
    }

    std::uint64_t translate(std::uint64_t offsetBlocks, std::size_t deviceIndex) const noexcept {
        const DeviceGeometry &primary = geometries_[primaryIndex_];
        const DeviceGeometry &target = geometries_[deviceIndex];
        if (offsetBlocks >= primary.journalStart &&
            offsetBlocks < primary.journalStart + primary.journalBlocks) {
            return target.journalStart + (offsetBlocks - primary.journalStart);
        }
        if (offsetBlocks >= primary.checkpointAStart &&
            offsetBlocks < primary.checkpointAStart + primary.checkpointBlocks) {
            return target.checkpointAStart + (offsetBlocks - primary.checkpointAStart);
        }
        return offsetBlocks;
    }

    SpdkRuntime &runtime_;
    const Config &config_;
    /* Own the geometry because Journal can outlive the Engine accessor's copy. */
    std::vector<DeviceGeometry> geometries_;
    std::size_t primaryIndex_;
    std::vector<std::pair<std::size_t, std::unique_ptr<RuntimeJournalDevice>>> devices_;
};

} // namespace

Result<std::unique_ptr<Journal>> Journal::open(Engine &engine, JournalTarget *target,
                                               std::size_t deviceIndex) {
    const Config &config = engine.config();
    const auto geometries = engine.deviceGeometries();
    if (deviceIndex >= geometries.size()) {
        return makeError<std::unique_ptr<Journal>>(Status::InvalidArgument,
                                                   "journal device index out of range");
    }
    auto &runtime = SpdkRuntime::instance();
    const std::uint32_t sectorSize = runtime.deviceBlockSize(deviceIndex);
    if (sectorSize == 0) {
        return makeError<std::unique_ptr<Journal>>(Status::InvalidState,
                                                   "engine is not mounted");
    }
    std::unique_ptr<JournalDevice> device;
    if (target != nullptr && config.metadataReplication > 1) {
        device = std::make_unique<MirroredRuntimeJournalDevice>(runtime, config, geometries,
                                                                deviceIndex);
    } else {
        device = std::make_unique<RuntimeJournalDevice>(runtime, deviceIndex,
                                                        config.metadataBlockSize, sectorSize);
    }
    /* Capture the raw pointer before transferring ownership to Journal. */
    JournalDevice *devicePtr = device.get();
    const auto &geometry = geometries[deviceIndex];
    return Journal::create(std::move(device), devicePtr, geometry.journalStart,
                           geometry.journalBlocks, geometry.checkpointAStart,
                           geometry.checkpointBStart, geometry.checkpointBlocks,
                           config.checkpointTriggerPercent,
                           config.checkpointForcePercent, target);
}

Result<std::unique_ptr<Journal>> Journal::open(JournalDevice &device,
                                               std::uint64_t journalStart,
                                               std::uint64_t journalBlocks,
                                               std::uint64_t checkpointAStart,
                                               std::uint64_t checkpointBStart,
                                               std::uint64_t checkpointBlocks,
                                               std::uint32_t triggerPercent,
                                               std::uint32_t forcePercent,
                                               JournalTarget *target) {
    return create(nullptr, &device, journalStart, journalBlocks, checkpointAStart,
                  checkpointBStart, checkpointBlocks, triggerPercent, forcePercent, target);
}

Result<void> Journal::replay() {
    auto &impl = *impl_;
    impl.corruption.clear();
    impl.validTransactions = 0;
    std::uint64_t sequence = impl.tail;
    std::uint64_t expectedTxid = impl.lastTxid + 1;
    std::vector<std::byte> slot;
    std::vector<std::byte> recordBuffer;
    std::vector<RecordPayload> records;

    while ((sequence - impl.tail) < impl.journalBlocks) {
        Result<void> error{Status::Ok};
        SlotHeader begin{};
        const SlotState beginState = impl.readSlotState(sequence, slot, &begin, &error);
        if (beginState == SlotState::Empty || beginState == SlotState::Stale) {
            break;
        }
        if (beginState == SlotState::Invalid) {
            if (!error.ok()) {
                return error;
            }
            impl.corruption = "invalid slot at sequence " + std::to_string(sequence);
            break;
        }
        if (begin.sequence != sequence) {
            break; /* stale ring content from an earlier wrap */
        }
        if (begin.type != static_cast<std::uint16_t>(JournalRecordType::Begin)) {
            impl.corruption = "orphan record at sequence " + std::to_string(sequence);
            break;
        }
        if (begin.txid != expectedTxid) {
            break; /* clean end */
        }
        if (begin.recordCount > impl.journalBlocks) {
            impl.corruption = "oversized transaction at sequence " + std::to_string(sequence);
            break;
        }
        records.clear();
        recordBuffer.clear();
        bool stop = false;
        for (std::uint32_t i = 0; i < begin.recordCount; ++i) {
            SlotHeader record{};
            const SlotState recordState =
                impl.readSlotState(sequence + 1 + i, slot, &record, &error);
            if (recordState == SlotState::Valid && record.sequence != sequence + 1 + i) {
                stop = true; /* stale ring content from an earlier wrap */
                break;
            }
            if (recordState != SlotState::Valid ||
                record.type != static_cast<std::uint16_t>(JournalRecordType::Record) ||
                record.txid != begin.txid ||
                record.payloadSize != sizeof(RecordPayload)) {
                if (recordState == SlotState::Empty) {
                    stop = true; /* crash between BEGIN and COMMIT */
                } else if (!error.ok()) {
                    return error;
                } else {
                    impl.corruption =
                        "invalid record at sequence " + std::to_string(sequence + 1 + i);
                    stop = true;
                }
                break;
            }
            RecordPayload payload{};
            std::memcpy(&payload, slot.data() + sizeof(SlotHeader), sizeof(payload));
            records.push_back(payload);
            const auto *raw = reinterpret_cast<const std::byte *>(&payload);
            recordBuffer.insert(recordBuffer.end(), raw, raw + sizeof(payload));
        }
        if (stop) {
            break;
        }
        const std::uint64_t commitSequence = sequence + 1 + begin.recordCount;
        SlotHeader commit{};
        const SlotState commitState =
            impl.readSlotState(commitSequence, slot, &commit, &error);
        if (commitState == SlotState::Valid && commit.sequence != commitSequence) {
            break; /* stale ring content from an earlier wrap */
        }
        if (commitState != SlotState::Valid ||
            commit.type != static_cast<std::uint16_t>(JournalRecordType::Commit) ||
            commit.txid != begin.txid) {
            if (commitState == SlotState::Empty) {
                break; /* crash before COMMIT became durable */
            }
            if (!error.ok()) {
                return error;
            }
            impl.corruption = "invalid commit at sequence " + std::to_string(commitSequence);
            break;
        }
        const std::uint32_t txSeed =
            crc32c(&begin.txid, sizeof(begin.txid));
        const std::uint32_t txCrc =
            crc32c(recordBuffer.data(), recordBuffer.size(), txSeed);
        if (commit.txCrc != txCrc) {
            impl.corruption = "commit crc mismatch at txid " + std::to_string(begin.txid);
            break;
        }
        if (impl.target != nullptr) {
            std::vector<std::byte> beforeImage;
            auto saved = impl.target->saveSnapshot(impl.lastTxid, beforeImage);
            if (!saved.ok()) {
                impl.corruption = "target snapshot failed before txid " +
                                   std::to_string(begin.txid);
                impl.refreshStats();
                return makeError(Status::Corrupt, impl.corruption);
            }
            for (const auto &payload : records) {
                JournalRecord record;
                record.type = static_cast<JournalRecordType>(payload.type);
                record.objectId = payload.objectId;
                record.arg0 = payload.arg0;
                record.arg1 = payload.arg1;
                auto applied = impl.target->apply(record);
                if (!applied.ok()) {
                    auto restored = impl.target->loadSnapshot(beforeImage);
                    impl.corruption = "target rejected txid " + std::to_string(begin.txid);
                    if (!restored.ok()) {
                        impl.corruption += "; target after-image restore failed";
                    }
                    impl.refreshStats();
                    return makeError(Status::Corrupt, impl.corruption);
                }
            }
        }
        impl.lastTxid = begin.txid;
        ++impl.validTransactions;
        expectedTxid += 1;
        sequence = commitSequence + 1;
    }

    impl.head = std::max(impl.head, sequence);
    if (impl.head < impl.tail) {
        impl.head = impl.tail;
    }
    impl.refreshStats();
    if (!impl.corruption.empty()) {
        log::error("journal replay stopped: %s", impl.corruption.c_str());
        return makeError(Status::Corrupt, impl.corruption);
    }
    return Result<void>(Status::Ok);
}

Result<std::uint64_t> Journal::append(std::span<const JournalRecord> records,
                                      bool flushDevice) {
    auto &impl = *impl_;
    if (records.empty()) {
        return makeError<std::uint64_t>(Status::InvalidArgument, "empty transaction");
    }
    if (checkpointForced()) {
        auto checkpointResult = checkpoint();
        if (!checkpointResult.ok()) {
            return makeError<std::uint64_t>(Status::NoSpace,
                                            "journal full and checkpoint failed: " +
                                                checkpointResult.message());
        }
    }
    const std::uint64_t needed = 2 + records.size();
    if ((impl.head - impl.tail) + needed > impl.journalBlocks) {
        auto checkpointResult = checkpoint();
        if (!checkpointResult.ok()) {
            return makeError<std::uint64_t>(Status::NoSpace,
                                            "journal full and checkpoint failed: " +
                                                checkpointResult.message());
        }
        if ((impl.head - impl.tail) + needed > impl.journalBlocks) {
            return makeError<std::uint64_t>(Status::NoSpace, "transaction does not fit");
        }
    }

    const std::uint64_t txid = impl.lastTxid + 1;
    using enum JournalRecordType;
    std::vector<std::byte> payloads;
    payloads.reserve(records.size() * sizeof(RecordPayload));
    for (const auto &record : records) {
        RecordPayload payload{};
        payload.type = static_cast<std::uint16_t>(record.type);
        payload.objectId = record.objectId;
        payload.arg0 = record.arg0;
        payload.arg1 = record.arg1;
        const auto *raw = reinterpret_cast<const std::byte *>(&payload);
        payloads.insert(payloads.end(), raw, raw + sizeof(payload));
    }
    const std::uint32_t txSeed = crc32c(&txid, sizeof(txid));
    const std::uint32_t txCrc = crc32c(payloads.data(), payloads.size(), txSeed);

    std::uint64_t sequence = impl.head;
    auto begin = impl.writeSlot(sequence, static_cast<std::uint16_t>(Begin), txid,
                                static_cast<std::uint32_t>(records.size()), 0, {});
    if (!begin.ok()) {
        return makeError<std::uint64_t>(begin.status(), begin.message());
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
        ++sequence;
        auto written = impl.writeSlot(
            sequence, static_cast<std::uint16_t>(Record), txid, 0, 0,
            std::span<const std::byte>(payloads).subspan(i * sizeof(RecordPayload),
                                                         sizeof(RecordPayload)));
        if (!written.ok()) {
            return makeError<std::uint64_t>(written.status(), written.message());
        }
    }
    ++sequence;
    auto commit = impl.writeSlot(sequence, static_cast<std::uint16_t>(Commit), txid, 0,
                                 txCrc, {});
    if (!commit.ok()) {
        return makeError<std::uint64_t>(commit.status(), commit.message());
    }
    test_hooks::crashIfRequested("journal_commit");
    if (flushDevice) {
        auto flushed = impl.device->flush();
        if (!flushed.ok()) {
            return makeError<std::uint64_t>(flushed.status(), flushed.message());
        }
    }
    impl.head = sequence + 1;
    if (impl.target != nullptr) {
        /* Committing a transaction publishes it to the in-memory state. */
        std::vector<std::byte> beforeImage;
        auto saved = impl.target->saveSnapshot(impl.lastTxid, beforeImage);
        if (!saved.ok()) {
            return makeError<std::uint64_t>(Status::Corrupt,
                                            "target snapshot failed before committed record");
        }
        for (const auto &record : records) {
            auto applied = impl.target->apply(record);
            if (!applied.ok()) {
                auto restored = impl.target->loadSnapshot(beforeImage);
                if (!restored.ok()) {
                    return makeError<std::uint64_t>(
                        Status::Corrupt, "replay target rejected record and restore failed");
                }
                return makeError<std::uint64_t>(Status::Corrupt,
                                                "replay target rejected committed record");
            }
        }
    }
    impl.lastTxid = txid;
    impl.refreshStats();
    return txid;
}

Result<std::uint64_t> Journal::append(std::initializer_list<JournalRecord> records,
                                      bool flushDevice) {
    return append(std::span<const JournalRecord>(records.begin(), records.size()),
                  flushDevice);
}

Result<void> Journal::checkpoint(bool deferFlush) {
    auto &impl = *impl_;
    if (impl.target == nullptr) {
        return makeError(Status::InvalidState, "checkpoint requires a replay target");
    }
    std::vector<std::byte> snapshot;
    const auto snapshotStart = std::chrono::steady_clock::now();
    auto saved = impl.target->saveSnapshot(impl.lastTxid, snapshot);
    stats::counters().syncCheckpointSnapshot.record(elapsedNs(snapshotStart));
    if (!saved.ok()) {
        return saved;
    }
    if (impl.target->snapshotTxid() != impl.lastTxid) {
        return makeError(Status::Corrupt, "snapshot txid does not match the journal tail");
    }
    auto checkpoint = impl.buildCheckpointRecord(snapshot);
    if (!checkpoint.ok()) {
        return makeError(checkpoint.status(), checkpoint.message());
    }
    auto superblockRecord = impl.buildSuperblockRecord();
    if (!superblockRecord.ok()) {
        return makeError(superblockRecord.status(), superblockRecord.message());
    }
    auto written = impl.writeCheckpointAndSuperblock(checkpoint.value(), superblockRecord.value(),
                                                     deferFlush);
    if (!written.ok()) {
        return written;
    }
    impl.tail = impl.head;
    impl.refreshStats();
    return Result<void>(Status::Ok);
}

Result<void> Journal::flush() { return impl_->device->flush(); }double Journal::usageRatio() const noexcept {
    const auto &impl = *impl_;
    if (impl.journalBlocks == 0) {
        return 0.0;
    }
    return static_cast<double>(impl.head - impl.tail) /
           static_cast<double>(impl.journalBlocks);
}

bool Journal::needsCheckpoint() const noexcept {
    return usageRatio() * 100.0 >= static_cast<double>(impl_->triggerPercent);
}

bool Journal::checkpointForced() const noexcept {
    return usageRatio() * 100.0 >= static_cast<double>(impl_->forcePercent);
}

const Journal::Stats &Journal::stats() const noexcept { return impl_->statsCache; }

} // namespace hfs
