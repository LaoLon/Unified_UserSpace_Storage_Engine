#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include "common/trace.hpp"
#include "test_support.hpp"

namespace {

constexpr const char *kTraceDirectory = "/tmp/hfs2-shard86-trace-test";

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
    hfs::trace::setSample(1);
    HFS_TEST_CHECK(hfs::trace::start(kTraceDirectory));
    HFS_TEST_CHECK(hfs::trace::sessionActive());
    hfs::trace::setEnabled(true);
    hfs::trace::event(hfs::trace::EventId::ActorBatch, 1, 2, 3, 4);
    hfs::trace::event(hfs::trace::EventId::FeSubmit, 5, 6, 7, 8);
    hfs::trace::event(hfs::trace::EventId::FeReap, 9, 10, 11, 12);
    HFS_TEST_CHECK(hfs::trace::recorded() == 3);
    hfs::trace::setEnabled(false);
    hfs::trace::stop();
    HFS_TEST_CHECK(!hfs::trace::sessionActive());
    HFS_TEST_CHECK(hfs::trace::dropped() == 0);

    char path[256];
    const int written = std::snprintf(path, sizeof(path), "%s/trace-0.bin",
                                      kTraceDirectory);
    struct stat fileStatus {};
    HFS_TEST_CHECK(written > 0 && static_cast<std::size_t>(written) < sizeof(path));
    HFS_TEST_CHECK(::stat(path, &fileStatus) == 0);
    HFS_TEST_CHECK(fileStatus.st_size >=
                   static_cast<off_t>(3 * sizeof(hfs::trace::TraceRecord)));

    hfs::trace::setEnabled(true);
    const std::uint64_t beforeFallback = hfs::trace::recorded();
    hfs::trace::event(hfs::trace::EventId::SchedDone, 13);
    HFS_TEST_CHECK(hfs::trace::recorded() == beforeFallback + 1);
    hfs::trace::setEnabled(false);
    cleanupTraceDirectory();
    std::printf("trace_test: ok\n");
    return 0;
}
