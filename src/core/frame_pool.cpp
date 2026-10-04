#include "core/frame_pool.hpp"

#include <limits>

#include "uuse/task.hpp"

namespace uuse {

namespace {

constexpr std::size_t kNoClass = std::numeric_limits<std::size_t>::max();

} // namespace

FramePool &FramePool::instance() noexcept {
    static FramePool pool;
    return pool;
}

void FramePool::setCapacity(std::size_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes != 0 && poolBytes_ == 0) {
        capacityBytes_ = bytes;
    }
}

std::size_t FramePool::classIndexFor(std::size_t size) const noexcept {
    for (std::size_t i = 0; i < kClassSizes.size(); ++i) {
        if (size <= kClassSizes[i]) {
            return i;
        }
    }
    return kNoClass;
}

void *FramePool::allocate(std::size_t size) noexcept {
    const std::size_t index = classIndexFor(size);
    if (index == kNoClass) {
        overflowAllocations_.fetch_add(1, std::memory_order_relaxed);
        return ::operator new(size);
    }
    const std::size_t classSize = kClassSizes[index];
    auto &sizeClass = classes_[index];
    FreeNode *node = nullptr;
    {
        std::lock_guard<std::mutex> lock(sizeClass.mutex);
        if (sizeClass.free != nullptr) {
            node = sizeClass.free;
            sizeClass.free = node->next;
            --sizeClass.count;
        }
    }
    if (node == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (poolBytes_ + classSize > capacityBytes_) {
            overflowAllocations_.fetch_add(1, std::memory_order_relaxed);
            return ::operator new(classSize);
        }
        poolBytes_ += classSize;
    }
    allocations_.fetch_add(1, std::memory_order_relaxed);
    return node != nullptr ? static_cast<void *>(node) : ::operator new(classSize);
}

void FramePool::deallocate(void *ptr, std::size_t size) noexcept {
    if (ptr == nullptr) {
        return;
    }
    const std::size_t index = classIndexFor(size);
    if (index == kNoClass) {
        ::operator delete(ptr);
        return;
    }
    auto &sizeClass = classes_[index];
    auto *node = static_cast<FreeNode *>(ptr);
    {
        std::lock_guard<std::mutex> lock(sizeClass.mutex);
        node->next = sizeClass.free;
        sizeClass.free = node;
        ++sizeClass.count;
    }
    deallocations_.fetch_add(1, std::memory_order_relaxed);
}

bool FramePool::hasBudget(std::size_t size) const noexcept {
    const std::size_t index = classIndexFor(size);
    if (index == kNoClass) {
        return true; /* not pooled; the caller's bound is elsewhere */
    }
    auto &sizeClass = classes_[index];
    {
        std::lock_guard<std::mutex> lock(sizeClass.mutex);
        if (sizeClass.free != nullptr) {
            return true;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return poolBytes_ + kClassSizes[index] <= capacityBytes_;
}

FramePool::Stats FramePool::stats() const noexcept {
    Stats stats;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats.poolBytes = poolBytes_;
        stats.capacityBytes = capacityBytes_;
    }
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        std::lock_guard<std::mutex> lock(classes_[i].mutex);
        stats.freeBlocks[i] = classes_[i].count;
    }
    stats.allocations = allocations_.load(std::memory_order_relaxed);
    stats.deallocations = deallocations_.load(std::memory_order_relaxed);
    stats.overflowAllocations = overflowAllocations_.load(std::memory_order_relaxed);
    return stats;
}

namespace detail {

void *allocateTaskFrame(std::size_t size) noexcept {
    return FramePool::instance().allocate(size);
}

void deallocateTaskFrame(void *ptr, std::size_t size) noexcept {
    FramePool::instance().deallocate(ptr, size);
}

} // namespace detail

} // namespace uuse
