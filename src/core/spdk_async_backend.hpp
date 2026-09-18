#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "core/async_data_service.hpp"
#include "runtime/spdk_runtime.hpp"

namespace hfs {

/* Actor-local SPDK backend with construction-time geometry and request pools. */
class SpdkAsyncDataBackend final : public AsyncDataBackend {
public:
    struct NativeRequest {
        std::uint64_t offsetBlocks = 0;
        std::uint64_t blocks = 0;
        std::uint32_t spanBytes = 0;
    };

    /* Converts planner-block coordinates into one native-device request. */
    static bool convertPlannerRequest(const Request &request,
                                      std::uint32_t plannerBlockSize,
                                      std::uint32_t nativeBlockSize,
                                      std::uint64_t capacityBytes,
                                      NativeRequest &output) noexcept;

    SpdkAsyncDataBackend(SpdkRuntime &runtime, std::uint32_t reactorIndex,
                         std::uint32_t plannerBlockSize, std::size_t maxInFlight);
    ~SpdkAsyncDataBackend() noexcept override;

    SpdkAsyncDataBackend(const SpdkAsyncDataBackend &) = delete;
    SpdkAsyncDataBackend &operator=(const SpdkAsyncDataBackend &) = delete;

    bool valid() const noexcept { return valid_; }
    std::size_t capacity() const noexcept { return maxInFlight_; }
    std::size_t inFlight() const noexcept {
        return inFlight_.load(std::memory_order_acquire);
    }

    SubmitResult submitRead(const Request &request, CompletionCallback callback,
                            void *context) noexcept override;
    SubmitResult submitWrite(const Request &request, CompletionCallback callback,
                             void *context) noexcept override;

private:
    struct Geometry;
    struct Slot;

    static void onRuntimeCompletion(void *context, int backendErrno) noexcept;

    SubmitResult submit(const Request &request, CompletionCallback callback,
                        void *context, bool write) noexcept;
    Slot *acquireSlot() noexcept;
    bool releaseSlot(Slot &slot) noexcept;

    static Status statusForErrno(int backendErrno) noexcept;
    static int normalizeErrno(const Result<void> &result) noexcept;

    SpdkRuntime &runtime_;
    std::uint32_t reactorIndex_ = 0;
    std::uint32_t plannerBlockSize_ = 0;
    std::size_t maxInFlight_ = 0;
    std::size_t deviceCount_ = 0;
    std::unique_ptr<Geometry[]> geometry_;
    std::unique_ptr<Slot[]> slots_;
    std::atomic<std::size_t> inFlight_{0};
    bool valid_ = false;
};

} // namespace hfs
