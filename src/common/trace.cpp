#include "common/trace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

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
    std::uint64_t ownerTid = 0;
    int fd = -1;
};

std::array<TraceChannel, kMaxChannels> g_channels;
std::atomic<bool> g_enabled{false};
/* Bumped on every enable/disable so each thread refreshes its TLS cache once. */
std::atomic<std::uint64_t> g_enabledVersion{1};

/* Per-thread cached enable state (hfs2.0.md §16.5): the off fast path is one
 * TLS bool read plus a relaxed version read; no per-event RMW, allocation or
 * lock. A thread that has seen the current version keeps its cached verdict. */
thread_local bool t_enabledCached = false;
thread_local std::uint64_t t_enabledVersion = 0;

/* Publishes a new enable state and invalidates every thread's TLS cache. */
void storeEnabled(bool value) noexcept {
    g_enabled.store(value, std::memory_order_release);
    g_enabledVersion.fetch_add(1, std::memory_order_relaxed);
}

/* Returns the cached enable state, refreshing it once per version change. */
bool enabledFast() noexcept {
    if (t_enabledCached) {
        return true;
    }
    const std::uint64_t version = g_enabledVersion.load(std::memory_order_relaxed);
    if (t_enabledVersion == version) {
        return false;
    }
    t_enabledVersion = version;
    t_enabledCached = g_enabled.load(std::memory_order_relaxed);
    return t_enabledCached;
}

std::atomic<bool> g_sessionActive{false};
std::atomic<bool> g_writerStop{false};
std::atomic<bool> g_writerStarted{false};
std::atomic<std::uint32_t> g_sample{1};
std::atomic<std::uint64_t> g_generation{0};
std::atomic<std::uint64_t> g_recorded{0};
std::atomic<std::uint64_t> g_dropped{0};
/* Producers that passed the enabled check and may still push one record. */
std::atomic<std::uint64_t> g_activeProducers{0};
pthread_t g_writer{};
char g_manifestPath[512]{};
std::uint64_t g_wallClockNs = 0;
std::uint64_t g_tscHz = 0;
/* False when the host has no invariant TSC; timestamps then use ns clocks. */
bool g_useTsc = false;

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
    case EventId::ShardMap:
    case EventId::ShardPump:
    case EventId::ShardBackendSubmit:
    case EventId::ShardBackendComplete:
    case EventId::RequestShardSubmit:
    case EventId::RequestShardComplete:
    case EventId::RequestBdevSubmit:
    case EventId::RequestBdevComplete:
    case EventId::RequestParentComplete:
    case EventId::SnapHit:
    case EventId::SnapBuild:
    case EventId::OpPoolAcquire:
    case EventId::DmaOwns:
    case EventId::FeSubmitBegin:
        return true;
    default:
        return false;
    }
}

/* Only an invariant TSC is comparable across CPUs/threads. */
bool invariantTscAvailable() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid_max(0x80000000U, nullptr) < 0x80000007U) {
        return false;
    }
    __cpuid(0x80000007U, eax, ebx, ecx, edx);
    return (edx & (1U << 8)) != 0; /* EDX[8] = invariant TSC */
#else
    return false;
#endif
}

std::uint64_t timestamp() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    if (g_useTsc) {
        return __builtin_ia32_rdtsc();
    }
#endif
    /* Fallback timestamps are nanoseconds; g_tscHz is set to 1e9 so the
     * trace dump conversion stays valid for every host. */
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uint64_t currentThreadId() noexcept {
#if defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    pthread_t self = pthread_self();
    std::uint64_t value = 0;
    std::memcpy(&value, &self, std::min(sizeof(value), sizeof(self)));
    return value;
#endif
}

/* Calibrate TSC against monotonic time once per trace session. Hosts without
 * an invariant TSC fall back to steady_clock nanoseconds. */
void calibrateTsc() noexcept {
    g_tscHz = 0;
    g_useTsc = false;
    if (!invariantTscAvailable()) {
        g_tscHz = 1000000000ULL; /* timestamps are nanoseconds */
        return;
    }

    /* Two samples reject a single scheduling hiccup during calibration. */
    std::uint64_t lastHz = 0;
    for (int sample = 0; sample < 2; ++sample) {
        const auto begin = std::chrono::steady_clock::now();
        const std::uint64_t beginTsc = __builtin_ia32_rdtsc();
        const timespec delay{0, 1000000};
        (void)::nanosleep(&delay, nullptr);
        const std::uint64_t endTsc = __builtin_ia32_rdtsc();
        const auto end = std::chrono::steady_clock::now();
        const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - begin).count();
        if (elapsedNs <= 0 || endTsc <= beginTsc) {
            continue;
        }
        const std::uint64_t ticks = endTsc - beginTsc;
        if (ticks > std::numeric_limits<std::uint64_t>::max() / 1000000000ULL) {
            continue;
        }
        const std::uint64_t hz = (ticks * 1000000000ULL) /
                                 static_cast<std::uint64_t>(elapsedNs);
        if (hz == 0) {
            continue;
        }
        if (lastHz != 0) {
            const std::uint64_t low = std::min(lastHz, hz);
            const std::uint64_t high = std::max(lastHz, hz);
            if (high - low > high / 20) { /* >5% spread: unstable TSC */
                g_tscHz = 1000000000ULL;
                return;
            }
        }
        lastHz = hz;
    }
    if (lastHz != 0) {
        g_tscHz = lastHz;
        g_useTsc = true;
    } else {
        g_tscHz = 1000000000ULL;
    }
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
            channel.ownerTid = currentThreadId();
            /* Publish the metadata with release so manifest readers that
             * acquire the claim observe initialized fields. */
            channel.claimed.store(true, std::memory_order_release);
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

