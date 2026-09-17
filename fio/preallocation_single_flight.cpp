#include "preallocation_single_flight.hpp"

#include <cerrno>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>

namespace hfs::fio {

namespace {

/* Build a helper-generated failure without depending on the HFS Result type. */
PreallocationResult helperFailure(PreallocationResultStatus status,
                                  int backendErrno) noexcept {
    PreallocationResult result;
    result.status = status;
    result.backendErrno = backendErrno;
    return result;
}

/* Normalize callback output before publishing it to all waiters. */
PreallocationResult normalizeOwnerResult(const PreallocationKey &key,
                                         PreallocationOwnerCallback callback,
                                         void *context) noexcept {
    if (callback == nullptr) {
        return helperFailure(PreallocationResultStatus::MissingOwnerCallback, EINVAL);
    }

    PreallocationResult result = callback(key, context);
    if (result.status == PreallocationResultStatus::Success) {
        if (!result.validates(key)) {
            result.status = PreallocationResultStatus::InvalidOwnerResult;
            result.backendErrno = EINVAL;
        }
        return result;
    }

    /* A failed owner must always expose a concrete errno to its waiters. */
    if (result.backendErrno == 0) {
        result.backendErrno = EIO;
    }
    return result;
}

} // namespace

/* Check the file identity and range arithmetic required by a real file key. */
bool PreallocationKey::valid() const noexcept {
    return fileHandle.valid() &&
           offset <= std::numeric_limits<std::uint64_t>::max() - length;
}

/* Compare every component so a generation or inode reuse cannot hit an old entry. */
bool PreallocationKey::operator==(const PreallocationKey &other) const noexcept {
    return engineGeneration == other.engineGeneration &&
           fileHandle.inodeId == other.fileHandle.inodeId &&
           fileHandle.generation == other.fileHandle.generation &&
           offset == other.offset && length == other.length;
}

/* Preserve all evidence fields so owner and waiter observations are identical. */
bool PreallocationResult::operator==(const PreallocationResult &other) const noexcept {
    return status == other.status && backendErrno == other.backendErrno &&
           logicalSize == other.logicalSize && coverageOffset == other.coverageOffset &&
           coverageLength == other.coverageLength &&
           coverageComplete == other.coverageComplete;
}

/* Create the success payload used after allocation and validation. */
PreallocationResult PreallocationResult::makeSuccess(std::uint64_t logicalSize,
                                                      std::uint64_t coverageOffset,
                                                      std::uint64_t coverageLength,
                                                      bool coverageComplete) noexcept {
    PreallocationResult result;
    result.status = PreallocationResultStatus::Success;
    result.logicalSize = logicalSize;
    result.coverageOffset = coverageOffset;
    result.coverageLength = coverageLength;
    result.coverageComplete = coverageComplete;
    return result;
}

/* Create a failed owner result while retaining the backend's errno. */
PreallocationResult PreallocationResult::makeFailure(int backendErrno) noexcept {
    PreallocationResult result;
    result.status = PreallocationResultStatus::OwnerFailed;
    result.backendErrno = backendErrno;
    return result;
}

/* Require complete coverage and a logical EOF that reaches the requested end. */
bool PreallocationResult::validates(const PreallocationKey &key) const noexcept {
    if (status != PreallocationResultStatus::Success || !key.valid() ||
        !coverageComplete) {
        return false;
    }

    const std::uint64_t requestEnd = key.offset + key.length;
    if (logicalSize < requestEnd || coverageOffset > key.offset) {
        return false;
    }

    const std::uint64_t prefix = key.offset - coverageOffset;
    if (coverageLength < prefix) {
        return false;
    }
    return coverageLength - prefix >= key.length;
}

/* One table slot remains stable for the lifetime of the helper. */
struct PreallocationSingleFlight::Impl {
    struct Entry {
        PreallocationKey key{};
        PreallocationState state = PreallocationState::Absent;
        PreallocationResult result{};
        std::size_t waiters = 0;
        std::condition_variable changed;
    };

    explicit Impl(std::size_t requestedCapacity) noexcept {
        if (requestedCapacity == 0) {
            return;
        }
        if (requestedCapacity > PreallocationSingleFlight::kMaximumCapacity) {
            requestedCapacity = PreallocationSingleFlight::kMaximumCapacity;
        }
        entries.reset(new (std::nothrow) Entry[requestedCapacity]);
        if (entries != nullptr) {
            capacity = requestedCapacity;
        }
    }

    /* The caller holds mutex while using these lookup helpers. */
    Entry *find(const PreallocationKey &key) noexcept {
        for (std::size_t index = 0; index < capacity; ++index) {
            Entry &entry = entries[index];
            if (entry.state != PreallocationState::Absent && entry.key == key) {
                return &entry;
            }
        }
        return nullptr;
    }

    /* The caller holds mutex while reserving this free slot. */
    Entry *freeEntry() noexcept {
        for (std::size_t index = 0; index < capacity; ++index) {
            if (entries[index].state == PreallocationState::Absent) {
                return &entries[index];
            }
        }
        return nullptr;
    }

