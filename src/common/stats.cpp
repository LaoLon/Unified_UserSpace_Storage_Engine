#include "common/stats.hpp"

namespace hfs::stats {

namespace {

Counters g_counters;
bool g_enabled = false;

} // namespace

Counters &counters() noexcept { return g_counters; }

bool enabled() noexcept { return g_enabled; }

void setEnabled(bool value) noexcept { g_enabled = value; }

} // namespace hfs::stats