bool appendManifestText(char *buffer, std::size_t bufferSize, std::size_t &offset,
                        const char *format, ...) noexcept {
    if (offset >= bufferSize) {
        return false;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(buffer + offset, bufferSize - offset, format,
                                       arguments);
    va_end(arguments);
    if (written < 0 || static_cast<std::size_t>(written) >= bufferSize - offset) {
        return false;
    }
    offset += static_cast<std::size_t>(written);
    return true;
}

bool writeManifest(bool closed) noexcept {
    if (g_manifestPath[0] == '\0') {
        return true;
    }
    char manifest[8192];
    std::size_t bytes = 0;
    if (!appendManifestText(
            manifest, sizeof(manifest), bytes,
            "{\"format_version\":%u,\"record_size\":%zu,\"ring_capacity\":%zu,"
            "\"channel_count\":%zu,\"wall_clock_ns\":%llu,\"tsc_hz\":%llu,"
            "\"sample\":%u,\"recorded\":%llu,\"dropped\":%llu,\"closed\":%s,"
            "\"source_threads\":[",
            static_cast<unsigned>(kTraceFormatVersion), sizeof(TraceRecord), kRingCapacity,
            kMaxChannels, static_cast<unsigned long long>(g_wallClockNs),
            static_cast<unsigned long long>(g_tscHz),
            g_sample.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(g_recorded.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_dropped.load(std::memory_order_relaxed)),
            closed ? "true" : "false")) {
        return false;
    }
    bool first = true;
    for (const TraceChannel &channel : g_channels) {
        if (!channel.claimed.load(std::memory_order_acquire)) {
            continue;
        }
        if (!appendManifestText(manifest, sizeof(manifest), bytes,
                                first ? "{\"source\":%u,\"tid\":%llu}"
                                       : ", {\"source\":%u,\"tid\":%llu}",
                                channel.sourceId,
                                static_cast<unsigned long long>(channel.ownerTid))) {
            return false;
        }
        first = false;
    }
    if (!appendManifestText(manifest, sizeof(manifest), bytes, "]}\n")) {
        return false;
    }
    const int fd = ::open(g_manifestPath, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    const bool ok = writeAll(fd, manifest, bytes);
    ::close(fd);
    return ok;
}

/* Return a stable request key for points that have a request id in their args. */
std::uint64_t requestSamplingKey(EventId id, std::uint64_t a0, std::uint64_t a1,
                                 std::uint64_t a2, std::uint64_t a3) noexcept {
    (void)a1;
    (void)a2;
    switch (id) {
    case EventId::FeSubmit:
    case EventId::FeReap:
    case EventId::CqPush:
    case EventId::RequestShardSubmit:
    case EventId::RequestShardComplete:
    case EventId::RequestBdevSubmit:
    case EventId::RequestBdevComplete:
    case EventId::RequestParentComplete:
        return a0;
    case EventId::ShardMap:
        return a3;
    default:
        return 0;
    }
}

} // namespace

bool enabled() noexcept { return enabledFast(); }

void setEnabled(bool value) noexcept { storeEnabled(value); }

void setSample(std::uint32_t sample) noexcept {
    g_sample.store(sample == 0 ? 1 : sample, std::memory_order_relaxed);
}

bool start(const char *directory) noexcept {
    if (directory == nullptr || directory[0] == '\0') {
        return false;
    }
    stop();
    storeEnabled(false);

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
        channel.ownerTid = 0;

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
    calibrateTsc();
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
    storeEnabled(false);
    /* Bounded wait for producers that already passed the enabled check, so the
     * final manifest and file set contain every recorded event. */
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (g_activeProducers.load(std::memory_order_acquire) == 0) {
            break;
        }
        const timespec pause{0, 100000};
        (void)::nanosleep(&pause, nullptr);
    }
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

namespace {

/* Keeps stop() from closing the session under an in-flight producer. */
struct ActiveProducerGuard {
    ActiveProducerGuard() noexcept {
        g_activeProducers.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ActiveProducerGuard() noexcept {
        g_activeProducers.fetch_sub(1, std::memory_order_acq_rel);
    }
};

} // namespace

void event(EventId id, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2,
           std::uint64_t a3) noexcept {
    if (!enabledFast()) {
        return;
    }
    ActiveProducerGuard producer;
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    if (isHighFrequency(id)) {
        const std::uint32_t sample = g_sample.load(std::memory_order_relaxed);
        ++g_tlsHighFrequencySequence;
        const std::uint64_t requestKey = requestSamplingKey(id, a0, a1, a2, a3);
        const std::uint64_t sampleKey = requestKey == 0
                                            ? g_tlsHighFrequencySequence
                                            : requestKey;
        if (sample > 1 && (sampleKey % sample) != 0) {
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
    record.formatVersion = kTraceFormatVersion;
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
