#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/operation.hpp"

namespace uuse {

/* Caller-owned intrusive scheduling node; must stay alive until complete().
 * The scheduler links it into an interval tree plus a FIFO pending list. */
struct SchedulerNode {
    Operation *operation = nullptr;
    std::uint64_t sequence = 0;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    OperationKind kind = OperationKind::Read;
    /* Inode-wide barrier: truncate, size mutation, gap-zero, sync/epoch. */
    bool inodeBarrier = false;
    bool admitted = false;
    bool queued = false;

    /* Intrusive interval tree (ordered by start, pruned by maxEnd). */
    SchedulerNode *left = nullptr;
    SchedulerNode *right = nullptr;
    SchedulerNode *parent = nullptr;
    std::uint64_t maxEnd = 0;

    /* Pending FIFO. */
    SchedulerNode *fifoPrev = nullptr;
    SchedulerNode *fifoNext = nullptr;
};

/* Single actor-owned inode dependency scheduler (uuse.md §7.2). */
class InodeDependencyScheduler {
public:
    using AdmitCallback = void (*)(void *ctx, SchedulerNode &node);

    struct Stats {
        std::uint64_t admits = 0;
        std::uint64_t queued = 0;
        std::uint64_t completes = 0;
        std::uint64_t dependencyWaits = 0;
        std::size_t pendingPeak = 0;
        std::size_t activePeak = 0;
    };

    void setAdmitCallback(AdmitCallback callback, void *ctx) noexcept;

    /* Submit a node: true when admitted immediately, false when queued. */
    bool submit(SchedulerNode &node) noexcept;
    /* Release a node; newly ready pending nodes are admitted (callbacks run). */
    void complete(SchedulerNode &node) noexcept;

    std::size_t pendingCount() const noexcept;
    std::size_t activeCount() const noexcept;
    Stats stats() const noexcept;

private:
    bool canAdmit(const SchedulerNode &node) const noexcept;
    bool conflicts(const SchedulerNode &node) const noexcept;
    bool searchConflict(const SchedulerNode *subtree,
                        const SchedulerNode &candidate) const noexcept;
    void admit(SchedulerNode &node) noexcept;

    void insertTree(SchedulerNode &node) noexcept;
    void removeTree(SchedulerNode &node) noexcept;
    void refreshMaxEnd(SchedulerNode *node) noexcept;
    SchedulerNode *minimum(SchedulerNode *node) noexcept;
    void transplant(SchedulerNode &node, SchedulerNode *replacement) noexcept;

    void fifoPush(SchedulerNode &node) noexcept;
    void fifoRemove(SchedulerNode &node) noexcept;
    void flushAdmissions() noexcept;

    AdmitCallback callback_ = nullptr;
    void *callbackCtx_ = nullptr;

    SchedulerNode *treeRoot_ = nullptr;
    SchedulerNode *fifoHead_ = nullptr;
    SchedulerNode *fifoTail_ = nullptr;
    SchedulerNode *pendingBarrier_ = nullptr;
    SchedulerNode *admittedBarrier_ = nullptr;

    std::uint64_t sequence_ = 0;
    std::size_t pending_ = 0;
    std::size_t active_ = 0;
    Stats stats_;
    std::vector<SchedulerNode *> admittedScratch_;
    bool flushing_ = false;
};

/* Files are pinned to one data shard: hash(file_id) % reactorCount. */
std::uint32_t shardForFile(std::uint64_t fileId, std::uint32_t reactorCount) noexcept;

} // namespace uuse
