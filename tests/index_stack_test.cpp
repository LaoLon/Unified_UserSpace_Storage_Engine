/* BoundedIndexStack MPMC churn: every index is delivered to exactly one
 * consumer and every index returns to the stack afterwards. */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "core/index_stack.hpp"
#include "test_support.hpp"

namespace {

constexpr std::uint32_t kCapacity = 256;
constexpr int kThreads = 4;
constexpr std::uint64_t kIterations = 200000;

} // namespace

int main() {
    uuse::BoundedIndexStack stack;
    UUSE_TEST_CHECK(stack.initialize(kCapacity));
    UUSE_TEST_CHECK(stack.capacity() == kCapacity);

    /* Per-index claim marker detects any duplicate delivery. */
    auto owner = std::make_unique<std::atomic<int>[]>(kCapacity);
    for (std::uint32_t index = 0; index < kCapacity; ++index) {
        owner[index].store(0, std::memory_order_relaxed);
    }

    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> empty{0};
    std::atomic<bool> failure{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&]() {
            for (std::uint64_t iteration = 0; iteration < kIterations; ++iteration) {
                std::uint32_t index = 0;
                if (!stack.pop(index)) {
                    empty.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (index >= kCapacity ||
                    owner[index].exchange(1, std::memory_order_acq_rel) != 0) {
                    failure.store(true);
                    break;
                }
                delivered.fetch_add(1, std::memory_order_relaxed);
                owner[index].store(0, std::memory_order_release);
                if (!stack.push(index)) {
                    failure.store(true);
                    break;
                }
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    UUSE_TEST_CHECK(!failure.load());
    UUSE_TEST_CHECK(delivered.load() > 0);

    /* Every index must be back on the stack exactly once. */
    std::size_t drained = 0;
    std::uint32_t index = 0;
    while (stack.pop(index)) {
        ++drained;
    }
    UUSE_TEST_CHECK(drained == kCapacity);
    UUSE_TEST_CHECK(!stack.pop(index));

    std::printf("index_stack_test: delivered=%llu empty=%llu ok\n",
                static_cast<unsigned long long>(delivered.load()),
                static_cast<unsigned long long>(empty.load()));
    return 0;
}
