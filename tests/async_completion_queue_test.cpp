#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/async_completion_queue.hpp"
#include "core/operation.hpp"
#include "test_support.hpp"

namespace {

/* Builds a value-only completion event for a shard producer. */
uuse::CompletionEvent makeEvent(std::uint64_t requestId, std::uint32_t shard) {
    uuse::CompletionEvent event;
    event.requestId = requestId;
    event.token = uuse::CompletionToken{static_cast<std::uint32_t>(requestId),
                                       static_cast<std::uint32_t>(requestId + 1)};
    event.status = shard == 0 ? uuse::Status::Ok : uuse::Status::IoError;
    event.backendErrno = shard == 0 ? 0 : 5;
    event.completedBytes = 4096 + shard;
    return event;
}

struct ExternalWaitContext {
    uuse::CompletionNotifier notifier;
    std::atomic<std::uint32_t> prepares{0};
};

void prepareExternalWait(void *opaque) noexcept {
    auto *context = static_cast<ExternalWaitContext *>(opaque);
    context->prepares.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t externalEpoch(void *opaque) noexcept {
    return static_cast<ExternalWaitContext *>(opaque)->notifier.epoch();
}

bool externalWaitSince(void *opaque, std::uint64_t observed,
                       std::chrono::microseconds timeout) noexcept {
    return static_cast<ExternalWaitContext *>(opaque)->notifier.waitSince(observed, timeout);
}

} // namespace

int main() {
    /* 1. IoHandle owns exactly one intrusive frontend reference and moves it. */
    {
        uuse::Operation::setPoolCapacity(8);
        auto *operation = uuse::Operation::create(77);
        UUSE_TEST_CHECK(operation != nullptr);
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 1);
        const uuse::CompletionToken token{3, 1};
        auto handle = operation->acquireFrontendHandle(token);
        operation->release();
        UUSE_TEST_CHECK(handle.valid());
        UUSE_TEST_CHECK(handle.requestId() == 77);
        UUSE_TEST_CHECK(handle.token().slot == token.slot &&
                       handle.token().generation == token.generation);

        uuse::IoHandle moved(std::move(handle));
        UUSE_TEST_CHECK(!handle.valid());
        UUSE_TEST_CHECK(moved.valid());
        uuse::IoHandle assigned;
        assigned = std::move(moved);
        UUSE_TEST_CHECK(!moved.valid());
        assigned.reset();
        UUSE_TEST_CHECK(!assigned.valid());
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);

