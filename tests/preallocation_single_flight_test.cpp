#include <cerrno>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "fio/preallocation_single_flight.hpp"
#include "test_support.hpp"

namespace {

using hfs::fio::PreallocationKey;
using hfs::fio::PreallocationResult;
using hfs::fio::PreallocationResultStatus;
using hfs::fio::PreallocationSingleFlight;
using hfs::fio::PreallocationState;

/* Keep callback state outside the helper to model a real File::allocate call. */
struct CallbackContext {
    std::atomic<std::uint32_t> calls{0};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{true};
    PreallocationResult result{};
};

/* Return a fixed owner result and optionally hold the owner until released. */
PreallocationResult ownerCallback(const PreallocationKey &, void *opaque) noexcept {
    auto *context = static_cast<CallbackContext *>(opaque);
    context->calls.fetch_add(1, std::memory_order_relaxed);
    context->entered.store(true, std::memory_order_release);
    while (!context->release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return context->result;
}

/* Build valid keys with distinct engine/file identities. */
PreallocationKey makeKey(std::uint64_t engineGeneration, std::uint64_t inodeId,
                         std::uint32_t fileGeneration, std::uint64_t offset,
                         std::uint64_t length) {
    return PreallocationKey{engineGeneration,
                            hfs::fio::FileHandle{inodeId, fileGeneration}, offset,
                            length};
}

} // namespace

int main() {
    const PreallocationKey key = makeKey(1, 9, 3, 4096, 8192);
    const PreallocationResult success =
        PreallocationResult::makeSuccess(16384, 0, 16384);

    /* One owner publishes one validated result to every concurrent waiter. */
    {
        PreallocationSingleFlight helper(8);
        CallbackContext context;
        context.result = success;
        context.release.store(false, std::memory_order_release);

        PreallocationResult ownerResult;
        std::thread owner([&]() {
            ownerResult = helper.getOrAllocate(key, ownerCallback, &context);
        });
        while (!context.entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        constexpr std::size_t waiterCount = 7;
        std::atomic<std::size_t> started{0};
        std::vector<PreallocationResult> waiterResults(waiterCount);
        std::vector<std::thread> waiters;
        waiters.reserve(waiterCount);
        for (std::size_t index = 0; index < waiterCount; ++index) {
            waiters.emplace_back([&, index]() {
                started.fetch_add(1, std::memory_order_release);
                waiterResults[index] =
                    helper.getOrAllocate(key, ownerCallback, &context);
            });
        }
        while (started.load(std::memory_order_acquire) != waiterCount) {
            std::this_thread::yield();
        }

        context.release.store(true, std::memory_order_release);
        owner.join();
        for (auto &waiter : waiters) {
            waiter.join();
        }

        HFS_TEST_CHECK(context.calls.load(std::memory_order_relaxed) == 1);
        HFS_TEST_CHECK(ownerResult == success);
        for (const auto &result : waiterResults) {
            HFS_TEST_CHECK(result == success);
        }
        HFS_TEST_CHECK(helper.stateFor(key) == PreallocationState::Ready);
        HFS_TEST_CHECK(helper.size() == 1);
    }

    /* A successful callback with incomplete evidence is retained as one failure. */
    {
        PreallocationSingleFlight helper(2);
        CallbackContext context;
        context.result = PreallocationResult::makeSuccess(4096, 4096, 4096);

        const PreallocationResult first =
            helper.getOrAllocate(key, ownerCallback, &context);
        HFS_TEST_CHECK(first.status == PreallocationResultStatus::InvalidOwnerResult);
        HFS_TEST_CHECK(helper.stateFor(key) == PreallocationState::Failed);

        context.result = success;
        const PreallocationResult second =
            helper.getOrAllocate(key, ownerCallback, &context);
        HFS_TEST_CHECK(second == first);
        HFS_TEST_CHECK(context.calls.load(std::memory_order_relaxed) == 1);
    }

    /* Owner failure is broadcast unchanged, including the backend errno. */
    {
        PreallocationSingleFlight helper(8);
        CallbackContext context;
        context.result = PreallocationResult::makeFailure(ENOSPC);
        context.release.store(false, std::memory_order_release);

        PreallocationResult ownerResult;
        std::thread owner([&]() {
            ownerResult = helper.getOrAllocate(key, ownerCallback, &context);
        });
        while (!context.entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        PreallocationResult waiterResult;
        std::thread waiter([&]() {
            waiterResult = helper.getOrAllocate(key, ownerCallback, &context);
        });
        context.release.store(true, std::memory_order_release);
        owner.join();
        waiter.join();

        HFS_TEST_CHECK(context.calls.load(std::memory_order_relaxed) == 1);
        HFS_TEST_CHECK(ownerResult.status == PreallocationResultStatus::OwnerFailed);
        HFS_TEST_CHECK(ownerResult.backendErrno == ENOSPC);
        HFS_TEST_CHECK(waiterResult == ownerResult);
        HFS_TEST_CHECK(helper.stateFor(key) == PreallocationState::Failed);
    }

    /* Capacity is bounded, and explicit clear permits a later lifecycle owner. */
    {
        PreallocationSingleFlight helper(1);
        CallbackContext context;
        context.result = success;
        const PreallocationKey secondKey = makeKey(1, 10, 4, 0, 4096);

        HFS_TEST_CHECK(helper.getOrAllocate(key, ownerCallback, &context).ok());
        const PreallocationResult full =
            helper.getOrAllocate(secondKey, ownerCallback, &context);
        HFS_TEST_CHECK(full.status == PreallocationResultStatus::CapacityExhausted);
        HFS_TEST_CHECK(context.calls.load(std::memory_order_relaxed) == 1);
        HFS_TEST_CHECK(helper.clear(key));
        HFS_TEST_CHECK(helper.getOrAllocate(secondKey, ownerCallback, &context).ok());
        HFS_TEST_CHECK(context.calls.load(std::memory_order_relaxed) == 2);
    }

    /* Generation reset removes only stale generations and reset removes all state. */
    {
        PreallocationSingleFlight helper(4);
        CallbackContext context;
        context.result = success;
        const PreallocationKey newer = makeKey(2, 9, 3, 4096, 8192);

        HFS_TEST_CHECK(helper.getOrAllocate(key, ownerCallback, &context).ok());
        HFS_TEST_CHECK(helper.getOrAllocate(newer, ownerCallback, &context).ok());
        HFS_TEST_CHECK(helper.resetForGeneration(2) == 1);
        HFS_TEST_CHECK(helper.stateFor(key) == PreallocationState::Absent);
        HFS_TEST_CHECK(helper.stateFor(newer) == PreallocationState::Ready);
        HFS_TEST_CHECK(helper.size() == 1);

        helper.reset();
        HFS_TEST_CHECK(helper.size() == 0);
        HFS_TEST_CHECK(helper.stateFor(newer) == PreallocationState::Absent);
    }

    /* Invalid keys never invoke an owner and never consume a bounded slot. */
    {
        PreallocationSingleFlight helper(1);
        CallbackContext context;
        context.result = success;
        const PreallocationKey invalid{0, hfs::fio::FileHandle{}, 0, 4096};
        const PreallocationResult result =
            helper.getOrAllocate(invalid, ownerCallback, &context);
        HFS_TEST_CHECK(result.status == PreallocationResultStatus::InvalidKey);
        HFS_TEST_CHECK(context.calls.load(std::memory_order_relaxed) == 0);
        HFS_TEST_CHECK(helper.size() == 0);
    }

    std::printf("preallocation_single_flight_test: ok\n");
    return 0;
}
