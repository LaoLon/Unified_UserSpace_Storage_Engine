#include "core/async_data_service.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "core/actor.hpp"
#include "common/trace.hpp"
#include "core/dma_arena.hpp"
#include "core/extent_map.hpp"
#include "core/parent_completion.hpp"
#include "core/scheduler.hpp"
#include "core/shard_ring.hpp"

namespace hfs {

class AsyncDataService::Impl {
private:
    static constexpr std::uint32_t kNoLease = std::numeric_limits<std::uint32_t>::max();

    struct RequestState;
    struct ShardState;
    struct InodeSlot;

    /* Fixed-capacity extent map captured before the request is published. */
    struct ExtentSnapshotMap final : ExtentMap {
        static constexpr std::size_t kCapacity = IoPlanner::Plan::kMaxChildren;

        std::array<ExtentView, kCapacity> extents{};
        std::size_t count = 0;

        void reset() noexcept { count = 0; }

        Result<void> capture(const ExtentMap &source, std::uint64_t offset,
                             std::uint64_t length) {
            count = 0;
            auto result = source.lookup(
                offset, length, std::span<ExtentView>(extents.data(), extents.size()));
            if (!result.ok()) {
                return makeError(result.status(), "extent snapshot lookup failed");
            }
            if (result.value() > extents.size()) {
                return makeError(Status::NoMemory, "extent snapshot capacity exceeded");
            }
            count = result.value();
            return Result<void>(Status::Ok);
        }

        Result<std::size_t> lookup(std::uint64_t offset, std::uint64_t length,
                                   std::span<ExtentView> out) const override {
            if (length == 0 ||
                offset > std::numeric_limits<std::uint64_t>::max() - length) {
                return makeError<std::size_t>(Status::InvalidArgument,
                                              "invalid extent snapshot range");
            }
            const std::uint64_t end = offset + length;
            std::size_t copied = 0;
            for (std::size_t index = 0; index < count; ++index) {
                const ExtentView &extent = extents[index];
                if (extent.length == 0 ||
                    extent.logicalOffset >
                        std::numeric_limits<std::uint64_t>::max() - extent.length) {
                    return makeError<std::size_t>(Status::InvalidArgument,
                                                  "invalid extent snapshot entry");
                }
                const std::uint64_t extentEnd = extent.logicalOffset + extent.length;
                if (extentEnd <= offset) {
                    continue;
                }
                if (extent.logicalOffset >= end) {
                    break;
                }
                if (copied >= out.size()) {
                    return makeError<std::size_t>(Status::NoMemory,
                                                  "extent lookup buffer too small");
                }
                const std::uint64_t clippedStart =
                    std::max(extent.logicalOffset, offset);
                const std::uint64_t clippedEnd = std::min(extentEnd, end);
                out[copied] = ExtentView{
                    clippedStart,
                    clippedEnd - clippedStart,
                    extent.physicalByte + (clippedStart - extent.logicalOffset),
                    extent.deviceId};
                ++copied;
            }
            return copied;
        }
    };

    /* One callback-visible node is embedded in every child. */
    struct ReadyNode {
        ReadyNode *next = nullptr;
        RequestState *state = nullptr;
        std::uint32_t childIndex = 0;
    };

    struct ChildState {
        enum class Phase : std::uint8_t {
            Idle,
            Data,
            ReadOld,
            WriteMerged,
        };

        struct CallbackContext {
            RequestState *state = nullptr;
            std::uint32_t childIndex = 0;
        };

        IoPlanner::Child plan{};
        BufferLease *lease = nullptr;
        std::uint32_t leaseIndex = kNoLease;
        Phase phase = Phase::Idle;
        bool submitted = false;
        std::atomic<bool> terminal{false};
        std::atomic<bool> readyQueued{false};
        std::atomic<int> completionStatus{static_cast<int>(Status::IoError)};
        std::atomic<int> completionErrno{0};
        std::atomic<std::uint32_t> completionBytes{0};
        ReadyNode ready{};
        CallbackContext callbackContext{};

        /* Returns the child to its pool-safe initial state. */
        void reset() noexcept {
            plan = {};
            lease = nullptr;
            leaseIndex = kNoLease;
            phase = Phase::Idle;
            submitted = false;
            terminal.store(false, std::memory_order_relaxed);
            readyQueued.store(false, std::memory_order_relaxed);
            completionStatus.store(static_cast<int>(Status::IoError),
                                   std::memory_order_relaxed);
            completionErrno.store(0, std::memory_order_relaxed);
            completionBytes.store(0, std::memory_order_relaxed);
            ready = {};
            callbackContext = {};
        }
    };

