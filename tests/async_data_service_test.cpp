#include <array>
#include <cerrno>
#include <chrono>
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

struct RecordingSink final : uuse::AsyncDataSink {
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

    bool publish(std::uint32_t shard, const uuse::CompletionEvent &event) noexcept override {
        (void)shard;
        events.push_back(event);
        return true;
    }

    void reap(std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            releaseCredit();
        }
    }

    std::size_t credits() const noexcept { return credits_; }

    std::vector<uuse::CompletionEvent> events;

private:
    std::size_t limit_ = 0;
    std::size_t credits_ = 0;
};

/* Owned sink used to prove the service keeps it alive until publish. */
struct OwnedSink final : uuse::AsyncDataSink {
    explicit OwnedSink(std::shared_ptr<std::size_t> counter) : published(std::move(counter)) {}

    bool tryAcquireCredit() noexcept override {
        if (credits_ >= 4) {
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

    bool publish(std::uint32_t, const uuse::CompletionEvent &) noexcept override {
        ++*published;
        return true;
    }

    std::shared_ptr<std::size_t> published;
    std::size_t credits_ = 0;
};

/* Sink that refuses delivery to exercise the dropped-completion accounting. */
struct RejectingSink final : uuse::AsyncDataSink {
    bool tryAcquireCredit() noexcept override { return true; }
    void releaseCredit() noexcept override {}
    bool publish(std::uint32_t, const uuse::CompletionEvent &) noexcept override {
        return false;
    }
};

struct FailingExtentMap final : uuse::ExtentMap {
    uuse::Result<std::size_t> lookup(std::uint64_t, std::uint64_t,
                                    std::span<uuse::ExtentView>) const override {
        return uuse::makeError<std::size_t>(uuse::Status::NoMemory,
                                            "synthetic snapshot failure");
    }
};

class FakeBackend final : public uuse::AsyncDataBackend {
public:
    struct Pending {
        Request request{};
        CompletionCallback callback = nullptr;
        void *context = nullptr;
    };

    SubmitResult submitRead(const Request &request, CompletionCallback callback,
                            void *context) noexcept override {
        ++readSubmissions;
        return submit(request, callback, context);
    }

    SubmitResult submitWrite(const Request &request, CompletionCallback callback,
                             void *context) noexcept override {
        ++writeSubmissions;
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
                     fastStatus == uuse::Status::Ok ? request.spanBytes : 0);
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

    bool completeAt(std::size_t index, uuse::Status status = uuse::Status::Ok,
                    int backendErrno = 0, std::uint32_t bytes = 0) {
        if (index >= pending.size()) {
            return false;
        }
        Pending item = pending[index];
        pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
        --inFlight;
        if (bytes == 0 && status == uuse::Status::Ok) {
            bytes = item.request.spanBytes;
        }
        item.callback(item.context, item.request.childIndex, status, backendErrno, bytes);
        return true;
    }

    bool completeFirst(uuse::Status status = uuse::Status::Ok, int backendErrno = 0,
                       std::uint32_t bytes = 0) {
        return completeAt(0, status, backendErrno, bytes);
    }

    bool fast = false;
    bool waitingForCredit = false;
    bool rejectNext = false;
    int rejectErrno = EIO;
    uuse::Status fastStatus = uuse::Status::Ok;
    int fastErrno = 0;
    std::size_t inFlight = 0;
    std::size_t maxInFlight = 0;
    std::size_t readSubmissions = 0;
    std::size_t writeSubmissions = 0;
    std::vector<Request> submissions;
    std::vector<Pending> pending;
};

struct Fixture {
    static uuse::AsyncDataServiceConfig makeConfig(std::size_t ringDepth,
                                                  std::size_t maxInodesPerShard) {
        uuse::AsyncDataServiceConfig config;
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

    uuse::DmaArena arena;
    FakeBackend backend;
    std::array<uuse::AsyncDataBackend *, 1> backends;
    uuse::AsyncDataServiceConfig serviceConfig;
    uuse::AsyncDataService service;
};

uuse::AsyncDataRequest makeRequest(const uuse::ExtentMap &map, std::vector<std::byte> &buffer,
                                  std::uint64_t fileId, std::uint64_t offset,
                                  bool write = false) {
    uuse::AsyncDataRequest request;
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

/* Stack sinks outlive every request in this test, so a no-op owner satisfies
 * the core ownership contract without transferring lifetime. */
template <typename Sink>
std::shared_ptr<void> borrowedOwner(Sink &sink) {
    return std::shared_ptr<void>(static_cast<void *>(&sink), [](void *) {});
}

template <typename Sink>
bool submitOne(Fixture &fixture, Sink &sink, const uuse::AsyncDataRequest &request,
               std::uint32_t generation, std::vector<uuse::IoHandle> &handles) {
    auto result = fixture.service.submit(
        request, uuse::CompletionToken{generation, generation}, sink, borrowedOwner(sink));
    if (!result.ok()) {
        return false;
    }
    handles.push_back(std::move(result.value()));
    return true;
}

void reap(RecordingSink &sink, std::vector<uuse::IoHandle> &handles) {
    for (uuse::IoHandle &handle : handles) {
        handle.reset();
    }
    sink.reap(sink.events.size());
    sink.events.clear();
}

} // namespace

int main() {
    uuse::Operation::setPoolCapacity(256);

    /* Read/read operations for one inode are admitted in parallel. */
    {
        Fixture fixture;
        RecordingSink sink(8);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::vector<std::byte>> buffers(4, std::vector<std::byte>(4096));
        std::vector<uuse::IoHandle> handles;
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            UUSE_TEST_CHECK(submitOne(fixture, sink,
                                     makeRequest(map, buffers[index], 7, index * 4096),
                                     static_cast<std::uint32_t>(index + 1), handles));
        }
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 4);
        UUSE_TEST_CHECK(fixture.backend.maxInFlight == 4);
        UUSE_TEST_CHECK(fixture.backend.readSubmissions == 4);
        UUSE_TEST_CHECK(fixture.backend.writeSubmissions == 0);
        while (!fixture.backend.pending.empty()) {
            UUSE_TEST_CHECK(fixture.backend.completeFirst());
            fixture.service.pump(0);
        }
        UUSE_TEST_CHECK(sink.events.size() == 4);
        UUSE_TEST_CHECK(fixture.service.stats().schedulerWaits == 0);
        reap(sink, handles);
    }

    /* Overlapping writes are serialized by the one inode scheduler. */
    {
        Fixture fixture;
        RecordingSink sink(4);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> firstBuffer(4096);
        std::vector<std::byte> secondBuffer(4096);
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, firstBuffer, 11, 0, true), 1,
                                 handles));
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, secondBuffer, 11, 0, true), 2,
                                 handles));
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
        UUSE_TEST_CHECK(fixture.backend.readSubmissions == 0);
        UUSE_TEST_CHECK(fixture.backend.writeSubmissions == 1);
        UUSE_TEST_CHECK(fixture.service.stats().schedulerWaits == 1);
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 2);
        UUSE_TEST_CHECK(sink.events[0].requestId < sink.events[1].requestId);
        reap(sink, handles);
    }

    /* A full ring returns Busy before publish and rolls the credit back. */
    {
        Fixture fixture(1U << 20, false, false, 2);
        RecordingSink sink(4);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> firstBuffer(4096);
        std::vector<std::byte> secondBuffer(4096);
        std::vector<std::byte> rejectedBuffer(4096);
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, firstBuffer, 13, 0), 1,
                                 handles));
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, secondBuffer, 13, 4096), 2,
                                 handles));
        auto rejected = fixture.service.submit(
            makeRequest(map, rejectedBuffer, 13, 8192), uuse::CompletionToken{3, 3}, sink,
            borrowedOwner(sink));
        UUSE_TEST_CHECK(!rejected.ok() && rejected.status() == uuse::Status::Busy);
        UUSE_TEST_CHECK(sink.credits() == 2);
        fixture.service.pump(0);
        while (!fixture.backend.pending.empty()) {
            UUSE_TEST_CHECK(fixture.backend.completeFirst());
            fixture.service.pump(0);
        }
        UUSE_TEST_CHECK(sink.events.size() == 2);
        reap(sink, handles);
    }

    /* The actor uses a fixed snapshot after the caller destroys its extent map. */
    {
        Fixture fixture;
        RecordingSink sink(2);
        std::vector<std::byte> buffer(4096);
        std::vector<uuse::IoHandle> handles;
        {
            uuse::SyntheticExtentMap map;
            map.add(0, 1U << 20, 0);
            UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 15, 0), 1,
                                     handles));
        }
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
        UUSE_TEST_CHECK(fixture.backend.submissions[0].physicalBlock == 0);
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        reap(sink, handles);
    }

    /* Mutating the source map after submit cannot change the captured mapping. */
    {
        Fixture fixture;
        RecordingSink sink(2);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 16, 0), 1,
                                 handles));
        map.clear();
        map.add(0, 1U << 20, 4096U * 77U);
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
        UUSE_TEST_CHECK(fixture.backend.submissions[0].physicalBlock == 0);
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        reap(sink, handles);
    }

    /* Accepted requests require an owner that keeps the sink alive. */
    {
        Fixture fixture;
        RecordingSink sink(1);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        auto result = fixture.service.submit(
            makeRequest(map, buffer, 17, 0), uuse::CompletionToken{1, 1}, sink, nullptr);
        UUSE_TEST_CHECK(!result.ok() && result.status() == uuse::Status::InvalidArgument);
        UUSE_TEST_CHECK(sink.credits() == 0);
        UUSE_TEST_CHECK(fixture.service.outstanding() == 0);
    }

    /* Snapshot failure rolls back credit, operation, sidecar, and publication. */
    {
        Fixture fixture;
        RecordingSink sink(1);
        FailingExtentMap map;
        std::vector<std::byte> buffer(4096);
        const std::size_t liveBefore = uuse::Operation::liveCount();
        auto result = fixture.service.submit(
            makeRequest(map, buffer, 18, 0), uuse::CompletionToken{1, 1}, sink,
            borrowedOwner(sink));
        UUSE_TEST_CHECK(!result.ok() && result.status() == uuse::Status::NoMemory);
        UUSE_TEST_CHECK(sink.credits() == 0);
        UUSE_TEST_CHECK(sink.events.empty());
        UUSE_TEST_CHECK(fixture.service.outstanding() == 0);
        UUSE_TEST_CHECK(fixture.service.stats().accepted == 0);
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == liveBefore);
    }

    /* An empty scheduler slot is reusable for serial requests from new inodes. */
    {
        Fixture fixture(1U << 20, false, false, 8, 1);
        RecordingSink sink(1);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        std::vector<uuse::IoHandle> handles;
        for (std::uint64_t fileId = 100; fileId < 105; ++fileId) {
            UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, fileId, 0),
                                     static_cast<std::uint32_t>(fileId), handles));
            fixture.service.pump(0);
            UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
            UUSE_TEST_CHECK(fixture.backend.completeFirst());
            fixture.service.pump(0);
            UUSE_TEST_CHECK(sink.events.size() == 1);
            reap(sink, handles);
            handles.clear();
        }
    }

    /* Planning and permanent DMA failures happen after acceptance. */
    {
        Fixture fixture;
        RecordingSink sink(2);
        uuse::SyntheticExtentMap map;
        map.add(8192, 4096, 0);
        std::vector<std::byte> buffer(4096);
        std::vector<uuse::IoHandle> handles;
        auto request = makeRequest(map, buffer, 17, 0, true);
        UUSE_TEST_CHECK(submitOne(fixture, sink, request, 1, handles));
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        UUSE_TEST_CHECK(sink.events[0].status == uuse::Status::NotFound);
        UUSE_TEST_CHECK(fixture.service.stats().planFailures == 1);
        reap(sink, handles);
    }
    {
        Fixture fixture(1U << 20, true);
        RecordingSink sink(2);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(100);
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 19, 1), 1, handles));
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        UUSE_TEST_CHECK(sink.events[0].status == uuse::Status::NoMemory);
        UUSE_TEST_CHECK(fixture.service.stats().dmaFailures == 1);
        reap(sink, handles);
    }

    /* Temporary arena pressure parks preparation without a scheduler node. */
    {
        Fixture fixture(4096);
        RecordingSink sink(2);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> firstBuffer(100);
        std::vector<std::byte> secondBuffer(100);
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, firstBuffer, 21, 1), 1,
                                 handles));
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, secondBuffer, 21, 8193), 2,
                                 handles));
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.service.stats().preparing == 1);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
        UUSE_TEST_CHECK(fixture.service.stats().preparing == 0);
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 2);
        reap(sink, handles);
    }

    /* A first child error is retained while the other child drains. */
    {
        Fixture fixture;
        RecordingSink sink(2);
        uuse::SyntheticExtentMap map;
        map.add(0, 4096, 0);
        map.add(4096, 4096, 4096);
        std::vector<std::byte> buffer(8192);
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 23, 0), 1, handles));
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 2);
        UUSE_TEST_CHECK(fixture.backend.completeFirst(uuse::Status::IoError, EIO, 0));
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.empty());
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        UUSE_TEST_CHECK(sink.events[0].status == uuse::Status::IoError);
        UUSE_TEST_CHECK(sink.events[0].backendErrno == EIO);
        UUSE_TEST_CHECK(sink.events[0].completedBytes == 4096);
        reap(sink, handles);
    }

    /* Fixed overwrite RMW keeps the read and merged write in one child chain. */
    {
        Fixture fixture;
        RecordingSink sink(2);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(100);
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 27, 1, true), 1,
                                 handles));
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
        UUSE_TEST_CHECK(!fixture.backend.submissions[0].write);
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
        UUSE_TEST_CHECK(fixture.backend.submissions.size() == 2);
        UUSE_TEST_CHECK(fixture.backend.submissions[1].write);
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        UUSE_TEST_CHECK(sink.events[0].completedBytes == buffer.size());
        /* One RMW child performs two backend phases but counts as one child. */
        const auto stats = fixture.service.stats();
        UUSE_TEST_CHECK(stats.childSubmits == 1);
        UUSE_TEST_CHECK(stats.childCompletes == 1);
        UUSE_TEST_CHECK(stats.childSubmitRejected == 0);
        reap(sink, handles);
    }

    /* The service keeps the sink alive until the terminal completion publish. */
    {
        Fixture fixture;
        auto published = std::make_shared<std::size_t>(0);
        auto sink = std::make_shared<OwnedSink>(published);
        std::weak_ptr<OwnedSink> weak = sink;
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        auto submitted = fixture.service.submit(
            makeRequest(map, buffer, 31, 0), uuse::CompletionToken{1, 1}, *sink, sink);
        UUSE_TEST_CHECK(submitted.ok());
        uuse::IoHandle handle = std::move(submitted).value();
        /* Drop the frontend reference while the request is still in flight. */
        sink.reset();
        UUSE_TEST_CHECK(!weak.expired());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(*published == 1);
        handle.reset();
        UUSE_TEST_CHECK(weak.expired());
    }

    /* A sink that refuses delivery is accounted as dropped, never published. */
    {
        Fixture fixture;
        RejectingSink sink;
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 33, 0), 1, handles));
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        const auto stats = fixture.service.stats();
        UUSE_TEST_CHECK(stats.completionPublished == 0);
        UUSE_TEST_CHECK(stats.completionDropped == 1);
        UUSE_TEST_CHECK(stats.accepted == 1);
        UUSE_TEST_CHECK(stats.terminal == 1);
        handles.clear();
    }

    /* WaitingForCredit is accepted and completes through the normal callback. */
    {
        Fixture fixture;
        fixture.backend.waitingForCredit = true;
        RecordingSink sink(2);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 35, 0), 1, handles));
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
        UUSE_TEST_CHECK(sink.events.empty());
        UUSE_TEST_CHECK(fixture.backend.completeFirst());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        UUSE_TEST_CHECK(fixture.service.stats().childSubmitRejected == 0);
        reap(sink, handles);
    }

    /* Hole (zero) children never consume bounce leases from a tiny arena. */
    {
        Fixture fixture(4096); /* arena smaller than the read */
        RecordingSink sink(2);
        uuse::SyntheticExtentMap map;
        map.add(1U << 20, 1U << 20, 0); /* extent starts after the read range */
        std::vector<std::byte> buffer(1U << 20, std::byte{0xAB});
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 37, 0), 1, handles));
        fixture.service.pump(0);
        UUSE_TEST_CHECK(fixture.backend.pending.empty());
        UUSE_TEST_CHECK(sink.events.size() == 1);
        UUSE_TEST_CHECK(sink.events[0].status == uuse::Status::Ok);
        for (const std::byte byte : buffer) {
            UUSE_TEST_CHECK(byte == std::byte{0});
        }
        reap(sink, handles);
    }

    /* Inode slot lookup stays O(1): a linear scan of max_inodes/shard cost
     * milliseconds per request once the file's slot had been reclaimed. */
    {
        Fixture fixture(1U << 20, false, false, 8, 262144);
        RecordingSink sink(2);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        const auto started = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < 2000; ++index) {
            std::vector<uuse::IoHandle> handles;
            UUSE_TEST_CHECK(submitOne(
                fixture, sink, makeRequest(map, buffer, 100000 + index, 0), 1, handles));
            fixture.service.pump(0);
            UUSE_TEST_CHECK(fixture.backend.pending.size() == 1);
            UUSE_TEST_CHECK(fixture.backend.completeFirst());
            fixture.service.pump(0);
            UUSE_TEST_CHECK(sink.events.size() == 1);
            reap(sink, handles);
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        UUSE_TEST_CHECK(elapsed < std::chrono::seconds(2));
    }

    /* Inline backend completion is deferred through the ready node and publishes once. */
    {
        Fixture fixture(1U << 20, false, true);
        RecordingSink sink(2);
        uuse::SyntheticExtentMap map;
        map.add(0, 1U << 20, 0);
        std::vector<std::byte> buffer(4096);
        std::vector<uuse::IoHandle> handles;
        UUSE_TEST_CHECK(submitOne(fixture, sink, makeRequest(map, buffer, 29, 0), 1, handles));
        UUSE_TEST_CHECK(sink.events.empty());
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        fixture.service.pump(0);
        fixture.service.pump(0);
        UUSE_TEST_CHECK(sink.events.size() == 1);
        UUSE_TEST_CHECK(fixture.service.stats().completionPublished == 1);
        reap(sink, handles);
    }

    std::printf("async_data_service_test: ok\n");
    return 0;
}
