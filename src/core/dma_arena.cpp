#include "core/dma_arena.hpp"

#include <chrono>
#include <limits>

#include "common/trace.hpp"

namespace hfs {

namespace {

/* Checks a non-empty range without forming an overflowing pointer value. */
bool containsRange(std::uintptr_t allocationStart, std::size_t allocationBytes,
                   std::uintptr_t rangeStart, std::size_t rangeBytes) noexcept {
    const auto maxAddress = std::numeric_limits<std::uintptr_t>::max();
    if (rangeBytes == 0 || allocationBytes == 0 ||
        rangeStart > maxAddress - rangeBytes ||
        allocationStart > maxAddress - allocationBytes) {
        return false;
    }
    const auto rangeEnd = rangeStart + rangeBytes;
    const auto allocationEnd = allocationStart + allocationBytes;
    return rangeStart >= allocationStart && rangeEnd <= allocationEnd;
}

} // namespace

std::size_t DmaArena::classSizeFor(std::size_t size) noexcept {
    if (size == 0) {
        return 0;
    }
    for (const std::size_t classSize : kClassSizes) {
        if (size <= classSize) {
            return classSize;
        }
    }
    return 0;
}

void DmaArena::configure(std::size_t capacityBytes, Allocator alloc, Deallocator dealloc,
                         void *ctx) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes_ != 0) {
        return; /* already in use */
    }
    capacityBytes_ = capacityBytes;
    allocator_ = alloc;
    deallocator_ = dealloc;
    allocatorCtx_ = ctx;
}

Result<std::span<std::byte>> DmaArena::acquire(std::size_t size, std::uint32_t timeoutMs) {
    if (size == 0) {
        return makeError<std::span<std::byte>>(Status::InvalidArgument,
                                               "empty DMA request");
    }
    const std::size_t classSize = classSizeFor(size);
    std::unique_lock<std::mutex> lock(mutex_);
    if (allocator_ == nullptr) {
        return makeError<std::span<std::byte>>(Status::InvalidState, "arena not configured");
    }
    if (classSize == 0) {
        if (capacityBytes_ != 0 && size > capacityBytes_) {
            return makeError<std::span<std::byte>>(
                Status::InvalidArgument, "DMA request exceeds arena capacity");
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (capacityBytes_ == 0 ||
                (bytes_ <= capacityBytes_ && size <= capacityBytes_ - bytes_)) {
                bytes_ += size;
                if (bytes_ > highWatermark_) highWatermark_ = bytes_;
                ++acquires_;
                Allocator allocator = allocator_;
                void *allocatorCtx = allocatorCtx_;
                trace::event(trace::EventId::DmaAcquire, size, bytes_);
                lock.unlock();
                void *buffer = allocator(size, allocatorCtx);
                if (buffer == nullptr) {
                    lock.lock();
                    bytes_ -= size;
                    --acquires_;
                    return makeError<std::span<std::byte>>(Status::NoMemory,
                                                           "DMA allocation failed");
                }
                lock.lock();
                live_.emplace(reinterpret_cast<std::uintptr_t>(buffer),
                              LiveEntry{size, static_cast<std::uint8_t>(kClassCount)});
                lock.unlock();
                return std::span<std::byte>(static_cast<std::byte *>(buffer), size);
            }
            ++busy_;
            trace::event(trace::EventId::DmaBusy, size, timeoutMs);
            if (timeoutMs == 0) {
                return makeError<std::span<std::byte>>(Status::Busy, "DMA arena exhausted");
            }
            ++waiters_;
            const bool timedOut = cv_.wait_until(lock, deadline) == std::cv_status::timeout;
            --waiters_;
            if (timedOut) {
                return makeError<std::span<std::byte>>(Status::Busy, "DMA arena exhausted");
            }
        }
    }
    std::size_t classIndex = 0;
    for (std::size_t i = 0; i < kClassCount; ++i) {
        if (kClassSizes[i] == classSize) {
            classIndex = i;
            break;
        }
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        auto &sizeClass = classes_[classIndex];
        if (!sizeClass.free.empty()) {
            void *buffer = sizeClass.free.back();
            sizeClass.free.pop_back();
            ++acquires_;
            live_.emplace(reinterpret_cast<std::uintptr_t>(buffer),
                          LiveEntry{classSize, static_cast<std::uint8_t>(classIndex)});
            trace::event(trace::EventId::DmaAcquire, classSize, bytes_);
            return std::span<std::byte>(static_cast<std::byte *>(buffer), classSize);
        }
        if (capacityBytes_ == 0 ||
            (bytes_ <= capacityBytes_ && classSize <= capacityBytes_ - bytes_)) {
            bytes_ += classSize;
            if (bytes_ > highWatermark_) {
                highWatermark_ = bytes_;
            }
            ++acquires_;
            Allocator allocator = allocator_;
            void *allocatorCtx = allocatorCtx_;
            trace::event(trace::EventId::DmaAcquire, classSize, bytes_);
            lock.unlock();
            void *buffer = allocator(classSize, allocatorCtx);
            if (buffer == nullptr) {
                lock.lock();
                bytes_ -= classSize;
                --acquires_;
                return makeError<std::span<std::byte>>(Status::NoMemory,
                                                       "DMA allocation failed");
            }
            lock.lock();
            live_.emplace(reinterpret_cast<std::uintptr_t>(buffer),
                          LiveEntry{classSize, static_cast<std::uint8_t>(classIndex)});
            lock.unlock();
            return std::span<std::byte>(static_cast<std::byte *>(buffer), classSize);
        }
        ++busy_;
        trace::event(trace::EventId::DmaBusy, classSize, timeoutMs);
        if (timeoutMs == 0) {
            return makeError<std::span<std::byte>>(Status::Busy, "DMA arena exhausted");
        }
        ++waiters_;
        const bool timedOut = cv_.wait_until(lock, deadline) == std::cv_status::timeout;
        --waiters_;
        if (timedOut) {
            return makeError<std::span<std::byte>>(Status::Busy, "DMA arena exhausted");
        }
    }
}

