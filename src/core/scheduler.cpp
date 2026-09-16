#include "core/scheduler.hpp"

#include "common/trace.hpp"

namespace hfs {

namespace {

bool readsShare(OperationKind left, OperationKind right) noexcept {
    return left == OperationKind::Read && right == OperationKind::Read;
}

} // namespace

std::uint32_t shardForFile(std::uint64_t fileId, std::uint32_t reactorCount) noexcept {
    if (reactorCount == 0) {
        return 0;
    }
    std::uint64_t hash = fileId * 0x9E3779B97F4A7C15ULL;
    hash ^= hash >> 33;
    hash *= 0xFF51AFD7ED558CCDULL;
    hash ^= hash >> 33;
    return static_cast<std::uint32_t>(hash % reactorCount);
}

void InodeDependencyScheduler::setAdmitCallback(AdmitCallback callback,
                                                void *ctx) noexcept {
    callback_ = callback;
    callbackCtx_ = ctx;
}

bool InodeDependencyScheduler::canAdmit(const SchedulerNode &node) const noexcept {
    if (admittedBarrier_ != nullptr) {
        return false;
    }
    if (pendingBarrier_ != nullptr && pendingBarrier_->sequence < node.sequence) {
        return false; /* nothing submitted after a queued barrier may pass it */
    }
    if (node.inodeBarrier) {
        /* A barrier may start only on an empty tree and at the queue head. */
        return treeRoot_ == nullptr && (fifoHead_ == nullptr || fifoHead_ == &node);
    }
    return !conflicts(node);
}

bool InodeDependencyScheduler::conflicts(const SchedulerNode &node) const noexcept {
    return searchConflict(treeRoot_, node);
}

bool InodeDependencyScheduler::searchConflict(
    const SchedulerNode *subtree, const SchedulerNode &candidate) const noexcept {
    if (subtree == nullptr) {
        return false;
    }
    if (subtree->maxEnd <= candidate.start) {
        return false; /* nothing in this subtree reaches the candidate range */
    }
    if (subtree->start < candidate.end && candidate.start < subtree->end &&
        !readsShare(subtree->kind, candidate.kind)) {
        return true;
    }
    if (searchConflict(subtree->left, candidate)) {
        return true;
    }
    if (subtree->start >= candidate.end) {
        return false;
    }
    return searchConflict(subtree->right, candidate);
}

bool InodeDependencyScheduler::submit(SchedulerNode &node) noexcept {
    node.sequence = ++sequence_;
    node.admitted = false;
    node.queued = false;
    if (canAdmit(node)) {
        admit(node);
        flushAdmissions();
        return true;
    }
    fifoPush(node);
    node.queued = true;
    ++pending_;
    if (pending_ > stats_.pendingPeak) {
        stats_.pendingPeak = pending_;
    }
    ++stats_.queued;
    ++stats_.dependencyWaits;
    if (node.inodeBarrier && pendingBarrier_ == nullptr) {
        pendingBarrier_ = &node;
    }
    trace::event(trace::EventId::SchedWait, node.sequence);
    return false;
}

void InodeDependencyScheduler::complete(SchedulerNode &node) noexcept {
    ++stats_.completes;
    if (node.queued) {
        fifoRemove(node);
        --pending_;
        if (pendingBarrier_ == &node) {
            pendingBarrier_ = nullptr;
        }
    } else if (node.admitted) {
        removeTree(node);
        --active_;
        if (admittedBarrier_ == &node) {
            admittedBarrier_ = nullptr;
        }
    }
    node.admitted = false;
    node.queued = false;

    /* Greedy re-admission in FIFO order; a queued barrier blocks later nodes. */
    for (SchedulerNode *current = fifoHead_; current != nullptr;) {
        SchedulerNode *next = current->fifoNext;
        if (canAdmit(*current)) {
            fifoRemove(*current);
            --pending_;
            if (pendingBarrier_ == current) {
                pendingBarrier_ = nullptr;
            }
            admit(*current);
            if (current->inodeBarrier) {
                break;
            }
        } else {
            ++stats_.dependencyWaits;
            if (current->inodeBarrier) {
                break; /* later nodes cannot pass it */
            }
        }
        current = next;
    }
    trace::event(trace::EventId::SchedDone, node.sequence);
    flushAdmissions();
}

void InodeDependencyScheduler::admit(SchedulerNode &node) noexcept {
    node.admitted = true;
    node.queued = false;
    insertTree(node);
    ++active_;
    if (active_ > stats_.activePeak) {
        stats_.activePeak = active_;
    }
    if (node.inodeBarrier) {
        admittedBarrier_ = &node;
    }
    ++stats_.admits;
    trace::event(trace::EventId::SchedAdmit, node.sequence);
    admittedScratch_.push_back(&node);
}

void InodeDependencyScheduler::flushAdmissions() noexcept {
    if (flushing_) {
        return;
    }
    flushing_ = true;
    while (!admittedScratch_.empty()) {
        SchedulerNode *node = admittedScratch_.back();
        admittedScratch_.pop_back();
        if (callback_ != nullptr) {
            callback_(callbackCtx_, *node);
        }
    }
    flushing_ = false;
}

void InodeDependencyScheduler::insertTree(SchedulerNode &node) noexcept {
    node.left = nullptr;
    node.right = nullptr;
    node.parent = nullptr;
    node.maxEnd = node.end;
    if (treeRoot_ == nullptr) {
        treeRoot_ = &node;
        return;
    }
    SchedulerNode *current = treeRoot_;
    for (;;) {
        if (current->maxEnd < node.end) {
            current->maxEnd = node.end;
        }
        if (node.start < current->start) {
            if (current->left == nullptr) {
                current->left = &node;
                node.parent = current;
                break;
            }
            current = current->left;
        } else {
            if (current->right == nullptr) {
                current->right = &node;
                node.parent = current;
                break;
            }
            current = current->right;
        }
    }
}

void InodeDependencyScheduler::transplant(SchedulerNode &node,
                                          SchedulerNode *replacement) noexcept {
    if (node.parent == nullptr) {
        treeRoot_ = replacement;
    } else if (node.parent->left == &node) {
        node.parent->left = replacement;
    } else {
        node.parent->right = replacement;
    }
    if (replacement != nullptr) {
        replacement->parent = node.parent;
    }
}

SchedulerNode *InodeDependencyScheduler::minimum(SchedulerNode *node) noexcept {
    while (node->left != nullptr) {
        node = node->left;
    }
    return node;
}

void InodeDependencyScheduler::refreshMaxEnd(SchedulerNode *node) noexcept {
    while (node != nullptr) {
        std::uint64_t maximum = node->end;
        if (node->left != nullptr && node->left->maxEnd > maximum) {
            maximum = node->left->maxEnd;
        }
        if (node->right != nullptr && node->right->maxEnd > maximum) {
            maximum = node->right->maxEnd;
        }
        node->maxEnd = maximum;
        node = node->parent;
    }
}

void InodeDependencyScheduler::removeTree(SchedulerNode &node) noexcept {
    SchedulerNode *refreshFrom = nullptr;
    if (node.left == nullptr) {
        refreshFrom = node.parent;
        transplant(node, node.right);
    } else if (node.right == nullptr) {
        refreshFrom = node.parent;
        transplant(node, node.left);
    } else {
        SchedulerNode *successor = minimum(node.right);
        if (successor->parent != &node) {
            refreshFrom = successor->parent;
            transplant(*successor, successor->right);
            successor->right = node.right;
            successor->right->parent = successor;
        } else {
            refreshFrom = successor;
        }
        transplant(node, successor);
        successor->left = node.left;
        successor->left->parent = successor;
    }
    node.left = nullptr;
    node.right = nullptr;
    node.parent = nullptr;
    refreshMaxEnd(refreshFrom);
}

void InodeDependencyScheduler::fifoPush(SchedulerNode &node) noexcept {
    node.fifoPrev = fifoTail_;
    node.fifoNext = nullptr;
    if (fifoTail_ != nullptr) {
        fifoTail_->fifoNext = &node;
    } else {
        fifoHead_ = &node;
    }
    fifoTail_ = &node;
}

void InodeDependencyScheduler::fifoRemove(SchedulerNode &node) noexcept {
    if (node.fifoPrev != nullptr) {
        node.fifoPrev->fifoNext = node.fifoNext;
    } else {
        fifoHead_ = node.fifoNext;
    }
    if (node.fifoNext != nullptr) {
        node.fifoNext->fifoPrev = node.fifoPrev;
    } else {
        fifoTail_ = node.fifoPrev;
    }
    node.fifoPrev = nullptr;
    node.fifoNext = nullptr;
}

std::size_t InodeDependencyScheduler::pendingCount() const noexcept { return pending_; }

std::size_t InodeDependencyScheduler::activeCount() const noexcept { return active_; }

InodeDependencyScheduler::Stats InodeDependencyScheduler::stats() const noexcept {
    return stats_;
}

} // namespace hfs
