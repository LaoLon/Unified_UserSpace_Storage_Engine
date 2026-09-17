#include "core/dma_arena.hpp"

#include <chrono>

#include "common/trace.hpp"

namespace hfs {

std::size_t DmaArena::classSizeFor(std::size_t size) noexcept {
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
                largeLive_.emplace(buffer, size);
                lock.unlock();
                return std::span<std::byte>(static_cast<std::byte *>(buffer), size);
            }
            ++busy_;
            trace::event(trace::EventId::DmaBusy, size, timeoutMs);
            if (timeoutMs == 0 || cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
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
            live_.insert(buffer);
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
            live_.insert(buffer);
            lock.unlock();
            return std::span<std::byte>(static_cast<std::byte *>(buffer), classSize);
        }
        ++busy_;
        trace::event(trace::EventId::DmaBusy, classSize, timeoutMs);
        if (timeoutMs == 0 || cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            return makeError<std::span<std::byte>>(Status::Busy, "DMA arena exhausted");
        }
    }
}

void DmaArena::release(std::span<std::byte> buffer) noexcept {
    if (buffer.empty()) {
        return;
    }
    std::size_t classIndex = kClassCount;
    for (std::size_t i = 0; i < kClassCount; ++i) {
        if (buffer.size() == kClassSizes[i]) {
            classIndex = i;
            break;
        }
    }
    if (classIndex == kClassCount) {
        Deallocator deallocator = nullptr;
        void *allocatorCtx = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = largeLive_.find(buffer.data());
            if (found == largeLive_.end()) return;
            bytes_ -= found->second;
            largeLive_.erase(found);
            ++releases_;
            deallocator = deallocator_;
            allocatorCtx = allocatorCtx_;
        }
        if (deallocator != nullptr) deallocator(buffer.data(), allocatorCtx);
        cv_.notify_one();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        classes_[classIndex].free.push_back(buffer.data());
        live_.erase(buffer.data());
        ++releases_;
    }
    trace::event(trace::EventId::DmaRelease, buffer.size());
    cv_.notify_one();
}

bool DmaArena::owns(const void *buffer) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_.count(buffer) != 0 || largeLive_.count(const_cast<void *>(buffer)) != 0;
}

DmaArena::~DmaArena() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deallocator_ == nullptr) {
        return;
    }
    for (auto &sizeClass : classes_) {
        for (void *block : sizeClass.free) {
            deallocator_(block, allocatorCtx_);
        }
        sizeClass.free.clear();
    }
    for (const auto &entry : largeLive_) {
        deallocator_(entry.first, allocatorCtx_);
    }
    largeLive_.clear();
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
