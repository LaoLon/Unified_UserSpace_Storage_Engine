#include "runtime/spdk_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/log.hpp"

#ifdef HFS_WITH_SPDK
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/init.h>
#include <spdk/log.h>
#include <spdk/rpc.h>
#include <spdk/thread.h>
#include <sys/syscall.h>
#endif

namespace hfs {

namespace {

/* Automatic data reactor count from hfs2.0.md §6.2. */
std::uint32_t reactorCountFor(const Config &config, std::uint32_t cpuCount) {
    std::uint32_t reactors = config.dataReactors;
    if (reactors == 0) {
        if (cpuCount <= 1) {
            reactors = 1;
        } else if (cpuCount <= 8) {
            reactors = 2;
        } else {
            reactors = std::min<std::uint32_t>(4, std::max<std::uint32_t>(2, cpuCount / 4));
        }
    }
    if (config.maxDataReactors != 0 && reactors > config.maxDataReactors) {
        reactors = config.maxDataReactors;
    }
    return reactors == 0 ? 1 : reactors;
}

} // namespace

#ifdef HFS_WITH_SPDK

namespace {

/* "Nvme0n1" -> "Nvme0"; names without a trailing n<digits> are used as-is. */
std::string controllerName(const std::string &bdevName) {
    std::size_t pos = bdevName.size();
    while (pos > 0 && std::isdigit(static_cast<unsigned char>(bdevName[pos - 1])) != 0) {
        --pos;
    }
    if (pos < bdevName.size() && pos > 0 && bdevName[pos - 1] == 'n') {
        return bdevName.substr(0, pos - 1);
    }
    return bdevName;
}

void pinToCpu(unsigned int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        log::info("cpu pin to %u failed", cpu);
    }
}

struct AsyncDone {
    std::atomic<bool> done{false};
    int rc = 0;
};

void subsystemDone(int rc, void *ctx) {
    auto *state = static_cast<AsyncDone *>(ctx);
    state->rc = rc;
    state->done.store(true, std::memory_order_release);
}

/* One control-path closure delivered through spdk_thread_send_msg. */
struct PostedCall {
    std::function<void()> fn;
};

void postedCallTrampoline(void *ctx) {
    auto *call = static_cast<PostedCall *>(ctx);
    call->fn();
    delete call;
}

/* bdev removal events are fatal for the current engine generation. */
void bdevEvent(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx) {
    (void)ctx;
    log::error("bdev event type=%d name=%s", static_cast<int>(type),
               bdev == nullptr ? "?" : spdk_bdev_get_name(bdev));
}

/* Channel destruction is deferred through thread messages; drain them before
 * destroying the owning SPDK thread. */
void drainThreadMessages(spdk_thread *thread) {
    for (int i = 0; i < 1024; ++i) {
        spdk_thread_poll(thread, 64, 0);
    }
}

} // namespace

struct SpdkRuntime::Impl {
    struct Reactor {
        std::jthread thread;
        std::atomic<bool> stop{false};
        std::atomic<bool> ready{false};
        std::atomic<int> result{0};
        std::atomic<std::uint64_t> osTid{0};
        spdk_thread *spdk = nullptr;
        std::vector<spdk_io_channel *> channels;
    };

    struct DeviceState {
        std::string name;
        spdk_bdev_desc *desc = nullptr;
    };

    mutable std::mutex mutex;
    std::condition_variable stateChanged;
    Config config;
    std::vector<DeviceState> devices;
    std::vector<std::unique_ptr<Reactor>> reactors; /* [0] metadata, [1..R] data */
    std::atomic<bool> activeFlag{false};
    std::atomic<bool> stoppingFlag{false};
    std::atomic<std::uint32_t> reactorCount{0};
    std::atomic<std::int64_t> pid{0};
    bool envInited = false;
    bool envReady = false;
    bool subsystemsReady = false;
    bool startDone = false;
    int startResult = 0;
    std::uint32_t readyCount = 0;

    void publishStartDone(int rc) {
        std::lock_guard<std::mutex> lock(mutex);
        startResult = rc;
        startDone = true;
        stateChanged.notify_all();
    }

    Reactor *reactorAt(std::size_t index) const { return reactors[index].get(); }

    void closeDevicesLocked() {
        for (auto &device : devices) {
            if (device.desc != nullptr) {
                spdk_bdev_close(device.desc);
                device.desc = nullptr;
            }
        }
    }

