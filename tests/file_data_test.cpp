#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <stop_token>
#include <unistd.h>
#include <vector>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/file.hpp"
#include "test_support.hpp"

using namespace hfs;

namespace {

constexpr std::uint64_t kExtent = 1ULL << 20; /* config extent_mb = 1 */

std::uint64_t checksum(std::span<const std::byte> bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void fillPattern(std::span<std::byte> bytes, std::uint8_t seed) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = std::byte{static_cast<std::uint8_t>(seed + i * 7)};
    }
}

Result<WriteResult> writeAt(File &file, std::span<const std::byte> data,
                            std::uint64_t offset) {
    IoVec vec{const_cast<std::byte *>(data.data()), data.size()};
    return file.write(std::span<const IoVec>(&vec, 1), offset, std::stop_token{})
        .wait()
        .value();
}

Result<ReadResult> readAt(File &file, std::span<std::byte> data, std::uint64_t offset) {
    IoVec vec{data.data(), data.size()};
    return file.read(std::span<IoVec>(&vec, 1), offset, std::stop_token{}).wait().value();
}

} // namespace

int main() {
    const std::string lockPath =
        "/tmp/hfs2-filedata-" + std::to_string(::getpid()) + ".lock";
    auto parsed = parseConfig(validConfigText(lockPath), "file_data_test");
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
            auto fileResult = client->open("/data.bin", OpenFlags::Create).wait().value();
            HFS_TEST_CHECK(fileResult.ok());
            auto file = fileResult.value();

            /* Pre-cancelled synchronous I/O must not reach the data plane or
             * reserve extents; in-flight cancellation uses the async API. */
            std::vector<std::byte> cancelledBytes(4096, std::byte{0xA5});
            IoVec cancelledVec{cancelledBytes.data(), cancelledBytes.size()};
            std::stop_source cancelled;
            cancelled.request_stop();
            auto cancelledWrite =
                file->write(std::span<const IoVec>(&cancelledVec, 1), 0,
                            cancelled.get_token())
                    .wait()
                    .value();
            HFS_TEST_CHECK(!cancelledWrite.ok() &&
                           cancelledWrite.status() == Status::Cancelled);
            auto cancelledStat = file->stat();
            HFS_TEST_CHECK(cancelledStat.ok() && cancelledStat.value().logicalSize == 0);
            std::vector<std::byte> cancelledRead(4096, std::byte{0x5A});
            IoVec cancelledReadVec{cancelledRead.data(), cancelledRead.size()};
            auto cancelledReadResult =
                file->read(std::span<IoVec>(&cancelledReadVec, 1), 0,
                           cancelled.get_token())
                    .wait()
                    .value();
            HFS_TEST_CHECK(!cancelledReadResult.ok() &&
                           cancelledReadResult.status() == Status::Cancelled);

            /* 1. explicit preallocation zero-initializes extents */
            auto allocateResult = file->allocate(0, 3 * kExtent).wait().value();
            if (!allocateResult.ok()) {
                std::fprintf(stderr, "allocate failed: %s (%s)\n",
                             statusName(allocateResult.status()),
                             allocateResult.message().c_str());
            }
            HFS_TEST_CHECK(allocateResult.ok());
            auto allocated = file->stat();
            HFS_TEST_CHECK(allocated.ok() && allocated.value().logicalSize == 3 * kExtent);
            auto coverage = file->allocatedBytes();
            HFS_TEST_CHECK(coverage.ok() && coverage.value() == 3 * kExtent);
            std::vector<std::byte> zeros(3 * kExtent, std::byte{0});
            std::vector<std::byte> readZeros(3 * kExtent, std::byte{0xFF});
            HFS_TEST_CHECK(readAt(*file, readZeros, 0).ok());
            HFS_TEST_CHECK(readZeros == zeros);

            /* 2. unaligned write crossing an extent boundary */
            std::vector<std::byte> unaligned(5000);
            fillPattern(unaligned, 0x22);
            const std::uint64_t unalignedOffset = 3 * 4096 - 1000;
            HFS_TEST_CHECK(writeAt(*file, unaligned, unalignedOffset).ok());
            std::vector<std::byte> unalignedRead(unaligned.size(), std::byte{0});
            HFS_TEST_CHECK(readAt(*file, unalignedRead, unalignedOffset).ok());
            HFS_TEST_CHECK(unalignedRead == unaligned);
            std::vector<std::byte> window(4 * 4096, std::byte{0});
            HFS_TEST_CHECK(readAt(*file, window, 0).ok());
            std::vector<std::byte> expected(window.size(), std::byte{0});
            std::memcpy(expected.data() + unalignedOffset, unaligned.data(),
                        unaligned.size());
            HFS_TEST_CHECK(window == expected);

            /* 3. overwrite keeps neighbours intact */
            std::vector<std::byte> block(64 * 1024);
            fillPattern(block, 0x33);
            HFS_TEST_CHECK(writeAt(*file, block, kExtent - 8192).ok());
            std::vector<std::byte> overwrite(4096);
            fillPattern(overwrite, 0x44);
            HFS_TEST_CHECK(writeAt(*file, overwrite, kExtent - 6000).ok());
            std::vector<std::byte> reread(block.size(), std::byte{0});
            HFS_TEST_CHECK(readAt(*file, reread, kExtent - 8192).ok());
            std::vector<std::byte> merged = block;
            std::memcpy(merged.data() + (8192 - 6000), overwrite.data(),
                        overwrite.size());
            HFS_TEST_CHECK(reread == merged);

            /* 4. append grows the file and preserves the tail */
            auto beforeAppend = file->stat();
            HFS_TEST_CHECK(beforeAppend.ok());
            std::vector<std::byte> appended(9000);
            fillPattern(appended, 0x55);
            IoVec appendVec{appended.data(), appended.size()};
            auto appendResult =
                file->append(std::span<const IoVec>(&appendVec, 1), std::stop_token{})
                    .wait()
                    .value();
            HFS_TEST_CHECK(appendResult.ok() &&
                           appendResult.value().bytes == appended.size());
            auto afterAppend = file->stat();
            HFS_TEST_CHECK(afterAppend.value().logicalSize ==
                           beforeAppend.value().logicalSize + appended.size());
            std::vector<std::byte> appendedRead(appended.size(), std::byte{0});
            HFS_TEST_CHECK(
                readAt(*file, appendedRead, beforeAppend.value().logicalSize).ok());
            HFS_TEST_CHECK(appendedRead == appended);

            /* 5. sparse writes leave holes that read as zeros */
            std::vector<std::byte> far(4096);
            fillPattern(far, 0x66);
            const std::uint64_t farOffset = 5 * kExtent;
            HFS_TEST_CHECK(writeAt(*file, far, farOffset).ok());
            std::vector<std::byte> hole(kExtent, std::byte{0xFF});
            HFS_TEST_CHECK(readAt(*file, hole, 4 * kExtent).ok());
            for (const std::byte byte : hole) {
                HFS_TEST_CHECK(byte == std::byte{0});
            }
            std::vector<std::byte> farRead(far.size(), std::byte{0});
            HFS_TEST_CHECK(readAt(*file, farRead, farOffset).ok());
            HFS_TEST_CHECK(farRead == far);

            /* 6. truncate shrinks (releasing extents) and grows sparsely */
            HFS_TEST_CHECK(file->truncate(2 * kExtent).wait().value().ok());
            auto shrunk = file->stat();
            HFS_TEST_CHECK(shrunk.ok() && shrunk.value().logicalSize == 2 * kExtent);
            auto shrunkCoverage = file->allocatedBytes();
            HFS_TEST_CHECK(shrunkCoverage.ok() && shrunkCoverage.value() == 2 * kExtent);
            std::vector<std::byte> beyond(4096, std::byte{0xAA});
            auto eofRead = readAt(*file, beyond, 2 * kExtent);
            HFS_TEST_CHECK(eofRead.ok() && eofRead.value().bytes == 0);
            HFS_TEST_CHECK(file->truncate(4 * kExtent).wait().value().ok());
            /* Growing the logical size must not fabricate extent coverage. */
            auto grownCoverage = file->allocatedBytes();
            HFS_TEST_CHECK(grownCoverage.ok() && grownCoverage.value() == 2 * kExtent);
            std::vector<std::byte> grown(4096, std::byte{0xFF});
            HFS_TEST_CHECK(readAt(*file, grown, (4 * kExtent) - 4096).ok());
            for (const std::byte byte : grown) {
                HFS_TEST_CHECK(byte == std::byte{0});
            }

            /* 7. multi-iovec write and read */
            std::vector<std::byte> partA(3000);
            std::vector<std::byte> partB(5000);
            fillPattern(partA, 0x77);
            fillPattern(partB, 0x88);
            IoVec writeVecs[2] = {{partA.data(), partA.size()},
                                  {partB.data(), partB.size()}};
            auto multiWrite = file->write(std::span<const IoVec>(writeVecs, 2), 1234,
                                          std::stop_token{})
                                  .wait()
                                  .value();
            HFS_TEST_CHECK(multiWrite.ok() &&
                           multiWrite.value().bytes == partA.size() + partB.size());
            std::vector<std::byte> readA(partA.size(), std::byte{0});
            std::vector<std::byte> readB(partB.size(), std::byte{0});
            IoVec readVecs[2] = {{readA.data(), readA.size()},
                                 {readB.data(), readB.size()}};
            auto multiRead = file->read(std::span<IoVec>(readVecs, 2), 1234,
                                        std::stop_token{})
                                 .wait()
                                 .value();
            HFS_TEST_CHECK(multiRead.ok() &&
                           multiRead.value().bytes == partA.size() + partB.size());
            HFS_TEST_CHECK(readA == partA && readB == partB);

            /* 8. large chunked I/O round-trip */
            std::vector<std::byte> large(2 * kExtent + 4096);
            fillPattern(large, 0x99);
            HFS_TEST_CHECK(writeAt(*file, large, 0).ok());
            std::vector<std::byte> largeRead(large.size(), std::byte{0});
            HFS_TEST_CHECK(readAt(*file, largeRead, 0).ok());
            HFS_TEST_CHECK(checksum(largeRead) == checksum(large));

            /* Drop both shared references before closing the client. */
            fileResult.value().reset();
            file.reset();
            HFS_TEST_CHECK(client->close().wait().value().ok());
            client.reset();
        }

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
    std::printf("file_data_test: ok\n");
    return 0;
}
