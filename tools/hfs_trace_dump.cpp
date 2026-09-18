#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/trace.hpp"

namespace {

/* Reads one fixed-size record and distinguishes EOF from a truncated record. */
int readRecord(int fd, hfs::trace::TraceRecord &record) noexcept {
    auto *bytes = reinterpret_cast<std::uint8_t *>(&record);
    std::size_t remaining = sizeof(record);
    while (remaining != 0) {
        const ssize_t count = ::read(fd, bytes, remaining);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return -1;
        }
        if (count == 0) {
            return remaining == sizeof(record) ? 0 : -2;
        }
        bytes += count;
        remaining -= static_cast<std::size_t>(count);
    }
    return 1;
}

bool makePath(char *buffer, std::size_t size, const char *directory,
              std::uint32_t source) noexcept {
    const int written = std::snprintf(buffer, size, "%s/trace-%u.bin", directory, source);
    return written > 0 && static_cast<std::size_t>(written) < size;
}

bool manifestField(const char *manifest, const char *field,
                   unsigned long long &value) noexcept {
    const char *cursor = std::strstr(manifest, field);
    if (cursor == nullptr) {
        return false;
    }
    cursor = std::strchr(cursor, ':');
    if (cursor == nullptr) {
        return false;
    }
    char *end = nullptr;
    value = ::strtoull(cursor + 1, &end, 10);
    return end != cursor + 1;
}

bool readManifest(const char *directory, unsigned long long &recorded,
                  unsigned long long &dropped, bool &closed) noexcept {
    char path[512];
    const int written = std::snprintf(path, sizeof(path), "%s/trace-manifest.json",
                                      directory);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(path)) {
        return false;
    }
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char manifest[4096]{};
    const ssize_t count = ::read(fd, manifest, sizeof(manifest) - 1);
    ::close(fd);
    if (count <= 0) {
        return false;
    }
    manifest[count] = '\0';
    recorded = 0;
    dropped = 0;
    if (!manifestField(manifest, "\"recorded\"", recorded) ||
        !manifestField(manifest, "\"dropped\"", dropped)) {
        return false;
    }
    closed = std::strstr(manifest, "\"closed\":true") != nullptr;
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: hfs_trace_dump TRACE_DIRECTORY\n");
        return 2;
    }

    std::array<std::uint64_t, 65536> eventCounts{};
    std::uint64_t recordCount = 0;
    std::uint32_t fileCount = 0;
    char path[512];
    for (std::uint32_t source = 0; source < 64; ++source) {
        if (!makePath(path, sizeof(path), argv[1], source)) {
            std::fprintf(stderr, "trace path is too long\n");
            return 1;
        }
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT) {
                continue;
            }
            std::fprintf(stderr, "cannot open %s: %s\n", path, std::strerror(errno));
            return 1;
        }
        ++fileCount;
        for (;;) {
            hfs::trace::TraceRecord record{};
            const int result = readRecord(fd, record);
            if (result == 0) {
                break;
            }
            if (result < 0) {
                std::fprintf(stderr, "invalid trace record in %s\n", path);
                ::close(fd);
                return 1;
            }
            ++recordCount;
            ++eventCounts[record.eventId];
        }
        ::close(fd);
    }

    std::printf("files=%u records=%llu\n", fileCount,
                static_cast<unsigned long long>(recordCount));
    unsigned long long manifestRecorded = 0;
    unsigned long long manifestDropped = 0;
    bool manifestClosed = false;
    if (readManifest(argv[1], manifestRecorded, manifestDropped, manifestClosed)) {
        std::printf("manifest_recorded=%llu manifest_dropped=%llu manifest_closed=%s\n",
                    manifestRecorded, manifestDropped, manifestClosed ? "true" : "false");
    }
    for (std::size_t event = 1; event < eventCounts.size(); ++event) {
        if (eventCounts[event] != 0) {
            std::printf("event=%zu count=%llu\n", event,
                        static_cast<unsigned long long>(eventCounts[event]));
        }
    }
    return 0;
}
