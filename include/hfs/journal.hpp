#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "hfs/export.hpp"
#include "hfs/types.hpp"

namespace hfs {

class Engine;

/* Journal record types; ids are frozen and only appended. Structural types
 * (Begin/Commit) frame a transaction, the others are metadata operations. */
enum class JournalRecordType : std::uint16_t {
    Begin = 1,
    Commit = 2,
    Record = 3,
    Create = 4,
    Link = 5,
    Unlink = 6,
    Rename = 7,
    Alloc = 8,
    Reserve = 9,
    Free = 10,
    SetSize = 11,
    SetTimes = 12,
    Truncate = 13,
    Checkpoint = 14,
};

/* One metadata operation inside a transaction. The fixed fields keep the
 * generic replay mechanism testable before real metadata semantics land. */
struct JournalRecord {
    JournalRecordType type = JournalRecordType::Create;
    std::uint64_t objectId = 0;
    std::uint64_t arg0 = 0;
    std::uint64_t arg1 = 0;
};

/* Replay target: the in-memory metadata state machine. */
class HFS_API JournalTarget {
public:
    virtual ~JournalTarget() = default;
    /* Apply one committed record; must be idempotent per replay run. */
    virtual Result<void> apply(const JournalRecord &record) = 0;
    /* Serialize the full state for a checkpoint snapshot at the given txid. */
    virtual Result<void> saveSnapshot(std::uint64_t txid, std::vector<std::byte> &out) = 0;
    /* Replace the state from a checkpoint snapshot. */
    virtual Result<void> loadSnapshot(std::span<const std::byte> data) = 0;
    /* Last applied transaction id (0 when empty). */
    virtual std::uint64_t snapshotTxid() const noexcept = 0;
};

/* Block device used by the journal; offsets are in metadata blocks. */
class HFS_API JournalDevice {
public:
    /* One write range of a batch; offsets are in metadata blocks. */
    struct WriteRange {
        std::uint64_t offsetBlocks = 0;
        std::span<const std::byte> buffer;
    };

    virtual ~JournalDevice() = default;
    virtual std::uint32_t blockSize() const noexcept = 0;
    virtual Result<void> read(std::uint64_t offsetBlocks, std::span<std::byte> buffer) = 0;
    virtual Result<void> write(std::uint64_t offsetBlocks,
                               std::span<const std::byte> buffer) = 0;
    /* Writes several ranges, letting the device overlap them. The default
     * keeps strict per-range ordering for devices without a batch path. */
    virtual Result<void> writeBatch(std::span<const WriteRange> ranges) {
        for (const WriteRange &range : ranges) {
            auto written = write(range.offsetBlocks, range.buffer);
            if (!written.ok()) {
                return written;
            }
        }
        return Result<void>(Status::Ok);
    }
    virtual Result<void> flush() = 0;
};

/* BEGIN/RECORD/COMMIT ring journal with A/B checkpoint slots (hfs2.0.md §9.3).
 * Format fields are frozen for Shard-79 metadata replicas. */
class HFS_API Journal {
public:
    struct Stats {
        std::uint64_t head = 0;        /* next free slot sequence */
        std::uint64_t tail = 0;        /* first un-checkpointed slot sequence */
        std::uint64_t lastTxid = 0;    /* last replayed/committed transaction */
        std::uint64_t lastCheckpointTxid = 0;
        std::uint64_t checkpointATxid = 0;
        std::uint64_t checkpointBTxid = 0;
        std::uint64_t checkpointASequence = 0;
        std::uint64_t checkpointBSequence = 0;
        bool checkpointAValid = false;
        bool checkpointBValid = false;
        std::uint32_t validTransactions = 0;
        std::string corruption;        /* empty when the replay hit a clean end */
    };

    /* Open the journal of the mounted engine on one device. target == nullptr
     * performs a scan-only replay (hfsctl check). */
    static Result<std::unique_ptr<Journal>> open(Engine &engine, JournalTarget *target,
                                                 std::size_t deviceIndex = 0);

    /* Open against an explicit block device (unit tests, tools). */
    static Result<std::unique_ptr<Journal>> open(JournalDevice &device,
                                                 std::uint64_t journalStart,
                                                 std::uint64_t journalBlocks,
                                                 std::uint64_t checkpointAStart,
                                                 std::uint64_t checkpointBStart,
                                                 std::uint64_t checkpointBlocks,
                                                 std::uint32_t triggerPercent,
                                                 std::uint32_t forcePercent,
                                                 JournalTarget *target);

    /* Replay committed transactions from the ring into the target. */
    Result<void> replay();

    /* Append one transaction; auto-checkpoints when the force threshold is
     * reached and returns NoSpace when the ring cannot make room. With
     * applyToTarget=false the resident state already holds the mutation and
     * only the durable record is written (metadata journaling, §9.3). */
    Result<std::uint64_t> append(std::span<const JournalRecord> records, bool flushDevice,
                                 bool applyToTarget = true);
    Result<std::uint64_t> append(std::initializer_list<JournalRecord> records,
                                 bool flushDevice, bool applyToTarget = true);

    /* Snapshot + checkpoint slot write + superblock publish + tail advance.
     * With deferFlush the caller must flush the journal device afterwards; the
     * barrier then covers data, checkpoint and superblock with one flush. */
    Result<void> checkpoint(bool deferFlush = false);

    /* Flush the journal device (sync path). */
    Result<void> flush();

    /* Journal state queries; all public entry points are internally
     * serialized because the background writer and a barrier may race. */
    double usageRatio() const;
    bool needsCheckpoint() const;
    bool checkpointForced() const;
    /* True when the next checkpoint writes a full base (no base yet or the
     * replay window reached force_percent). The barrier may then skip
     * journaling staged records because the snapshot covers them. */
    bool nextCheckpointWillBeFull() const;
    /* Forces the next checkpoint to write a full base; used for namespace
     * shape changes, which the journal record format cannot replay. */
    void requireFullCheckpoint() noexcept;
    Stats stats() const;

    /* Instances are created by open(); the constructor is public only so the
     * library can build the unique_ptr. */
    Journal();
    ~Journal();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /* Unlocked bodies shared by the public serialized entry points. */
    Result<std::uint64_t> appendLocked(std::span<const JournalRecord> records,
                                       bool flushDevice, bool applyToTarget);
    Result<void> checkpointLocked(bool deferFlush, bool forceFullBase = false);
    Result<void> replayLocked();

    /* Internal factory shared by the two open() overloads. */
    static Result<std::unique_ptr<Journal>> create(std::unique_ptr<JournalDevice> owned,
                                                   JournalDevice *device,
                                                   std::uint64_t journalStart,
                                                   std::uint64_t journalBlocks,
                                                   std::uint64_t checkpointAStart,
                                                   std::uint64_t checkpointBStart,
                                                   std::uint64_t checkpointBlocks,
                                                   std::uint32_t triggerPercent,
                                                   std::uint32_t forcePercent,
                                                   JournalTarget *target);
};

} // namespace hfs
