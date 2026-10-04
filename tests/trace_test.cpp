#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/trace.hpp"
#include "test_support.hpp"

namespace {

constexpr const char *kTraceDirectory = "/tmp/uuse-shard86-trace-test";

void cleanupTraceDirectory() noexcept {
    char path[256];
    for (std::uint32_t index = 0; index < 64; ++index) {
        const int written = std::snprintf(path, sizeof(path), "%s/trace-%u.bin",
                                          kTraceDirectory, index);
        if (written > 0 && static_cast<std::size_t>(written) < sizeof(path)) {
            (void)::unlink(path);
        }
    }
    const int written = std::snprintf(path, sizeof(path), "%s/trace-manifest.json",
                                      kTraceDirectory);
    if (written > 0 && static_cast<std::size_t>(written) < sizeof(path)) {
        (void)::unlink(path);
    }
    (void)::rmdir(kTraceDirectory);
}

} // namespace

int main() {
    cleanupTraceDirectory();
    uuse::trace::setSample(1);
    UUSE_TEST_CHECK(uuse::trace::start(kTraceDirectory));
    UUSE_TEST_CHECK(uuse::trace::sessionActive());
    uuse::trace::setEnabled(true);
    uuse::trace::event(uuse::trace::EventId::ActorBatch, 1, 2, 3, 4);
    uuse::trace::event(uuse::trace::EventId::FeSubmit, 5, 6, 7, 8);
    uuse::trace::event(uuse::trace::EventId::FeReap, 9, 10, 11, 12);
    UUSE_TEST_CHECK(uuse::trace::recorded() == 3);
    uuse::trace::setEnabled(false);
    uuse::trace::stop();
    UUSE_TEST_CHECK(!uuse::trace::sessionActive());
    UUSE_TEST_CHECK(uuse::trace::dropped() == 0);

    const int manifestFd = ::open("/tmp/uuse-shard86-trace-test/trace-manifest.json",
                                  O_RDONLY | O_CLOEXEC);
    UUSE_TEST_CHECK(manifestFd >= 0);
    char manifest[4096]{};
    const ssize_t manifestBytes = ::read(manifestFd, manifest, sizeof(manifest) - 1);
    ::close(manifestFd);
    UUSE_TEST_CHECK(manifestBytes > 0);
    UUSE_TEST_CHECK(std::strstr(manifest, "\"format_version\":2") != nullptr);
    UUSE_TEST_CHECK(std::strstr(manifest, "\"closed\":true") != nullptr);
    UUSE_TEST_CHECK(std::strstr(manifest, "\"source_threads\":[") != nullptr);

    char path[256];
    const int written = std::snprintf(path, sizeof(path), "%s/trace-0.bin",
                                      kTraceDirectory);
    struct stat fileStatus {};
    UUSE_TEST_CHECK(written > 0 && static_cast<std::size_t>(written) < sizeof(path));
    UUSE_TEST_CHECK(::stat(path, &fileStatus) == 0);
    UUSE_TEST_CHECK(fileStatus.st_size >=
                   static_cast<off_t>(3 * sizeof(uuse::trace::TraceRecord)));

    uuse::trace::setEnabled(true);
    const std::uint64_t beforeFallback = uuse::trace::recorded();
    uuse::trace::event(uuse::trace::EventId::SchedDone, 13);
    UUSE_TEST_CHECK(uuse::trace::recorded() == beforeFallback + 1);
    uuse::trace::setEnabled(false);

    /* Concurrent producers stopped mid-flight keep recorded == on-disk. */
    {
        UUSE_TEST_CHECK(uuse::trace::start(kTraceDirectory));
        uuse::trace::setEnabled(true);
        std::atomic<bool> stopNow{false};
        std::atomic<std::uint64_t> emitted{0};
        std::vector<std::thread> producers;
        for (int index = 0; index < 4; ++index) {
            producers.emplace_back([&stopNow, &emitted] {
                while (!stopNow.load(std::memory_order_acquire) &&
                       emitted.fetch_add(1, std::memory_order_relaxed) < 16000) {
                    uuse::trace::event(uuse::trace::EventId::ActorBatch, 1);
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stopNow.store(true, std::memory_order_release);
        uuse::trace::stop();
        for (std::thread &thread : producers) {
            thread.join();
        }
        UUSE_TEST_CHECK(uuse::trace::dropped() == 0);

        std::uint64_t onDisk = 0;
        for (std::uint32_t index = 0; index < 64; ++index) {
            const int line = std::snprintf(path, sizeof(path), "%s/trace-%u.bin",
                                           kTraceDirectory, index);
            if (line <= 0 || static_cast<std::size_t>(line) >= sizeof(path)) {
                break;
            }
            struct stat fileStatus {};
            if (::stat(path, &fileStatus) == 0) {
                onDisk += static_cast<std::uint64_t>(fileStatus.st_size) /
                          sizeof(uuse::trace::TraceRecord);
            }
        }
        UUSE_TEST_CHECK(onDisk == uuse::trace::recorded());
    }

    cleanupTraceDirectory();
    std::printf("trace_test: ok\n");
    return 0;
}
