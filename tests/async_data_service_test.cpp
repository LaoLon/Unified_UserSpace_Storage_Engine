#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "core/async_data_service.hpp"
#include "core/dma_arena.hpp"
#include "core/extent_map.hpp"
#include "test_support.hpp"

namespace {

void *arenaAlloc(std::size_t size, void *) {
    return std::malloc(size);
}

void *failingAlloc(std::size_t, void *) { return nullptr; }

void arenaFree(void *pointer, void *) { std::free(pointer); }

struct RecordingSink final : hfs::AsyncDataSink {
    explicit RecordingSink(std::size_t limit) : limit_(limit) {}

    bool tryAcquireCredit() noexcept override {
        if (credits_ >= limit_) {
            return false;
        }
        ++credits_;
        return true;
    }

    void releaseCredit() noexcept override {
        if (credits_ != 0) {
            --credits_;
        }
    }

    void publish(std::uint32_t shard, const hfs::CompletionEvent &event) noexcept override {
        (void)shard;
        events.push_back(event);
    }

    void reap(std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            releaseCredit();
        }
    }

    std::size_t credits() const noexcept { return credits_; }

    std::vector<hfs::CompletionEvent> events;

private:
    std::size_t limit_ = 0;
    std::size_t credits_ = 0;
};

struct FailingExtentMap final : hfs::ExtentMap {
    hfs::Result<std::size_t> lookup(std::uint64_t, std::uint64_t,
                                    std::span<hfs::ExtentView>) const override {
        return hfs::makeError<std::size_t>(hfs::Status::NoMemory,
                                            "synthetic snapshot failure");
    }
};

class FakeBackend final : public hfs::AsyncDataBackend {
public:
    struct Pending {
        Request request{};
        CompletionCallback callback = nullptr;
        void *context = nullptr;
    };

    SubmitResult submitRead(const Request &request, CompletionCallback callback,
                            void *context) noexcept override {
        return submit(request, callback, context);
    }

    SubmitResult submitWrite(const Request &request, CompletionCallback callback,
                             void *context) noexcept override {
        return submit(request, callback, context);
    }

    SubmitResult submit(const Request &request, CompletionCallback callback,
                        void *context) noexcept {
        submissions.push_back(request);
        if (rejectNext) {
            rejectNext = false;
            return SubmitResult{SubmitDisposition::Rejected, rejectErrno};
        }
        if (fast) {
            callback(context, request.childIndex, fastStatus, fastErrno,
                     fastStatus == hfs::Status::Ok ? request.spanBytes : 0);
            return SubmitResult{SubmitDisposition::Accepted, 0};
        }
        pending.push_back(Pending{request, callback, context});
        ++inFlight;
        if (inFlight > maxInFlight) {
            maxInFlight = inFlight;
        }
        return SubmitResult{waitingForCredit ? SubmitDisposition::WaitingForCredit
                                             : SubmitDisposition::Accepted,
                            0};
    }

    bool completeAt(std::size_t index, hfs::Status status = hfs::Status::Ok,
                    int backendErrno = 0, std::uint32_t bytes = 0) {
        if (index >= pending.size()) {
            return false;
        }
        Pending item = pending[index];
        pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
        --inFlight;
        if (bytes == 0 && status == hfs::Status::Ok) {
            bytes = item.request.spanBytes;
        }
        item.callback(item.context, item.request.childIndex, status, backendErrno, bytes);
        return true;
    }

    bool completeFirst(hfs::Status status = hfs::Status::Ok, int backendErrno = 0,
                       std::uint32_t bytes = 0) {
        return completeAt(0, status, backendErrno, bytes);
    }

    bool fast = false;
    bool waitingForCredit = false;
    bool rejectNext = false;
    int rejectErrno = EIO;
    hfs::Status fastStatus = hfs::Status::Ok;
    int fastErrno = 0;
    std::size_t inFlight = 0;
    std::size_t maxInFlight = 0;
    std::vector<Request> submissions;
    std::vector<Pending> pending;
};

struct Fixture {
    static hfs::AsyncDataServiceConfig makeConfig(std::size_t ringDepth,
                                                  std::size_t maxInodesPerShard) {
        hfs::AsyncDataServiceConfig config;
        config.ringDepth = ringDepth;
        config.maxOperations = 16;
        config.maxInodesPerShard = maxInodesPerShard;
        return config;
    }