    struct RequestState {
        Impl *owner = nullptr;
        Operation *operation = nullptr;
        AsyncDataSink *sink = nullptr;
        AsyncDataRequest request{};
        ExtentSnapshotMap extentSnapshot{};
        std::uint32_t shard = 0;
        ShardState *shardState = nullptr;
        InodeDependencyScheduler *scheduler = nullptr;
        InodeSlot *inodeSlot = nullptr;
        SchedulerNode schedulerNode{};
        IoPlanner::Plan plan{};
        std::array<ChildState, IoPlanner::Plan::kMaxChildren> children{};
        std::array<std::size_t, IoPlanner::Plan::kMaxChildren> leaseSizes{};
        std::array<BufferLease, IoPlanner::Plan::kMaxChildren> leases{};
        ParentCompletion parent;
        std::uint32_t childCount = 0;
        std::uint32_t leaseCount = 0;
        std::uint32_t submittedChildren = 0;
        std::uint32_t drainedChildren = 0;
        std::atomic<bool> inUse{false};
        bool planned = false;
        bool parked = false;
        bool startingChildren = false;
        bool allChildrenDone = false;
        bool firstErrorSet = false;
        Status firstStatus = Status::Ok;
        int firstErrno = 0;

        /* Clears all borrowed resources after every backend callback drained. */
        void reset() noexcept {
            for (BufferLease &lease : leases) {
                lease.reset();
            }
            for (ChildState &child : children) {
                child.reset();
            }
            owner = nullptr;
            operation = nullptr;
            sink = nullptr;
            request = {};
            extentSnapshot.reset();
            shard = 0;
            shardState = nullptr;
            scheduler = nullptr;
            inodeSlot = nullptr;
            schedulerNode = {};
            plan = {};
            leaseSizes.fill(0);
            childCount = 0;
            leaseCount = 0;
            submittedChildren = 0;
            drainedChildren = 0;
            planned = false;
            parked = false;
            startingChildren = false;
            allChildrenDone = false;
            firstErrorSet = false;
            firstStatus = Status::Ok;
            firstErrno = 0;
        }
    };

    class StatePool {
    public:
        StatePool() noexcept = default;

        bool initialize(std::size_t capacity) noexcept {
            if (capacity == 0 || states_ != nullptr) {
                return false;
            }
            auto states = std::unique_ptr<RequestState[]>(new (std::nothrow)
                                                               RequestState[capacity]);
            if (states == nullptr) {
                return false;
            }
            states_ = std::move(states);
            capacity_ = capacity;
            for (std::size_t index = 0; index < capacity_; ++index) {
                states_[index].inUse.store(false, std::memory_order_relaxed);
            }
            return true;
        }

        RequestState *acquire() noexcept {
            if (states_ == nullptr) {
                return nullptr;
            }
            for (std::size_t index = 0; index < capacity_; ++index) {
                bool expected = false;
                if (states_[index].inUse.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    states_[index].reset();
                    return &states_[index];
                }
            }
            return nullptr;
        }

        void release(RequestState *state) noexcept {
            if (state == nullptr) {
                return;
            }
            state->reset();
            state->inUse.store(false, std::memory_order_release);
        }

    private:
        std::unique_ptr<RequestState[]> states_;
        std::size_t capacity_ = 0;
    };

    /* Per-shard inode table and actor-owned preparation FIFO. */
    struct InodeSlot {
        bool used = false;
        FileId fileId = 0;
        InodeDependencyScheduler scheduler;
    };

    struct ShardState {
        std::unique_ptr<ShardRing> ring;
        std::unique_ptr<ShardActor> actor;
        std::unique_ptr<InodeSlot[]> inodes;
        std::unique_ptr<RequestState *[]> preparing;
        std::size_t preparingCapacity = 0;
        std::size_t preparingHead = 0;
        std::size_t preparingTail = 0;
        std::size_t preparingCount = 0;
        std::atomic<ReadyNode *> readyHead{nullptr};
        AsyncDataBackend *backend = nullptr;
    };

public:
    Impl(const AsyncDataServiceConfig &config, DmaArena &arena,
         std::span<AsyncDataBackend *> backends) noexcept
        : config_(config), arena_(arena), planner_(config.planner) {
        if (config_.shardCount == 0 || config_.ringDepth == 0 ||
            config_.maxOperations == 0 || config_.maxInodesPerShard == 0 ||
            backends.size() < config_.shardCount ||
            !statePool_.initialize(config_.maxOperations)) {
            return;
        }

        shards_ = std::unique_ptr<ShardState[]>(new (std::nothrow)
                                                     ShardState[config_.shardCount]);
        if (shards_ == nullptr) {
            return;
        }

        const std::size_t preparingCapacity =
            config_.maxPreparingPerShard == 0 ? config_.maxOperations
                                              : config_.maxPreparingPerShard;
        if (preparingCapacity == 0) {
            return;
        }

        for (std::uint32_t shard = 0; shard < config_.shardCount; ++shard) {
            ShardState &state = shards_[shard];
            state.backend = backends[shard];
            if (state.backend == nullptr) {
                return;
            }
            state.ring = std::unique_ptr<ShardRing>(new (std::nothrow)
                                                        ShardRing(config_.ringDepth));
            state.inodes = std::unique_ptr<InodeSlot[]>(
                new (std::nothrow) InodeSlot[config_.maxInodesPerShard]);
            state.preparing = std::unique_ptr<RequestState *[]>(
                new (std::nothrow) RequestState *[preparingCapacity]);
            if (state.ring == nullptr || state.inodes == nullptr ||
                state.preparing == nullptr) {
                return;
            }
            state.preparingCapacity = preparingCapacity;
            state.actor = std::unique_ptr<ShardActor>(new (std::nothrow) ShardActor(
                *state.ring, &Impl::actorStep, this,
                std::max<std::size_t>(1, std::min(config_.ringDepth, std::size_t{64}))));
            if (state.actor == nullptr) {
                return;
            }
            for (std::size_t inode = 0; inode < config_.maxInodesPerShard; ++inode) {
                state.inodes[inode].scheduler.setAdmitCallback(&Impl::onAdmitted, this);
            }
        }
        valid_ = true;
    }

