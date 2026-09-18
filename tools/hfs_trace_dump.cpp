#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unordered_map>
#include <vector>
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

bool readManifest(const char *directory, unsigned long long &formatVersion,
                  unsigned long long &recorded, unsigned long long &dropped,
                  unsigned long long &tscHz, bool &closed) noexcept {
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
    char manifest[8192]{};
    const ssize_t count = ::read(fd, manifest, sizeof(manifest) - 1);
    ::close(fd);
    if (count <= 0) {
        return false;
    }
    manifest[count] = '\0';
    if (!manifestField(manifest, "\"format_version\"", formatVersion) ||
        !manifestField(manifest, "\"recorded\"", recorded) ||
        !manifestField(manifest, "\"dropped\"", dropped) ||
        !manifestField(manifest, "\"tsc_hz\"", tscHz)) {
        return false;
    }
    closed = std::strstr(manifest, "\"closed\":true") != nullptr;
    return true;
}

struct RequestChildKey {
    std::uint64_t requestId = 0;
    std::uint32_t childIndex = 0;

    bool operator==(const RequestChildKey &other) const noexcept {
        return requestId == other.requestId && childIndex == other.childIndex;
    }
};

struct RequestChildHash {
    std::size_t operator()(const RequestChildKey &key) const noexcept {
        std::uint64_t value = key.requestId + 0x9e3779b97f4a7c15ULL;
        value ^= static_cast<std::uint64_t>(key.childIndex) +
                 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31;
        return static_cast<std::size_t>(value);
    }
};

std::uint64_t percentile(std::vector<std::uint64_t> &values, std::size_t percent) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = (values.size() - 1) * percent / 100;
    return values[index];
}

