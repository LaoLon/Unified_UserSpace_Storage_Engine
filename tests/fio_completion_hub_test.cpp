#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "fio/fio_completion_hub.hpp"
#include "test_support.hpp"

int main() {
    hfs::fio::FioCompletionHub hub;
    HFS_TEST_CHECK(hub.initialize());
    auto *endpoint = hub.acquire();
    HFS_TEST_CHECK(endpoint != nullptr);
    auto *other = hub.acquire();
    HFS_TEST_CHECK(other != nullptr);

    const std::uint32_t first = endpoint->epoch();
    endpoint->prepareWait();
    std::thread producer([endpoint] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        endpoint->notify();
        endpoint->notify();
    });
    HFS_TEST_CHECK(endpoint->waitSince(first, std::chrono::milliseconds(250)));
    producer.join();
    HFS_TEST_CHECK(endpoint->epoch() != first);
    const std::uint32_t otherFirst = other->epoch();
    other->notify();
    HFS_TEST_CHECK(other->epoch() != otherFirst);
    HFS_TEST_CHECK(endpoint->epoch() != first);

    const std::uint32_t second = endpoint->epoch();
    endpoint->prepareWait();
    HFS_TEST_CHECK(!endpoint->waitSince(second, std::chrono::milliseconds(5)));

    endpoint->prepareWait();
    hfs::fio::FioCompletionHub::wakeCallback(endpoint);
    HFS_TEST_CHECK(endpoint->epoch() != second);
    HFS_TEST_CHECK(endpoint->waitSince(second, std::chrono::microseconds(0)));

    /* release() wakes and waits for an active waiter before reuse. */
    {
        auto *busy = hub.acquire();
        HFS_TEST_CHECK(busy != nullptr);
        const std::uint32_t observed = busy->epoch();
        busy->prepareWait();
        std::atomic<bool> waiterDone{false};
        std::thread waiter([busy, observed, &waiterDone] {
            (void)busy->waitSince(observed, std::chrono::milliseconds(500));
            waiterDone.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        hub.release(busy);
        waiter.join();
        HFS_TEST_CHECK(waiterDone.load(std::memory_order_acquire));
        auto *reused = hub.acquire();
        HFS_TEST_CHECK(reused != nullptr);
        hub.release(reused);
    }

    hub.release(other);
    hub.release(endpoint);
    hub.shutdown();
    std::printf("fio_completion_hub_test: ok\n");
    return 0;
}