    /* Build the bdev subsystem JSON config for the configured device set. */
    Result<std::string> buildBdevConfig() const {
        std::ostringstream json;
        json << "{\"subsystems\":[{\"subsystem\":\"bdev\",\"config\":[";
        bool first = true;
        std::vector<std::string> seenControllers;
        for (const auto &device : config.devices) {
            const std::string controller = controllerName(device.bdevName);
            if (device.pciBdf.empty()) {
                return makeError<std::string>(Status::InvalidArgument,
                                              "device '" + device.id + "' has no pci_bdf");
            }
            if (std::find(seenControllers.begin(), seenControllers.end(), controller) !=
                seenControllers.end()) {
                continue; /* same controller: attach once */
            }
            seenControllers.push_back(controller);
            if (!first) {
                json << ",";
            }
            first = false;
            json << "{\"method\":\"bdev_nvme_attach_controller\",\"params\":{"
                 << "\"name\":\"" << controller << "\","
                 << "\"trtype\":\"PCIe\",\"traddr\":\"" << device.pciBdf << "\"}}";
        }
        json << "]}]}";
        return json.str();
    }

    int initEnv() {
        struct spdk_env_opts opts;
        spdk_env_opts_init(&opts);
        opts.name = "hfs2";
        opts.opts_size = sizeof(opts);
        std::vector<struct spdk_pci_addr> allowlist(devices.size());
        for (std::size_t i = 0; i < devices.size(); ++i) {
            if (spdk_pci_addr_parse(&allowlist[i], config.devices[i].pciBdf.c_str()) != 0) {
                log::error("invalid pci_bdf '%s'", config.devices[i].pciBdf.c_str());
                return -EINVAL;
            }
        }
        opts.pci_allowed = allowlist.empty() ? nullptr : allowlist.data();
        opts.num_pci_addr = allowlist.size();
        if (spdk_env_init(&opts) != 0) {
            log::error("spdk_env_init failed");
            return -EIO;
        }
        if (spdk_thread_lib_init(nullptr, 0) != 0) {
            log::error("spdk_thread_lib_init failed");
            spdk_env_fini();
            return -EIO;
        }
        spdk_log_set_print_level(SPDK_LOG_ERROR);
        spdk_log_set_level(SPDK_LOG_ERROR);
        spdk_log_open(nullptr);
        return 0;
    }

    int initSubsystems(Reactor *metadata) {
        {
            AsyncDone done;
            spdk_subsystem_init(subsystemDone, &done);
            while (!done.done.load(std::memory_order_acquire)) {
                spdk_thread_poll(metadata->spdk, 0, 0);
            }
            if (done.rc != 0) {
                log::error("spdk_subsystem_init failed rc=%d", done.rc);
                return done.rc;
            }
            /* The event framework normally flips this; without it the RUNTIME
             * RPC methods (e.g. bdev_nvme_attach_controller) are skipped. */
            spdk_rpc_set_state(SPDK_RPC_RUNTIME);
        }
        auto json = buildBdevConfig();
        if (!json.ok()) {
            log::error("bdev config: %s", json.message().c_str());
            return -EINVAL;
        }
        std::vector<char> buffer(json.value().begin(), json.value().end());
        {
            AsyncDone done;
            spdk_subsystem_load_config(buffer.data(), static_cast<ssize_t>(buffer.size()),
                                       subsystemDone, &done, true);
            while (!done.done.load(std::memory_order_acquire)) {
                spdk_thread_poll(metadata->spdk, 0, 0);
            }
            if (done.rc != 0) {
                log::error("spdk_subsystem_load_config failed rc=%d", done.rc);
                return done.rc;
            }
        }
        return 0;
    }

    int openDevices() {
        for (auto &device : devices) {
            if (spdk_bdev_get_by_name(device.name.c_str()) == nullptr) {
                log::error("bdev '%s' not found", device.name.c_str());
                return -ENOENT;
            }
            if (spdk_bdev_open_ext(device.name.c_str(), true, bdevEvent, this, &device.desc) != 0) {
                log::error("cannot open bdev '%s'", device.name.c_str());
                return -EIO;
            }
        }
        return 0;
    }

