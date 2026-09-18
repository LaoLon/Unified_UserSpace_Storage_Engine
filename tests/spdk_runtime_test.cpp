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
#include "runtime/spdk_runtime.hpp"
#include "test_support.hpp"

namespace {

struct CompletionProbe {
    std::atomic<int> calls{0};
    std::atomic<int> rc{0};
};

/* Test callback records completion without entering the frontend. */
void recordCompletion(void *ctx, int rc) {
    auto *probe = static_cast<CompletionProbe *>(ctx);
    probe->rc.store(rc, std::memory_order_release);
    probe->calls.fetch_add(1, std::memory_order_acq_rel);
}

/* Conflicting registration probe must never replace the engine actor pump. */
void conflictingPoller(void *ctx) noexcept { (void)ctx; }

} // namespace

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

    auto format = engine->format({true}).wait();
    HFS_TEST_CHECK(format.ok());
    HFS_TEST_CHECK(format.value().ok());

    auto mount = engine->mount().wait();
    HFS_TEST_CHECK(mount.ok());
    HFS_TEST_CHECK(mount.value().ok());
    HFS_TEST_CHECK(engine->mounted());
    HFS_TEST_CHECK(engine->dataReactorCount() == 2); /* 8 vCPU auto rule */
    HFS_TEST_CHECK(engine->channelCount() == 3);     /* metadata + 2 data x 1 device */

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

    hfs::SpdkRuntime &runtime = hfs::SpdkRuntime::instance();
    CompletionProbe wrongThreadProbe;
    hfs::DataIoRequest wrongThreadRequest;
    /* Actor-local calls from the frontend thread are rejected before any
     * buffer or bdev access and never invoke the completion callback. */
    auto wrongThread = runtime.submitDataReadOnReactor(
        0, 0, wrongThreadRequest, reinterpret_cast<void *>(1), 0, 1,
        recordCompletion, &wrongThreadProbe);
    HFS_TEST_CHECK(!wrongThread.ok());
    HFS_TEST_CHECK(wrongThread.status() == hfs::Status::InvalidState);
    HFS_TEST_CHECK(wrongThreadProbe.calls.load(std::memory_order_acquire) == 0);

    CompletionProbe badIndexProbe;
    hfs::DataIoRequest badIndexRequest;
    auto badIndex = runtime.submitDataReadOnReactor(
        runtime.dataReactorCount(), 0, badIndexRequest, reinterpret_cast<void *>(1), 0, 1,
        recordCompletion, &badIndexProbe);
    HFS_TEST_CHECK(!badIndex.ok());
    HFS_TEST_CHECK(badIndex.status() == hfs::Status::InvalidArgument);
    HFS_TEST_CHECK(badIndexProbe.calls.load(std::memory_order_acquire) == 0);

    std::atomic<bool> ownerValidationRan{false};
    std::atomic<int> ownerValidationStatus{-1};
    auto ownerValidation = engine->postToDataReactor(0, [&]() {
        hfs::DataIoRequest request;
        CompletionProbe probe;
        auto result = runtime.submitDataReadOnReactor(0, 0, request, nullptr, 0, 0,
                                                      recordCompletion, &probe);
        ownerValidationStatus.store(static_cast<int>(result.status()),
                                    std::memory_order_release);
        ownerValidationRan.store(true, std::memory_order_release);
    });
    HFS_TEST_CHECK(ownerValidation.ok());
    for (int i = 0; i < 1000 && !ownerValidationRan.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    HFS_TEST_CHECK(ownerValidationRan.load(std::memory_order_acquire));
    HFS_TEST_CHECK(ownerValidationStatus.load(std::memory_order_acquire) ==
                   static_cast<int>(hfs::Status::InvalidArgument));

    /* Engine mount owns the sole actor pump on every data reactor. */
    int conflictingContext = 0;
    auto pollerSet = runtime.setDataReactorPoller(
        0, conflictingPoller, &conflictingContext);
    HFS_TEST_CHECK(!pollerSet.ok());
    HFS_TEST_CHECK(pollerSet.status() == hfs::Status::Busy);

    HFS_TEST_CHECK(engine->stop().wait().value().ok());
    HFS_TEST_CHECK(engine->channelCount() == 0);

    /* restart in the same process: env stays alive, bdevs/channels reopen */
    auto created2 = hfs::Engine::create(parsed.value());
    HFS_TEST_CHECK(created2.ok());
    auto engine2 = std::move(created2).value();
    HFS_TEST_CHECK(engine2->mount().wait().value().ok());
    HFS_TEST_CHECK(engine2->channelCount() == 3);
    HFS_TEST_CHECK(engine2->stop().wait().value().ok());
    HFS_TEST_CHECK(engine2->channelCount() == 0);

    ::unlink(lockPath.c_str());
    std::printf("spdk_runtime_test: ok\n");
    return 0;
}