    Fixture(std::size_t arenaCapacity = 1U << 20, bool failAllocator = false,
            bool fastCompletion = false, std::size_t ringDepth = 8,
            std::size_t maxInodesPerShard = 8)
        : backends{&backend}, serviceConfig(makeConfig(ringDepth, maxInodesPerShard)),
          service(serviceConfig, arena, backends) {
        backend.fast = fastCompletion;
        arena.configure(arenaCapacity, failAllocator ? &failingAlloc : &arenaAlloc,
                        &arenaFree, nullptr);
    }

    hfs::DmaArena arena;
    FakeBackend backend;
    std::array<hfs::AsyncDataBackend *, 1> backends;
    hfs::AsyncDataServiceConfig serviceConfig;
    hfs::AsyncDataService service;
};

hfs::AsyncDataRequest makeRequest(const hfs::ExtentMap &map, std::vector<std::byte> &buffer,
                                  std::uint64_t fileId, std::uint64_t offset,
                                  bool write = false) {
    hfs::AsyncDataRequest request;
    request.fileId = fileId;
    request.offset = offset;
    request.length = buffer.size();
    request.buffer = std::span<std::byte>(buffer.data(), buffer.size());
    request.extentMap = &map;
    request.logicalSize = 1U << 20;
    request.write = write;
    request.fixedOverwrite = true;
    return request;
}

bool submitOne(Fixture &fixture, RecordingSink &sink, const hfs::AsyncDataRequest &request,
               std::uint32_t generation, std::vector<hfs::IoHandle> &handles) {
    auto result = fixture.service.submit(
        request, hfs::CompletionToken{generation, generation}, sink);
    if (!result.ok()) {
        return false;
    }
    handles.push_back(std::move(result.value()));
    return true;
}

void reap(RecordingSink &sink, std::vector<hfs::IoHandle> &handles) {
    for (hfs::IoHandle &handle : handles) {
        handle.reset();
    }
    sink.reap(sink.events.size());
    sink.events.clear();
}

} // namespace