    /* bdev_nvme attach completes asynchronously after the config load
     * callback, so wait for every configured bdev name to appear. */
    int waitForDevices(Reactor *metadata, int timeoutMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            bool allFound = true;
            for (const auto &device : devices) {
                if (spdk_bdev_get_by_name(device.name.c_str()) == nullptr) {
                    allFound = false;
                    break;
                }
            }
            if (allFound) {
                return 0;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::string available;
                for (struct spdk_bdev *bdev = spdk_bdev_first(); bdev != nullptr;
                     bdev = spdk_bdev_next(bdev)) {
                    if (!available.empty()) {
                        available += ",";
                    }
                    available += spdk_bdev_get_name(bdev);
                }
                log::error("timed out waiting for bdevs; available: %s",
                           available.empty() ? "(none)" : available.c_str());
                return -ENOENT;
            }
            spdk_thread_poll(metadata->spdk, 0, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void metadataMain() {
        auto *metadata = reactorAt(0);
        metadata->osTid.store(static_cast<std::uint64_t>(::syscall(SYS_gettid)),
                              std::memory_order_release);
        pinToCpu(config.metadataCpu);

        int rc = 0;
        if (!envInited) {
            rc = initEnv();
            if (rc != 0) {
                publishStartDone(rc);
                return;
            }
            envInited = true;
        }
        metadata->spdk = spdk_thread_create("hfs2-meta", nullptr);
        if (metadata->spdk == nullptr) {
            publishStartDone(-ENOMEM);
            return;
        }
        spdk_set_thread(metadata->spdk);

        if (!subsystemsReady) {
            rc = initSubsystems(metadata);
            if (rc != 0) {
                spdk_set_thread(nullptr);
                spdk_thread_destroy(metadata->spdk);
                metadata->spdk = nullptr;
                publishStartDone(rc);
                return;
            }
            subsystemsReady = true;
        }
        envReady = true;

        rc = waitForDevices(metadata, 10000);
        if (rc != 0) {
            closeDevicesLocked();
            spdk_set_thread(nullptr);
            spdk_thread_destroy(metadata->spdk);
            metadata->spdk = nullptr;
            publishStartDone(rc);
            return;
        }
        rc = openDevices();
        if (rc != 0) {
            closeDevicesLocked();
            spdk_set_thread(nullptr);
            spdk_thread_destroy(metadata->spdk);
            metadata->spdk = nullptr;
            publishStartDone(rc);
            return;
        }

        pid.store(static_cast<std::int64_t>(::getpid()), std::memory_order_release);
        publishStartDone(0);
        log::info("spdk runtime ready: devices=%zu", devices.size());

        while (!metadata->stop.load(std::memory_order_acquire)) {
            spdk_thread_poll(metadata->spdk, 0, 0);
        }

        closeDevicesLocked();
        drainThreadMessages(metadata->spdk);
        spdk_set_thread(nullptr);
        spdk_thread_destroy(metadata->spdk);
        metadata->spdk = nullptr;
        log::info("metadata reactor stopped");
    }

    int dataCpuFor(std::size_t dataIndex) const {
        if (config.dataCpus != "auto") {
            std::size_t begin = 0;
            std::size_t seen = 0;
            while (begin <= config.dataCpus.size()) {
                std::size_t comma = config.dataCpus.find(',', begin);
                std::string item = config.dataCpus.substr(
                    begin, comma == std::string::npos ? std::string::npos : comma - begin);
                item.erase(0, item.find_first_not_of(" \t"));
                item.erase(item.find_last_not_of(" \t") + 1);
                ++seen;
                if (seen == dataIndex) {
                    return std::atoi(item.c_str());
                }
                if (comma == std::string::npos) {
                    break;
                }
                begin = comma + 1;
            }
        }
        return static_cast<int>(config.metadataCpu + dataIndex);
    }

    void dataMain(std::size_t dataIndex) {
        auto *reactor = reactorAt(dataIndex);
        reactor->osTid.store(static_cast<std::uint64_t>(::syscall(SYS_gettid)),
                             std::memory_order_release);
        pinToCpu(static_cast<unsigned int>(dataCpuFor(dataIndex)));

        const std::string name = "hfs2-data" + std::to_string(dataIndex - 1);
        reactor->spdk = spdk_thread_create(name.c_str(), nullptr);
        if (reactor->spdk == nullptr) {
            reactor->result.store(-ENOMEM, std::memory_order_release);
            std::lock_guard<std::mutex> lock(mutex);
            ++readyCount;
            stateChanged.notify_all();
            return;
        }
        spdk_set_thread(reactor->spdk);

        int rc = 0;
        for (auto &device : devices) {
            spdk_io_channel *channel = spdk_bdev_get_io_channel(device.desc);
            if (channel == nullptr) {
                rc = -ENOMEM;
                break;
            }
            reactor->channels.push_back(channel);
        }
        reactor->result.store(rc, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++readyCount;
            stateChanged.notify_all();
        }

        while (!reactor->stop.load(std::memory_order_acquire)) {
            spdk_thread_poll(reactor->spdk, 0, 0);
        }

        for (auto *channel : reactor->channels) {
            spdk_put_io_channel(channel);
        }
        reactor->channels.clear();
        drainThreadMessages(reactor->spdk);
        spdk_set_thread(nullptr);
        spdk_thread_destroy(reactor->spdk);
        reactor->spdk = nullptr;
    }

    /* Stop and join all reactors; safe to call from start() failure paths. */
    void shutdownReactors() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto &reactor : reactors) {
                reactor->stop.store(true, std::memory_order_release);
            }
        }
        for (auto &reactor : reactors) {
            if (reactor->thread.joinable()) {
                reactor->thread.join();
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        reactors.clear();
    }
};

SpdkRuntime::SpdkRuntime() : impl_(std::make_unique<Impl>()) {}

SpdkRuntime::~SpdkRuntime() = default;

SpdkRuntime &SpdkRuntime::instance() noexcept {
    static SpdkRuntime runtime;
    return runtime;
}

Result<void> SpdkRuntime::start(const Config &config) {
    auto &impl = *impl_;
    std::unique_lock<std::mutex> lock(impl.mutex);

    if (impl.activeFlag.load(std::memory_order_acquire)) {
        return makeError(Status::Busy, "SPDK runtime is already active");
    }
    if (impl.stoppingFlag.load(std::memory_order_acquire)) {
        return makeError(Status::Busy, "SPDK runtime is stopping");
    }
    const std::int64_t pid = impl.pid.load(std::memory_order_acquire);
    if (pid != 0 && pid != static_cast<std::int64_t>(::getpid())) {
        return makeError(Status::ForkedProcess, "SPDK runtime belongs to another process");
    }

    /* The device set is fixed for the process lifetime once initialized. */
    if (impl.envReady) {
        if (impl.devices.size() != config.devices.size()) {
            return makeError(Status::InvalidState,
                             "device set differs from the initialized runtime");
        }
        for (std::size_t i = 0; i < impl.devices.size(); ++i) {
            if (impl.devices[i].name != config.devices[i].bdevName) {
                return makeError(Status::InvalidState,
                                 "device set differs from the initialized runtime");
            }
        }
    }

    const long onlineCpus = ::sysconf(_SC_NPROCESSORS_ONLN);
    const std::uint32_t cpuCount = static_cast<std::uint32_t>(onlineCpus > 0 ? onlineCpus : 1);
    const std::uint32_t dataReactors = reactorCountFor(config, cpuCount);

    impl.config = config;
    if (!impl.envReady) {
        impl.devices.clear();
        for (const auto &device : config.devices) {
            impl.devices.push_back(Impl::DeviceState{device.bdevName, nullptr});
        }
    }
    impl.reactorCount.store(dataReactors, std::memory_order_release);
    impl.startDone = false;
    impl.startResult = 0;
    impl.readyCount = 0;
    impl.stoppingFlag.store(false, std::memory_order_release);

    impl.reactors.clear();
    impl.reactors.push_back(std::make_unique<Impl::Reactor>());
    for (std::uint32_t i = 0; i < dataReactors; ++i) {
        impl.reactors.push_back(std::make_unique<Impl::Reactor>());
    }

    impl.reactors[0]->thread = std::jthread([&impl]() { impl.metadataMain(); });

    while (!impl.startDone) {
        impl.stateChanged.wait(lock);
    }
    if (impl.startResult != 0) {
        const int result = impl.startResult;
        lock.unlock();
        impl.shutdownReactors();
        return makeError(Status::IoError, "SPDK runtime start failed: " +
                                              std::string(std::strerror(-result)) + " (" +
                                              std::to_string(result) + ")");
    }

    for (std::uint32_t i = 1; i <= dataReactors; ++i) {
        auto *reactor = impl.reactors[i].get();
        reactor->thread = std::jthread([&impl, i]() { impl.dataMain(i); });
    }
    while (impl.readyCount < dataReactors) {
        impl.stateChanged.wait(lock);
    }
    for (std::uint32_t i = 1; i <= dataReactors; ++i) {
        if (impl.reactors[i]->result.load(std::memory_order_acquire) != 0) {
            lock.unlock();
            impl.shutdownReactors();
            return makeError(Status::IoError, "SPDK data reactor start failed");
        }
    }

    impl.activeFlag.store(true, std::memory_order_release);
    log::info("spdk runtime active: data_reactors=%u devices=%zu", dataReactors,
              impl.devices.size());
    return Result<void>(Status::Ok);
}

Result<void> SpdkRuntime::stop() {
    auto &impl = *impl_;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.activeFlag.load(std::memory_order_acquire)) {
            return Result<void>(Status::Ok);
        }
        impl.stoppingFlag.store(true, std::memory_order_release);
    }
    impl.shutdownReactors();
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.activeFlag.store(false, std::memory_order_release);
        impl.stoppingFlag.store(false, std::memory_order_release);
        impl.reactorCount.store(0, std::memory_order_release);
        impl.stateChanged.notify_all();
    }
    log::info("spdk runtime stopped");
    return Result<void>(Status::Ok);
}

