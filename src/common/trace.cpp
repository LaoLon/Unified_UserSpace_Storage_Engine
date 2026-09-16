#include "common/trace.hpp"

#include <atomic>

#include "common/stats.hpp"

namespace hfs::trace {

namespace {

bool g_enabled = false;
std::uint32_t g_sample = 1;
std::atomic<std::uint64_t> g_sequence{0};
std::atomic<std::uint64_t> g_recorded{0};

/* High-frequency points are sampled; low-frequency points are always kept. */
bool isHighFrequency(EventId id) noexcept {
    switch (id) {
    case EventId::FeSubmit:
    case EventId::FeReap:
    case EventId::CqPush:
    case EventId::CqReap:
    case EventId::BdevSubmit:
    case EventId::BdevComplete:
    case EventId::CopyIn:
    case EventId::CopyOut:
        return true;
    default:
        return false;
    }
}

} // namespace

bool enabled() noexcept { return g_enabled; }

void setEnabled(bool value) noexcept { g_enabled = value; }

void setSample(std::uint32_t sample) noexcept { g_sample = sample == 0 ? 1 : sample; }

void event(EventId id, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2,
           std::uint64_t a3) noexcept {
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    if (!g_enabled) {
        return;
    }
    const std::uint64_t sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (isHighFrequency(id) && (sequence % g_sample) != 0) {
        return;
    }
    g_recorded.fetch_add(1, std::memory_order_relaxed);
    /* The bounded per-reactor rings and offline writer land in Shard-75. */
    stats::counters().traceDropped.fetch_add(0, std::memory_order_relaxed);
}

std::uint64_t recorded() noexcept {
    return g_recorded.load(std::memory_order_relaxed);
}

} // namespace hfs::trace