        auto *retainedOperation = uuse::Operation::create(78);
        UUSE_TEST_CHECK(retainedOperation != nullptr);
        auto retained = retainedOperation->acquireFrontendHandle(token);
        retainedOperation->release();
        UUSE_TEST_CHECK(retained.valid());
        retained.reset();
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);
    }

    /* 2. Capacity is guaranteed per shard and overflow is observable. */
    {
        uuse::AsyncCompletionQueue queue(4, 8);
        UUSE_TEST_CHECK(queue.valid());
        UUSE_TEST_CHECK(queue.totalCapacity() == 32);
        for (std::uint64_t index = 0; index < 8; ++index) {
            UUSE_TEST_CHECK(queue.push(2, makeEvent(index, 2)));
        }
        UUSE_TEST_CHECK(!queue.push(2, makeEvent(8, 2)));
        UUSE_TEST_CHECK(queue.shardDiagnostics(2).overflow == 1);
        UUSE_TEST_CHECK(queue.diagnostics().overflow == 1);

        std::vector<uuse::CompletionEvent> output(8);
        UUSE_TEST_CHECK(queue.drain(output, output.size()) == 8);
        UUSE_TEST_CHECK(queue.shardDiagnostics(2).size == 0);
    }

    /* 3. External frontends can disable per-client eventfd creation. */
    {
        uuse::AsyncCompletionQueue queue;
        UUSE_TEST_CHECK(queue.initialize(1, 4, false));
        UUSE_TEST_CHECK(!queue.diagnostics().notifier.eventFd);
        UUSE_TEST_CHECK(queue.push(0, makeEvent(900, 0)));
        std::vector<uuse::CompletionEvent> output(1);
        UUSE_TEST_CHECK(queue.drain(output, output.size()) == 1);
    }

    /* 4. Four shard producers feed independent SPSC rings and one consumer drains them. */
    {
        constexpr std::size_t shardCount = 4;
        constexpr std::size_t perShard = 300;
        constexpr std::size_t capacity = 512;
        uuse::AsyncCompletionQueue queue(shardCount, capacity);
        UUSE_TEST_CHECK(queue.valid());
        std::atomic<bool> failed{false};
        std::vector<std::thread> producers;
        producers.reserve(shardCount);
        for (std::size_t shard = 0; shard < shardCount; ++shard) {
            producers.emplace_back([&queue, &failed, shard] {
                for (std::size_t index = 0; index < perShard; ++index) {
                    const std::uint64_t requestId =
                        static_cast<std::uint64_t>(shard * perShard + index);
                    while (!queue.push(shard, makeEvent(requestId,
                                                         static_cast<std::uint32_t>(shard)))) {
                        if (queue.diagnostics().invalidShard != 0) {
                            failed.store(true, std::memory_order_release);
                            return;
                        }
                        std::this_thread::yield();
                    }
                }
            });
        }

        const std::size_t total = shardCount * perShard;
        std::size_t received = 0;
        std::vector<uuse::CompletionEvent> output(37);
        std::vector<bool> seen(total, false);
        while (received < total && !failed.load(std::memory_order_acquire)) {
            const std::size_t count = queue.drain(output, output.size());
            for (std::size_t index = 0; index < count; ++index) {
                const auto requestId = output[index].requestId;
                UUSE_TEST_CHECK(requestId < total);
                UUSE_TEST_CHECK(!seen[static_cast<std::size_t>(requestId)]);
                seen[static_cast<std::size_t>(requestId)] = true;
                ++received;
            }
            if (count == 0) {
                std::this_thread::yield();
            }
        }
        for (auto &producer : producers) {
            producer.join();
        }
        while (received < total) {
            const std::size_t count = queue.drain(output, output.size());
            UUSE_TEST_CHECK(count != 0);
            for (std::size_t index = 0; index < count; ++index) {
                const auto requestId = output[index].requestId;
                UUSE_TEST_CHECK(requestId < total);
                UUSE_TEST_CHECK(!seen[static_cast<std::size_t>(requestId)]);
                seen[static_cast<std::size_t>(requestId)] = true;
                ++received;
            }
        }
        UUSE_TEST_CHECK(!failed.load(std::memory_order_acquire));
        UUSE_TEST_CHECK(received == total);
        UUSE_TEST_CHECK(queue.diagnostics().overflow == 0);
        UUSE_TEST_CHECK(queue.diagnostics().drained == total);
        for (bool value : seen) {
            UUSE_TEST_CHECK(value);
        }
    }

    /* 5. Poll validates min/max, is nonblocking for zero timeout, and preserves events. */
    {
        uuse::AsyncCompletionQueue queue(2, 4);
        std::vector<uuse::CompletionEvent> output(2);
        auto invalid = queue.poll(output, 2, 1, std::chrono::microseconds(0));
        UUSE_TEST_CHECK(invalid.status == uuse::Status::InvalidArgument);
        auto empty = queue.poll(output, 1, 1, std::chrono::microseconds(0));
        UUSE_TEST_CHECK(empty.status == uuse::Status::Ok && empty.count == 0);
        UUSE_TEST_CHECK(queue.diagnostics().drained == 0);

        UUSE_TEST_CHECK(queue.push(1, makeEvent(991, 1)));
        auto immediate = queue.poll(output, 1, 1, std::chrono::microseconds(0));
        UUSE_TEST_CHECK(immediate.status == uuse::Status::Ok && immediate.count == 1);
        UUSE_TEST_CHECK(output[0].requestId == 991);
    }

    /* 6. A new producer still wakes after the previous poll drained to empty. */
    {
        uuse::AsyncCompletionQueue queue(1, 4);
        std::vector<uuse::CompletionEvent> output(1);
        UUSE_TEST_CHECK(queue.push(0, makeEvent(1000, 0)));
        auto first = queue.poll(output, 1, 1, std::chrono::microseconds(0));
        UUSE_TEST_CHECK(first.status == uuse::Status::Ok && first.count == 1);

        std::atomic<bool> producerFailed{false};
        std::thread producer([&queue, &producerFailed] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!queue.push(0, makeEvent(1001, 0))) {
                producerFailed.store(true, std::memory_order_release);
            }
        });
        auto second = queue.poll(output, 1, 1, std::chrono::milliseconds(250));
        producer.join();
        UUSE_TEST_CHECK(!producerFailed.load(std::memory_order_acquire));
        UUSE_TEST_CHECK(second.status == uuse::Status::Ok && second.count == 1);
        UUSE_TEST_CHECK(output[0].requestId == 1001);
    }

    /* 7. Timed poll wakes from a producer and leaves in-flight state untouched on timeout. */
    {
        uuse::AsyncCompletionQueue queue(1, 4);
        std::vector<uuse::CompletionEvent> output(1);
        const auto start = std::chrono::steady_clock::now();
        auto timeout = queue.poll(output, 1, 1, std::chrono::milliseconds(20));
        const auto elapsed = std::chrono::steady_clock::now() - start;
        UUSE_TEST_CHECK(timeout.status == uuse::Status::Ok && timeout.count == 0);
        UUSE_TEST_CHECK(elapsed >= std::chrono::milliseconds(10));

        std::atomic<bool> producerFailed{false};
        std::thread producer([&queue, &producerFailed] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!queue.push(0, makeEvent(1001, 0))) {
                producerFailed.store(true, std::memory_order_release);
            }
        });
        const auto wakeStart = std::chrono::steady_clock::now();
        auto woken = queue.poll(output, 1, 1, std::chrono::milliseconds(250));
        const auto wakeElapsed = std::chrono::steady_clock::now() - wakeStart;
        producer.join();
        UUSE_TEST_CHECK(!producerFailed.load(std::memory_order_acquire));
        UUSE_TEST_CHECK(woken.status == uuse::Status::Ok && woken.count == 1);
        UUSE_TEST_CHECK(output[0].requestId == 1001);
        UUSE_TEST_CHECK(wakeElapsed < std::chrono::milliseconds(200));
        UUSE_TEST_CHECK(queue.diagnostics().drained == 1);
    }

    /* 8. Rearming clears stale wake state without losing a queued event. */
    {
        uuse::AsyncCompletionRing ring(4);
        UUSE_TEST_CHECK(ring.push(makeEvent(2000, 0)));
        UUSE_TEST_CHECK(ring.rearmIfNonEmpty());
        std::vector<uuse::CompletionEvent> output(1);
        UUSE_TEST_CHECK(ring.drain(output) == 1);
        UUSE_TEST_CHECK(!ring.rearmIfNonEmpty());
        UUSE_TEST_CHECK(ring.push(makeEvent(2001, 0)));
        UUSE_TEST_CHECK(ring.rearmIfNonEmpty());
        UUSE_TEST_CHECK(ring.drain(output) == 1);
    }

    /* 9. An external wake source uses the same core spin/rescan/deadline path. */
    {
        uuse::AsyncCompletionQueue queue;
        UUSE_TEST_CHECK(queue.initialize(1, 4, false));
        ExternalWaitContext context;
        UUSE_TEST_CHECK(context.notifier.initialize(false));
        const uuse::CompletionWaitStrategy strategy{
            &prepareExternalWait, &externalEpoch, &externalWaitSince, &context};
        std::vector<uuse::CompletionEvent> output(1);
        std::atomic<bool> producerFailed{false};
        std::thread producer([&queue, &context, &producerFailed] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!queue.push(0, makeEvent(3000, 0)) || !context.notifier.notify()) {
                producerFailed.store(true, std::memory_order_release);
            }
        });
        const auto result = queue.poll(output, 1, 1, std::chrono::milliseconds(250), &strategy);
        producer.join();
        UUSE_TEST_CHECK(!producerFailed.load(std::memory_order_acquire));
        UUSE_TEST_CHECK(result.status == uuse::Status::Ok && result.count == 1);
        UUSE_TEST_CHECK(output[0].requestId == 3000);
        UUSE_TEST_CHECK(context.prepares.load(std::memory_order_relaxed) != 0);
    }

    uuse::CompletionNotifier notifier;
    UUSE_TEST_CHECK(notifier.initialize());
    std::printf("async_completion_queue_test: ok (eventfd=%s)\n",
                 notifier.usingEventFd() ? "yes" : "no");
    return 0;
}
