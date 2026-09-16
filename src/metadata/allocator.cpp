#include "metadata/allocator.hpp"

#include <algorithm>
#include <cstring>

#include "hfs/crc.hpp"

namespace hfs::metadata {

namespace {

constexpr std::uint32_t kSnapshotMagic = 0x4c413248; /* "H2AL" */
constexpr std::uint32_t kSnapshotVersion = 1;
constexpr std::size_t kSnapshotHeaderBytes = Allocator::kSnapshotHeaderBytes;

template <typename T>
void append(std::vector<std::byte> &out, T value) {
    const auto *raw = reinterpret_cast<const std::byte *>(&value);
    out.insert(out.end(), raw, raw + sizeof(T));
}

template <typename T>
T readField(const std::vector<std::byte> &data, std::size_t offset) {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

bool stateValid(std::uint8_t state) {
    return state <= static_cast<std::uint8_t>(ExtentState::Committed);
}

} // namespace

PlacementPicker::PlacementPicker(PlacementPolicy policy, std::uint64_t deviceCount) noexcept
    : policy_(policy), deviceCount_(deviceCount == 0 ? 1 : deviceCount) {}

std::uint64_t PlacementPicker::pick(std::span<const std::uint64_t> freeExtents) noexcept {
    if (freeExtents.empty()) {
        return 0;
    }
    if (policy_ == PlacementPolicy::RoundRobin) {
        const std::uint64_t index = cursor_ % freeExtents.size();
        ++cursor_;
        return index;
    }
    std::uint64_t best = 0;
    for (std::size_t i = 1; i < freeExtents.size(); ++i) {
        if (freeExtents[i] > freeExtents[best]) {
            best = i;
        }
    }
    return best;
}

Allocator::Allocator(const AllocatorConfig &config) : config_(config) {}

Allocator::~Allocator() = default;

Result<std::unique_ptr<Allocator>> Allocator::create(const AllocatorConfig &config) {
    if (config.totalExtents == 0 || config.maxReserveExtents == 0) {
        return makeError<std::unique_ptr<Allocator>>(Status::InvalidArgument,
                                                     "invalid allocator geometry");
    }
    auto allocator = std::unique_ptr<Allocator>(new Allocator(config));
    auto &self = *allocator;
    self.capacityExtents_ =
        config.capacityExtents == 0 ? config.totalExtents : config.capacityExtents;
    if (self.capacityExtents_ > config.totalExtents) {
        return makeError<std::unique_ptr<Allocator>>(Status::InvalidArgument,
                                                     "capacity exceeds total extents");
    }
    const auto total = static_cast<std::size_t>(config.totalExtents);
    self.states_.assign(total, static_cast<std::uint8_t>(ExtentState::Free));
    self.l2_.assign((total + 63) / 64, 0);
    self.l1_.assign((self.l2_.size() + 63) / 64, 0);
    self.freeExtents_ = config.totalExtents;
    self.freeRuns_[0] = config.totalExtents;
    return allocator;
}

bool Allocator::isUsed(std::uint64_t extent) const noexcept {
    const std::uint64_t word = extent / 64;
    const std::uint64_t bit = extent % 64;
    return (l2_[static_cast<std::size_t>(word)] >> bit) & 1ULL;
}

void Allocator::setUsed(std::uint64_t extent, bool used) noexcept {
    const std::uint64_t word = extent / 64;
    const std::uint64_t bit = extent % 64;
    auto &l2 = l2_[static_cast<std::size_t>(word)];
    if (used) {
        l2 |= (1ULL << bit);
        l1_[static_cast<std::size_t>(word / 64)] |= (1ULL << (word % 64));
    } else {
        l2 &= ~(1ULL << bit);
        if (l2 == 0) {
            l1_[static_cast<std::size_t>(word / 64)] &= ~(1ULL << (word % 64));
        }
    }
}

void Allocator::insertRun(std::uint64_t start, std::uint64_t count) {
    auto it = freeRuns_.lower_bound(start);
    if (it != freeRuns_.begin()) {
        auto previous = std::prev(it);
        if (previous->first + previous->second == start) {
            start = previous->first;
            count += previous->second;
            freeRuns_.erase(previous);
        }
    }
    auto next = freeRuns_.lower_bound(start);
    if (next != freeRuns_.end() && start + count == next->first) {
        count += next->second;
        freeRuns_.erase(next);
    }
    freeRuns_[start] = count;
}

void Allocator::splitRunForRange(std::uint64_t start, std::uint64_t count) {
    const std::uint64_t end = start + count;
    auto it = freeRuns_.upper_bound(start);
    if (it != freeRuns_.begin()) {
        auto previous = std::prev(it);
        if (previous->first < start && previous->first + previous->second > start) {
            const std::uint64_t rightLength = previous->first + previous->second - start;
            previous->second = start - previous->first;
            freeRuns_[start] = rightLength;
        }
    }
    it = freeRuns_.upper_bound(end);
    if (it != freeRuns_.begin()) {
        auto previous = std::prev(it);
        if (previous->first < end && previous->first + previous->second > end) {
            const std::uint64_t rightLength = previous->first + previous->second - end;
            previous->second = end - previous->first;
            freeRuns_[end] = rightLength;
        }
    }
    it = freeRuns_.lower_bound(start);
    while (it != freeRuns_.end() && it->first < end) {
        it = freeRuns_.erase(it);
    }
}

Result<std::uint64_t> Allocator::takeRun(std::uint64_t count) {
    auto findRun = [&](std::uint64_t from) {
        auto it = freeRuns_.lower_bound(from);
        for (; it != freeRuns_.end(); ++it) {
            if (it->second >= count) {
                return it;
            }
        }
        for (it = freeRuns_.begin(); it != freeRuns_.end() && it->first < from; ++it) {
            if (it->second >= count) {
                return it;
            }
        }
        return freeRuns_.end();
    };
    auto it = findRun(searchCursor_);
    if (it == freeRuns_.end()) {
        return makeError<std::uint64_t>(Status::NoSpace, "no free run is large enough");
    }
    const std::uint64_t start = it->first;
    const std::uint64_t runLength = it->second;
    freeRuns_.erase(it);
    if (runLength > count) {
        freeRuns_[start + count] = runLength - count;
    }
    searchCursor_ = start + count;
    if (searchCursor_ >= totalExtents()) {
        searchCursor_ = 0;
    }
    return start;
}

Result<ExtentRun> Allocator::reserve(std::uint64_t count) {
    if (count == 0 || count > config_.maxReserveExtents) {
        return makeError<ExtentRun>(Status::InvalidArgument, "invalid reservation size");
    }
    const std::uint64_t used = totalExtents() - freeExtents_;
    if (used + count > capacityExtents_) {
        return makeError<ExtentRun>(Status::NoSpace, "allocator capacity reached");
    }
    auto start = takeRun(count);
    if (!start.ok()) {
        return makeError<ExtentRun>(start.status(), start.message());
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        states_[static_cast<std::size_t>(start.value() + i)] =
            static_cast<std::uint8_t>(ExtentState::Reserved);
        setUsed(start.value() + i, true);
    }
    freeExtents_ -= count;
    return ExtentRun{start.value(), count, config_.deviceId};
}

Result<void> Allocator::markReserved(std::uint64_t start, std::uint64_t count) {
    if (count == 0 || start + count > totalExtents()) {
        return makeError(Status::InvalidArgument, "extent range out of bounds");
    }
    std::uint64_t newlyUsed = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (stateAt(start + i) == ExtentState::Free) {
            ++newlyUsed;
        }
    }
    const std::uint64_t used = totalExtents() - freeExtents_;
    if (used + newlyUsed > capacityExtents_) {
        return makeError(Status::NoSpace, "allocator capacity reached");
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        auto &state = states_[static_cast<std::size_t>(start + i)];
        if (state == static_cast<std::uint8_t>(ExtentState::Free)) {
            state = static_cast<std::uint8_t>(ExtentState::Reserved);
            setUsed(start + i, true);
        }
    }
    if (newlyUsed > 0) {
        splitRunForRange(start, count);
        freeExtents_ -= newlyUsed;
    }
    return Result<void>(Status::Ok);
}

Result<std::vector<ExtentRun>> Allocator::reserveScattered(std::uint64_t count) {
    if (count == 0) {
        return makeError<std::vector<ExtentRun>>(Status::InvalidArgument,
                                                 "zero reservation");
    }
    const std::uint64_t used = totalExtents() - freeExtents_;
    if (used + count > capacityExtents_) {
        return makeError<std::vector<ExtentRun>>(Status::NoSpace, "allocator capacity reached");
    }
    if (freeExtents_ < count) {
        return makeError<std::vector<ExtentRun>>(Status::NoSpace, "not enough free extents");
    }
    std::vector<ExtentRun> runs;
    std::uint64_t remaining = count;
    while (remaining > 0) {
        const std::uint64_t want = std::min(remaining, config_.maxReserveExtents);
        auto run = reserve(want);
        if (!run.ok()) {
            /* roll back the partial reservation to keep the request atomic */
            for (const ExtentRun &taken : runs) {
                release(taken.start, taken.count);
            }
            return makeError<std::vector<ExtentRun>>(run.status(), run.message());
        }
        runs.push_back(run.value());
        remaining -= want;
    }
    return runs;
}

Result<void> Allocator::markInitialized(std::uint64_t start, std::uint64_t count) {
    if (count == 0 || start + count > totalExtents()) {
        return makeError(Status::InvalidArgument, "extent range out of bounds");
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        auto &state = states_[static_cast<std::size_t>(start + i)];
        if (state == static_cast<std::uint8_t>(ExtentState::Free)) {
            return makeError(Status::InvalidArgument, "extent is not reserved");
        }
        if (state == static_cast<std::uint8_t>(ExtentState::Reserved)) {
            state = static_cast<std::uint8_t>(ExtentState::Initialized);
        }
    }
    return Result<void>(Status::Ok);
}

Result<void> Allocator::commit(std::uint64_t start, std::uint64_t count) {
    if (count == 0 || start + count > totalExtents()) {
        return makeError(Status::InvalidArgument, "extent range out of bounds");
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        auto &state = states_[static_cast<std::size_t>(start + i)];
        if (state == static_cast<std::uint8_t>(ExtentState::Free)) {
            return makeError(Status::InvalidArgument, "extent is not reserved");
        }
        state = static_cast<std::uint8_t>(ExtentState::Committed);
    }
    return Result<void>(Status::Ok);
}

Result<void> Allocator::release(std::uint64_t start, std::uint64_t count) {
    if (count == 0 || start + count > totalExtents()) {
        return makeError(Status::InvalidArgument, "extent range out of bounds");
    }
    std::uint64_t freed = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        auto &state = states_[static_cast<std::size_t>(start + i)];
        if (state == static_cast<std::uint8_t>(ExtentState::Free)) {
            continue;
        }
        state = static_cast<std::uint8_t>(ExtentState::Free);
        setUsed(start + i, false);
        ++freed;
    }
    if (freed > 0) {
        freeExtents_ += freed;
        splitRunForRange(start, count);
        insertRun(start, count);
    }
    return Result<void>(Status::Ok);
}

