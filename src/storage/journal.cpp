#include "uuse/journal.hpp"

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
#include "common/trace.hpp"
#include "uuse/core.hpp"
#include "uuse/crc.hpp"
#include "uuse/superblock.hpp"
#include "runtime/spdk_runtime.hpp"

namespace uuse {

namespace {

/* Nanoseconds elapsed since a phase start, for barrier diagnostics. */
std::uint64_t elapsedNs(std::chrono::steady_clock::time_point start) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start)
            .count());
}

constexpr std::uint64_t kJournalSlotMagic = 0x555553454a524e4cULL; /* "UUSEJRNL" */
constexpr std::uint32_t kJournalFormatVersion = 1;
constexpr std::uint64_t kCheckpointMagic = 0x5555534543505431ULL; /* "UUSECPT1" */
constexpr std::uint32_t kCheckpointFormatVersion = 2;
/* v1 checkpoint slots have no kind field and are always full snapshots. */
constexpr std::uint32_t kCheckpointVersionFullOnly = 1;
constexpr std::uint32_t kCheckpointKindFull = 0;
constexpr std::uint32_t kCheckpointKindDelta = 1;
/* A journal-backed delta keeps the window (base, head]; a full base is written
 * once the window reaches force_percent of the ring (or the ring is about to
 * fill). The window is what a crash replays, so this bounds replay time. */
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
    std::uint32_t kind; /* kCheckpointKindFull / kCheckpointKindDelta */
};

/* Delta checkpoint payload: the full base slot this delta anchors to. */
struct DeltaPayload {
    std::uint64_t baseTxid;
    std::uint64_t baseSequence;
    std::uint8_t baseSlotIsA;
    std::uint8_t padding[7];
};
static_assert(sizeof(DeltaPayload) == 24, "delta payload layout is frozen");

std::uint64_t bytesToBlocks(std::uint64_t bytes, std::uint64_t blockSize) {
    return (bytes + blockSize - 1) / blockSize;
}

/* Test-only checkpoint barrier fault; production behavior is unchanged unless
 * the caller explicitly requests this environment-variable injection. */