    ~Impl() = default;

    bool valid() const noexcept { return valid_; }

    Result<IoHandle> submit(const AsyncDataRequest &request, CompletionToken token,
                            AsyncDataSink &sink) {
        const Status validation = validateRequest(request, token);
        if (validation != Status::Ok) {
            return makeError<IoHandle>(validation, "invalid asynchronous data request");
        }
        if (!valid_) {
            return makeError<IoHandle>(Status::InvalidState,
                                       "asynchronous data service is not initialized");
        }
        if (!sink.tryAcquireCredit()) {
            busy_.fetch_add(1, std::memory_order_relaxed);
            return makeError<IoHandle>(Status::Busy, "client outstanding credit exhausted");
        }

        const std::uint64_t requestId =
            nextRequestId_.fetch_add(1, std::memory_order_relaxed);
        Operation *operation = Operation::create(requestId);
        if (operation == nullptr) {
            sink.releaseCredit();
            busy_.fetch_add(1, std::memory_order_relaxed);
            return makeError<IoHandle>(Status::Busy, "operation pool exhausted");
        }
        RequestState *state = statePool_.acquire();
        if (state == nullptr) {
            operation->release();
            sink.releaseCredit();
            busy_.fetch_add(1, std::memory_order_relaxed);
            return makeError<IoHandle>(Status::Busy, "async sidecar pool exhausted");
        }

        /* Capture the borrowed extent map while the caller still owns it. */
        auto snapshot = state->extentSnapshot.capture(*request.extentMap, request.offset,
                                                       request.length);
        if (!snapshot.ok()) {
            operation->release();
            statePool_.release(state);
            sink.releaseCredit();
            return makeError<IoHandle>(snapshot.status(),
                                       "extent map snapshot failed before publish");
        }

        const std::uint32_t shard = shardForFile(request.fileId, config_.shardCount);
        trace::event(trace::EventId::ShardMap, request.fileId, shard,
                     request.write ? 1U : 0U, requestId);
        state->owner = this;
        state->operation = operation;
        state->sink = &sink;
        state->request = request;
        state->request.extentMap = &state->extentSnapshot;
        state->shard = shard;
        state->shardState = &shards_[shard];

        operation->kind = request.write ? OperationKind::Write : OperationKind::Read;
        operation->requestId = requestId;
        operation->fileId = request.fileId;
        operation->offset = request.offset;
        operation->length = request.length;
        operation->shard = shard;
        operation->setCompletionToken(token);
        operation->setCompletionSink(
            CompletionSink{state, &Impl::publishOperationCompletion});
        operation->asyncState().token = token;
        operation->asyncState().backendState = state;
        operation->asyncState().childCount = 0;

        /* Establish both actor and frontend references before ring publish. */
        operation->acquireRef(OperationRefRole::Actor);
        IoHandle handle = operation->acquireFrontendHandle(token);
        if (!handle.valid()) {
            operation->releaseRef(OperationRefRole::Actor);
            operation->asyncState().backendState = nullptr;
            statePool_.release(state);
            operation->release();
            sink.releaseCredit();
            busy_.fetch_add(1, std::memory_order_relaxed);
            return makeError<IoHandle>(Status::Busy, "frontend handle pool exhausted");
        }

        ShardState &shardState = shards_[shard];
        const ShardRing::Reservation reservation = shardState.ring->reserve();
        if (!reservation.valid()) {
            handle.reset();
            operation->releaseRef(OperationRefRole::Actor);
            operation->asyncState().backendState = nullptr;
            statePool_.release(state);
            operation->release();
            sink.releaseCredit();
            busy_.fetch_add(1, std::memory_order_relaxed);
            return makeError<IoHandle>(Status::Busy, "shard ring is full");
        }

        operation->state.store(OperationState::Enqueued, std::memory_order_release);
        active_.fetch_add(1, std::memory_order_acq_rel);
        shardState.ring->publish(reservation, operation);

        /* The ring's actor reference is now the sole non-frontend owner. */
        operation->release();
        accepted_.fetch_add(1, std::memory_order_relaxed);
        return Result<IoHandle>(std::move(handle));
    }

