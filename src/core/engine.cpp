#include "hfs/core.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <unistd.h>

#include "common/log.hpp"
#include "common/stats.hpp"
#include "common/trace.hpp"
#include "runtime/spdk_runtime.hpp"

namespace hfs {

namespace {

struct HugepageInfo {
    bool available = false;
    std::uint64_t totalPages = 0;
    std::uint64_t freePages = 0;
    std::uint64_t pageKb = 0;
};

#if defined(__linux__)
HugepageInfo readHugepageInfo() {
    HugepageInfo info;
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) {
        return info;
    }
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream stream(line);
        std::string key;
        std::uint64_t value = 0;
        std::string unit;
        stream >> key >> value >> unit;
        if (key == "HugePages_Total:") {
            info.totalPages = value;
        } else if (key == "HugePages_Free:") {
            info.freePages = value;
        } else if (key == "Hugepagesize:") {
            info.pageKb = value;
        }
    }
    info.available = info.pageKb > 0;
    return info;
}
#else
HugepageInfo readHugepageInfo() { return {}; }
#endif

/* required = ceil((DMA cap + SPDK baseline reserve) * 1.15 / page size) */
std::uint64_t hugepagesRequired(const Config &config, std::uint64_t pageKb) {
    constexpr std::uint64_t kBaseReserveMb = 128;
    std::uint64_t bytes = (config.dmaArenaMb + kBaseReserveMb) << 20;
    bytes = (bytes * 115 + 99) / 100;
    const std::uint64_t pageBytes = pageKb << 10;
    return (bytes + pageBytes - 1) / pageBytes;
}

} // namespace

struct Engine::Impl {
    Config config;
    int lockFd = -1;
    bool mounted = false;
    bool stopped = false;
    pid_t ownerPid = 0;
};

Engine::Engine(Config config) : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
}

Engine::~Engine() {
    if (impl_ && impl_->lockFd >= 0) {
        ::flock(impl_->lockFd, LOCK_UN);
        ::close(impl_->lockFd);
        impl_->lockFd = -1;
    }
}

Result<std::unique_ptr<Engine>> Engine::create(const Config &config) {
    auto valid = validateConfig(config);
    if (!valid.ok()) {
        return makeError<std::unique_ptr<Engine>>(valid.status(), valid.message());
    }

    auto engine = std::unique_ptr<Engine>(new Engine(config));
    auto &impl = *engine->impl_;
    impl.ownerPid = ::getpid();

    impl.lockFd = ::open(config.processLockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (impl.lockFd < 0) {
        return makeError<std::unique_ptr<Engine>>(
            Status::IoError, "cannot open process lock '" + config.processLockPath +
                                 "': " + std::strerror(errno));
    }
    if (::flock(impl.lockFd, LOCK_EX | LOCK_NB) != 0) {
        ::close(impl.lockFd);
        impl.lockFd = -1;
        return makeError<std::unique_ptr<Engine>>(
            Status::Busy, "another engine already holds '" + config.processLockPath + "'");
    }

    const HugepageInfo pages = readHugepageInfo();
    if (!pages.available) {
        log::info("hugepage preflight skipped (no hugepage info available)");
    } else {
        const std::uint64_t required = hugepagesRequired(config, pages.pageKb);
        if (required > pages.freePages) {
            return makeError<std::unique_ptr<Engine>>(
                Status::NoMemory, "hugepage preflight failed: required " +
                                      std::to_string(required) + " pages, available " +
                                      std::to_string(pages.freePages) + " pages of " +
                                      std::to_string(pages.pageKb) + " KiB");
        }
        log::info("hugepage preflight ok: required %llu, available %llu of %llu pages",
                  static_cast<unsigned long long>(required),
                  static_cast<unsigned long long>(pages.freePages),
                  static_cast<unsigned long long>(pages.totalPages));
    }

    log::info("engine created: instance='%s' abi=%u", config.instanceName.c_str(),
              coreAbiMajor());
    return std::unique_ptr<Engine>(engine.release());
}

Task<Result<void>> Engine::mount() {
    auto &impl = *impl_;
    if (!processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (impl.stopped) {
        co_return Result<void>(Status::InvalidState, "engine already stopped");
    }
    if (impl.mounted) {
        co_return Result<void>(Status::InvalidState, "engine already mounted");
    }
    /* Shard-69: initialize SPDK env/reactors, open the bdev set and create
     * per-reactor channels. Shard-70/71 add the superblock/journal mount. */
    auto started = SpdkRuntime::instance().start(impl.config);
    if (!started.ok()) {
        co_return started;
    }
    impl.mounted = true;
    stats::setEnabled(impl.config.stats);
    trace::setEnabled(impl.config.trace);
    trace::setSample(impl.config.traceSample);
    log::info("engine mounted");
    co_return Result<void>(Status::Ok);
}

Task<Result<void>> Engine::drain() {
    if (!processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    /* Phase 0 has no in-flight operations; Shard-73+ drains rings and
     * coroutine frames here. */
    co_return Result<void>(Status::Ok);
}

Task<Result<void>> Engine::stop() {
    auto &impl = *impl_;
    if (!processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (impl.stopped) {
        co_return Result<void>(Status::Ok);
    }
    if (impl.mounted) {
        auto stopped = SpdkRuntime::instance().stop();
        if (!stopped.ok()) {
            co_return stopped;
        }
        impl.mounted = false;
    }
    if (impl.lockFd >= 0) {
        ::flock(impl.lockFd, LOCK_UN);
        ::close(impl.lockFd);
        impl.lockFd = -1;
    }
    impl.stopped = true;
    trace::setEnabled(false);
    stats::setEnabled(false);
    log::info("engine stopped");
    co_return Result<void>(Status::Ok);
}

const Config &Engine::config() const noexcept { return impl_->config; }

bool Engine::mounted() const noexcept { return impl_->mounted; }

bool Engine::processValid() const noexcept {
    return impl_ != nullptr && impl_->ownerPid != 0 &&
           impl_->ownerPid == static_cast<pid_t>(::getpid());
}

std::uint32_t Engine::dataReactorCount() const noexcept {
    return SpdkRuntime::instance().dataReactorCount();
}

std::size_t Engine::channelCount() const noexcept {
    return SpdkRuntime::instance().channelCount();
}

Result<void> Engine::postToDataReactor(std::uint32_t index, std::function<void()> fn) {
    if (!processValid()) {
        return makeError(Status::ForkedProcess, "engine belongs to another process");
    }
    return SpdkRuntime::instance().postToDataReactor(index, std::move(fn));
}

std::uint64_t Engine::dataReactorThreadId(std::uint32_t index) const {
    return SpdkRuntime::instance().dataReactorThreadId(index);
}

std::uint32_t coreAbiMajor() noexcept {
#ifdef HFS_CORE_ABI_MAJOR
    return HFS_CORE_ABI_MAJOR;
#else
    return 0;
#endif
}

} // namespace hfs