int main() {
    hfs::Operation::setPoolCapacity(256);

    /* Read/read operations for one inode are admitted in parallel. */
    {
        Fixture fixture;
        RecordingSink sink(8);
        hfs::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::vector<std::byte>> buffers(4, std::vector<std::byte>(4096));
        std::vector<hfs::IoHandle> handles;
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            HFS_TEST_CHECK(submitOne(fixture, sink,
                                     makeRequest(map, buffers[index], 7, index * 4096),
                                     static_cast<std::uint32_t>(index + 1), handles));
        }
        fixture.service.pump(0);
        HFS_TEST_CHECK(fixture.backend.pending.size() == 4);
        HFS_TEST_CHECK(fixture.backend.maxInFlight == 4);
        while (!fixture.backend.pending.empty()) {
            HFS_TEST_CHECK(fixture.backend.completeFirst());
            fixture.service.pump(0);
        }
        HFS_TEST_CHECK(sink.events.size() == 4);
        HFS_TEST_CHECK(fixture.service.stats().schedulerWaits == 0);
        reap(sink, handles);
    }

    /* Overlapping writes are serialized by the one inode scheduler. */
    {
        Fixture fixture;
        RecordingSink sink(4);
        hfs::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> firstBuffer(4096);
        std::vector<std::byte> secondBuffer(4096);
        std::vector<hfs::IoHandle> handles;
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, firstBuffer, 11, 0, true), 1,
                                 handles));
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, secondBuffer, 11, 0, true), 2,
                                 handles));
        fixture.service.pump(0);
        HFS_TEST_CHECK(fixture.backend.pending.size() == 1);
        HFS_TEST_CHECK(fixture.service.stats().schedulerWaits == 1);
        HFS_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 1);
        HFS_TEST_CHECK(fixture.backend.pending.size() == 1);
        HFS_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 2);
        HFS_TEST_CHECK(sink.events[0].requestId < sink.events[1].requestId);
        reap(sink, handles);
    }

    /* A full ring returns Busy before publish and rolls the credit back. */
    {
        Fixture fixture(1U << 20, false, false, 2);
        RecordingSink sink(4);
        hfs::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> firstBuffer(4096);
        std::vector<std::byte> secondBuffer(4096);
        std::vector<std::byte> rejectedBuffer(4096);
        std::vector<hfs::IoHandle> handles;
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, firstBuffer, 13, 0), 1,
                                 handles));
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, secondBuffer, 13, 4096), 2,
                                 handles));
        auto rejected = fixture.service.submit(
            makeRequest(map, rejectedBuffer, 13, 8192), hfs::CompletionToken{3, 3}, sink);
        HFS_TEST_CHECK(!rejected.ok() && rejected.status() == hfs::Status::Busy);
        HFS_TEST_CHECK(sink.credits() == 2);
        fixture.service.pump(0);
        while (!fixture.backend.pending.empty()) {
            HFS_TEST_CHECK(fixture.backend.completeFirst());
            fixture.service.pump(0);
        }
        HFS_TEST_CHECK(sink.events.size() == 2);
        reap(sink, handles);
    }

    /* The actor uses a fixed snapshot after the caller destroys its extent map. */
    {
        Fixture fixture;
        RecordingSink sink(2);
        std::vector<std::byte> buffer(4096);
        std::vector<hfs::IoHandle> handles;
        {
            hfs::SyntheticExtentMap map;
            map.add(0, 1U << 20, 0);
            HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 15, 0), 1,
                                     handles));
        }
        fixture.service.pump(0);
        HFS_TEST_CHECK(fixture.backend.pending.size() == 1);
        HFS_TEST_CHECK(fixture.backend.submissions[0].physicalBlock == 0);
        HFS_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 1);
        reap(sink, handles);
    }

    /* Mutating the source map after submit cannot change the captured mapping. */
    {
        Fixture fixture;
        RecordingSink sink(2);
        hfs::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        std::vector<hfs::IoHandle> handles;
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 16, 0), 1,
                                 handles));
        map.clear();
        map.add(0, 1U << 20, 4096U * 77U);
        fixture.service.pump(0);
        HFS_TEST_CHECK(fixture.backend.pending.size() == 1);
        HFS_TEST_CHECK(fixture.backend.submissions[0].physicalBlock == 0);
        HFS_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 1);
        reap(sink, handles);
    }

    /* Snapshot failure rolls back credit, operation, sidecar, and publication. */
    {
        Fixture fixture;
        RecordingSink sink(1);
        FailingExtentMap map;
        std::vector<std::byte> buffer(4096);
        const std::size_t liveBefore = hfs::Operation::liveCount();
        auto result = fixture.service.submit(
            makeRequest(map, buffer, 18, 0), hfs::CompletionToken{1, 1}, sink);
        HFS_TEST_CHECK(!result.ok() && result.status() == hfs::Status::NoMemory);
        HFS_TEST_CHECK(sink.credits() == 0);
        HFS_TEST_CHECK(sink.events.empty());
        HFS_TEST_CHECK(fixture.service.outstanding() == 0);
        HFS_TEST_CHECK(fixture.service.stats().accepted == 0);
        HFS_TEST_CHECK(hfs::Operation::liveCount() == liveBefore);
    }

    /* An empty scheduler slot is reusable for serial requests from new inodes. */
    {
        Fixture fixture(1U << 20, false, false, 8, 1);
        RecordingSink sink(1);
        hfs::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        std::vector<hfs::IoHandle> handles;
        for (std::uint64_t fileId = 100; fileId < 105; ++fileId) {
            HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, fileId, 0),
                                     static_cast<std::uint32_t>(fileId), handles));
            fixture.service.pump(0);
            HFS_TEST_CHECK(fixture.backend.pending.size() == 1);
            HFS_TEST_CHECK(fixture.backend.completeFirst());
            fixture.service.pump(0);
            HFS_TEST_CHECK(sink.events.size() == 1);
            reap(sink, handles);
            handles.clear();
        }
    }

    /* Planning and permanent DMA failures happen after acceptance. */
    {
        Fixture fixture;
        RecordingSink sink(2);
        hfs::SyntheticExtentMap map;
        map.add(8192, 4096, 0);
        std::vector<std::byte> buffer(4096);
        std::vector<hfs::IoHandle> handles;
        auto request = makeRequest(map, buffer, 17, 0, true);
        HFS_TEST_CHECK(submitOne(fixture, sink, request, 1, handles));
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 1);
        HFS_TEST_CHECK(sink.events[0].status == hfs::Status::NotFound);
        HFS_TEST_CHECK(fixture.service.stats().planFailures == 1);
        reap(sink, handles);
    }
    {
        Fixture fixture(1U << 20, true);
        RecordingSink sink(2);
        hfs::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(100);
        std::vector<hfs::IoHandle> handles;
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 19, 1), 1, handles));
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 1);
        HFS_TEST_CHECK(sink.events[0].status == hfs::Status::NoMemory);
        HFS_TEST_CHECK(fixture.service.stats().dmaFailures == 1);
        reap(sink, handles);
    }

    /* Temporary arena pressure parks preparation without a scheduler node. */
    {
        Fixture fixture(4096);
        RecordingSink sink(2);
        hfs::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> firstBuffer(100);
        std::vector<std::byte> secondBuffer(100);
        std::vector<hfs::IoHandle> handles;
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, firstBuffer, 21, 1), 1,
                                 handles));
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, secondBuffer, 21, 8193), 2,
                                 handles));
        fixture.service.pump(0);
        HFS_TEST_CHECK(fixture.service.stats().preparing == 1);
        HFS_TEST_CHECK(fixture.backend.pending.size() == 1);
        HFS_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 1);
        fixture.service.pump(0);
        HFS_TEST_CHECK(fixture.backend.pending.size() == 1);
        HFS_TEST_CHECK(fixture.service.stats().preparing == 0);
        HFS_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 2);
        reap(sink, handles);
    }

    /* A first child error is retained while the other child drains. */
    {
        Fixture fixture;
        RecordingSink sink(2);
        hfs::SyntheticExtentMap map;
        map.add(0, 4096, 0);
        map.add(4096, 4096, 4096);
        std::vector<std::byte> buffer(8192);
        std::vector<hfs::IoHandle> handles;
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 23, 0), 1, handles));
        fixture.service.pump(0);
        HFS_TEST_CHECK(fixture.backend.pending.size() == 2);
        HFS_TEST_CHECK(fixture.backend.completeFirst(hfs::Status::IoError, EIO, 0));
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.empty());
        HFS_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 1);
        HFS_TEST_CHECK(sink.events[0].status == hfs::Status::IoError);
        HFS_TEST_CHECK(sink.events[0].backendErrno == EIO);
        HFS_TEST_CHECK(sink.events[0].completedBytes == 4096);
        reap(sink, handles);
    }

    /* Fixed overwrite RMW keeps the read and merged write in one child chain. */
    {
        Fixture fixture;
        RecordingSink sink(2);
        hfs::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(100);
        std::vector<hfs::IoHandle> handles;
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 27, 1, true), 1,
                                 handles));
        fixture.service.pump(0);
        HFS_TEST_CHECK(fixture.backend.pending.size() == 1);
        HFS_TEST_CHECK(!fixture.backend.submissions[0].write);
        HFS_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        HFS_TEST_CHECK(fixture.backend.pending.size() == 1);
        HFS_TEST_CHECK(fixture.backend.submissions.size() == 2);
        HFS_TEST_CHECK(fixture.backend.submissions[1].write);
        HFS_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 1);
        HFS_TEST_CHECK(sink.events[0].completedBytes == buffer.size());
        reap(sink, handles);
    }

    /* Inline backend completion is deferred through the ready node and publishes once. */
    {
        Fixture fixture(1U << 20, false, true);
        RecordingSink sink(2);
        hfs::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        std::vector<hfs::IoHandle> handles;
        HFS_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 29, 0), 1, handles));
        HFS_TEST_CHECK(sink.events.empty());
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 1);
        fixture.service.pump(0);
        fixture.service.pump(0);
        HFS_TEST_CHECK(sink.events.size() == 1);
        HFS_TEST_CHECK(fixture.service.stats().completionPublished == 1);
        reap(sink, handles);
    }

    std::printf("async_data_service_test: ok\n");
    return 0;
}