void DmaArena::release(std::span<std::byte> buffer) noexcept {
    if (buffer.empty()) {
        return;
    }
    /* Look up the recorded allocation: callers may release with the requested
     * size instead of the rounded span size. */
    void *pointer = buffer.data();
    Deallocator deallocator = nullptr;
    void *allocatorCtx = nullptr;
    bool large = false;
    bool notifyWaiter = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = live_.find(reinterpret_cast<std::uintptr_t>(pointer));
        if (found == live_.end()) {
            return; /* foreign pointer: not owned by this arena */
        }
        const LiveEntry entry = found->second;
        live_.erase(found);
        if (entry.classIndex < kClassCount) {
            classes_[entry.classIndex].free.push_back(pointer);
        } else {
            bytes_ -= entry.bytes;
            deallocator = deallocator_;
            allocatorCtx = allocatorCtx_;
            large = true;
        }
        ++releases_;
        notifyWaiter = waiters_ != 0;
    }
    if (large && deallocator != nullptr) {
        deallocator(pointer, allocatorCtx);
    }
    if (notifyWaiter) {
        cv_.notify_one();
    }
    trace::event(trace::EventId::DmaRelease, buffer.size());
}

bool DmaArena::owns(const void *buffer) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_.count(reinterpret_cast<std::uintptr_t>(buffer)) != 0;
}

bool DmaArena::ownsRange(const void *ptr, std::size_t bytes) const {
    if (ptr == nullptr || bytes == 0) {
        return false;
    }
    /* Review §7.3: attribute the ownership lookup only while tracing. */
    const bool measure = trace::enabled();
    const auto start = measure ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    const auto rangeStart = reinterpret_cast<std::uintptr_t>(ptr);
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        /* Ordered index: the only candidate is the allocation at or before the
         * range start. */
        auto found = live_.upper_bound(rangeStart);
        if (found != live_.begin()) {
            --found;
            owned = containsRange(found->first, found->second.bytes, rangeStart, bytes);
        }
    }
    if (measure) {
        trace::event(trace::EventId::DmaOwns, bytes, owned ? 1 : 0,
                     static_cast<std::uint64_t>(
                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count()));
    }
    return owned;
}

