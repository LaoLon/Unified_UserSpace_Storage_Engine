#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/file.hpp"
#include "test_support.hpp"

using namespace hfs;

int main() {
    const std::string lockPath =
        "/tmp/hfs2-fileapi-" + std::to_string(::getpid()) + ".lock";
    auto parsed = parseConfig(validConfigText(lockPath), "file_api_test");
    HFS_TEST_CHECK(parsed.ok());

    {
        auto created = Engine::create(parsed.value());
        HFS_TEST_CHECK(created.ok());
        auto engine = std::move(created).value();
        HFS_TEST_CHECK(engine->format({true}).wait().value().ok());
        HFS_TEST_CHECK(engine->mount().wait().value().ok());

        {
            auto clientResult = Client::create(*engine, ClientOptions{});
            HFS_TEST_CHECK(clientResult.ok());
            auto client = clientResult.value();

            /* 1. root directory is empty and names are ordered */
            auto rootPage = client->getChildren("/", "", 16).wait().value();
            HFS_TEST_CHECK(rootPage.ok() && rootPage.value().entries.empty());

            /* 2. create/open semantics through the path API */
            auto dbDir =
                client->open("/db", OpenFlags::Create | OpenFlags::Directory).wait().value();
            HFS_TEST_CHECK(dbDir.ok());
            auto first = client->open("/db/FILE-000001", OpenFlags::Create).wait().value();
            HFS_TEST_CHECK(first.ok());
            const auto firstStat = first.value()->stat();
            HFS_TEST_CHECK(firstStat.ok() && !firstStat.value().directory);
            HFS_TEST_CHECK(firstStat.value().logicalSize == 0);
            HFS_TEST_CHECK(first.value()->handle().valid());

            /* Reject arithmetic overflow before reserving or mutating metadata. */
            auto overflowAllocate =
                first.value()->allocate(std::numeric_limits<std::uint64_t>::max(), 1)
                    .wait()
                    .value();
            HFS_TEST_CHECK(!overflowAllocate.ok() &&
                           overflowAllocate.status() == Status::InvalidArgument);

            auto exclusive = client->open("/db/FILE-000001",
                                          OpenFlags::Create | OpenFlags::Exclusive)
                                 .wait()
                                 .value();
            HFS_TEST_CHECK(exclusive.status() == Status::AlreadyExists);
            auto missing = client->open("/db/none").wait().value();
            HFS_TEST_CHECK(missing.status() == Status::NotFound);
            auto dirAsFile = client->open("/db").wait().value();
            HFS_TEST_CHECK(dirAsFile.status() == Status::InvalidArgument);
            auto fileAsDir =
                client->open("/db/FILE-000001", OpenFlags::Directory).wait().value();
            HFS_TEST_CHECK(fileAsDir.status() == Status::InvalidArgument);
            auto dirOk = client->open("/db", OpenFlags::Directory).wait().value();
            HFS_TEST_CHECK(dirOk.ok());

            /* 2b. path stat and recursive mkdir (Phase 7 Shard-100) */
            auto missingStat = client->stat("/db/none").wait().value();
            HFS_TEST_CHECK(!missingStat.ok() &&
                           missingStat.status() == Status::NotFound);
            auto filePathStat = client->stat("/db/FILE-000001").wait().value();
            HFS_TEST_CHECK(filePathStat.ok() && !filePathStat.value().directory);
            auto dirPathStat = client->stat("/db").wait().value();
            HFS_TEST_CHECK(dirPathStat.ok() && dirPathStat.value().directory);
            auto rootPathStat = client->stat("/").wait().value();
            HFS_TEST_CHECK(rootPathStat.ok() && rootPathStat.value().directory);

            HFS_TEST_CHECK(client->createDirectories("/a/b/c").wait().value().ok());
            HFS_TEST_CHECK(client->createDirectories("/a/b/c").wait().value().ok());
            auto nestedStat = client->stat("/a/b/c").wait().value();
            HFS_TEST_CHECK(nestedStat.ok() && nestedStat.value().directory);
            auto nestedChildren = client->getChildren("/a/b", "", 8).wait().value();
            HFS_TEST_CHECK(nestedChildren.ok() &&
                           nestedChildren.value().entries.size() == 1);
            HFS_TEST_CHECK(nestedChildren.value().entries[0].name == "c");
            HFS_TEST_CHECK(client->createDirectories("/").wait().value().ok());
            auto relativeMkdir = client->createDirectories("a/b").wait().value();
            HFS_TEST_CHECK(!relativeMkdir.ok() &&
                           relativeMkdir.status() == Status::InvalidArgument);
            auto trailingMkdir = client->createDirectories("/a/b/").wait().value();
            HFS_TEST_CHECK(!trailingMkdir.ok() &&
                           trailingMkdir.status() == Status::InvalidArgument);
            /* A regular file component fails closed without replacing it. */
            auto fileConflict =
                client->createDirectories("/db/FILE-000001/sub").wait().value();
            HFS_TEST_CHECK(!fileConflict.ok() &&
                           fileConflict.status() == Status::AlreadyExists);
            auto fileConflictStat = client->stat("/db/FILE-000001").wait().value();
            HFS_TEST_CHECK(fileConflictStat.ok() &&
                           !fileConflictStat.value().directory);

            /* 3. ordered pages and rename through the path API */
            for (int i = 2; i <= 5; ++i) {
                const std::string path = "/db/FILE-00000" + std::to_string(i);
                HFS_TEST_CHECK(client->open(path, OpenFlags::Create).wait().value().ok());
            }
            auto page1 = client->getChildren("/db", "", 3).wait().value();
            HFS_TEST_CHECK(page1.ok() && page1.value().entries.size() == 3);
            HFS_TEST_CHECK(page1.value().entries[0].name == "FILE-000001");
            HFS_TEST_CHECK(page1.value().entries[2].name == "FILE-000003");
            HFS_TEST_CHECK(page1.value().nextCursor == "FILE-000003");

            auto page2 =
                client->getChildren("/db", page1.value().nextCursor, 8).wait().value();
            HFS_TEST_CHECK(page2.ok() && page2.value().entries.size() == 2);
            HFS_TEST_CHECK(page2.value().entries[0].name == "FILE-000004");
            HFS_TEST_CHECK(page2.value().entries[1].name == "FILE-000005");
            HFS_TEST_CHECK(page2.value().nextCursor.empty());

            HFS_TEST_CHECK(client->rename("/db/FILE-000002", "/db/FILE-000099")
                               .wait()
                               .value()
                               .ok());
            HFS_TEST_CHECK(client->open("/db/FILE-000002").wait().value().status() ==
                           Status::NotFound);
            auto renamed = client->open("/db/FILE-000099").wait().value();
            HFS_TEST_CHECK(renamed.ok());

            /* 4. remove keeps open handles alive; slot reuse bumps the generation */
            auto older = client->open("/db/gen", OpenFlags::Create).wait().value();
            HFS_TEST_CHECK(older.ok());
            const auto olderStat = older.value()->stat();
            HFS_TEST_CHECK(olderStat.ok());
            HFS_TEST_CHECK(client->remove("/db/gen").wait().value().ok());
            HFS_TEST_CHECK(older.value()->stat().ok()); /* handle survives remove */
            older.value().reset();                     /* closes and frees the inode */
            auto newer = client->open("/db/gen", OpenFlags::Create).wait().value();
            HFS_TEST_CHECK(newer.ok());
            const auto newerStat = newer.value()->stat();
            HFS_TEST_CHECK(newerStat.ok());
            HFS_TEST_CHECK(newerStat.value().inodeId == olderStat.value().inodeId);
            HFS_TEST_CHECK(newerStat.value().generation != olderStat.value().generation);

            /* 5. file data path smoke test (full matrix in file_data_test) */
            {
                std::vector<std::byte> pattern(8192);
                for (std::size_t i = 0; i < pattern.size(); ++i) {
                    pattern[i] = std::byte{static_cast<std::uint8_t>(i * 13)};
                }
                IoVec writeVec{pattern.data(), pattern.size()};
                auto written = first.value()
                                   ->write(std::span<const IoVec>(&writeVec, 1), 100,
                                           std::stop_token{})
                                   .wait()
                                   .value();
                HFS_TEST_CHECK(written.ok() && written.value().bytes == pattern.size());
                auto resized = first.value()->stat();
                HFS_TEST_CHECK(resized.ok() && resized.value().logicalSize == 100 + 8192);
                std::vector<std::byte> readBack(8192, std::byte{0});
                IoVec readVec{readBack.data(), readBack.size()};
                auto read = first.value()
                                ->read(std::span<IoVec>(&readVec, 1), 100,
                                       std::stop_token{})
                                .wait()
                                .value();
                HFS_TEST_CHECK(read.ok() && read.value().bytes == pattern.size());
                HFS_TEST_CHECK(readBack == pattern);
                /* A data flush failure must stop before the metadata checkpoint. */
                HFS_TEST_CHECK(::setenv("HFS_TEST_FAIL_FLUSH_DEVICE", "0", 1) == 0);
                auto failedDatasync = first.value()->datasync().wait().value();
                HFS_TEST_CHECK(!failedDatasync.ok() &&
                               failedDatasync.status() == Status::IoError);
                HFS_TEST_CHECK(::unsetenv("HFS_TEST_FAIL_FLUSH_DEVICE") == 0);
                /* Both barriers now flush data and checkpoint metadata. */
                HFS_TEST_CHECK(first.value()->datasync().wait().value().ok());
                /* A checkpoint slot write must not publish without its flush.
                 * Namespace shape changes are not journaled, so create+remove
                 * forces the snapshot path for the next barrier. */
                {
                    auto forced = client->open("/db/.checkpoint-force", OpenFlags::Create)
                                      .wait()
                                      .value();
                    HFS_TEST_CHECK(forced.ok());
                }
                HFS_TEST_CHECK(client->remove("/db/.checkpoint-force").wait().value().ok());
                std::vector<std::byte> checkpointPattern(4096, std::byte{0x3C});
                IoVec checkpointVec{checkpointPattern.data(), checkpointPattern.size()};
                HFS_TEST_CHECK(first.value()
                                   ->write(std::span<const IoVec>(&checkpointVec, 1), 8192,
                                           std::stop_token{})
                                   .wait()
                                   .value()
                                   .ok());
                HFS_TEST_CHECK(::setenv("HFS_TEST_FAIL_CHECKPOINT_FLUSH", "1", 1) == 0);
                auto failedCheckpoint = first.value()->datasync().wait().value();
                HFS_TEST_CHECK(!failedCheckpoint.ok() &&
                               failedCheckpoint.status() == Status::IoError);
                auto checkpointFailureStat = first.value()->stat();
                HFS_TEST_CHECK(checkpointFailureStat.ok() &&
                               checkpointFailureStat.value().logicalSize == 12288 &&
                               checkpointFailureStat.value().durableSize <
                                   checkpointFailureStat.value().logicalSize);
                HFS_TEST_CHECK(::unsetenv("HFS_TEST_FAIL_CHECKPOINT_FLUSH") == 0);
                HFS_TEST_CHECK(first.value()->datasync().wait().value().ok());
                auto checkpointRetryStat = first.value()->stat();
                HFS_TEST_CHECK(checkpointRetryStat.ok() &&
                               checkpointRetryStat.value().durableSize ==
                                   checkpointRetryStat.value().logicalSize);
                HFS_TEST_CHECK(first.value()->sync().wait().value().ok());
                /* Data is durable, but the metadata commit boundary must still
                 * reject publication and allow a later retry. */
                std::vector<std::byte> metadataPattern(4096, std::byte{0x4D});
                IoVec metadataVec{metadataPattern.data(), metadataPattern.size()};
                HFS_TEST_CHECK(first.value()
                                   ->write(std::span<const IoVec>(&metadataVec, 1), 12288,
                                           std::stop_token{})
                                   .wait()
                                   .value()
                                   .ok());
                HFS_TEST_CHECK(::setenv("HFS_TEST_FAIL_METADATA_COMMIT", "1", 1) == 0);
                auto failedMetadata = first.value()->datasync().wait().value();
                HFS_TEST_CHECK(!failedMetadata.ok() &&
                               failedMetadata.status() == Status::IoError);
                auto metadataFailureStat = first.value()->stat();
                HFS_TEST_CHECK(metadataFailureStat.ok() &&
                               metadataFailureStat.value().logicalSize == 16384 &&
                               metadataFailureStat.value().durableSize <
                                   metadataFailureStat.value().logicalSize);
                HFS_TEST_CHECK(::unsetenv("HFS_TEST_FAIL_METADATA_COMMIT") == 0);
                HFS_TEST_CHECK(first.value()->datasync().wait().value().ok());
                auto metadataRetryStat = first.value()->stat();
                HFS_TEST_CHECK(metadataRetryStat.ok() &&
                               metadataRetryStat.value().durableSize ==
                                   metadataRetryStat.value().logicalSize);
            }

            /* 6. client open-file limit */
            auto limitedResult = Client::create(*engine, ClientOptions{2});
            HFS_TEST_CHECK(limitedResult.ok());
            auto limited = limitedResult.value();
            auto limit1 = limited->open("/db/FILE-000001").wait().value();
            auto limit2 = limited->open("/db/FILE-000003").wait().value();
            HFS_TEST_CHECK(limit1.ok() && limit2.ok());
            auto limit3 = limited->open("/db/FILE-000004").wait().value();
            HFS_TEST_CHECK(!limit3.ok() && limit3.status() == Status::NoMemory);
            limit1.value().reset();
            limit2.value().reset();
            auto limit4 = limited->open("/db/FILE-000004").wait().value();
            HFS_TEST_CHECK(limit4.ok());
            limit4.value().reset();
            limited.reset();

            /* 7. A reaped operation still blocks close until its IoHandle is
             * released, independently of open File references. */
            auto asyncBuffer = engine->acquireDmaBuffer(4096, 0);
            HFS_TEST_CHECK(asyncBuffer.ok());
            IoVec asyncIo{asyncBuffer.value().data(), asyncBuffer.value().size()};
            auto asyncRead = client->submitRead(
                first.value(), asyncIo, 0, CompletionToken{7, 1});
            HFS_TEST_CHECK(asyncRead.ok());
            std::array<CompletionEvent, 1> asyncEvents{};
            const auto asyncPoll = client->poll(asyncEvents, 1, 1,
                                                std::chrono::seconds(5));
            HFS_TEST_CHECK(asyncPoll.status == Status::Ok && asyncPoll.count == 1);
            HFS_TEST_CHECK(asyncEvents[0].status == Status::Ok);

            /* Exercise the full bounded slot table and request-id lookup path. */
            HFS_TEST_CHECK(first.value()
                               ->allocate(0, 32U * 4096U)
                               .wait()
                               .value()
                               .ok());
            {
                std::vector<std::span<std::byte>> batchBuffers;
                std::vector<IoHandle> batchHandles;
                batchBuffers.reserve(32);
                batchHandles.reserve(32);
                for (std::size_t index = 0; index < 32; ++index) {
                    auto acquired = engine->acquireDmaBuffer(4096, 0);
                    HFS_TEST_CHECK(acquired.ok());
                    batchBuffers.push_back(acquired.value());
                    IoVec batchIo{batchBuffers.back().data(), batchBuffers.back().size()};
                    auto submitted = client->submitRead(
                        first.value(), batchIo, index * 4096,
                        CompletionToken{static_cast<std::uint32_t>(100 + index), 1});
                    HFS_TEST_CHECK(submitted.ok());
                    batchHandles.push_back(std::move(submitted).value());
                }
                std::vector<CompletionEvent> batchEvents(32);
                const auto batchPoll = client->poll(batchEvents, 32, 32,
                                                    std::chrono::seconds(5));
                HFS_TEST_CHECK(batchPoll.status == Status::Ok && batchPoll.count == 32);
                for (std::size_t index = 0; index < batchPoll.count; ++index) {
                    HFS_TEST_CHECK(batchEvents[index].status == Status::Ok);
                }
                for (auto &handle : batchHandles) {
                    handle.reset();
                }
                for (const auto buffer : batchBuffers) {
                    engine->releaseDmaBuffer(buffer.data(), buffer.size());
                }
            }

            /* 8. close rejects live file handles and reaped IoHandles, then
             * becomes terminal after every frontend reference is released. */
            auto closeWithFiles = client->close().wait().value();
            HFS_TEST_CHECK(!closeWithFiles.ok() &&
                           closeWithFiles.status() == Status::Busy);
            dbDir.value().reset();
            first.value().reset();
            dirOk.value().reset();
            renamed.value().reset();
            newer.value().reset();
            auto closeWithIoHandle = client->close().wait().value();
            HFS_TEST_CHECK(!closeWithIoHandle.ok() &&
                           closeWithIoHandle.status() == Status::Busy);
            asyncRead.value().reset();
            engine->releaseDmaBuffer(asyncBuffer.value().data(),
                                     asyncBuffer.value().size());
            HFS_TEST_CHECK(client->close().wait().value().ok());
            auto afterClose = client->open("/db").wait().value();
            HFS_TEST_CHECK(afterClose.status() == Status::InvalidState);

            client.reset();
        }

        /* 9. stop is Busy while a client or open file still exists */
        auto busyClient = Client::create(*engine, ClientOptions{});
        HFS_TEST_CHECK(busyClient.ok());
        auto busyFile = busyClient.value()->open("/db/FILE-000001").wait().value();
        HFS_TEST_CHECK(busyFile.ok());
        auto busyStop = engine->stop().wait().value();
        HFS_TEST_CHECK(busyStop.status() == Status::Busy);
            busyFile.value().reset();
            busyClient.value().reset();
            /* Clean shutdown publication is itself a fail-closed boundary. */
            HFS_TEST_CHECK(::setenv("HFS_TEST_FAIL_CLEAN_FLAG", "1", 1) == 0);
            auto failedCleanStop = engine->stop().wait().value();
            HFS_TEST_CHECK(!failedCleanStop.ok() &&
                           failedCleanStop.status() == Status::IoError);
            HFS_TEST_CHECK(::unsetenv("HFS_TEST_FAIL_CLEAN_FLAG") == 0);
            HFS_TEST_CHECK(engine->stop().wait().value().ok());
    }

    /* restore a clean formatted image for the other device tests */
    {
        auto created = Engine::create(parsed.value());
        HFS_TEST_CHECK(created.ok());
        auto engine = std::move(created).value();
        HFS_TEST_CHECK(engine->format({true}).wait().value().ok());
        HFS_TEST_CHECK(engine->stop().wait().value().ok());
    }

    ::unlink(lockPath.c_str());
    std::printf("file_api_test: ok\n");
    return 0;
}