bool testCheckpointFlushFailureRequested() {
    const char *value = std::getenv("UUSE_TEST_FAIL_CHECKPOINT_FLUSH");
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

    UuseSuperblock superblock{};
    std::uint32_t sbSlot = 0; /* slot holding the newest superblock */
    bool sbValid = false;

    std::uint64_t head = 0;
    std::uint64_t tail = 0;
    std::uint64_t lastTxid = 0;
    std::uint32_t validTransactions = 0;
    /* Journal-backed delta checkpoint state (format v2): the current full base
     * slot plus the sequence of the last checkpoint write (delta or full). */
    bool baseValid = false;
    bool baseSlotIsA = false;
    std::uint64_t baseTxid = 0;
    std::uint64_t baseSequence = 0;
    std::uint64_t lastCheckpointSequence = 0;
    /* Set by requireFullCheckpoint(): the next checkpoint must write a full
     * base because the window contains a shape change the journal cannot
     * replay. Cleared once that base is written. */
    bool forceFullPending = false;
    std::uint64_t fullCheckpoints = 0;
    std::uint64_t deltaCheckpoints = 0;
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
    /* Serializes append/checkpoint/replay/flush: the background metadata
     * journal writer and a durability barrier may call concurrently. */
    mutable std::mutex mutex;

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

    /* Builds one slot image into a caller-provided block. */
    void buildSlot(std::span<std::byte> slot, std::uint64_t sequence, std::uint16_t type,
                   std::uint64_t txid, std::uint32_t recordCount, std::uint64_t txCrc,
                   std::span<const std::byte> payload) {
        std::fill(slot.begin(), slot.end(), std::byte{0});
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
    }

    Result<void> writeSlot(std::uint64_t sequence, std::uint16_t type, std::uint64_t txid,
                           std::uint32_t recordCount, std::uint64_t txCrc,
                           std::span<const std::byte> payload) {
        std::vector<std::byte> slot(blockSize, std::byte{0});
        buildSlot(slot, sequence, type, txid, recordCount, txCrc, payload);
        const std::uint64_t offset = journalStart + (sequence % journalBlocks);
        return writeBlock(offset, slot);
    }

    /* Writes a contiguous run of ring slots starting at sequence; splits at the
     * ring wrap so each part stays contiguous. */
    Result<void> writeSlots(std::uint64_t sequence, std::span<const std::byte> buffer) {
        std::size_t done = 0;
        while (done < buffer.size()) {
            const std::uint64_t slotIndex =
                (sequence + done / blockSize) % journalBlocks;
            const std::size_t blocks = (buffer.size() - done) / blockSize;
            const std::size_t contiguous =
                std::min<std::size_t>(blocks, journalBlocks - slotIndex);
            const std::size_t bytes = contiguous * blockSize;
            auto written = writeBlock(journalStart + slotIndex,
                                      buffer.subspan(done, bytes));
            if (!written.ok()) {
                return written;
            }
            done += bytes;
        }
        return Result<void>(Status::Ok);
    }

    Result<void> readSuperblocks() {
        std::vector<std::byte> record(kSuperblockRecordSize);
        UuseSuperblock slotA{};
        UuseSuperblock slotB{};
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
            return makeError(Status::NotFound, "no valid UUSE 2.0 superblock on device");
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
        std::uint32_t kind = kCheckpointKindFull;
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
                                std::uint32_t *kind, DeltaPayload *delta,
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
        *kind = kCheckpointKindFull;
        if (header.magic != kCheckpointMagic ||
            (header.formatVersion != kCheckpointFormatVersion &&
             header.formatVersion != kCheckpointVersionFullOnly) ||
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
        const std::uint32_t checkpointKind =
            header.formatVersion == kCheckpointVersionFullOnly ? kCheckpointKindFull
                                                              : header.kind;
        if (checkpointKind == kCheckpointKindDelta) {
            if (header.payloadSize < sizeof(DeltaPayload)) {
                return Result<void>(Status::Ok);
            }
            std::memcpy(delta, record.data() + sizeof(CheckpointHeader), sizeof(DeltaPayload));
            payload->clear();
        } else {
            payload->assign(record.begin() + sizeof(CheckpointHeader), record.end());
        }
        *valid = true;
        *kind = checkpointKind;
        *txid = header.txid;
        *sequence = header.sequence;
        *generation = header.generation;
        return Result<void>(Status::Ok);
    }

    /* Full base checkpoint: freeze/encode already happened; the record carries
     * the whole namespace snapshot. Always written to the non-base slot so any
     * delta that follows can still reach its base. */
    Result<CheckpointRecord> buildFullCheckpointRecord(std::span<const std::byte> snapshot) {
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
        header.kind = kCheckpointKindFull;
        std::memcpy(built.record.data(), &header, sizeof(header));
        std::memcpy(built.record.data() + sizeof(header), snapshot.data(), snapshot.size());
        auto *headerView = reinterpret_cast<CheckpointHeader *>(built.record.data());
        headerView->crc = crc32c(built.record.data() + sizeof(CheckpointHeader), snapshot.size(),
                                 crc32c(built.record.data(), offsetof(CheckpointHeader, crc)));

        /* The non-base slot keeps the current base intact for deltas. */
        built.writeA = !baseValid || !baseSlotIsA;
        built.offsetBlocks = built.writeA ? checkpointAStart : checkpointBStart;
        built.txid = lastTxid;
        built.sequence = head;
        built.generation = superblock.generation;
        built.kind = kCheckpointKindFull;
        return built;
    }

    /* Delta checkpoint: points at the current full base and records the ring
     * head; the journal keeps (base, head] so recovery replays the difference. */
    Result<CheckpointRecord> buildDeltaCheckpointRecord() {
        const std::uint64_t slotBlocks = checkpointBlocks / 2;
        const std::uint64_t totalBytes = sizeof(CheckpointHeader) + sizeof(DeltaPayload);
        if (bytesToBlocks(totalBytes, blockSize) > slotBlocks) {
            return makeError<CheckpointRecord>(Status::NoSpace,
                                               "delta checkpoint does not fit the slot");
        }
        CheckpointRecord built;
        built.record.assign(bytesToBlocks(totalBytes, blockSize) * blockSize, std::byte{0});
        CheckpointHeader header{};
        header.magic = kCheckpointMagic;
        header.formatVersion = kCheckpointFormatVersion;
        header.payloadSize = sizeof(DeltaPayload);
        header.txid = lastTxid;
        header.sequence = head;
        header.generation = superblock.generation;
        header.uuid = superblock.uuid;
        header.kind = kCheckpointKindDelta;
        DeltaPayload delta{};
        delta.baseTxid = baseTxid;
        delta.baseSequence = baseSequence;
        delta.baseSlotIsA = baseSlotIsA ? 1 : 0;
        std::memcpy(built.record.data(), &header, sizeof(header));
        std::memcpy(built.record.data() + sizeof(header), &delta, sizeof(delta));
        auto *headerView = reinterpret_cast<CheckpointHeader *>(built.record.data());
        headerView->crc = crc32c(built.record.data() + sizeof(CheckpointHeader),
                                 sizeof(DeltaPayload),
                                 crc32c(built.record.data(), offsetof(CheckpointHeader, crc)));

        /* Deltas always replace the non-base slot. */
        built.writeA = !baseSlotIsA;
        built.offsetBlocks = built.writeA ? checkpointAStart : checkpointBStart;
        built.txid = lastTxid;
        built.sequence = head;
        built.generation = superblock.generation;
        built.kind = kCheckpointKindDelta;
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
        if (checkpoint.kind == kCheckpointKindDelta) {
            ++deltaCheckpoints;
            log::info("checkpoint written: slot=%c kind=delta txid=%llu sequence=%llu "
                      "bytes=%zu base=%c",
                      checkpoint.writeA ? 'A' : 'B',
                      static_cast<unsigned long long>(checkpoint.txid),
                      static_cast<unsigned long long>(checkpoint.sequence),
                      checkpoint.record.size(), baseSlotIsA ? 'A' : 'B');
        } else {
            ++fullCheckpoints;
            log::info("checkpoint written: slot=%c kind=full txid=%llu sequence=%llu bytes=%zu",
                      checkpoint.writeA ? 'A' : 'B',
                      static_cast<unsigned long long>(checkpoint.txid),
                      static_cast<unsigned long long>(checkpoint.sequence),
                      checkpoint.record.size());
        }
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

    /* Newest valid checkpoint wins; its sequence resumes the ring scan. A
     * delta record resolves to the full base it points at, and the journal
     * replay then starts from the base txid (uuse.md §10.4 b). */
    std::vector<std::byte> payloadA;
    std::vector<std::byte> payloadB;
    std::uint64_t sequenceA = 0;
    std::uint64_t sequenceB = 0;
    std::uint64_t txidA = 0;
    std::uint64_t txidB = 0;
    std::uint64_t generationA = 0;
    std::uint64_t generationB = 0;
    std::uint32_t kindA = kCheckpointKindFull;
    std::uint32_t kindB = kCheckpointKindFull;
    DeltaPayload deltaA{};
    DeltaPayload deltaB{};
    auto readA = impl.readCheckpoint(impl.checkpointAStart, &impl.checkpointAValid, &txidA,
                                     &sequenceA, &generationA, &kindA, &deltaA, &payloadA);
    if (!readA.ok()) {
        return makeError<std::unique_ptr<Journal>>(readA.status(), readA.message());
    }
    auto readB = impl.readCheckpoint(impl.checkpointBStart, &impl.checkpointBValid, &txidB,
                                     &sequenceB, &generationB, &kindB, &deltaB, &payloadB);
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
    if (useA || useB) {
        const bool newestIsA = useA;
        const std::uint32_t newestKind = newestIsA ? kindA : kindB;
        const std::uint64_t newestTxid = newestIsA ? txidA : txidB;
        const std::uint64_t newestSequence = newestIsA ? sequenceA : sequenceB;
        std::uint64_t baseTxid = newestTxid;
        std::uint64_t baseSequence = newestSequence;
        bool baseSlotIsA = newestIsA;
        const std::vector<std::byte> *basePayload = newestIsA ? &payloadA : &payloadB;
        if (newestKind == kCheckpointKindDelta) {
            const DeltaPayload &delta = newestIsA ? deltaA : deltaB;
            const bool candidateIsA = delta.baseSlotIsA != 0;
            const bool candidateValid = candidateIsA ? impl.checkpointAValid
                                                     : impl.checkpointBValid;
            const std::uint32_t candidateKind = candidateIsA ? kindA : kindB;
            const std::uint64_t candidateTxid = candidateIsA ? txidA : txidB;
            const std::uint64_t candidateSequence = candidateIsA ? sequenceA : sequenceB;
            if (candidateValid && candidateKind == kCheckpointKindFull &&
                candidateTxid == delta.baseTxid &&
                candidateSequence == delta.baseSequence) {
                baseTxid = delta.baseTxid;
                baseSequence = delta.baseSequence;
                baseSlotIsA = candidateIsA;
                basePayload = candidateIsA ? &payloadA : &payloadB;
            } else {
                /* Dangling delta: fall back to the other slot when it is a
                 * usable full snapshot; otherwise the volume is corrupt. */
                const bool otherIsA = !candidateIsA;
                const bool otherValid = otherIsA ? impl.checkpointAValid
                                                 : impl.checkpointBValid;
                const std::uint32_t otherKind = otherIsA ? kindA : kindB;
                const std::uint64_t otherTxid = otherIsA ? txidA : txidB;
                const std::uint64_t otherSequence = otherIsA ? sequenceA : sequenceB;
                if (!otherValid || otherKind != kCheckpointKindFull) {
                    return makeError<std::unique_ptr<Journal>>(
                        Status::Corrupt, "delta checkpoint base is missing");
                }
                baseTxid = otherTxid;
                baseSequence = otherSequence;
                baseSlotIsA = otherIsA;
                basePayload = otherIsA ? &payloadA : &payloadB;
            }
        }
        if (baseSequence >= impl.tail) {
            impl.tail = baseSequence;
            impl.lastTxid = baseTxid;
            impl.baseValid = true;
            impl.baseSlotIsA = baseSlotIsA;
            impl.baseTxid = baseTxid;
            impl.baseSequence = baseSequence;
            impl.lastCheckpointSequence = newestSequence;
            if (impl.target != nullptr) {
                auto loaded = impl.target->loadSnapshot(*basePayload);
                if (!loaded.ok()) {
                    return makeError<std::unique_ptr<Journal>>(
                        loaded.status(),
                        "checkpoint snapshot rejected: " + loaded.message());
                }
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
            if (isSuperblock(offsetBlocks) && replica.size() >= sizeof(UuseSuperblock)) {
                UuseSuperblock superblock{};
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
                    replica.size() >= sizeof(UuseSuperblock)) {
                    UuseSuperblock superblock{};
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
    std::lock_guard<std::mutex> lock(impl_->mutex);
    trace::event(trace::EventId::RecoveryBegin, impl_->tail, impl_->head);
    auto replayed = replayLocked();
    trace::event(trace::EventId::RecoveryEnd, impl_->validTransactions,
                 static_cast<std::uint64_t>(replayed.status()), impl_->lastTxid);
    return replayed;
}

Result<void> Journal::replayLocked() {
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
            /* Replay applies CRC-validated committed records; a rejected record
             * is unrecoverable corruption and aborts the mount, so no
             * per-transaction before-image is captured (it made replay
             * O(transactions x namespace)). */
            for (const auto &payload : records) {
                JournalRecord record;
                record.type = static_cast<JournalRecordType>(payload.type);
                record.objectId = payload.objectId;
                record.arg0 = payload.arg0;
                record.arg1 = payload.arg1;
                auto applied = impl.target->apply(record);
                if (!applied.ok()) {
                    impl.corruption = "target rejected txid " + std::to_string(begin.txid) +
                                      " record type " + std::to_string(payload.type) +
                                      " object " + std::to_string(payload.objectId) +
                                      " args " + std::to_string(payload.arg0) + "/" +
                                      std::to_string(payload.arg1) + ": " + applied.message();
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
                                      bool flushDevice, bool applyToTarget) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return appendLocked(records, flushDevice, applyToTarget);
}

Result<std::uint64_t> Journal::appendLocked(std::span<const JournalRecord> records,
                                            bool flushDevice, bool applyToTarget) {
    auto &impl = *impl_;
    if (records.empty()) {
        return makeError<std::uint64_t>(Status::InvalidArgument, "empty transaction");
    }
    /* Unlocked force check: the caller already holds the journal mutex. */
    const bool forced =
        impl.journalBlocks != 0 &&
        static_cast<double>(impl.head - impl.tail) * 100.0 >=
            static_cast<double>(impl.forcePercent) * static_cast<double>(impl.journalBlocks);
    if (forced) {
        auto checkpointResult = checkpointLocked(/*deferFlush=*/false, /*forceFullBase=*/true);
        if (!checkpointResult.ok()) {
            return makeError<std::uint64_t>(Status::NoSpace,
                                            "journal full and checkpoint failed: " +
                                                checkpointResult.message());
        }
    }
    const std::uint64_t needed = 2 + records.size();
    if ((impl.head - impl.tail) + needed > impl.journalBlocks) {
        auto checkpointResult = checkpointLocked(/*deferFlush=*/false, /*forceFullBase=*/true);
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

    /* One contiguous write covers BEGIN + records + COMMIT, so a metadata
     * transaction costs a single device round trip (plus the barrier flush). */
    std::uint64_t sequence = impl.head;
    const std::size_t slotCount = 2 + records.size();
    std::vector<std::byte> transaction(slotCount * impl.blockSize, std::byte{0});
    impl.buildSlot(std::span<std::byte>(transaction.data(), impl.blockSize), sequence,
                   static_cast<std::uint16_t>(Begin), txid,
                   static_cast<std::uint32_t>(records.size()), 0, {});
    for (std::size_t i = 0; i < records.size(); ++i) {
        impl.buildSlot(
            std::span<std::byte>(transaction.data() + (1 + i) * impl.blockSize,
                                 impl.blockSize),
            sequence + 1 + i, static_cast<std::uint16_t>(Record), txid, 0, 0,
            std::span<const std::byte>(payloads).subspan(i * sizeof(RecordPayload),
                                                         sizeof(RecordPayload)));
    }
    impl.buildSlot(
        std::span<std::byte>(transaction.data() + (1 + records.size()) * impl.blockSize,
                             impl.blockSize),
        sequence + 1 + records.size(), static_cast<std::uint16_t>(Commit), txid, 0,
        txCrc, {});
    auto written = impl.writeSlots(sequence, transaction);
    if (!written.ok()) {
        return makeError<std::uint64_t>(written.status(), written.message());
    }
    sequence += records.size() + 1;
    test_hooks::crashIfRequested("journal_commit");
    if (flushDevice) {
        auto flushed = impl.device->flush();
        if (!flushed.ok()) {
            return makeError<std::uint64_t>(flushed.status(), flushed.message());
        }
    }
    impl.head = sequence + 1;
    if (applyToTarget && impl.target != nullptr) {
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
    trace::event(trace::EventId::JournalAppend, txid,
                 static_cast<std::uint64_t>(records.size()),
                 static_cast<std::uint64_t>(slotCount) * impl.blockSize,
                 flushDevice ? 1ULL : 0ULL);
    return txid;
}

Result<std::uint64_t> Journal::append(std::initializer_list<JournalRecord> records,
                                      bool flushDevice, bool applyToTarget) {
    return append(std::span<const JournalRecord>(records.begin(), records.size()),
                  flushDevice, applyToTarget);
}

Result<void> Journal::checkpoint(bool deferFlush) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return checkpointLocked(deferFlush);
}

Result<void> Journal::checkpointLocked(bool deferFlush, bool forceFullBase) {
    auto &impl = *impl_;
    if (impl.target == nullptr) {
        return makeError(Status::InvalidState, "checkpoint requires a replay target");
    }
    const std::uint64_t checkpointTxid = impl.lastTxid;
    trace::event(trace::EventId::ChkptBegin, checkpointTxid, impl.head - impl.tail);
    /* Journal-backed delta (uuse.md §10.4 b): a delta only points at the
     * current full base; the journal keeps (base, head] for recovery. A full
     * base is written when none exists, when the replay window grows past
     * force_percent of the ring, or when the ring is about to fill (append
     * forces it). */
    const std::uint64_t window = impl.head - impl.baseSequence;
    const std::uint64_t fullWindow = impl.journalBlocks * impl.forcePercent / 100;
    const bool writeFullBase =
        forceFullBase || impl.forceFullPending || !impl.baseValid || window >= fullWindow;
    std::vector<std::byte> snapshot;
    if (writeFullBase) {
        const auto snapshotStart = std::chrono::steady_clock::now();
        auto saved = impl.target->saveSnapshot(impl.lastTxid, snapshot);
        stats::counters().syncCheckpointSnapshot.record(elapsedNs(snapshotStart));
        if (!saved.ok()) {
            trace::event(trace::EventId::ChkptEnd, checkpointTxid,
                         static_cast<std::uint64_t>(saved.status()));
            return saved;
        }
        if (impl.target->snapshotTxid() != impl.lastTxid) {
            trace::event(trace::EventId::ChkptEnd, checkpointTxid,
                         static_cast<std::uint64_t>(Status::Corrupt));
            return makeError(Status::Corrupt,
                             "snapshot txid does not match the journal tail");
        }
    }
    auto checkpoint = writeFullBase ? impl.buildFullCheckpointRecord(snapshot)
                                    : impl.buildDeltaCheckpointRecord();
    if (!checkpoint.ok()) {
        trace::event(trace::EventId::ChkptEnd, checkpointTxid,
                     static_cast<std::uint64_t>(checkpoint.status()));
        return makeError(checkpoint.status(), checkpoint.message());
    }
    trace::event(trace::EventId::ChkptBytes, checkpointTxid,
                 writeFullBase ? static_cast<std::uint64_t>(snapshot.size())
                               : static_cast<std::uint64_t>(sizeof(DeltaPayload)));
    auto superblockRecord = impl.buildSuperblockRecord();
    if (!superblockRecord.ok()) {
        trace::event(trace::EventId::ChkptEnd, checkpointTxid,
                     static_cast<std::uint64_t>(superblockRecord.status()));
        return makeError(superblockRecord.status(), superblockRecord.message());
    }
    auto written = impl.writeCheckpointAndSuperblock(checkpoint.value(), superblockRecord.value(),
                                                     deferFlush);
    if (!written.ok()) {
        trace::event(trace::EventId::ChkptEnd, checkpointTxid,
                     static_cast<std::uint64_t>(written.status()));
        return written;
    }
    if (writeFullBase) {
        impl.baseValid = true;
        impl.baseSlotIsA = checkpoint.value().writeA;
        impl.baseTxid = checkpointTxid;
        impl.baseSequence = impl.head;
        /* The base covers everything up to head; older records can be reused. */
        impl.tail = impl.head;
        impl.forceFullPending = false;
    }
    impl.lastCheckpointSequence = impl.head;
    impl.refreshStats();
    trace::event(trace::EventId::ChkptEnd, checkpointTxid,
                 static_cast<std::uint64_t>(Status::Ok));
    return Result<void>(Status::Ok);
}

Result<void> Journal::flush() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->device->flush();
}

double Journal::usageRatio() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto &impl = *impl_;
    if (impl.journalBlocks == 0) {
        return 0.0;
    }
    return static_cast<double>(impl.head - impl.tail) /
           static_cast<double>(impl.journalBlocks);
}

bool Journal::needsCheckpoint() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto &impl = *impl_;
    if (impl.journalBlocks == 0) {
        return false;
    }
    /* Journal-backed deltas do not advance tail, so the trigger counts appends
     * since the last checkpoint instead of tail occupancy (uuse.md §10.4).
     * Deltas are O(1) records, so the cadence is half the configured trigger:
     * a full base is still bounded by force_percent of the ring. */
    const std::uint64_t appended = impl.head - impl.lastCheckpointSequence;
    const std::uint64_t cadence = std::max<std::uint64_t>(impl.triggerPercent / 2, 1);
    return appended * 100 >= impl.journalBlocks * cadence;
}

bool Journal::nextCheckpointWillBeFull() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto &impl = *impl_;
    if (impl.journalBlocks == 0 || !impl.baseValid || impl.forceFullPending) {
        return true;
    }
    const std::uint64_t window = impl.head - impl.baseSequence;
    return window >= impl.journalBlocks * impl.forcePercent / 100;
}

void Journal::requireFullCheckpoint() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->forceFullPending = true;
}

bool Journal::checkpointForced() const {
    return usageRatio() * 100.0 >= static_cast<double>(impl_->forcePercent);
}

Journal::Stats Journal::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->statsCache;
}

} // namespace uuse