void printLatency(const char *name, std::vector<std::uint64_t> &values,
                  unsigned long long tscHz) {
    const std::uint64_t p50 = percentile(values, 50);
    const std::uint64_t p99 = percentile(values, 99);
    std::printf("%s_count=%llu %s_p50_tsc=%llu %s_p99_tsc=%llu\n", name,
                static_cast<unsigned long long>(values.size()), name,
                static_cast<unsigned long long>(p50), name,
                static_cast<unsigned long long>(p99));
    if (tscHz != 0 && !values.empty()) {
        const double scale = 1000000.0 / static_cast<double>(tscHz);
        std::printf("%s_p50_us=%.3f %s_p99_us=%.3f\n", name,
                    static_cast<double>(p50) * scale, name,
                    static_cast<double>(p99) * scale);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: hfs_trace_dump TRACE_DIRECTORY\n");
        return 2;
    }

    std::array<std::uint64_t, 65536> eventCounts{};
    std::vector<hfs::trace::TraceRecord> records;
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
            if (result < 0 || record.formatVersion == 0 || record.formatVersion > 2 ||
                record.sourceId != source) {
                std::fprintf(stderr, "invalid trace record in %s\n", path);
                ::close(fd);
                return 1;
            }
            records.push_back(record);
            ++eventCounts[record.eventId];
        }
        ::close(fd);
    }

    unsigned long long manifestFormat = 0;
    unsigned long long manifestRecorded = 0;
    unsigned long long manifestDropped = 0;
    unsigned long long manifestTscHz = 0;
    bool manifestClosed = false;
    if (!readManifest(argv[1], manifestFormat, manifestRecorded, manifestDropped,
                      manifestTscHz, manifestClosed)) {
        std::fprintf(stderr, "missing or invalid trace manifest\n");
        return 1;
    }
    if (manifestFormat == 0 || manifestFormat > 2 || !manifestClosed ||
        manifestRecorded < manifestDropped ||
        manifestRecorded - manifestDropped != records.size()) {
        std::fprintf(stderr,
                     "trace manifest integrity mismatch: recorded=%llu dropped=%llu files=%llu closed=%s\n",
                     manifestRecorded, manifestDropped,
                     static_cast<unsigned long long>(records.size()),
                     manifestClosed ? "true" : "false");
        return 1;
    }

    std::sort(records.begin(), records.end(), [](const auto &left, const auto &right) {
        if (left.timestamp != right.timestamp) {
            return left.timestamp < right.timestamp;
        }
        return left.sourceId < right.sourceId;
    });

    std::unordered_map<std::uint64_t, std::uint64_t> requestSubmit;
    std::unordered_map<RequestChildKey, std::uint64_t, RequestChildHash> shardSubmit;
    std::unordered_map<RequestChildKey, std::uint64_t, RequestChildHash> bdevSubmit;
    std::vector<std::uint64_t> requestLatency;
    std::vector<std::uint64_t> parentLatency;
    std::vector<std::uint64_t> shardLatency;
    std::vector<std::uint64_t> bdevLatency;

    const auto eventId = [](hfs::trace::EventId id) {
        return static_cast<std::uint16_t>(id);
    };
    for (const hfs::trace::TraceRecord &record : records) {
        if (record.eventId == eventId(hfs::trace::EventId::FeSubmit)) {
            if (record.arg0 != 0) {
                requestSubmit[record.arg0] = record.timestamp;
            }
        } else if (record.eventId == eventId(hfs::trace::EventId::FeReap)) {
            const auto found = requestSubmit.find(record.arg0);
            if (found != requestSubmit.end() && record.timestamp >= found->second) {
                requestLatency.push_back(record.timestamp - found->second);
            }
        } else if (record.eventId == eventId(hfs::trace::EventId::RequestParentComplete)) {
            const auto found = requestSubmit.find(record.arg0);
            if (found != requestSubmit.end() && record.timestamp >= found->second) {
                parentLatency.push_back(record.timestamp - found->second);
            }
        } else if (record.eventId == eventId(hfs::trace::EventId::RequestShardSubmit)) {
            shardSubmit[RequestChildKey{record.arg0, static_cast<std::uint32_t>(record.arg2)}] =
                record.timestamp;
        } else if (record.eventId == eventId(hfs::trace::EventId::RequestShardComplete)) {
            const RequestChildKey key{record.arg0, static_cast<std::uint32_t>(record.arg2)};
            const auto found = shardSubmit.find(key);
            if (found != shardSubmit.end() && record.timestamp >= found->second) {
                shardLatency.push_back(record.timestamp - found->second);
                shardSubmit.erase(found);
            }
        } else if (record.eventId == eventId(hfs::trace::EventId::RequestBdevSubmit)) {
            bdevSubmit[RequestChildKey{record.arg0, static_cast<std::uint32_t>(record.arg1)}] =
                record.timestamp;
        } else if (record.eventId == eventId(hfs::trace::EventId::RequestBdevComplete)) {
            const RequestChildKey key{record.arg0, static_cast<std::uint32_t>(record.arg1)};
            const auto found = bdevSubmit.find(key);
            if (found != bdevSubmit.end() && record.timestamp >= found->second) {
                bdevLatency.push_back(record.timestamp - found->second);
                bdevSubmit.erase(found);
            }
        }
    }

    std::printf("files=%u records=%llu\n", fileCount,
                static_cast<unsigned long long>(records.size()));
    std::printf("manifest_format=%llu manifest_recorded=%llu manifest_dropped=%llu "
                "manifest_tsc_hz=%llu manifest_closed=%s\n",
                manifestFormat, manifestRecorded, manifestDropped, manifestTscHz,
                manifestClosed ? "true" : "false");
    printLatency("request_latency", requestLatency, manifestTscHz);
    printLatency("parent_latency", parentLatency, manifestTscHz);
    printLatency("shard_latency", shardLatency, manifestTscHz);
    printLatency("bdev_latency", bdevLatency, manifestTscHz);
    for (std::size_t event = 1; event < eventCounts.size(); ++event) {
        if (eventCounts[event] != 0) {
            std::printf("event=%zu count=%llu\n", event,
                        static_cast<unsigned long long>(eventCounts[event]));
        }
    }
    return 0;
}