    std::size_t pump(std::uint32_t shard) noexcept {
        if (!valid_ || shard >= config_.shardCount) {
            return 0;
        }
        ShardState &state = shards_[shard];
        std::size_t work = state.actor->runOnce();
        work += retryPreparing(state);
        work += drainReady(state);
        if (trace::enabled()) {
            const ShardRing::Stats ringStats = state.ring->stats();
            trace::event(trace::EventId::ShardPump, shard, work,
                         ringStats.highWatermark, state.preparingCount);
        }
        return work;
    }

    std::size_t pumpAll() noexcept {
        if (!valid_) {
            return 0;
        }
        std::size_t work = 0;
        for (std::uint32_t shard = 0; shard < config_.shardCount; ++shard) {
            work += pump(shard);
        }
        return work;
    }

    AsyncDataService::Stats stats() const noexcept {
        AsyncDataService::Stats result;
        result.accepted = accepted_.load(std::memory_order_relaxed);
        result.busy = busy_.load(std::memory_order_relaxed);
        result.terminal = terminal_.load(std::memory_order_relaxed);
        result.completionPublished = completionPublished_.load(std::memory_order_relaxed);
        result.planFailures = planFailures_.load(std::memory_order_relaxed);
        result.dmaFailures = dmaFailures_.load(std::memory_order_relaxed);
        result.preparing = preparing_.load(std::memory_order_relaxed);
        result.schedulerWaits = schedulerWaits_.load(std::memory_order_relaxed);
        result.childSubmits = childSubmits_.load(std::memory_order_relaxed);
        result.childCompletes = childCompletes_.load(std::memory_order_relaxed);
        result.tokenEvents = tokenEvents_.load(std::memory_order_relaxed);
        return result;
    }

    std::size_t outstanding() const noexcept {
        return active_.load(std::memory_order_acquire);
    }

    std::size_t shardCount() const noexcept { return config_.shardCount; }

private:
    static Status validateRequest(const AsyncDataRequest &request,
                                  CompletionToken token) noexcept {
        if (!token.valid() || request.extentMap == nullptr || request.length == 0 ||
            request.buffer.data() == nullptr || request.buffer.size() < request.length ||
            request.offset > std::numeric_limits<std::uint64_t>::max() - request.length) {
            return Status::InvalidArgument;
        }
        return Status::Ok;
    }

    static Status statusForBackendErrno(int backendErrno) noexcept {
        switch (backendErrno) {
        case 0:
            return Status::IoError;
        case EINVAL:
            return Status::InvalidArgument;
        case ENOSPC:
            return Status::NoSpace;
        case ENOMEM:
            return Status::NoMemory;
        case ECANCELED:
            return Status::Cancelled;
        case ETIMEDOUT:
            return Status::TimedOut;
        default:
            return Status::IoError;
        }
    }

    static bool isRmw(IoPlanner::Child::Kind kind) noexcept {
        return kind == IoPlanner::Child::Kind::HeadRmw ||
               kind == IoPlanner::Child::Kind::TailRmw;
    }

    static bool isReadPhase(const RequestState &state,
                            const ChildState &child) noexcept {
        /* RMW always starts with ReadOld. A direct Data phase follows the
         * parent direction and must not turn ordinary reads into writes. */
        return child.phase == ChildState::Phase::ReadOld ||
               (child.phase == ChildState::Phase::Data && !state.request.write);
    }

    static void actorStep(void *context, Operation *operation) noexcept {
        auto *self = static_cast<Impl *>(context);
        if (self != nullptr) {
            self->acceptFromRing(operation);
        }
    }

    static void onAdmitted(void *context, SchedulerNode &node) noexcept {
        auto *self = static_cast<Impl *>(context);
        if (self == nullptr || node.operation == nullptr) {
            return;
        }
        auto *state = static_cast<RequestState *>(node.operation->asyncState().backendState);
        if (state != nullptr && state->owner == self) {
            self->startChildren(*state);
        }
    }

    /* Backend callbacks only record a value and push the embedded ready node. */
    static void backendCompletion(void *context, std::uint32_t childIndex,
                                  Status status, int backendErrno,
                                  std::uint32_t completedBytes) noexcept {
        auto *callbackContext = static_cast<ChildState::CallbackContext *>(context);
        if (callbackContext == nullptr || callbackContext->state == nullptr ||
            callbackContext->childIndex != childIndex) {
            return;
        }
        RequestState *state = callbackContext->state;
        if (state->owner == nullptr || childIndex >= state->childCount) {
            return;
        }
        ChildState &child = state->children[childIndex];
        trace::event(trace::EventId::ShardBackendComplete, state->request.fileId,
                     state->shard, childIndex,
                     static_cast<std::uint64_t>(completedBytes));
        if (child.terminal.load(std::memory_order_acquire)) {
            return;
        }
        child.completionStatus.store(static_cast<int>(status), std::memory_order_relaxed);
        child.completionErrno.store(backendErrno, std::memory_order_relaxed);
        child.completionBytes.store(completedBytes, std::memory_order_relaxed);

        bool expected = false;
        if (!child.readyQueued.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
            return;
        }
        ReadyNode *node = &child.ready;
        node->state = state;
        node->childIndex = childIndex;
        if (state->shardState == nullptr) {
            return;
        }
        ReadyNode *head = state->shardState->readyHead.load(std::memory_order_relaxed);
        do {
            node->next = head;
        } while (!state->shardState->readyHead.compare_exchange_weak(
            head, node, std::memory_order_release, std::memory_order_acquire));
    }

