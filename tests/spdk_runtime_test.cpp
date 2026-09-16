#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "test_support.hpp"

int main() {
    const std::string lockPath = "/tmp/hfs2-spdk-" + std::to_string(::getpid()) + ".lock";
    auto parsed = hfs::parseConfig(validConfigText(lockPath), "memory");
    HFS_TEST_CHECK(parsed.ok());

    auto created = hfs::Engine::create(parsed.value());
    HFS_TEST_CHECK(created.ok());
    auto engine = std::move(created).value();

    /* fork child: inherited engine APIs must fail with ForkedProcess */
    {
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        HFS_TEST_CHECK(pid >= 0);
        if (pid == 0) {
            auto result = engine->mount().wait();
            const bool ok =
                result.ok() && result.value().status() == hfs::Status::ForkedProcess;
            ::_exit(ok ? 0 : 3);
        }
        int status = 0;
        HFS_TEST_CHECK(::waitpid(pid, &status, 0) == pid);
        HFS_TEST_CHECK(WIFEXITED(status));
        HFS_TEST_CHECK(WEXITSTATUS(status) == 0);
    }

    auto mount = engine->mount().wait();
    HFS_TEST_CHECK(mount.ok());
    HFS_TEST_CHECK(mount.value().ok());
    HFS_TEST_CHECK(engine->mounted());
    HFS_TEST_CHECK(engine->dataReactorCount() == 2); /* 8 vCPU auto rule */
    HFS_TEST_CHECK(engine->channelCount() == 2);     /* 1 device x 2 data reactors */

    /* doorbell: the closure runs on data reactor 1 with spdk_thread_send_msg */
    std::atomic<bool> ran{false};
    std::atomic<std::uint64_t> tid{0};
    auto posted = engine->postToDataReactor(1, [&ran, &tid]() {
        tid.store(static_cast<std::uint64_t>(::syscall(SYS_gettid)),
                  std::memory_order_release);
        ran.store(true, std::memory_order_release);
    });
    HFS_TEST_CHECK(posted.ok());
    for (int i = 0; i < 1000 && !ran.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    HFS_TEST_CHECK(ran.load(std::memory_order_acquire));
    HFS_TEST_CHECK(tid.load(std::memory_order_acquire) != 0);
    HFS_TEST_CHECK(tid.load(std::memory_order_acquire) == engine->dataReactorThreadId(1));

    HFS_TEST_CHECK(engine->stop().wait().value().ok());
    HFS_TEST_CHECK(engine->channelCount() == 0);

    /* restart in the same process: env stays alive, bdevs/channels reopen */
    auto created2 = hfs::Engine::create(parsed.value());
    HFS_TEST_CHECK(created2.ok());
    auto engine2 = std::move(created2).value();
    HFS_TEST_CHECK(engine2->mount().wait().value().ok());
    HFS_TEST_CHECK(engine2->channelCount() == 2);
    HFS_TEST_CHECK(engine2->stop().wait().value().ok());
    HFS_TEST_CHECK(engine2->channelCount() == 0);

    ::unlink(lockPath.c_str());
    std::printf("spdk_runtime_test: ok\n");
    return 0;
}
