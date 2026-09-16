#pragma once

#include <atomic>
#include <cstdint>

namespace hfs::stats {

/* Always-cheap counters; enabled state is switched in the control phase. */
struct Counters {
    std::atomic<std::uint64_t> submit{0};
    std::atomic<std::uint64_t> complete{0};
    std::atomic<std::uint64_t> busyReject{0};
    std::atomic<std::uint64_t> traceDropped{0};
};

Counters &counters() noexcept;
bool enabled() noexcept;
void setEnabled(bool value) noexcept;

} // namespace hfs::stats
