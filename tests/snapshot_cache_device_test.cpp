/* Verifies the per-inode immutable snapshot cache (review P1-1): growing one
 * file must not invalidate another file's cached stat/extent view, while the
 * file's own size/extent changes must invalidate it. Runs on the real device. */

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/stats.hpp"
#include "hfs/core.hpp"
#include "hfs/diagnostics.hpp"
#include "hfs/file.hpp"
#include "test_support.hpp"

using namespace hfs;

namespace {

/* Writes one buffer through the synchronous file API. */
bool writeBytes(const std::shared_ptr<File> &file, std::span<std::byte> buffer,
                std::uint64_t offset) {
    std::array<IoVec, 1> vecs{IoVec{buffer.data(), buffer.size()}};
    auto result = file->write(vecs, offset).wait();
    return result.ok() && result.value().ok();
}

/* One asynchronous read reaped through Client::poll. */
bool readOnce(const std::shared_ptr<Client> &client, const std::shared_ptr<File> &file,
              std::byte *buffer, std::size_t bytes) {
    const CompletionToken token{0, 1};
    auto submitted = client->submitRead(file, IoVec{buffer, bytes}, 0, token);
    if (!submitted.ok()) {
        return false;
    }
    IoHandle handle = std::move(submitted).value();
    std::array<CompletionEvent, 8> events{};
    const auto polled = client->poll(events, 1, 8, std::chrono::milliseconds(5000));
    const bool ok = polled.status == Status::Ok && polled.count == 1 &&
                    events[0].status == Status::Ok && events[0].completedBytes == bytes;
    handle.reset();
    return ok;
}

} // namespace

int main() {
    const std::string lockPath =
        "/tmp/hfs2-snapshot-" + std::to_string(::getpid()) + ".lock";
    auto parsed = parseConfig(validConfigText(lockPath), "snapshot_cache_device_test");
    HFS_TEST_CHECK(parsed.ok());

    auto created = Engine::create(parsed.value());
    HFS_TEST_CHECK(created.ok());
    auto engine = std::move(created).value();
    HFS_TEST_CHECK(engine->format({true}).wait().value().ok());
    HFS_TEST_CHECK(engine->mount().wait().value().ok());

    ClientOptions options;
    options.maxOutstanding = 8;
    auto clientCreated = Client::create(*engine, options);
    HFS_TEST_CHECK(clientCreated.ok());
    auto client = std::move(clientCreated).value();

    auto openA = client->open("/snap-a", OpenFlags::Create | OpenFlags::Truncate).wait();
    HFS_TEST_CHECK(openA.ok() && openA.value().ok());
    auto fileA = std::move(openA).value().value();
    auto openB = client->open("/snap-b", OpenFlags::Create | OpenFlags::Truncate).wait();
    HFS_TEST_CHECK(openB.ok() && openB.value().ok());
    auto fileB = std::move(openB).value().value();

    std::vector<std::byte> payload(4096, std::byte{0x5A});
    std::vector<std::byte> overlay(4096, std::byte{0x33});
    HFS_TEST_CHECK(writeBytes(fileA, payload, 0));
    HFS_TEST_CHECK(writeBytes(fileB, payload, 0));

    /* Count snapshot cache behavior only for this phase. */
    stats::setEnabled(true);
    diagnostics::reset();

    auto dma = engine->acquireDmaBuffer(4096, 0);
    HFS_TEST_CHECK(dma.ok());
    std::byte *buffer = dma.value().data();

    /* 1. First read of B builds the immutable snapshot. */
    HFS_TEST_CHECK(readOnce(client, fileB, buffer, 4096));
    HFS_TEST_CHECK(diagnostics::snapshot().snapshotBuilds == 1);
    HFS_TEST_CHECK(diagnostics::snapshot().snapshotHits == 0);

    /* 2. An unchanged file hits the cached snapshot. */
    HFS_TEST_CHECK(readOnce(client, fileB, buffer, 4096));
    HFS_TEST_CHECK(diagnostics::snapshot().snapshotBuilds == 1);
    HFS_TEST_CHECK(diagnostics::snapshot().snapshotHits == 1);

    /* 3. Growing another file must not invalidate B's snapshot. The append to A
     * rebuilds A's own map once after its commit; B's read still hits. */
    HFS_TEST_CHECK(writeBytes(fileA, overlay, 4096));
    HFS_TEST_CHECK(readOnce(client, fileB, buffer, 4096));
    HFS_TEST_CHECK(diagnostics::snapshot().snapshotBuilds == 2);
    HFS_TEST_CHECK(diagnostics::snapshot().snapshotHits == 2);

    /* 4. A new extent in B itself invalidates its snapshot: the pre-write size
     * hits B's cache, the post-commit map rebuild caches the committed map,
     * then the setSize that follows the data write bumps the version once more,
     * so the next read rebuilds and re-caches (builds +2, hits +1). */
    HFS_TEST_CHECK(writeBytes(fileB, overlay, 1ULL << 20));
    HFS_TEST_CHECK(readOnce(client, fileB, buffer, 4096));
    HFS_TEST_CHECK(diagnostics::snapshot().snapshotBuilds == 4);
    HFS_TEST_CHECK(diagnostics::snapshot().snapshotHits == 3);

    /* 5. A fixed in-place overwrite keeps both the data map and the size valid,
     * so the write (pre/post) and the read all hit the cache. */
    HFS_TEST_CHECK(writeBytes(fileB, overlay, 0));
    HFS_TEST_CHECK(readOnce(client, fileB, buffer, 4096));
    HFS_TEST_CHECK(diagnostics::snapshot().snapshotBuilds == 4);
    HFS_TEST_CHECK(diagnostics::snapshot().snapshotHits == 6);

    engine->releaseDmaBuffer(buffer, dma.value().size());
    const auto finalStats = diagnostics::snapshot();
    std::printf("snapshot_cache_device_test: hits=%llu builds=%llu ok\n",
                static_cast<unsigned long long>(finalStats.snapshotHits),
                static_cast<unsigned long long>(finalStats.snapshotBuilds));

    /* Close file handles before closing the client (close requires 0 files). */
    fileA.reset();
    fileB.reset();
    auto closed = client->close().wait();
    HFS_TEST_CHECK(closed.ok() && closed.value().ok());
    HFS_TEST_CHECK(engine->stop().wait().value().ok());
    return 0;
}