    std::unique_ptr<Entry[]> entries;
    std::size_t capacity = 0;
    std::size_t activeEntries = 0;
    mutable std::mutex mutex;
};

/* Allocate the bounded slot array without throwing into the FIO adapter. */
PreallocationSingleFlight::PreallocationSingleFlight(std::size_t capacity) noexcept
    : impl_(new (std::nothrow) Impl(capacity)) {}

/* Drain the table before releasing stable entry storage. */
PreallocationSingleFlight::~PreallocationSingleFlight() noexcept {
    reset();
}

/* Reserve one owner slot, wait for an existing owner, or return its terminal result. */
PreallocationResult PreallocationSingleFlight::getOrAllocate(
    const PreallocationKey &key, PreallocationOwnerCallback callback,
    void *context) noexcept {
    if (impl_ == nullptr || impl_->capacity == 0) {
        return helperFailure(PreallocationResultStatus::CapacityExhausted, ENOSPC);
    }
    if (!key.valid()) {
        return helperFailure(PreallocationResultStatus::InvalidKey, EINVAL);
    }

    std::unique_lock<std::mutex> lock(impl_->mutex);
    for (;;) {
        Impl::Entry *entry = impl_->find(key);
        if (entry != nullptr) {
            if (entry->state == PreallocationState::Allocating) {
                ++entry->waiters;
                entry->changed.wait(lock, [entry]() noexcept {
                    return entry->state != PreallocationState::Allocating;
                });
                --entry->waiters;
                const PreallocationState state = entry->state;
                const PreallocationResult result = entry->result;
                entry->changed.notify_all();
                if (state == PreallocationState::Ready ||
                    state == PreallocationState::Failed) {
                    return result;
                }
                continue;
            }
            return entry->result;
        }

        entry = impl_->freeEntry();
        if (entry == nullptr) {
            return helperFailure(PreallocationResultStatus::CapacityExhausted, ENOSPC);
        }

        entry->key = key;
        entry->result = PreallocationResult{};
        entry->state = PreallocationState::Allocating;
        entry->waiters = 0;
        ++impl_->activeEntries;

        /* The real allocate operation must never run while holding the table lock. */
        lock.unlock();
        PreallocationResult result = normalizeOwnerResult(key, callback, context);
        lock.lock();

        entry->result = result;
        entry->state = result.ok() ? PreallocationState::Ready
                                   : PreallocationState::Failed;
        lock.unlock();
        entry->changed.notify_all();
        return result;
    }
}

/* Clear one completed entry without invalidating an in-flight owner. */
bool PreallocationSingleFlight::clear(const PreallocationKey &key) noexcept {
    if (impl_ == nullptr || !key.valid()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::Entry *entry = impl_->find(key);
    if (entry == nullptr || entry->state == PreallocationState::Allocating ||
        entry->waiters != 0) {
        return false;
    }
    entry->state = PreallocationState::Absent;
    entry->result = PreallocationResult{};
    entry->key = PreallocationKey{};
    --impl_->activeEntries;
    return true;
}

/* Drop all terminal entries from generations older than the active engine. */
std::size_t PreallocationSingleFlight::resetForGeneration(
    std::uint64_t engineGeneration) noexcept {
    if (impl_ == nullptr) {
        return 0;
    }

    std::unique_lock<std::mutex> lock(impl_->mutex);
    for (;;) {
        bool waiting = false;
        for (std::size_t index = 0; index < impl_->capacity; ++index) {
            Impl::Entry &entry = impl_->entries[index];
            if (entry.state != PreallocationState::Absent &&
                entry.key.engineGeneration != engineGeneration &&
                (entry.state == PreallocationState::Allocating ||
                 entry.waiters != 0)) {
                entry.changed.wait(lock, [&entry]() noexcept {
                    return entry.state != PreallocationState::Allocating &&
                           entry.waiters == 0;
                });
                waiting = true;
                break;
            }
        }
        if (!waiting) {
            break;
        }
    }

    std::size_t removed = 0;
    for (std::size_t index = 0; index < impl_->capacity; ++index) {
        Impl::Entry &entry = impl_->entries[index];
        if (entry.state != PreallocationState::Absent &&
            entry.key.engineGeneration != engineGeneration) {
            entry.state = PreallocationState::Absent;
            entry.result = PreallocationResult{};
            entry.key = PreallocationKey{};
            ++removed;
        }
    }
    impl_->activeEntries -= removed;
    return removed;
}

/* Drain all owners and waiters before dropping every terminal result. */
void PreallocationSingleFlight::reset() noexcept {
    if (impl_ == nullptr) {
        return;
    }

    std::unique_lock<std::mutex> lock(impl_->mutex);
    for (;;) {
        Impl::Entry *busy = nullptr;
        for (std::size_t index = 0; index < impl_->capacity; ++index) {
            Impl::Entry &entry = impl_->entries[index];
            if (entry.state == PreallocationState::Allocating ||
                entry.waiters != 0) {
                busy = &entry;
                break;
            }
        }
        if (busy == nullptr) {
            break;
        }
        busy->changed.wait(lock, [busy]() noexcept {
            return busy->state != PreallocationState::Allocating &&
                   busy->waiters == 0;
        });
    }

    for (std::size_t index = 0; index < impl_->capacity; ++index) {
        Impl::Entry &entry = impl_->entries[index];
        entry.state = PreallocationState::Absent;
        entry.result = PreallocationResult{};
        entry.key = PreallocationKey{};
        entry.waiters = 0;
    }
    impl_->activeEntries = 0;
}

/* Return only a snapshot; this method never creates or reserves a slot. */
PreallocationState PreallocationSingleFlight::stateFor(
    const PreallocationKey &key) const noexcept {
    if (impl_ == nullptr || !key.valid()) {
        return PreallocationState::Absent;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (std::size_t index = 0; index < impl_->capacity; ++index) {
        const Impl::Entry &entry = impl_->entries[index];
        if (entry.state != PreallocationState::Absent && entry.key == key) {
            return entry.state;
        }
    }
    return PreallocationState::Absent;
}

/* Report the number of terminal or allocating entries currently retained. */
std::size_t PreallocationSingleFlight::size() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->activeEntries;
}

/* Report the effective bounded capacity after constructor clamping/allocation. */
std::size_t PreallocationSingleFlight::capacity() const noexcept {
    return impl_ == nullptr ? 0 : impl_->capacity;
}

} // namespace hfs::fio