    static void publishOperationCompletion(void *context,
                                           const CompletionEvent &event) noexcept {
        auto *state = static_cast<RequestState *>(context);
        if (state == nullptr || state->sink == nullptr) {
            return;
        }
        state->sink->publish(state->shard, event);
        state->owner->completionPublished_.fetch_add(1, std::memory_order_relaxed);
        state->owner->tokenEvents_.fetch_add(1, std::memory_order_relaxed);
    }

    static void parentDone(void *context, int, std::uint64_t) noexcept {
        auto *state = static_cast<RequestState *>(context);
        if (state == nullptr || state->owner == nullptr) {
            return;
        }
        state->allChildrenDone = true;
        if (!state->startingChildren) {
            state->owner->finish(*state, state->firstErrorSet ? state->firstStatus : Status::Ok,
                                 state->firstErrorSet ? state->firstErrno : 0);
        }
    }

    void acceptFromRing(Operation *operation) noexcept {
        if (operation == nullptr) {
            return;
        }
        auto *state = static_cast<RequestState *>(operation->asyncState().backendState);
        if (state == nullptr || state->owner != this || state->operation != operation) {
            return;
        }
        if (!operation->beginPreparing()) {
            finish(*state, Status::Corrupt, 0);
            return;
        }
        prepare(*state);
    }

