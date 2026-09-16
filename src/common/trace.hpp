#pragma once

#include <cstdint>

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
};

bool enabled() noexcept;
void setEnabled(bool value) noexcept;
void setSample(std::uint32_t sample) noexcept;

/* Record one event. With tracing off this is a single cached-bool check;
 * Phase 2 wires the per-reactor SPSC rings and the offline writer. */
void event(EventId id, std::uint64_t a0 = 0, std::uint64_t a1 = 0,
           std::uint64_t a2 = 0, std::uint64_t a3 = 0) noexcept;

std::uint64_t recorded() noexcept;

} // namespace hfs::trace