Result<void> DmaArena::tryAcquireBatch(std::span<const std::size_t> sizes,
                                       std::span<BufferLease> leases) {
    if (sizes.empty() || sizes.size() != leases.size()) {
        return makeError(Status::InvalidArgument, "invalid DMA batch shape");
    }
    for (const BufferLease &lease : leases) {
        if (!lease.span().empty()) {
            return makeError(Status::InvalidArgument, "DMA batch output is not empty");
        }
    }
    for (const std::size_t size : sizes) {
        if (size == 0) {
            return makeError(Status::InvalidArgument, "empty DMA batch request");
        }
    }

    std::size_t acquired = 0;
    for (; acquired < sizes.size(); ++acquired) {
        auto result = acquire(sizes[acquired], 0);
        if (!result.ok()) {
            for (std::size_t i = 0; i < acquired; ++i) {
                leases[i].reset();
            }
            return makeError(result.status(), result.message());
        }
        leases[acquired].arena_ = this;
        leases[acquired].bytes_ = result.value();
    }
    return Result<void>(Status::Ok);
}

DmaArena::~DmaArena() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deallocator_ == nullptr) {
        return;
    }
    /* Free every block the arena still owns: cached class blocks and any
     * allocation whose lease was never returned. A later lease release
     * becomes a harmless no-op. */
    for (auto &sizeClass : classes_) {
        for (void *block : sizeClass.free) {
            deallocator_(block, allocatorCtx_);
        }
        sizeClass.free.clear();
    }
    for (const auto &entry : live_) {
        deallocator_(reinterpret_cast<void *>(entry.first), allocatorCtx_);
    }
    live_.clear();
}

DmaArena::Stats DmaArena::stats() const {    std::lock_guard<std::mutex> lock(mutex_);
    Stats stats;
    stats.bytes = bytes_;
    stats.capacityBytes = capacityBytes_;
    stats.highWatermark = highWatermark_;
    stats.acquires = acquires_;
    stats.releases = releases_;
    stats.busy = busy_;
    return stats;
}

BufferLease::BufferLease(BufferLease &&other) noexcept
    : arena_(other.arena_), bytes_(other.bytes_) {
    other.arena_ = nullptr;
    other.bytes_ = {};
}

BufferLease &BufferLease::operator=(BufferLease &&other) noexcept {
    if (this != &other) {
        reset();
        arena_ = other.arena_;
        bytes_ = other.bytes_;
        other.arena_ = nullptr;
        other.bytes_ = {};
    }
    return *this;
}

BufferLease::~BufferLease() { reset(); }

Result<BufferLease> BufferLease::fromCaller(std::span<std::byte> bytes) {
    if (bytes.empty()) {
        return makeError<BufferLease>(Status::InvalidArgument, "empty caller buffer");
    }
    BufferLease lease;
    lease.bytes_ = bytes;
    return lease;
}

Result<BufferLease> BufferLease::fromArena(DmaArena &arena, std::size_t size,
                                           std::uint32_t timeoutMs) {
    auto slice = arena.acquire(size, timeoutMs);
    if (!slice.ok()) {
        return makeError<BufferLease>(slice.status(), slice.message());
    }
    BufferLease lease;
    lease.arena_ = &arena;
    lease.bytes_ = slice.value();
    return lease;
}

std::span<std::byte> BufferLease::span() const noexcept { return bytes_; }

bool BufferLease::fromArena() const noexcept { return arena_ != nullptr; }

void BufferLease::reset() noexcept {
    if (arena_ != nullptr) {
        arena_->release(bytes_);
        arena_ = nullptr;
    }
    bytes_ = {};
}

} // namespace hfs