    void prepare(RequestState &state) noexcept {
        if (!state.planned) {
            const std::uint64_t end = state.request.offset + state.request.length;
            if (state.request.write &&
                (!state.request.fixedOverwrite || end > state.request.logicalSize)) {
                finish(state, Status::InvalidArgument, EINVAL);
                return;
            }
            auto planned = planner_.plan(state.extentSnapshot, state.request.offset,
                                         state.request.length, state.request.write,
                                         !state.request.write);
            if (!planned.ok()) {
                planFailures_.fetch_add(1, std::memory_order_relaxed);
                finish(state, planned.status(), 0);
                return;
            }
            state.plan = std::move(planned).value();
            state.childCount = state.plan.count;
            state.planned = true;
            state.operation->asyncState().childCount = state.childCount;
            state.parent.init(state.childCount, &Impl::parentDone, &state);
            state.operation->asyncState().parent = &state.parent;
            state.operation->parent = &state.parent;
            if (state.childCount == 0) {
                planFailures_.fetch_add(1, std::memory_order_relaxed);
                finish(state, Status::NotFound, 0);
                return;
            }
            for (std::uint32_t index = 0; index < state.childCount; ++index) {
                state.children[index].plan = state.plan.children[index];
                state.children[index].callbackContext.state = &state;
                state.children[index].callbackContext.childIndex = index;
                state.children[index].ready.state = &state;
                state.children[index].ready.childIndex = index;
            }
        }

        const Status dmaStatus = acquireLeases(state);
        if (dmaStatus == Status::Busy) {
            if (!parkPreparing(state)) {
                dmaFailures_.fetch_add(1, std::memory_order_relaxed);
                finish(state, Status::NoMemory, ENOMEM);
            }
            return;
        }
        if (dmaStatus != Status::Ok) {
            dmaFailures_.fetch_add(1, std::memory_order_relaxed);
            finish(state, dmaStatus, dmaStatus == Status::NoMemory ? ENOMEM : 0);
            return;
        }

        InodeSlot *inode = findInode(state.shard, state.request.fileId);
        if (inode == nullptr) {
            finish(state, Status::NoMemory, ENOMEM);
            return;
        }
        state.scheduler = &inode->scheduler;
        state.inodeSlot = inode;
        state.schedulerNode.operation = state.operation;
        state.schedulerNode.start = state.request.offset;
        state.schedulerNode.end = state.request.offset + state.request.length;
        state.schedulerNode.kind =
            state.request.write ? OperationKind::Write : OperationKind::Read;
        state.schedulerNode.inodeBarrier = false;
        state.operation->schedulerNode = &state.schedulerNode;
        state.operation->asyncState().schedulerNode = &state.schedulerNode;

        const bool admitted = state.scheduler->submit(state.schedulerNode);
        if (!admitted && !state.operation->isTerminal()) {
            state.operation->state.store(OperationState::WaitingDependency,
                                         std::memory_order_release);
            schedulerWaits_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    Status acquireLeases(RequestState &state) noexcept {
        state.leaseCount = 0;
        state.leaseSizes.fill(0);
        for (std::uint32_t index = 0; index < state.childCount; ++index) {
            ChildState &child = state.children[index];
            child.lease = nullptr;
            child.leaseIndex = kNoLease;
            const IoPlanner::Child &plan = child.plan;
            if (plan.spanBytes == 0 ||
                static_cast<std::uint64_t>(plan.bufferOffset) + plan.bytes >
                    state.request.buffer.size()) {
                return Status::InvalidArgument;
            }
            const std::byte *userBuffer = state.request.buffer.data() + plan.bufferOffset;
            const bool needsBounce = isRmw(plan.kind) ||
                                     !arena_.ownsRange(userBuffer, plan.spanBytes);
            if (!needsBounce) {
                continue;
            }
            if (state.leaseCount >= state.leases.size()) {
                return Status::NoMemory;
            }
            child.leaseIndex = state.leaseCount;
            state.leaseSizes[state.leaseCount++] = plan.spanBytes;
        }

        if (state.leaseCount == 0) {
            return Status::Ok;
        }
        auto acquired = arena_.tryAcquireBatch(
            std::span<const std::size_t>(state.leaseSizes.data(), state.leaseCount),
            std::span<BufferLease>(state.leases.data(), state.leaseCount));
        if (!acquired.ok()) {
            state.leaseCount = 0;
            return acquired.status();
        }
        for (std::uint32_t index = 0; index < state.childCount; ++index) {
            ChildState &child = state.children[index];
            if (child.leaseIndex != kNoLease) {
                child.lease = &state.leases[child.leaseIndex];
            }
        }
        return Status::Ok;
    }

    InodeSlot *findInode(std::uint32_t shard, FileId fileId) noexcept {
        ShardState &state = shards_[shard];
        InodeSlot *freeSlot = nullptr;
        for (std::size_t index = 0; index < config_.maxInodesPerShard; ++index) {
            InodeSlot &inode = state.inodes[index];
            if (inode.used && inode.fileId == fileId) {
                return &inode;
            }
            if (!inode.used && freeSlot == nullptr) {
                freeSlot = &inode;
            }
        }
        if (freeSlot == nullptr) {
            return nullptr;
        }
        freeSlot->used = true;
        freeSlot->fileId = fileId;
        return freeSlot;
    }

    /* Reclaims an empty inode scheduler after its last node is completed. */
    static void reclaimInodeSlot(RequestState &state) noexcept {
        if (state.inodeSlot == nullptr || state.scheduler == nullptr) {
            return;
        }
        if (state.scheduler->activeCount() == 0 &&
            state.scheduler->pendingCount() == 0) {
            state.inodeSlot->used = false;
            state.inodeSlot->fileId = 0;
        }
    }

    bool parkPreparing(RequestState &state) noexcept {
        ShardState &shard = shards_[state.shard];
        if (state.parked) {
            return true;
        }
        if (shard.preparingCount >= shard.preparingCapacity) {
            return false;
        }
        shard.preparing[shard.preparingTail] = &state;
        shard.preparingTail = (shard.preparingTail + 1) % shard.preparingCapacity;
        ++shard.preparingCount;
        state.parked = true;
        preparing_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    RequestState *popPreparing(ShardState &shard) noexcept {
        if (shard.preparingCount == 0) {
            return nullptr;
        }
        RequestState *state = shard.preparing[shard.preparingHead];
        shard.preparing[shard.preparingHead] = nullptr;
        shard.preparingHead = (shard.preparingHead + 1) % shard.preparingCapacity;
        --shard.preparingCount;
        if (state != nullptr) {
            state->parked = false;
            preparing_.fetch_sub(1, std::memory_order_relaxed);
        }
        return state;
    }

    std::size_t retryPreparing(ShardState &shard) noexcept {
        const std::size_t attempts = shard.preparingCount;
        std::size_t work = 0;
        for (std::size_t index = 0; index < attempts; ++index) {
            RequestState *state = popPreparing(shard);
            if (state == nullptr) {
                continue;
            }
            ++work;
            prepare(*state);
        }
        return work;
    }

    void startChildren(RequestState &state) noexcept {
        if (state.operation->isTerminal()) {
            return;
        }
        state.operation->state.store(OperationState::Ready, std::memory_order_release);
        state.operation->state.store(OperationState::Submitted, std::memory_order_release);
        state.startingChildren = true;
        for (std::uint32_t index = 0; index < state.childCount; ++index) {
            ChildState &child = state.children[index];
            if (child.plan.kind == IoPlanner::Child::Kind::Zero) {
                std::byte *destination = state.request.buffer.data() + child.plan.bufferOffset;
                std::memset(destination, 0, child.plan.bytes);
                completeChild(state, index, Status::Ok, 0, child.plan.bytes);
                continue;
            }
            child.phase = isRmw(child.plan.kind)
                              ? (state.request.write ? ChildState::Phase::ReadOld
                                                      : ChildState::Phase::ReadOld)
                              : ChildState::Phase::Data;
            submitCurrentPhase(state, index);
        }
        state.startingChildren = false;
        if (state.allChildrenDone) {
            finish(state, state.firstErrorSet ? state.firstStatus : Status::Ok,
                   state.firstErrorSet ? state.firstErrno : 0);
        }
    }

    AsyncDataBackend::Request backendRequest(const RequestState &state,
                                             const ChildState &child) const noexcept {
        AsyncDataBackend::Request request;
        request.write = child.phase == ChildState::Phase::WriteMerged ||
                        (child.phase == ChildState::Phase::Data && state.request.write);
        request.rmw = isRmw(child.plan.kind);
        request.physicalBlock = child.plan.physicalBlock;
        request.deviceId = child.plan.deviceId;
        request.bytes = child.plan.bytes;
        request.spanBytes = child.plan.spanBytes;
        request.spanOffset = child.plan.spanOffset;
        request.childIndex = static_cast<std::uint32_t>(&child - state.children.data());
        if (child.lease != nullptr) {
            request.buffer = child.lease->span().data();
            request.bufferBytes = child.lease->span().size();
        } else {
            request.buffer = state.request.buffer.data() + child.plan.bufferOffset;
            request.bufferBytes = child.plan.spanBytes;
        }
        return request;
    }

    void submitCurrentPhase(RequestState &state, std::uint32_t childIndex) noexcept {
        ChildState &child = state.children[childIndex];
        if (child.terminal.load(std::memory_order_acquire)) {
            return;
        }
        if (child.lease != nullptr && state.request.write &&
            (child.phase == ChildState::Phase::Data ||
             child.phase == ChildState::Phase::WriteMerged)) {
            const auto target = child.lease->span().first(child.plan.spanBytes);
            const auto source = state.request.buffer.subspan(child.plan.bufferOffset,
                                                               child.plan.bytes);
            if (child.plan.spanOffset + child.plan.bytes > target.size()) {
                completeChild(state, childIndex, Status::InvalidArgument, EINVAL, 0);
                return;
            }
            std::memcpy(target.data() + child.plan.spanOffset, source.data(), source.size());
        }

        child.submitted = true;
        const AsyncDataBackend::Request request = backendRequest(state, child);
        trace::event(trace::EventId::ShardBackendSubmit, state.request.fileId,
                     state.shard, childIndex, request.bytes);
        const AsyncDataBackend::SubmitResult result =
            isReadPhase(state, child)
                ? state.owner->shards_[state.shard].backend->submitRead(
                      request, &Impl::backendCompletion, &child.callbackContext)
                : state.owner->shards_[state.shard].backend->submitWrite(
                      request, &Impl::backendCompletion, &child.callbackContext);
        if (result.disposition == AsyncDataBackend::SubmitDisposition::Rejected) {
            child.submitted = false;
            const Status status = result.backendErrno == 0
                                      ? Status::IoError
                                      : statusForBackendErrno(result.backendErrno);
            completeChild(state, childIndex, status, result.backendErrno, 0);
            return;
        }
        ++state.submittedChildren;
        state.operation->asyncState().submittedChildren = state.submittedChildren;
        childSubmits_.fetch_add(1, std::memory_order_relaxed);
    }

    void handleReady(RequestState &state, std::uint32_t childIndex) noexcept {
        if (childIndex >= state.childCount) {
            return;
        }
        ChildState &child = state.children[childIndex];
        child.readyQueued.store(false, std::memory_order_release);
        if (child.terminal.load(std::memory_order_acquire)) {
            return;
        }
        child.submitted = false;
        const Status callbackStatus = static_cast<Status>(
            child.completionStatus.load(std::memory_order_acquire));
        const int backendErrno = child.completionErrno.load(std::memory_order_acquire);
        const std::uint32_t completedBytes =
            child.completionBytes.load(std::memory_order_acquire);
        Status status = callbackStatus;
        if (status == Status::Ok && backendErrno != 0) {
            status = statusForBackendErrno(backendErrno);
        }
        if (status != Status::Ok) {
            completeChild(state, childIndex, status, backendErrno, 0);
            return;
        }

        if (child.phase == ChildState::Phase::ReadOld && state.request.write &&
            isRmw(child.plan.kind)) {
            child.phase = ChildState::Phase::WriteMerged;
            submitCurrentPhase(state, childIndex);
            return;
        }

        if (child.lease != nullptr && !state.request.write) {
            const auto source = child.lease->span().first(child.plan.spanBytes);
            if (child.plan.spanOffset + child.plan.bytes > source.size()) {
                completeChild(state, childIndex, Status::InvalidArgument, EINVAL, 0);
                return;
            }
            auto destination = state.request.buffer.subspan(child.plan.bufferOffset,
                                                             child.plan.bytes);
            std::memcpy(destination.data(), source.data() + child.plan.spanOffset,
                        destination.size());
        }
        if (child.lease != nullptr && !isRmw(child.plan.kind) && state.request.write) {
            /* The write copy was performed before submit; no copy-out is needed. */
        }
        const std::uint32_t logicalBytes =
            std::min<std::uint32_t>(completedBytes, child.plan.bytes);
        completeChild(state, childIndex, Status::Ok, 0, logicalBytes);
    }

    void completeChild(RequestState &state, std::uint32_t childIndex, Status status,
                       int backendErrno, std::uint32_t bytes) noexcept {
        ChildState &child = state.children[childIndex];
        bool expected = false;
        if (!child.terminal.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return;
        }
        child.submitted = false;
        ++state.drainedChildren;
        state.operation->asyncState().drainedChildren = state.drainedChildren;
        if (status != Status::Ok && !state.firstErrorSet) {
            state.firstErrorSet = true;
            state.firstStatus = status;
            state.firstErrno = backendErrno;
        }
        childCompletes_.fetch_add(1, std::memory_order_relaxed);
        const int parentError = status == Status::Ok ? 0 : static_cast<int>(status) + 1;
        state.parent.childDone(parentError, bytes);
    }

    std::size_t drainReady(ShardState &shard) noexcept {
        ReadyNode *list = shard.readyHead.exchange(nullptr, std::memory_order_acq_rel);
        if (list == nullptr) {
            return 0;
        }
        std::size_t count = 0;
        ReadyNode *reverse = nullptr;
        while (list != nullptr) {
            ReadyNode *next = list->next;
            list->next = reverse;
            reverse = list;
            list = next;
        }
        while (reverse != nullptr) {
            ReadyNode *node = reverse;
            reverse = reverse->next;
            RequestState *state = node->state;
            if (state != nullptr && state->owner == this && state->shardState == &shard) {
                handleReady(*state, node->childIndex);
                ++count;
            }
        }
        return count;
    }

    void finish(RequestState &state, Status status, int backendErrno) noexcept {
        if (state.operation == nullptr || state.operation->isTerminal()) {
            return;
        }
        if (status == Status::Ok && state.firstErrorSet) {
            status = state.firstStatus;
            backendErrno = state.firstErrno;
        }
        const std::uint64_t bytes = state.parent.completedBytes();
        if (!state.operation->completeTerminal(status, backendErrno, bytes)) {
            return;
        }
        terminal_.fetch_add(1, std::memory_order_relaxed);
        if (state.scheduler != nullptr &&
            (state.schedulerNode.admitted || state.schedulerNode.queued)) {
            state.scheduler->complete(state.schedulerNode);
        }
        reclaimInodeSlot(state);
        state.operation->asyncState().schedulerNode = nullptr;
        state.operation->schedulerNode = nullptr;
        state.operation->publishCompletion();
        state.operation->asyncState().backendState = nullptr;
        state.operation->asyncState().parent = nullptr;
        state.operation->parent = nullptr;
        Operation *operation = state.operation;
        statePool_.release(&state);
        active_.fetch_sub(1, std::memory_order_acq_rel);
        operation->releaseRef(OperationRefRole::Actor);
    }

    AsyncDataServiceConfig config_;
    DmaArena &arena_;
    IoPlanner planner_;
    StatePool statePool_;
    std::unique_ptr<ShardState[]> shards_;
    std::atomic<std::uint64_t> nextRequestId_{1};
    std::atomic<bool> valid_{false};
    std::atomic<std::size_t> active_{0};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> busy_{0};
    std::atomic<std::uint64_t> terminal_{0};
    std::atomic<std::uint64_t> completionPublished_{0};
    std::atomic<std::uint64_t> planFailures_{0};
    std::atomic<std::uint64_t> dmaFailures_{0};
    std::atomic<std::uint64_t> preparing_{0};
    std::atomic<std::uint64_t> schedulerWaits_{0};
    std::atomic<std::uint64_t> childSubmits_{0};
    std::atomic<std::uint64_t> childCompletes_{0};
    std::atomic<std::uint64_t> tokenEvents_{0};
};

AsyncDataService::AsyncDataService(const AsyncDataServiceConfig &config, DmaArena &arena,
                                   std::span<AsyncDataBackend *> backends) noexcept
    : impl_(std::unique_ptr<Impl>(new (std::nothrow) Impl(config, arena, backends))) {}

AsyncDataService::~AsyncDataService() = default;

bool AsyncDataService::valid() const noexcept { return impl_ != nullptr && impl_->valid(); }

Result<IoHandle> AsyncDataService::submit(const AsyncDataRequest &request,
                                          CompletionToken token, AsyncDataSink &sink) {
    if (impl_ == nullptr) {
        return makeError<IoHandle>(Status::InvalidState, "service has no implementation");
    }
    return impl_->submit(request, token, sink);
}

std::size_t AsyncDataService::pump(std::uint32_t shard) noexcept {
    return impl_ == nullptr ? 0 : impl_->pump(shard);
}

std::size_t AsyncDataService::pumpAll() noexcept {
    return impl_ == nullptr ? 0 : impl_->pumpAll();
}

AsyncDataService::Stats AsyncDataService::stats() const noexcept {
    return impl_ == nullptr ? Stats{} : impl_->stats();
}

std::size_t AsyncDataService::outstanding() const noexcept {
    return impl_ == nullptr ? 0 : impl_->outstanding();
}

std::size_t AsyncDataService::shardCount() const noexcept {
    return impl_ == nullptr ? 0 : impl_->shardCount();
}

} // namespace hfs