Result<void> Allocator::reclaimReserved() {
    std::uint64_t index = 0;
    while (index < totalExtents()) {
        if (states_[static_cast<std::size_t>(index)] ==
            static_cast<std::uint8_t>(ExtentState::Reserved)) {
            const std::uint64_t start = index;
            while (index < totalExtents() &&
                   states_[static_cast<std::size_t>(index)] ==
                       static_cast<std::uint8_t>(ExtentState::Reserved)) {
                ++index;
            }
            auto released = release(start, index - start);
            if (!released.ok()) {
                return released;
            }
            continue;
        }
        ++index;
    }
    return Result<void>(Status::Ok);
}

ExtentState Allocator::stateAt(std::uint64_t extent) const {
    if (extent >= totalExtents()) {
        return ExtentState::Free;
    }
    return static_cast<ExtentState>(states_[static_cast<std::size_t>(extent)]);
}

Allocator::Stats Allocator::stats() const {
    Stats stats;
    stats.total = totalExtents();
    stats.capacity = capacityExtents_;
    stats.free = freeExtents_;
    stats.freeRuns = freeRuns_.size();
    for (const std::uint8_t state : states_) {
        switch (static_cast<ExtentState>(state)) {
        case ExtentState::Reserved: ++stats.reserved; break;
        case ExtentState::Initialized: ++stats.initialized; break;
        case ExtentState::Committed: ++stats.committed; break;
        case ExtentState::Free: break;
        }
    }
    return stats;
}

