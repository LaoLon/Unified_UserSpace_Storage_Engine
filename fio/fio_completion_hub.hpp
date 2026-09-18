#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace hfs::fio {

/* Process-wide endpoint registry used by FIO workers without sharing Client slots. */
class FioCompletionHub final {
public:
    /* One endpoint is owned by one FIO worker and one frontend Client. */
    class Endpoint final {
    public:
        Endpoint() noexcept = default;

        Endpoint(const Endpoint &) = delete;
        Endpoint &operator=(const Endpoint &) = delete;

        void reset() noexcept;
        void notify() noexcept;
        void prepareWait() noexcept;
        std::uint32_t epoch() const noexcept;
        bool waitSince(std::uint32_t observed,
                       std::chrono::microseconds timeout) noexcept;

    private:
        friend class FioCompletionHub;
        std::atomic<std::uint32_t> epoch_{0};
        std::atomic<std::uint32_t> waiters_{0};
        std::atomic<bool> armed_{false};
        std::atomic<bool> inUse_{false};
    };

    FioCompletionHub() noexcept = default;

    FioCompletionHub(const FioCompletionHub &) = delete;
    FioCompletionHub &operator=(const FioCompletionHub &) = delete;

    /* Resets endpoint ownership before the first Client of one engine lifetime. */
    bool initialize() noexcept;

    /* Invalidates future waits after the last FIO worker has drained. */
    void shutdown() noexcept;

    Endpoint *acquire() noexcept;
    void release(Endpoint *endpoint) noexcept;

    static void wakeCallback(void *context) noexcept;

private:
    static constexpr std::size_t kMaximumEndpoints = 64;
    std::array<Endpoint, kMaximumEndpoints> endpoints_;
    std::atomic<bool> active_{false};
};

} // namespace hfs::fio
