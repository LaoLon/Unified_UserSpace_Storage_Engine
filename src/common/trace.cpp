#include "common/trace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <new>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/stats.hpp"

namespace hfs::trace {

namespace {

constexpr std::size_t kMaxChannels = 64;
constexpr std::size_t kRingCapacity = 16384;
constexpr std::size_t kDrainBatch = 1024;

/* Each producer owns one channel; the writer is the only consumer. */
struct TraceChannel {
    std::unique_ptr<TraceRecord[]> records;
    std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> tail{0};
    std::atomic<bool> claimed{false};
    std::uint32_t sourceId = 0;
    int fd = -1;
};

std::array<TraceChannel, kMaxChannels> g_channels;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_sessionActive{false};
std::atomic<bool> g_writerStop{false};
std::atomic<bool> g_writerStarted{false};
std::atomic<std::uint32_t> g_sample{1};
std::atomic<std::uint64_t> g_generation{0};
std::atomic<std::uint64_t> g_recorded{0};
std::atomic<std::uint64_t> g_dropped{0};
pthread_t g_writer{};
char g_manifestPath[512]{};
std::uint64_t g_wallClockNs = 0;

thread_local TraceChannel *g_tlsChannel = nullptr;
thread_local std::uint64_t g_tlsGeneration = 0;
thread_local std::uint32_t g_tlsHighFrequencySequence = 0;

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
    case EventId::NotifyWrite:
    case EventId::NotifyWait:
    case EventId::NotifyRead:
    case EventId::PollSpin:
    case EventId::PollReturn:
        return true;
    default:
        return false;
    }
}

std::uint64_t timestamp() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