bool Allocator::verify() const {
    std::uint64_t freeCount = 0;
    for (std::uint64_t extent = 0; extent < totalExtents(); ++extent) {
        const bool free = stateAt(extent) == ExtentState::Free;
        if (isUsed(extent) == free) {
            return false;
        }
        if (free) {
            ++freeCount;
        }
    }
    if (freeCount != freeExtents_) {
        return false;
    }
    std::uint64_t runExtents = 0;
    std::uint64_t expectedStart = 0;
    for (const auto &[start, count] : freeRuns_) {
        if (start < expectedStart || count == 0) {
            return false;
        }
        for (std::uint64_t i = 0; i < count; ++i) {
            if (stateAt(start + i) != ExtentState::Free) {
                return false;
            }
        }
        runExtents += count;
        expectedStart = start + count;
    }
    return runExtents == freeExtents_;
}

std::vector<std::byte> Allocator::serialize() const {
    std::vector<std::byte> out;
    out.reserve(kSnapshotHeaderBytes + states_.size() + sizeof(std::uint32_t));
    const auto total = totalExtents();
    append(out, kSnapshotMagic);
    append(out, kSnapshotVersion);
    append(out, config_.deviceId);
    append(out, static_cast<std::uint32_t>(0)); /* reserved */
    append(out, total);
    append(out, capacityExtents_);
    append(out, config_.maxReserveExtents);
    append(out, static_cast<std::uint64_t>(states_.size()));
    append(out, static_cast<std::uint64_t>(0)); /* reserved */
    append(out, static_cast<std::uint64_t>(0)); /* reserved */
    const auto *raw = reinterpret_cast<const std::byte *>(states_.data());
    out.insert(out.end(), raw, raw + states_.size());
    const std::uint32_t crc = crc32c(out.data(), out.size());
    append(out, crc);
    return out;
}

