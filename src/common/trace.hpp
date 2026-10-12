#pragma once

#include <cstdint>
#include <cstddef>

namespace uuse::trace {

/* Tracepoint ids; ids are stable and only appended, per uuse.md §16.5. */
enum class EventId : std::uint16_t {
    FeSubmit = 1,
    FeReap,
    CqPush,
    CqReap,
    RingReserve,
    RingDoorbell,
    RingBusy,
    ActorBatch,
    SchedWait,
    SchedAdmit,
    SchedDone,
    DmaAcquire,
    DmaBusy,
    DmaRelease,
    ExtentPlan,
    BdevSubmit,
    BdevComplete,
    ParentComplete,
    CopyIn,
    CopyOut,
    MetaTxBegin,
    MetaTxCommit,
    JournalAppend,
    JournalFlush,
    ChkptBegin,
    ChkptEnd,
    ChkptBytes,
    FsyncBegin,
    FsyncEnd,
    EpochPublish,
    RecoveryBegin,
    RecoveryEnd,
    BackendErr,
    Retry,
    BusyReject,
    TraceDrop,
    NotifyWrite,
    NotifyWait,
    NotifyRead,
    PollSpin,
    PollReturn,
    ShardMap,
    ShardPump,
    ShardBackendSubmit,
    ShardBackendComplete,
    RequestShardSubmit,
    RequestShardComplete,
    RequestBdevSubmit,
    RequestBdevComplete,
    RequestParentComplete,
    /* Appended for plan-latency attribution; ids are frozen and only grow. */
    ActorPickup = 51,
    PlanReady = 52,
    /* Appended for per-inode snapshot-cache attribution. */
    SnapHit = 53,
    SnapBuild = 54,
    /* Appended for checkpoint freeze/encode attribution (P2-2). */
    ChkptFreeze = 55,
    /* Appended for metadata lock, operation pool, DMA ownership and frontend
     * submit attribution (review §7.3). */
    MetaLockWait = 56,
    OpPoolAcquire = 57,
    DmaOwns = 58,
    FeSubmitBegin = 59,
    /* Appended for Shard-9.5 write-path barrier attribution: one record per
     * barrier phase plus one record per served sync batch. */
    SyncPhase = 60,
    SyncBatch = 61,
    /* Appended for Shard-9.5 read-path attribution: one record per completed
     * sync data-path I/O with its device range and status. */
    DataIoDone = 62,
};

/* Phase ids carried in SyncPhase.arg0 (append-only, Shard-9.5). */
enum class SyncPhaseId : std::uint64_t {
    JoinWait = 1,       /* joiner waiting for the running batch */
    Window = 2,         /* group-commit coalescing sleep */
    StagedTake = 3,     /* staged metadata record extraction */
    JournalAppend = 4,  /* staged record transaction write */
    PrepareDurable = 5, /* durable-size staging under the namespace lock */
    RequireFull = 6,    /* full-checkpoint requirement under the journal lock */
    Snapshot = 7,       /* checkpoint view freeze + encode */
    CheckpointWrite = 8,/* checkpoint record + superblock device write */
    Flush = 9,          /* durability device flush */
    Publish = 10,       /* durable epoch publication */
};

inline constexpr std::uint16_t kTraceFormatVersion = 2;

/* Fixed-width on-disk event record; offline tools must not depend on ABI types. */
struct TraceRecord {
    std::uint16_t eventId = 0;
    std::uint16_t formatVersion = kTraceFormatVersion;
    std::uint32_t sourceId = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t arg0 = 0;
    std::uint64_t arg1 = 0;
    std::uint64_t arg2 = 0;
    std::uint64_t arg3 = 0;
};

static_assert(sizeof(TraceRecord) == 48, "trace record ABI must stay 48 bytes");

bool enabled() noexcept;
void setEnabled(bool value) noexcept;
void setSample(std::uint32_t sample) noexcept;

/* Starts a bounded trace session and opens trace files below directory. */
bool start(const char *directory) noexcept;

/* Stops the writer, drains all records, and closes the current session. */
void stop() noexcept;

/* Indicates whether a file-backed trace session is active. */
bool sessionActive() noexcept;

/* Record one scalar event. With tracing off this is a single cached-bool check. */
void event(EventId id, std::uint64_t a0 = 0, std::uint64_t a1 = 0,
           std::uint64_t a2 = 0, std::uint64_t a3 = 0) noexcept;

std::uint64_t recorded() noexcept;
std::uint64_t dropped() noexcept;

} // namespace uuse::trace
