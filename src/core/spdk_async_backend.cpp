#include "core/spdk_async_backend.hpp"

#include <cerrno>
#include <cstdint>
#include <limits>
#include <new>

namespace hfs {

struct SpdkAsyncDataBackend::Geometry {
    std::uint64_t capacityBytes = 0;
    std::uint32_t nativeBlockSize = 0;
};

struct SpdkAsyncDataBackend::Slot {
    SpdkAsyncDataBackend *backend = nullptr;
    DataIoRequest request;
    CompletionCallback callback = nullptr;
    void *context = nullptr;
    std::uint32_t childIndex = 0;
    std::uint32_t completedBytes = 0;
    std::atomic<bool> inUse{false};
};

/* One accepted request waiting for slot credit; actor-thread private. */
struct SpdkAsyncDataBackend::PendingEntry {
    Request request{};
    CompletionCallback callback = nullptr;
    void *context = nullptr;
    bool write = false;
};

bool SpdkAsyncDataBackend::convertPlannerRequest(const Request &request,
                                                 std::uint32_t plannerBlockSize,
                                                 std::uint32_t nativeBlockSize,
                                                 std::uint64_t capacityBytes,
                                                 NativeRequest &output) noexcept {
    output = {};
    if (plannerBlockSize == 0 || nativeBlockSize == 0 || capacityBytes == 0 ||
        request.buffer == nullptr || request.bytes == 0 || request.spanBytes == 0 ||
        request.bufferBytes < request.spanBytes ||
        request.spanBytes % plannerBlockSize != 0 ||
        request.spanOffset > request.spanBytes ||
        request.bytes > request.spanBytes - request.spanOffset) {
        return false;
    }

    if (request.physicalBlock >
        std::numeric_limits<std::uint64_t>::max() / plannerBlockSize) {
        return false;
    }
    const std::uint64_t physicalByte =
        request.physicalBlock * static_cast<std::uint64_t>(plannerBlockSize);
    const std::uint64_t spanBytes = request.spanBytes;
    if (physicalByte > capacityBytes || spanBytes > capacityBytes - physicalByte ||
        physicalByte % nativeBlockSize != 0 || spanBytes % nativeBlockSize != 0) {
        return false;
    }

    const std::uint64_t blocks = spanBytes / nativeBlockSize;
    if (blocks == 0) {
        return false;
    }
    output.offsetBlocks = physicalByte / nativeBlockSize;
    output.blocks = blocks;
    output.spanBytes = request.spanBytes;
    return true;
}

SpdkAsyncDataBackend::SpdkAsyncDataBackend(SpdkRuntime &runtime,
                                           std::uint32_t reactorIndex,
                                           std::uint32_t plannerBlockSize,
                                           std::size_t maxInFlight)
    : runtime_(runtime), reactorIndex_(reactorIndex), plannerBlockSize_(plannerBlockSize),
      maxInFlight_(maxInFlight) {
    if (plannerBlockSize_ == 0 || maxInFlight_ == 0) {
        return;
    }

    deviceCount_ = runtime_.deviceCount();
    if (deviceCount_ == 0) {
        return;
    }
    geometry_ = std::unique_ptr<Geometry[]>(new (std::nothrow) Geometry[deviceCount_]);
    slots_ = std::unique_ptr<Slot[]>(new (std::nothrow) Slot[maxInFlight_]);
    pending_ = std::unique_ptr<PendingEntry[]>(new (std::nothrow) PendingEntry[maxInFlight_]);
    if (geometry_ == nullptr || slots_ == nullptr || pending_ == nullptr) {
        return;
    }
    pendingCapacity_ = maxInFlight_;
    for (std::size_t index = 0; index < deviceCount_; ++index) {
        geometry_[index].capacityBytes = runtime_.deviceCapacityBytes(index);
        geometry_[index].nativeBlockSize = runtime_.deviceBlockSize(index);
        if (geometry_[index].capacityBytes == 0 || geometry_[index].nativeBlockSize == 0) {
            return;
        }
    }
    for (std::size_t index = 0; index < maxInFlight_; ++index) {
        slots_[index].backend = this;
    }
    valid_ = true;
}

SpdkAsyncDataBackend::~SpdkAsyncDataBackend() noexcept = default;

SpdkAsyncDataBackend::Slot *SpdkAsyncDataBackend::acquireSlot() noexcept {
    if (slots_ == nullptr) {
        return nullptr;
    }
    for (std::size_t index = 0; index < maxInFlight_; ++index) {
        Slot &slot = slots_[index];
        bool expected = false;
        if (slot.inUse.compare_exchange_weak(expected, true, std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
            return &slot;
        }
    }
    return nullptr;
}

bool SpdkAsyncDataBackend::releaseSlot(Slot &slot) noexcept {
    bool expected = true;
    if (!slot.inUse.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
        return false;
    }
    slot.request.stateTag = 0;
    slot.request.traceRequestId = 0;
    slot.request.traceChildIndex = 0;
    slot.callback = nullptr;
    slot.context = nullptr;
    slot.childIndex = 0;
    slot.completedBytes = 0;
    inFlight_.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

int SpdkAsyncDataBackend::normalizeErrno(const Result<void> &result) noexcept {
    switch (result.status()) {
    case Status::InvalidArgument:
        return EINVAL;
    case Status::InvalidState:
        return EINVAL;
    case Status::Busy:
        return EBUSY;
    case Status::NoMemory:
        return ENOMEM;
    case Status::NotSupported:
        return ENOTSUP;
    case Status::Cancelled:
        return ECANCELED;
    case Status::TimedOut:
        return ETIMEDOUT;
    default:
        return EIO;
    }
}

Status SpdkAsyncDataBackend::statusForErrno(int backendErrno) noexcept {
    if (backendErrno < 0) {
        backendErrno = -backendErrno;
    }
    switch (backendErrno) {
    case 0:
        return Status::Ok;
    case EINVAL:
        return Status::InvalidArgument;
    case ENOMEM:
        return Status::NoMemory;
    case EBUSY:
    case EAGAIN:
        return Status::Busy;
    case ECANCELED:
        return Status::Cancelled;
    case ETIMEDOUT:
        return Status::TimedOut;
    case ENOTSUP:
        return Status::NotSupported;
    default:
        return Status::IoError;
    }
}

void SpdkAsyncDataBackend::onRuntimeCompletion(void *context, int backendErrno) noexcept {
    auto *slot = static_cast<Slot *>(context);
    if (slot == nullptr || slot->backend == nullptr) {
        return;
    }
    SpdkAsyncDataBackend *backend = slot->backend;
    const CompletionCallback callback = slot->callback;
    void *const callbackContext = slot->context;
    const std::uint32_t childIndex = slot->childIndex;
    const std::uint32_t completedBytes = slot->completedBytes;
    if (!backend->releaseSlot(*slot)) {
        return;
    }

    if (callback == nullptr) {
        return;
    }
    const int normalizedErrno = backendErrno < 0 ? -backendErrno : backendErrno;
    callback(callbackContext, childIndex, statusForErrno(normalizedErrno), normalizedErrno,
             normalizedErrno == 0 ? completedBytes : 0);
}

AsyncDataBackend::SubmitResult SpdkAsyncDataBackend::submitRead(
    const Request &request, CompletionCallback callback, void *context) noexcept {
    return submit(request, callback, context, false);
}

AsyncDataBackend::SubmitResult SpdkAsyncDataBackend::submitWrite(
    const Request &request, CompletionCallback callback, void *context) noexcept {
    return submit(request, callback, context, true);
}

AsyncDataBackend::SubmitResult SpdkAsyncDataBackend::submit(
    const Request &request, CompletionCallback callback, void *context, bool write) noexcept {
    if (!valid_) {
        return SubmitResult{SubmitDisposition::Rejected, ENODEV};
    }
    if (callback == nullptr || request.deviceId >= deviceCount_) {
        return SubmitResult{SubmitDisposition::Rejected, EINVAL};
    }

    NativeRequest native;
    const Geometry &geometry = geometry_[request.deviceId];
    if (!convertPlannerRequest(request, plannerBlockSize_, geometry.nativeBlockSize,
                               geometry.capacityBytes, native)) {
        return SubmitResult{SubmitDisposition::Rejected, EINVAL};
    }
    if (!runtime_.isDmaCapable(request.buffer, native.spanBytes)) {
        return SubmitResult{SubmitDisposition::Rejected, EINVAL};
    }

    Slot *slot = acquireSlot();
    if (slot == nullptr) {
        return enqueuePending(request, callback, context, write);
    }
    slot->callback = callback;
    slot->context = context;
    slot->childIndex = request.childIndex;
    slot->completedBytes = native.spanBytes;
    slot->request.traceRequestId = request.requestId;
    slot->request.traceChildIndex = request.childIndex;
    inFlight_.fetch_add(1, std::memory_order_acq_rel);

    Result<void> submitted = write
                                 ? runtime_.submitDataWriteOnReactor(
                                       reactorIndex_, request.deviceId, slot->request,
                                       request.buffer, native.offsetBlocks, native.blocks,
                                       &SpdkAsyncDataBackend::onRuntimeCompletion, slot)
                                 : runtime_.submitDataReadOnReactor(
                                       reactorIndex_, request.deviceId, slot->request,
                                       request.buffer, native.offsetBlocks, native.blocks,
                                       &SpdkAsyncDataBackend::onRuntimeCompletion, slot);
    if (!submitted.ok()) {
        (void)releaseSlot(*slot);
        return SubmitResult{SubmitDisposition::Rejected, normalizeErrno(submitted)};
    }
    return SubmitResult{SubmitDisposition::Accepted, 0};
}

AsyncDataBackend::SubmitResult SpdkAsyncDataBackend::enqueuePending(
    const Request &request, CompletionCallback callback, void *context,
    bool write) noexcept {
    if (pendingCapacity_ == 0 || pendingCount_ >= pendingCapacity_) {
        /* Bounded last resort: only a full pending queue is terminal. */
        pendingDropped_.fetch_add(1, std::memory_order_relaxed);
        return SubmitResult{SubmitDisposition::Rejected, ENOMEM};
    }
    const std::size_t index = (pendingHead_ + pendingCount_) % pendingCapacity_;
    PendingEntry &entry = pending_[index];
    entry.request = request;
    entry.callback = callback;
    entry.context = context;
    entry.write = write;
    ++pendingCount_;
    return SubmitResult{SubmitDisposition::WaitingForCredit, 0};
}

void SpdkAsyncDataBackend::submitPending() noexcept {
    while (pendingCount_ != 0) {
        Slot *slot = acquireSlot();
        if (slot == nullptr) {
            return;
        }
        PendingEntry &entry = pending_[pendingHead_];
        const Request request = entry.request;
        const CompletionCallback callback = entry.callback;
        void *const context = entry.context;
        const bool write = entry.write;
        pendingHead_ = (pendingHead_ + 1) % pendingCapacity_;
        --pendingCount_;

        NativeRequest native;
        const Geometry &geometry = geometry_[request.deviceId];
        if (!convertPlannerRequest(request, plannerBlockSize_, geometry.nativeBlockSize,
                                   geometry.capacityBytes, native) ||
            !runtime_.isDmaCapable(request.buffer, native.spanBytes)) {
            (void)releaseSlot(*slot);
            if (callback != nullptr) {
                callback(context, request.childIndex, Status::InvalidArgument, EINVAL, 0);
            }
            continue;
        }

        slot->callback = callback;
        slot->context = context;
        slot->childIndex = request.childIndex;
        slot->completedBytes = native.spanBytes;
        slot->request.traceRequestId = request.requestId;
        slot->request.traceChildIndex = request.childIndex;
        inFlight_.fetch_add(1, std::memory_order_acq_rel);

        Result<void> submitted =
            write ? runtime_.submitDataWriteOnReactor(
                        reactorIndex_, request.deviceId, slot->request, request.buffer,
                        native.offsetBlocks, native.blocks,
                        &SpdkAsyncDataBackend::onRuntimeCompletion, slot)
                  : runtime_.submitDataReadOnReactor(
                        reactorIndex_, request.deviceId, slot->request, request.buffer,
                        native.offsetBlocks, native.blocks,
                        &SpdkAsyncDataBackend::onRuntimeCompletion, slot);
        if (!submitted.ok()) {
            const int backendErrno = normalizeErrno(submitted);
            (void)releaseSlot(*slot);
            if (callback != nullptr) {
                callback(context, request.childIndex, statusForErrno(backendErrno),
                         backendErrno, 0);
            }
        }
    }
}

void SpdkAsyncDataBackend::pump() noexcept { submitPending(); }

} // namespace hfs