bool writeAll(int fd, const void *data, std::size_t size) noexcept {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    while (size != 0) {
        const ssize_t written = ::write(fd, bytes, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

void closeFiles() noexcept {
    for (TraceChannel &channel : g_channels) {
        if (channel.fd >= 0) {
            ::close(channel.fd);
            channel.fd = -1;
        }
    }
}

void markDropped(std::uint64_t count) noexcept {
    g_dropped.fetch_add(count, std::memory_order_relaxed);
    stats::counters().traceDropped.fetch_add(count, std::memory_order_relaxed);
}

bool drainChannel(TraceChannel &channel) noexcept {
    std::uint64_t tail = channel.tail.load(std::memory_order_relaxed);
    const std::uint64_t head = channel.head.load(std::memory_order_acquire);
    if (tail == head) {
        return false;
    }

    const std::uint64_t pending = head - tail;
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(pending, kDrainBatch));
    const std::size_t first = std::min<std::size_t>(
        count, kRingCapacity - static_cast<std::size_t>(tail & (kRingCapacity - 1)));
    const std::size_t second = count - first;
    bool ok = channel.fd >= 0;
    if (ok) {
        const auto firstIndex = static_cast<std::size_t>(tail & (kRingCapacity - 1));
        ok = writeAll(channel.fd, channel.records.get() + firstIndex,
                      first * sizeof(TraceRecord));
        if (ok && second != 0) {
            ok = writeAll(channel.fd, channel.records.get(),
                          second * sizeof(TraceRecord));
        }
    }
    if (!ok) {
        markDropped(count);
    }
    channel.tail.store(tail + count, std::memory_order_release);
    return true;
}

bool hasPending() noexcept {
    for (const TraceChannel &channel : g_channels) {
        if (channel.tail.load(std::memory_order_relaxed) !=
            channel.head.load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void *writerMain(void *) noexcept {
    for (;;) {
        bool progress = false;
        for (TraceChannel &channel : g_channels) {
            progress = drainChannel(channel) || progress;
        }
        if (g_writerStop.load(std::memory_order_acquire) && !hasPending()) {
            break;
        }
        if (!progress) {
            const timespec delay{0, 1000000};
            ::nanosleep(&delay, nullptr);
        }
    }
    return nullptr;
}

TraceChannel *claimChannel() noexcept {
    const std::uint64_t generation = g_generation.load(std::memory_order_acquire);
    if (g_tlsChannel != nullptr && g_tlsGeneration == generation) {
        return g_tlsChannel;
    }

    for (std::size_t index = 0; index < g_channels.size(); ++index) {
        TraceChannel &channel = g_channels[index];
        bool expected = false;
        if (channel.claimed.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
            channel.sourceId = static_cast<std::uint32_t>(index);
            g_tlsChannel = &channel;
            g_tlsGeneration = generation;
            return g_tlsChannel;
        }
    }
    return nullptr;
}

bool makePath(char *buffer, std::size_t bufferSize, const char *directory,
              std::uint32_t sourceId) noexcept {
    const int written = std::snprintf(buffer, bufferSize, "%s/trace-%u.bin", directory,
                                      sourceId);
    return written > 0 && static_cast<std::size_t>(written) < bufferSize;
}

bool writeManifest(bool closed) noexcept {
    if (g_manifestPath[0] == '\0') {
        return true;
    }
    char manifest[1024];
    const int bytes = std::snprintf(
        manifest, sizeof(manifest),
        "{\"format_version\":1,\"record_size\":%zu,\"ring_capacity\":%zu,"
        "\"channel_count\":%zu,\"wall_clock_ns\":%llu,\"sample\":%u,\"recorded\":%llu,"
        "\"dropped\":%llu,\"closed\":%s}\n",
        sizeof(TraceRecord), kRingCapacity, kMaxChannels,
        static_cast<unsigned long long>(g_wallClockNs),
        g_sample.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(g_recorded.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_dropped.load(std::memory_order_relaxed)),
        closed ? "true" : "false");
    if (bytes <= 0 || static_cast<std::size_t>(bytes) >= sizeof(manifest)) {
        return false;
    }
    const int fd = ::open(g_manifestPath, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    const bool ok = writeAll(fd, manifest, static_cast<std::size_t>(bytes));
    ::close(fd);
    return ok;
}

} // namespace

bool enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void setEnabled(bool value) noexcept { g_enabled.store(value, std::memory_order_release); }

void setSample(std::uint32_t sample) noexcept {
    g_sample.store(sample == 0 ? 1 : sample, std::memory_order_relaxed);
}

bool start(const char *directory) noexcept {
    if (directory == nullptr || directory[0] == '\0') {
        return false;
    }
    stop();
    g_enabled.store(false, std::memory_order_release);

    if (::mkdir(directory, 0755) != 0 && errno != EEXIST) {
        return false;
    }

    for (std::size_t index = 0; index < g_channels.size(); ++index) {
        TraceChannel &channel = g_channels[index];
        if (!channel.records) {
            channel.records.reset(new (std::nothrow) TraceRecord[kRingCapacity]);
            if (!channel.records) {
                closeFiles();
                return false;
            }
        }
        channel.head.store(0, std::memory_order_relaxed);
        channel.tail.store(0, std::memory_order_relaxed);
        channel.claimed.store(false, std::memory_order_relaxed);
        channel.sourceId = static_cast<std::uint32_t>(index);

        char path[512];
        if (!makePath(path, sizeof(path), directory, channel.sourceId)) {
            closeFiles();
            return false;
        }
        channel.fd = ::open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
        if (channel.fd < 0) {
            closeFiles();
            return false;
        }
    }

    const int manifestLength = std::snprintf(g_manifestPath, sizeof(g_manifestPath),
                                             "%s/trace-manifest.json", directory);
    if (manifestLength <= 0 || static_cast<std::size_t>(manifestLength) >= sizeof(g_manifestPath)) {
        g_manifestPath[0] = '\0';
        closeFiles();
        return false;
    }
    g_recorded.store(0, std::memory_order_relaxed);
    g_dropped.store(0, std::memory_order_relaxed);
    g_wallClockNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (!writeManifest(false)) {
        g_manifestPath[0] = '\0';
        closeFiles();
        return false;
    }

    g_generation.fetch_add(1, std::memory_order_acq_rel);
    g_writerStop.store(false, std::memory_order_release);
    if (::pthread_create(&g_writer, nullptr, &writerMain, nullptr) != 0) {
        (void)writeManifest(true);
        g_manifestPath[0] = '\0';
        closeFiles();
        return false;
    }
    g_writerStarted.store(true, std::memory_order_release);
    g_sessionActive.store(true, std::memory_order_release);
    return true;
}

void stop() noexcept {
    g_enabled.store(false, std::memory_order_release);
    if (g_writerStarted.load(std::memory_order_acquire)) {
        g_writerStop.store(true, std::memory_order_release);
        ::pthread_join(g_writer, nullptr);
        g_writerStarted.store(false, std::memory_order_release);
    }
    g_sessionActive.store(false, std::memory_order_release);
    (void)writeManifest(true);
    closeFiles();
    g_manifestPath[0] = '\0';
}

bool sessionActive() noexcept { return g_sessionActive.load(std::memory_order_acquire); }

void event(EventId id, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2,
           std::uint64_t a3) noexcept {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    if (isHighFrequency(id)) {
        const std::uint32_t sample = g_sample.load(std::memory_order_relaxed);
        ++g_tlsHighFrequencySequence;
        if (sample > 1 && (g_tlsHighFrequencySequence % sample) != 0) {
            return;
        }
    }

    if (!g_sessionActive.load(std::memory_order_acquire)) {
        g_recorded.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    TraceChannel *channel = claimChannel();
    if (channel == nullptr) {
        markDropped(1);
        return;
    }
    const std::uint64_t head = channel->head.load(std::memory_order_relaxed);
    const std::uint64_t tail = channel->tail.load(std::memory_order_acquire);
    if (head - tail >= kRingCapacity) {
        markDropped(1);
        return;
    }

    TraceRecord &record = channel->records[head & (kRingCapacity - 1)];
    record.eventId = static_cast<std::uint16_t>(id);
    record.formatVersion = 1;
    record.sourceId = channel->sourceId;
    record.timestamp = timestamp();
    record.arg0 = a0;
    record.arg1 = a1;
    record.arg2 = a2;
    record.arg3 = a3;
    channel->head.store(head + 1, std::memory_order_release);
    g_recorded.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t recorded() noexcept { return g_recorded.load(std::memory_order_relaxed); }

std::uint64_t dropped() noexcept { return g_dropped.load(std::memory_order_relaxed); }

} // namespace hfs::trace
