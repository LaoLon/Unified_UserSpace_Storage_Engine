/* Process-level crash injection for crash-consistency tests (design §10.2).
 * The hook is inert unless HFS_TEST_CRASH_AT matches the requested point. */

#pragma once

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

} // namespace hfs::test_hooks
