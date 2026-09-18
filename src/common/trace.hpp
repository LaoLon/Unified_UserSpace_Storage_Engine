#pragma once

#include <cstdint>
#include <cstddef>

namespace hfs::trace {

/* Tracepoint ids; ids are stable and only appended, per hfs2.0.md §16.5. */
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
};

/* Fixed-width on-disk event record; offline tools must not depend on ABI types. */
struct TraceRecord {
    std::uint16_t eventId = 0;
    std::uint16_t formatVersion = 1;
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

} // namespace hfs::trace