bool SpdkRuntime::active() const noexcept {
    return impl_->activeFlag.load(std::memory_order_acquire);
}

std::uint32_t SpdkRuntime::dataReactorCount() const noexcept {
    return impl_->reactorCount.load(std::memory_order_acquire);
}

std::size_t SpdkRuntime::deviceCount() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->devices.size();
}

std::string SpdkRuntime::deviceName(std::size_t index) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return index < impl_->devices.size() ? impl_->devices[index].name : std::string();
}

Result<void> SpdkRuntime::postToDataReactor(std::uint32_t index, std::function<void()> fn) {
    auto &impl = *impl_;
    spdk_thread *target = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.activeFlag.load(std::memory_order_acquire)) {
            return makeError(Status::Busy, "SPDK runtime is not active");
        }
        if (index + 1 >= impl.reactors.size() || impl.reactors[index + 1]->spdk == nullptr) {
            return makeError(Status::InvalidArgument, "data reactor index out of range");
        }
        target = impl.reactors[index + 1]->spdk;
    }
    auto *call = new PostedCall{std::move(fn)};
    if (spdk_thread_send_msg(target, postedCallTrampoline, call) != 0) {
        delete call;
        return makeError(Status::Busy, "spdk_thread_send_msg failed");
    }
    return Result<void>(Status::Ok);
}

