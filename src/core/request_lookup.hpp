#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace hfs {

/* Fixed-capacity requestId lookup with backward-shift deletion. */
class RequestLookupTable final {
public:
    RequestLookupTable() noexcept = default;

    RequestLookupTable(const RequestLookupTable &) = delete;
    RequestLookupTable &operator=(const RequestLookupTable &) = delete;

    bool initialize(std::size_t maximumEntries) noexcept {
        if (maximumEntries == 0 || entries_ != nullptr) {
            return false;
        }
        if (maximumEntries > std::numeric_limits<std::size_t>::max() / 2) {
            return false;
        }
        const std::size_t required = maximumEntries * 2;
        std::size_t capacity = 1;
        while (capacity < required) {
            if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
                return false;
            }
            capacity <<= 1;
        }
        auto storage = std::unique_ptr<Entry[]>(new (std::nothrow) Entry[capacity]());
        if (storage == nullptr) {
            return false;
        }
        entries_ = std::move(storage);
        capacity_ = capacity;
        return true;
    }

    void reset() noexcept {
        entries_.reset();
        capacity_ = 0;
    }

    bool valid() const noexcept { return entries_ != nullptr && capacity_ != 0; }

    bool insert(std::uint64_t requestId, std::uint32_t slotIndex) noexcept {
        if (requestId == 0 || !valid()) {
            return false;
        }
        const std::size_t mask = capacity_ - 1;
        const std::size_t start = lookupHash(requestId) & mask;
        for (std::size_t probe = 0; probe < capacity_; ++probe) {
            Entry &entry = entries_[(start + probe) & mask];
            if (entry.state == State::Empty) {
                entry.requestId = requestId;
                entry.slotIndex = slotIndex;
                entry.state = State::Occupied;
                return true;
            }
            if (entry.requestId == requestId) {
                return false;
            }
        }
        return false;
    }

    bool find(std::uint64_t requestId, std::uint32_t &slotIndex) const noexcept {
        if (requestId == 0 || !valid()) {
            return false;
        }
        const std::size_t mask = capacity_ - 1;
        const std::size_t start = lookupHash(requestId) & mask;
        for (std::size_t probe = 0; probe < capacity_; ++probe) {
            const Entry &entry = entries_[(start + probe) & mask];
            if (entry.state == State::Empty) {
                return false;
            }
            if (entry.requestId == requestId) {
                slotIndex = entry.slotIndex;
                return true;
            }
        }
        return false;
    }

    bool erase(std::uint64_t requestId) noexcept {
        if (requestId == 0 || !valid()) {
            return false;
        }
        const std::size_t mask = capacity_ - 1;
        const std::size_t start = lookupHash(requestId) & mask;
        std::size_t hole = capacity_;
        for (std::size_t probe = 0; probe < capacity_; ++probe) {
            const std::size_t index = (start + probe) & mask;
            const Entry &entry = entries_[index];
            if (entry.state == State::Empty) {
                return false;
            }
            if (entry.requestId == requestId) {
                hole = index;
                break;
            }
        }
        if (hole == capacity_) {
            return false;
        }

        std::size_t scan = (hole + 1) & mask;
        while (entries_[scan].state == State::Occupied) {
            const std::size_t home = lookupHash(entries_[scan].requestId) & mask;
            const bool crossesHole = hole <= scan
                                         ? (home <= hole || home > scan)
                                         : (home <= hole && home > scan);
            if (crossesHole) {
                entries_[hole] = entries_[scan];
                entries_[scan] = Entry{};
                hole = scan;
            }
            scan = (scan + 1) & mask;
        }
        entries_[hole] = Entry{};
        return true;
    }

private:
    enum class State : std::uint8_t {
        Empty,
        Occupied,
    };

    struct Entry {
        State state = State::Empty;
        std::uint64_t requestId = 0;
        std::uint32_t slotIndex = 0;
    };

    static std::size_t lookupHash(std::uint64_t requestId) noexcept {
        requestId ^= requestId >> 30;
        requestId *= 0xbf58476d1ce4e5b9ULL;
        requestId ^= requestId >> 27;
        requestId *= 0x94d049bb133111ebULL;
        requestId ^= requestId >> 31;
        return static_cast<std::size_t>(requestId);
    }

    std::unique_ptr<Entry[]> entries_;
    std::size_t capacity_ = 0;
};

} // namespace hfs
