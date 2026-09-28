/* Process-level crash injection for crash-consistency tests (design §10.2).
 * The hook is inert unless HFS_TEST_CRASH_AT matches the requested point. */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unistd.h>

namespace hfs::test_hooks {

/* True when the caller requested a crash at the named durability point. */
inline bool crashRequested(const char *point) noexcept {
    const char *value = std::getenv("HFS_TEST_CRASH_AT");
    if (value == nullptr || *value == '\0') {
        return false;
    }
    return std::strcmp(value, point) == 0;
}

/* Terminates the process without cleanup, simulating a hard crash. When
 * HFS_TEST_CRASH_ARM_FILE is set, the hook stays inert until that file exists,
 * which lets the workload arm the crash after the database is open. The first
 * HFS_TEST_CRASH_SKIP occurrences are skipped as an additional control. */
inline void crashIfRequested(const char *point) noexcept {
    if (!crashRequested(point)) {
        return;
    }
    const char *armFile = std::getenv("HFS_TEST_CRASH_ARM_FILE");
    if (armFile != nullptr && *armFile != '\0' && ::access(armFile, F_OK) != 0) {
        return;
    }
    const char *skipText = std::getenv("HFS_TEST_CRASH_SKIP");
    const unsigned long skip = skipText != nullptr ? std::strtoul(skipText, nullptr, 10) : 0;
    static std::mutex mutex;
    static std::unordered_map<std::string, unsigned long> seen;
    unsigned long count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        count = seen[point]++;
    }
    if (count < skip) {
        return;
    }
    std::fprintf(stderr, "hfs: test crash at %s (occurrence %lu)\n", point, count + 1);
    std::fflush(stderr);
    ::_exit(137);
}

/* Data-path I/O delay injection used by device tests. The parsed value lives
 * in cached atomics so production completion callbacks never call getenv;
 * tests switch it at runtime through the setters below. */
enum class DataIoDelayKind : std::uint32_t {
    Generic = 0,
    Read = 1,
    Write = 2,
    Count = 3,
};

struct DataIoDelaySetting {
    bool present = false;
    std::uint32_t milliseconds = 0;
};

namespace detail {

inline constexpr const char *kDataIoDelayEnv[static_cast<std::size_t>(
    DataIoDelayKind::Count)] = {"HFS_TEST_DATA_IO_DELAY_MS",
                                "HFS_TEST_DATA_IO_READ_DELAY_MS",
                                "HFS_TEST_DATA_IO_WRITE_DELAY_MS"};

/* Missing or invalid values are treated as no injection. */
inline DataIoDelaySetting parseDataIoDelay(const char *name) noexcept {
    const char *text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return {};
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 60000) {
        return {};
    }
    return DataIoDelaySetting{true, static_cast<std::uint32_t>(value)};
}

struct DataIoDelayCache {
    std::atomic<std::uint32_t> milliseconds{0};
    std::atomic<bool> resolved{false};
    std::atomic<bool> present{false};

    DataIoDelaySetting load(const char *name) noexcept {
        if (resolved.load(std::memory_order_acquire)) {
            return DataIoDelaySetting{present.load(std::memory_order_relaxed),
                                      milliseconds.load(std::memory_order_relaxed)};
        }
        const DataIoDelaySetting parsed = parseDataIoDelay(name);
        milliseconds.store(parsed.milliseconds, std::memory_order_relaxed);
        present.store(parsed.present, std::memory_order_relaxed);
        resolved.store(true, std::memory_order_release);
        return parsed;
    }

    void store(std::uint32_t value) noexcept {
        milliseconds.store(value, std::memory_order_relaxed);
        present.store(value != 0, std::memory_order_relaxed);
        resolved.store(true, std::memory_order_release);
    }
};

inline DataIoDelayCache &dataIoDelayCache(DataIoDelayKind kind) noexcept {
    static DataIoDelayCache caches[static_cast<std::size_t>(DataIoDelayKind::Count)];
    return caches[static_cast<std::size_t>(kind)];
}

} // namespace detail

/* Returns the cached injection for one phase kind; the environment default is
 * resolved once on first use. */
inline DataIoDelaySetting dataIoDelay(DataIoDelayKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= static_cast<std::size_t>(DataIoDelayKind::Count)) {
        return {};
    }
    return detail::dataIoDelayCache(kind).load(detail::kDataIoDelayEnv[index]);
}

/* Read/write phases fall back to the generic injection when not set. */
inline DataIoDelaySetting effectiveDataIoDelay(bool writePhase) noexcept {
    const DataIoDelaySetting specific =
        dataIoDelay(writePhase ? DataIoDelayKind::Write : DataIoDelayKind::Read);
    if (specific.present) {
        return specific;
    }
    return dataIoDelay(DataIoDelayKind::Generic);
}

/* Test setter; 0 disables the injection for that kind. */
inline void setDataIoDelay(DataIoDelayKind kind, std::uint32_t milliseconds) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= static_cast<std::size_t>(DataIoDelayKind::Count)) {
        return;
    }
    detail::dataIoDelayCache(kind).store(milliseconds);
}

} // namespace hfs::test_hooks
