#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "uuse/file.hpp"

namespace uuse::fio {

using FileHandle = ::uuse::FileHandle;

/* The bounded identity of one shared-file preallocation request. */
struct PreallocationKey {
    std::uint64_t engineGeneration = 0;
    FileHandle fileHandle{};
    std::uint64_t offset = 0;
    std::uint64_t length = 0;

    bool valid() const noexcept;
    bool operator==(const PreallocationKey &other) const noexcept;
};

/* The lifecycle state held by one bounded table entry. */
enum class PreallocationState : std::uint8_t {
    Absent,
    Allocating,
    Ready,
    Failed,
};

/* Result details retained for both the owner and every waiter. */
enum class PreallocationResultStatus : std::uint8_t {
    Success,
    OwnerFailed,
    InvalidKey,
    InvalidOwnerResult,
    CapacityExhausted,
    MissingOwnerCallback,
};

/* Allocation evidence is deliberately independent of fio and SPDK headers. */
struct PreallocationResult {
    PreallocationResultStatus status = PreallocationResultStatus::OwnerFailed;
    int backendErrno = 0;
    std::uint64_t logicalSize = 0;
    std::uint64_t coverageOffset = 0;
    std::uint64_t coverageLength = 0;
    bool coverageComplete = false;

    /* Construct a successful result with explicit logical and coverage evidence. */
    static PreallocationResult makeSuccess(std::uint64_t logicalSize,
                                            std::uint64_t coverageOffset,
                                            std::uint64_t coverageLength,
                                            bool coverageComplete = true) noexcept;

    /* Construct an owner failure carrying the backend errno for fio mapping. */
    static PreallocationResult makeFailure(int backendErrno) noexcept;

    bool ok() const noexcept {
        return status == PreallocationResultStatus::Success;
    }

    /* Validate the evidence against the exact range represented by a key. */
    bool validates(const PreallocationKey &key) const noexcept;

    bool operator==(const PreallocationResult &other) const noexcept;
};

/* The only callback that may execute the real allocation operation. */
using PreallocationOwnerCallback =
    PreallocationResult (*)(const PreallocationKey &key, void *context) noexcept;

/*
 * Process-level single-flight table for the fio open/preallocation phase.
 *
 * The table has a fixed maximum capacity.  A callback runs outside the table
 * mutex, while waiters block only on the Allocating entry's condition variable.
 */
class PreallocationSingleFlight {
public:
    static constexpr std::size_t kMaximumCapacity = 4096;

    explicit PreallocationSingleFlight(std::size_t capacity) noexcept;
    ~PreallocationSingleFlight() noexcept;

    PreallocationSingleFlight(const PreallocationSingleFlight &) = delete;
    PreallocationSingleFlight &operator=(const PreallocationSingleFlight &) = delete;

    /* Run one owner callback or return the retained result to a waiter. */
    PreallocationResult getOrAllocate(const PreallocationKey &key,
                                      PreallocationOwnerCallback callback,
                                      void *context) noexcept;

    /* Alias with the same single-flight semantics for call sites using ensure wording. */
    PreallocationResult ensure(const PreallocationKey &key,
                               PreallocationOwnerCallback callback,
                               void *context) noexcept {
        return getOrAllocate(key, callback, context);
    }

    /* Remove one terminal entry; returns false while its owner is still active. */
    bool clear(const PreallocationKey &key) noexcept;

    /* Remove entries from older engine generations after they have drained. */
    std::size_t resetForGeneration(std::uint64_t engineGeneration) noexcept;

    /* Wait for active owners/waiters, then release every retained entry. */
    void reset() noexcept;

    /* Snapshot the state of one key without creating an entry. */
    PreallocationState stateFor(const PreallocationKey &key) const noexcept;

    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uuse::fio