Result<void> Allocator::deserialize(std::span<const std::byte> data) {
    if (data.size() < kSnapshotHeaderBytes + sizeof(std::uint32_t)) {
        return makeError(Status::Corrupt, "allocator snapshot is too small");
    }
    std::vector<std::byte> buffer(data.begin(), data.end());
    const std::size_t payloadBytes = buffer.size() - sizeof(std::uint32_t);
    const std::uint32_t storedCrc = readField<std::uint32_t>(buffer, payloadBytes);
    if (crc32c(buffer.data(), payloadBytes) != storedCrc) {
        return makeError(Status::Corrupt, "allocator snapshot CRC mismatch");
    }
    const std::uint32_t magic = readField<std::uint32_t>(buffer, 0);
    const std::uint32_t version = readField<std::uint32_t>(buffer, 4);
    const std::uint32_t deviceId = readField<std::uint32_t>(buffer, 8);
    const std::uint64_t total = readField<std::uint64_t>(buffer, 16);
    const std::uint64_t capacity = readField<std::uint64_t>(buffer, 24);
    const std::uint64_t maxReserve = readField<std::uint64_t>(buffer, 32);
    const std::uint64_t stateCount = readField<std::uint64_t>(buffer, 40);
    if (magic != kSnapshotMagic || version != kSnapshotVersion) {
        return makeError(Status::Corrupt, "allocator snapshot header mismatch");
    }
    if (total == 0 || stateCount != total || capacity > total ||
        kSnapshotHeaderBytes + total > payloadBytes) {
        return makeError(Status::Corrupt, "allocator snapshot geometry mismatch");
    }
    std::vector<std::uint8_t> states(total);
    std::memcpy(states.data(), buffer.data() + kSnapshotHeaderBytes,
                static_cast<std::size_t>(total));
    std::uint64_t freeCount = 0;
    for (const std::uint8_t state : states) {
        if (!stateValid(state)) {
            return makeError(Status::Corrupt, "allocator snapshot has invalid states");
        }
        if (state == static_cast<std::uint8_t>(ExtentState::Free)) {
            ++freeCount;
        }
    }
    config_.deviceId = deviceId;
    config_.maxReserveExtents = maxReserve;
    states_ = std::move(states);
    capacityExtents_ = capacity;
    freeExtents_ = freeCount;
    searchCursor_ = 0;
    l2_.assign((total + 63) / 64, 0);
    l1_.assign((l2_.size() + 63) / 64, 0);
    freeRuns_.clear();
    for (std::uint64_t extent = 0; extent < total; ++extent) {
        if (stateAt(extent) != ExtentState::Free) {
            setUsed(extent, true);
        }
    }
    if (freeCount > 0) {
        std::uint64_t runStart = 0;
        std::uint64_t runCount = 0;
        for (std::uint64_t extent = 0; extent <= total; ++extent) {
            const bool free = extent < total && stateAt(extent) == ExtentState::Free;
            if (free) {
                if (runCount == 0) {
                    runStart = extent;
                }
                ++runCount;
            } else if (runCount > 0) {
                freeRuns_[runStart] = runCount;
                runCount = 0;
            }
        }
    }
    return Result<void>(Status::Ok);
}

} // namespace hfs::metadata