std::uint64_t SpdkRuntime::dataReactorThreadId(std::uint32_t index) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index + 1 >= impl_->reactors.size()) {
        return 0;
    }
    return impl_->reactors[index + 1]->osTid.load(std::memory_order_acquire);
}

std::size_t SpdkRuntime::channelCount() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::size_t count = 0;
    for (const auto &reactor : impl_->reactors) {
        count += reactor->channels.size();
    }
    return count;
}

std::int64_t SpdkRuntime::ownerPid() const noexcept {
    return impl_->pid.load(std::memory_order_acquire);
}

#else /* !HFS_WITH_SPDK */

struct SpdkRuntime::Impl {};

SpdkRuntime::SpdkRuntime() : impl_(std::make_unique<Impl>()) {}
SpdkRuntime::~SpdkRuntime() = default;

SpdkRuntime &SpdkRuntime::instance() noexcept {
    static SpdkRuntime runtime;
    return runtime;
}

Result<void> SpdkRuntime::start(const Config &config) {
    (void)config;
    return makeError(Status::NotSupported, "core built without SPDK support");
}

Result<void> SpdkRuntime::stop() { return Result<void>(Status::Ok); }
bool SpdkRuntime::active() const noexcept { return false; }
std::uint32_t SpdkRuntime::dataReactorCount() const noexcept { return 0; }
std::size_t SpdkRuntime::deviceCount() const noexcept { return 0; }
std::string SpdkRuntime::deviceName(std::size_t index) const {
    (void)index;
    return {};
}
Result<void> SpdkRuntime::postToDataReactor(std::uint32_t index, std::function<void()> fn) {
    (void)index;
    (void)fn;
    return makeError(Status::NotSupported, "core built without SPDK support");
}
std::uint64_t SpdkRuntime::dataReactorThreadId(std::uint32_t index) const {
    (void)index;
    return 0;
}
std::size_t SpdkRuntime::channelCount() const noexcept { return 0; }
std::int64_t SpdkRuntime::ownerPid() const noexcept { return 0; }

#endif /* HFS_WITH_SPDK */

} // namespace hfs
