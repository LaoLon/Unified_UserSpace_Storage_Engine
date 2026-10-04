#include "runtime/spdk_runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <limits>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

#include "common/log.hpp"
#include "common/stats.hpp"
#include "common/test_hooks.hpp"
#include "common/trace.hpp"
#include "runtime/numa_topology.hpp"

#ifdef UUSE_WITH_SPDK
#include <poll.h>
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/init.h>
#include <spdk/log.h>
#include <spdk/rpc.h>
#include <spdk/thread.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#endif

namespace uuse {

namespace {

/* Automatic data reactor count from uuse.md §6.2. */
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

#ifdef UUSE_WITH_SPDK

namespace {

/* Reactor-thread identity is derived from spdk_get_thread(); TLS markers were
 * removed from the hot path (__tls_get_addr showed up at ~1.7% reactor CPU). */

constexpr std::uint64_t kPollerEnabledBit = std::uint64_t{1} << 63;
constexpr std::uint64_t kPollerReaderMask = ~kPollerEnabledBit;

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

/* Pins only when the target CPU is inside the caller's allowed affinity set,
 * so an outer taskset/cgroup restriction is never widened by reactor pins. */
void pinToCpu(unsigned int cpu) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) == 0 &&
        !CPU_ISSET(cpu, &allowed)) {
        log::info("cpu pin to %u skipped (outside allowed cpuset)", cpu);
        return;
    }
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

/* Bounded multi-producer/single-consumer queue for synchronous data
 * submissions. Producers reserve a slot with a CAS; the owning reactor drains
 * it in its poll loop, so the common path needs no heap allocation and no SPDK
 * thread message. A full queue is reported to the caller as bounded backpressure. */
template <typename T, std::size_t Capacity>
class BoundedMpscQueue {
public:
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "capacity must be a power of two");

    BoundedMpscQueue() noexcept {
        for (std::size_t index = 0; index < Capacity; ++index) {
            slots_[index].sequence.store(index, std::memory_order_relaxed);
        }
    }

    BoundedMpscQueue(const BoundedMpscQueue &) = delete;
    BoundedMpscQueue &operator=(const BoundedMpscQueue &) = delete;

    bool tryPush(const T &value) noexcept {
        std::uint64_t position = head_.load(std::memory_order_relaxed);
        for (;;) {
            Slot &slot = slots_[position & kMask];
            const std::uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
            const std::int64_t difference =
                static_cast<std::int64_t>(sequence) - static_cast<std::int64_t>(position);
            if (difference == 0) {
                if (head_.compare_exchange_weak(position, position + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
                    slot.value = value;
                    slot.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false; /* full: the consumer has not freed this slot yet */
            } else {
                position = head_.load(std::memory_order_relaxed);
            }
        }
    }

    /* Consumer-only, including the reactor's idle re-check. */
    bool tryPop(T &value) noexcept {
        Slot &slot = slots_[tail_ & kMask];
        const std::uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
        if (static_cast<std::int64_t>(sequence) -
                static_cast<std::int64_t>(tail_ + 1) != 0) {
            return false;
        }
        value = slot.value;
        slot.sequence.store(tail_ + Capacity, std::memory_order_release);
        ++tail_;
        return true;
    }

    bool hasPending() const noexcept {
        const Slot &slot = slots_[tail_ & kMask];
        const std::uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
        return static_cast<std::int64_t>(sequence) -
                   static_cast<std::int64_t>(tail_ + 1) == 0;
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    struct Slot {
        std::atomic<std::uint64_t> sequence{0};
        T value{};
    };

    std::array<Slot, Capacity> slots_{};
    std::atomic<std::uint64_t> head_{0};
    std::uint64_t tail_ = 0; /* consumer only */
};

/* Keep a backend-provided negative errno when the bdev exposes one. NVMe
 * completion status has no portable errno field, so it falls back to EIO
 * except for the explicitly representable abort case. */
int dataIoCompletionErrno(const struct spdk_bdev_io *bdevIo) noexcept {
    int aioResult = 0;
    spdk_bdev_io_get_aio_status(bdevIo, &aioResult);
    if (aioResult != -EIO) {
        return aioResult == 0 ? -EIO : (aioResult < 0 ? aioResult : -aioResult);
    }

    std::uint32_t cdw0 = 0;
    int sct = 0;
    int sc = 0;
    spdk_bdev_io_get_nvme_status(bdevIo, &cdw0, &sct, &sc);
    if (sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_ABORTED_BY_REQUEST) {
        return -ECANCELED;
    }
    return -EIO;
}

/* One control-path block I/O executed on the metadata reactor. */
struct ControlIo {
    enum class Op { Read, Write, Flush };
    Op op = Op::Read;
    std::size_t device = 0;
    std::uint64_t offsetBlocks = 0;
    std::size_t blocks = 0;
    void *dma = nullptr; /* DMA bounce buffer for read/write */
    std::span<std::byte> readTarget;
    std::span<const std::byte> writeSource;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool abandoned = false;
    int rc = 0;
};

/* Returns whether an explicitly requested test flush fault targets this device.
 * The hook is inert unless the caller sets UUSE_TEST_FAIL_FLUSH_DEVICE. */
bool testFlushFailureRequested(std::size_t deviceIndex) {
    const char *value = std::getenv("UUSE_TEST_FAIL_FLUSH_DEVICE");
    if (value == nullptr || *value == '\0') {
        return false;
    }
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return end != value && *end == '\0' && parsed == deviceIndex;
}

int failControlIo(const std::shared_ptr<ControlIo> &io, int rc) {
    if (io->dma != nullptr) {
        spdk_dma_free(io->dma);
        io->dma = nullptr;
    }
    std::lock_guard<std::mutex> lock(io->mutex);
    io->rc = rc;
    io->done = true;
    io->cv.notify_all();
    return rc;
}

/* Completion runs on the metadata reactor; the holder keeps the I/O alive and
 * releases the reactor's in-flight control-I/O credit. */
struct ControlCompletion {
    std::shared_ptr<ControlIo> io;
    std::atomic<std::size_t> *inFlight = nullptr;
};

void controlIoDone(struct spdk_bdev_io *bdev_io, bool success, void *arg) {
    auto *holder = static_cast<ControlCompletion *>(arg);
    std::shared_ptr<ControlIo> io = std::move(holder->io);
    if (holder->inFlight != nullptr) {
        holder->inFlight->fetch_sub(1, std::memory_order_acq_rel);
    }
    delete holder;
    spdk_bdev_free_io(bdev_io);

    std::lock_guard<std::mutex> lock(io->mutex);
    if (!io->abandoned && success && io->op == ControlIo::Op::Read && io->dma != nullptr) {
        std::memcpy(io->readTarget.data(), io->dma, io->readTarget.size());
    }
    if (io->dma != nullptr) {
        spdk_dma_free(io->dma);
        io->dma = nullptr;
    }
    io->rc = success ? 0 : -EIO;
    io->done = true;
    io->cv.notify_all();
}

} // namespace

struct SpdkRuntime::Impl {
    struct DataIoState;

    struct Reactor {
        std::jthread thread;
        std::atomic<bool> stop{false};
        std::atomic<bool> ready{false};
        std::atomic<int> result{0};
        std::atomic<std::uint64_t> osTid{0};
        spdk_thread *spdk = nullptr;
        std::vector<spdk_io_channel *> channels;
        DataIoState *retryHead = nullptr;
        DataIoState *retryTail = nullptr;
        DataIoState *completionHead = nullptr;
        DataIoState *completionTail = nullptr;
        DataIoState *activeHead = nullptr;
        std::mutex pollerMutex;
        DataReactorPoller poller = nullptr;
        void *pollerContext = nullptr;
        /* The high bit closes admission before registration data is retired. */
        std::atomic<std::uint64_t> pollerState{0};
        bool pollerStopping = false;
        std::int32_t numaNode = -1;
        std::uint32_t cpu = 0;
        /* Zero-based data reactor index; also the ingress/holder slot. */
        std::size_t ingressSlot = 0;
        std::uint32_t idlePolls = 0;
        std::uint64_t pollSleeps = 0;
        /* Doorbell: producers write this eventfd only while idleWait is set. */
        int wakeFd = -1;
        std::atomic<bool> idleWait{false};
        std::uint64_t doorbellWakes = 0;
        std::uint64_t doorbellTimeouts = 0;
        /* Metadata reactor only: in-flight control I/O keeps it awake. */
        std::atomic<std::size_t> controlInFlight{0};

        /* Safety net for failure paths that clear reactors before a close. */
        ~Reactor() {
            if (wakeFd >= 0) {
                ::close(wakeFd);
                wakeFd = -1;
            }
        }
    };

    struct DeviceState {
        std::string name;
        spdk_bdev *bdev = nullptr;
        spdk_bdev_desc *desc = nullptr;
        std::uint64_t numBlocks = 0;
        std::uint32_t blockSize = 0;
    };

    mutable std::mutex mutex;
    std::condition_variable stateChanged;
    Config config;
    std::vector<DeviceState> devices;
    /* Per-device data durability sequences (uuse.md 10.2/20.3): a write
     * completion advances writtenSeq, a device flush publishes coverage. */
    std::vector<std::atomic<std::uint64_t>> dataWrittenSeq;
    std::vector<std::atomic<std::uint64_t>> dataFlushedSeq;
    std::vector<std::shared_ptr<Reactor>> reactors; /* [0] metadata, [1..R] data */
    std::vector<std::uint32_t> deviceReactors;
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

    /* Advances a device's completed-write sequence; called by both the
     * synchronous and the actor-local data completion paths. */
    void noteDataWriteComplete(std::size_t deviceIndex) noexcept {
        if (deviceIndex < dataWrittenSeq.size()) {
            dataWrittenSeq[deviceIndex].fetch_add(1, std::memory_order_release);
        }
    }

    /* True when some device has a completed data write not yet covered by a
     * device flush. */
    bool dataFlushPending() const noexcept {
        for (std::size_t i = 0; i < dataWrittenSeq.size(); ++i) {
            if (dataWrittenSeq[i].load(std::memory_order_acquire) >
                dataFlushedSeq[i].load(std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    /* Publishes flush coverage: a successful flush covers every write that
     * completed before the flush was submitted, i.e. the sampled target. */
    void publishDataFlushed(std::size_t deviceIndex, std::uint64_t target) noexcept {
        if (deviceIndex >= dataFlushedSeq.size()) {
            return;
        }
        auto &flushed = dataFlushedSeq[deviceIndex];
        std::uint64_t observed = flushed.load(std::memory_order_relaxed);
        while (observed < target &&
               !flushed.compare_exchange_weak(observed, target, std::memory_order_release,
                                              std::memory_order_relaxed)) {
        }
    }

    /* Process exit without an explicit stop() must not join busy-polling
     * reactors without asking them to stop first. */
    ~Impl() {
        disableDataReactorPollers();
        for (auto &reactor : reactors) {
            if (reactor != nullptr) {
                reactor->stop.store(true, std::memory_order_release);
            }
        }
        for (auto &reactor : reactors) {
            if (reactor != nullptr && reactor->thread.joinable()) {
                reactor->thread.join();
            }
        }
        reactors.clear();
    }

    Reactor *reactorAt(std::size_t index) const { return reactors[index].get(); }

    /* Stop accepting poller callbacks and synchronize with the one callback
     * that may currently be using its caller-owned context. */
    void disableDataReactorPollers() noexcept {
        for (std::size_t index = 1; index < reactors.size(); ++index) {
            Reactor *reactor = reactors[index].get();
            if (reactor == nullptr) {
                continue;
            }
            std::unique_lock<std::mutex> lock(reactor->pollerMutex);
            reactor->pollerStopping = true;
            const std::uint64_t previousState = reactor->pollerState.fetch_and(
                kPollerReaderMask, std::memory_order_acq_rel);
            if ((previousState & kPollerReaderMask) != 0 &&
                spdk_get_thread() == reactor->spdk) {
                /* The current callback owns its local copy and will release
                 * the reader after this function returns. */
            } else {
                for (;;) {
                    const std::uint64_t state =
                        reactor->pollerState.load(std::memory_order_acquire);
                    if ((state & kPollerReaderMask) == 0) {
                        break;
                    }
                    reactor->pollerState.wait(state, std::memory_order_acquire);
                }
            }
            reactor->poller = nullptr;
            reactor->pollerContext = nullptr;
        }
    }

    /* Invoke the registered hook without taking the registration mutex. The
     * reader count lets clear/stop retire caller-owned context safely. Returns
     * the callback's work count so idle detection can skip sleeping while the
     * poller is still making progress. */
    int invokeDataReactorPoller(Reactor *reactor) noexcept {
        for (;;) {
            std::uint64_t state = reactor->pollerState.load(std::memory_order_acquire);
            if ((state & kPollerEnabledBit) == 0) {
                return 0;
            }
            if (reactor->pollerState.compare_exchange_weak(
                    state, state + 1, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                break;
            }
        }
        const auto leave = [reactor]() {
            const std::uint64_t previousState = reactor->pollerState.fetch_sub(
                1, std::memory_order_acq_rel);
            if ((previousState & kPollerReaderMask) == 1) {
                reactor->pollerState.notify_all();
            }
        };

        const DataReactorPoller callback = reactor->poller;
        void *const context = reactor->pollerContext;
        if (callback == nullptr) {
            leave();
            return 0;
        }
        const int work = callback(context);
        leave();
        return work;
    }

    /* Clears a pending doorbell, re-checks the actor, then waits up to delayUs.
     * Producers only write the eventfd while idleWait is set, so a publish
     * that misses the flag is always caught by the re-check. */
    void waitDataReactorIdle(Reactor *reactor, std::uint32_t delayUs) noexcept {
        if (reactor->wakeFd < 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
            return;
        }
        std::uint64_t value = 0;
        ssize_t drained = -1;
        do {
            drained = ::read(reactor->wakeFd, &value, sizeof(value));
        } while (drained < 0 && errno == EINTR);
        (void)drained;
        reactor->idleWait.store(true, std::memory_order_release);
        if (invokeDataReactorPoller(reactor) != 0 ||
            dataIngressPending(reactor->ingressSlot)) {
            reactor->idleWait.store(false, std::memory_order_release);
            return;
        }
        pollfd descriptor{};
        descriptor.fd = reactor->wakeFd;
        descriptor.events = POLLIN;
        timespec timeout{};
        timeout.tv_sec = static_cast<time_t>(delayUs / 1000000);
        timeout.tv_nsec = static_cast<long>(delayUs % 1000000) * 1000;
        const int rc = ::ppoll(&descriptor, 1, &timeout, nullptr);
        reactor->idleWait.store(false, std::memory_order_release);
        if (rc > 0) {
            ++reactor->doorbellWakes;
        } else {
            ++reactor->doorbellTimeouts;
        }
    }

    /* Wakes the target data reactor only while it is idle-waiting. The
     * lock-free entry point is Impl::wakeDataReactor() below. */
    void wakeReactor(Reactor *reactor) noexcept {
        if (reactor == nullptr || reactor->wakeFd < 0 ||
            !reactor->idleWait.load(std::memory_order_acquire)) {
            return;
        }
        const std::uint64_t one = 1;
        ssize_t written = -1;
        do {
            written = ::write(reactor->wakeFd, &one, sizeof(one));
        } while (written < 0 && errno == EINTR);
        /* EAGAIN means a wake is already pending. */
        (void)written;
    }

    /* Metadata reactor idle wait: same doorbell protocol, but the re-check
     * also keeps the reactor awake while control I/O is in flight. */
    void waitMetadataReactorIdle(Reactor *metadata, std::uint32_t delayUs) noexcept {
        if (metadata == nullptr || metadata->wakeFd < 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
            return;
        }
        std::uint64_t value = 0;
        ssize_t drained = -1;
        do {
            drained = ::read(metadata->wakeFd, &value, sizeof(value));
        } while (drained < 0 && errno == EINTR);
        (void)drained;
        metadata->idleWait.store(true, std::memory_order_release);
        if (spdk_thread_poll(metadata->spdk, 0, 0) != 0 ||
            metadata->controlInFlight.load(std::memory_order_acquire) != 0) {
            metadata->idleWait.store(false, std::memory_order_release);
            return;
        }
        pollfd descriptor{};
        descriptor.fd = metadata->wakeFd;
        descriptor.events = POLLIN;
        timespec timeout{};
        timeout.tv_sec = static_cast<time_t>(delayUs / 1000000);
        timeout.tv_nsec = static_cast<long>(delayUs % 1000000) * 1000;
        (void)::ppoll(&descriptor, 1, &timeout, nullptr);
        metadata->idleWait.store(false, std::memory_order_release);
    }

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
        opts.name = "uuse";
        opts.opts_size = sizeof(opts);
        /* Keep DPDK's EAL main lcore on the configured metadata CPU. */
        opts.main_core = static_cast<int>(config.metadataCpu);
        if (config.metadataCpu >= 64) {
            log::error("metadata_cpus %u cannot be represented by the SPDK core mask",
                       config.metadataCpu);
            return -EINVAL;
        }
        const std::uint64_t ealCoreMask = std::uint64_t{1} << config.metadataCpu;
        std::ostringstream ealCoreMaskText;
        ealCoreMaskText << "0x" << std::hex << ealCoreMask;
        const std::string ealCoreMaskString = ealCoreMaskText.str();
        opts.core_mask = ealCoreMaskString.c_str();
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
            struct spdk_bdev *bdev = spdk_bdev_get_by_name(device.name.c_str());
            if (bdev == nullptr) {
                log::error("bdev '%s' not found", device.name.c_str());
                return -ENOENT;
            }
            if (spdk_bdev_open_ext(device.name.c_str(), true, bdevEvent, this, &device.desc) != 0) {
                log::error("cannot open bdev '%s'", device.name.c_str());
                return -EIO;
            }
            device.bdev = bdev;
            device.numBlocks = spdk_bdev_get_num_blocks(bdev);
            device.blockSize = spdk_bdev_get_block_size(bdev);
        }
        return 0;
    }

    /* Submit one control I/O; runs on the metadata reactor. */
    void submitControlIo(std::size_t index, std::shared_ptr<ControlIo> io) {
        if (index >= devices.size() || reactors.empty() || reactors[0]->spdk == nullptr ||
            index >= reactors[0]->channels.size() || devices[index].desc == nullptr) {
            failControlIo(io, -EINVAL);
            return;
        }
        spdk_bdev_desc *desc = devices[index].desc;
        spdk_io_channel *channel = reactors[0]->channels[index];
        auto *holder = new ControlCompletion{io, &reactors[0]->controlInFlight};
        int rc = 0;
        switch (io->op) {
        case ControlIo::Op::Read:
            rc = spdk_bdev_read_blocks(desc, channel, io->dma, io->offsetBlocks, io->blocks,
                                       controlIoDone, holder);
            break;
        case ControlIo::Op::Write:
            rc = spdk_bdev_write_blocks(desc, channel, io->dma, io->offsetBlocks, io->blocks,
                                        controlIoDone, holder);
            break;
        case ControlIo::Op::Flush:
            /* io->offsetBlocks and io->blocks are already in bdev blocks. */
            rc = spdk_bdev_flush_blocks(desc, channel, io->offsetBlocks, io->blocks,
                                        controlIoDone, holder);
            break;
        }
        if (rc != 0) {
            delete holder;
            failControlIo(io, rc);
            return;
        }
        reactors[0]->controlInFlight.fetch_add(1, std::memory_order_acq_rel);
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

        int rc = 0;
        if (!envInited) {
            rc = initEnv();
            if (rc != 0) {
                publishStartDone(rc);
                return;
            }
            envInited = true;
        }
        /* EAL may replace the caller affinity during initialization; apply
         * the configured metadata CPU again after EAL is fully initialized. */
        pinToCpu(config.metadataCpu);
        std::fprintf(stderr, "[CPU MAP] metadata reactor: tid=%llu configured_cpu=%u "
                             "actual_cpu=%d\n",
                     static_cast<unsigned long long>(metadata->osTid.load(
                         std::memory_order_relaxed)),
                     config.metadataCpu, sched_getcpu());
        metadata->spdk = spdk_thread_create("uuse-meta", nullptr);
        if (metadata->spdk == nullptr) {
            publishStartDone(-ENOMEM);
            return;
        }
        spdk_set_thread(metadata->spdk);
        metadata->wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        const auto closeMetadataWakeFd = [metadata]() {
            if (metadata->wakeFd >= 0) {
                ::close(metadata->wakeFd);
                metadata->wakeFd = -1;
            }
        };

        if (!subsystemsReady) {
            rc = initSubsystems(metadata);
            if (rc != 0) {
                closeMetadataWakeFd();
                spdk_set_thread(nullptr);
                spdk_thread_destroy(metadata->spdk);
                metadata->spdk = nullptr;
                publishStartDone(rc);
                return;
            }
            subsystemsReady = true;
        }
        envReady = true;

        const int deviceWaitMs = static_cast<int>(std::min<std::uint64_t>(
            config.metadataWaitTimeoutMs == 0 ? 5000 : config.metadataWaitTimeoutMs, 5000));
        rc = waitForDevices(metadata, deviceWaitMs);
        if (rc != 0) {
            closeDevicesLocked();
            closeMetadataWakeFd();
            spdk_set_thread(nullptr);
            spdk_thread_destroy(metadata->spdk);
            metadata->spdk = nullptr;
            publishStartDone(rc);
            return;
        }
        rc = openDevices();
        if (rc != 0) {
            closeDevicesLocked();
            closeMetadataWakeFd();
            spdk_set_thread(nullptr);
            spdk_thread_destroy(metadata->spdk);
            metadata->spdk = nullptr;
            publishStartDone(rc);
            return;
        }

        /* Metadata reactor keeps one channel per device for control I/O. */
        for (auto &device : devices) {
            spdk_io_channel *channel = spdk_bdev_get_io_channel(device.desc);
            if (channel == nullptr) {
                rc = -ENOMEM;
                break;
            }
            metadata->channels.push_back(channel);
        }
        if (rc != 0) {
            for (auto *channel : metadata->channels) {
                spdk_put_io_channel(channel);
            }
            metadata->channels.clear();
            drainThreadMessages(metadata->spdk);
            closeDevicesLocked();
            drainThreadMessages(metadata->spdk);
            closeMetadataWakeFd();
            spdk_set_thread(nullptr);
            spdk_thread_destroy(metadata->spdk);
            metadata->spdk = nullptr;
            publishStartDone(rc);
            return;
        }

        pid.store(static_cast<std::int64_t>(::getpid()), std::memory_order_release);
        publishStartDone(0);
        log::info("spdk runtime ready: devices=%zu", devices.size());

        const bool metadataPollingActive = config.numa.polling == "active";
        const bool metadataPollingInterrupt = config.numa.polling == "interrupt";
        while (!metadata->stop.load(std::memory_order_acquire)) {
            const int polled = spdk_thread_poll(metadata->spdk, 0, 0);
            const bool hasWork =
                polled != 0 ||
                metadata->controlInFlight.load(std::memory_order_acquire) != 0;
            if (hasWork || metadataPollingActive) {
                metadata->idlePolls = 0;
            } else if (metadataPollingInterrupt) {
                ++metadata->pollSleeps;
                waitMetadataReactorIdle(metadata, 100);
            } else {
                /* Control-path reactor: 1 ms bounded wait; every cross-thread
                 * post wakes the eventfd, so submissions are not delayed. */
                ++metadata->pollSleeps;
                waitMetadataReactorIdle(metadata, 1000);
            }
        }

        for (auto *channel : metadata->channels) {
            spdk_put_io_channel(channel);
        }
        metadata->channels.clear();
        drainThreadMessages(metadata->spdk);
        closeDevicesLocked();
        drainThreadMessages(metadata->spdk);
        closeMetadataWakeFd();
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

    /* Select a CPU-local reactor for a device, falling back deterministically
     * when the host exposes no usable NUMA CPU mapping. */
    void configureReactorPlacement(const NumaTopologySnapshot &topology,
                                   std::uint32_t dataReactors) {
        deviceReactors.assign(devices.size(), 0);
        for (std::size_t deviceIndex = 0; deviceIndex < devices.size(); ++deviceIndex) {
            std::int32_t deviceNode = -1;
            if (deviceIndex < topology.devices.size()) {
                deviceNode = topology.devices[deviceIndex].node;
            }
            std::uint32_t selected = static_cast<std::uint32_t>(deviceIndex % dataReactors);
            for (std::uint32_t reactorIndex = 0; reactorIndex < dataReactors;
                 ++reactorIndex) {
                const int cpu = dataCpuFor(reactorIndex + 1);
                for (const auto &placement : topology.cpuPlacements) {
                    if (placement.cpu == static_cast<std::uint32_t>(cpu) &&
                        placement.node >= 0 && placement.node == deviceNode) {
                        selected = reactorIndex;
                        reactorIndex = dataReactors;
                        break;
                    }
                }
            }
            deviceReactors[deviceIndex] = selected;
        }
        for (std::uint32_t reactorIndex = 0; reactorIndex < dataReactors;
             ++reactorIndex) {
            Reactor *reactor = reactors[reactorIndex + 1].get();
            reactor->cpu = static_cast<std::uint32_t>(dataCpuFor(reactorIndex + 1));
            for (const auto &placement : topology.cpuPlacements) {
                if (placement.cpu == reactor->cpu) {
                    reactor->numaNode = placement.node;
                    break;
                }
            }
        }
    }

    void dataMain(std::size_t dataIndex) {
        auto *reactor = reactorAt(dataIndex);
        reactor->osTid.store(static_cast<std::uint64_t>(::syscall(SYS_gettid)),
                             std::memory_order_release);
        pinToCpu(reactor->cpu == 0 ? static_cast<unsigned int>(dataCpuFor(dataIndex))
                                   : reactor->cpu);
        std::fprintf(stderr, "[CPU MAP] data reactor %zu: tid=%llu configured_cpu=%u "
                             "actual_cpu=%d\n",
                     dataIndex - 1,
                     static_cast<unsigned long long>(reactor->osTid.load(
                         std::memory_order_relaxed)),
                     reactor->cpu == 0 ? static_cast<unsigned int>(dataCpuFor(dataIndex))
                                       : reactor->cpu,
                     sched_getcpu());

        const std::string name = "uuse-data" + std::to_string(dataIndex - 1);
        reactor->spdk = spdk_thread_create(name.c_str(), nullptr);
        if (reactor->spdk == nullptr) {
            reactor->result.store(-ENOMEM, std::memory_order_release);
            std::lock_guard<std::mutex> lock(mutex);
            ++readyCount;
            stateChanged.notify_all();
            return;
        }
        spdk_set_thread(reactor->spdk);
        reactor->wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

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

        /* A stopping reactor drains its lists, but never spins forever on a
         * stranded io-wait entry (device removal or a lost completion). */
        bool drainDeadlineSet = false;
        std::chrono::steady_clock::time_point drainDeadline{};
        /* The polling policy never changes while the reactor runs; hoist the
         * string comparisons out of the hot loop. */
        const bool pollingActive = config.numa.polling == "active";
        const bool pollingInterrupt = config.numa.polling == "interrupt";
        while (!reactor->stop.load(std::memory_order_acquire) ||
               reactor->activeHead != nullptr || reactor->retryHead != nullptr ||
               reactor->completionHead != nullptr ||
               dataIngressPending(reactor->ingressSlot)) {
            const int polled = spdk_thread_poll(reactor->spdk, 0, 0);
            const std::size_t ingested = drainDataIngress(reactor->ingressSlot);
            if (reactor->retryHead != nullptr) {
                drainDataIoRetries(reactor);
            }
            if (reactor->completionHead != nullptr) {
                drainDataIoCompletions(reactor);
            }
            const int pumped = invokeDataReactorPoller(reactor);
            const bool hasWork = polled != 0 || ingested != 0 || pumped != 0 ||
                                 reactor->activeHead != nullptr ||
                                 reactor->retryHead != nullptr ||
                                 reactor->completionHead != nullptr;
            if (hasWork || pollingActive) {
                reactor->idlePolls = 0;
            } else if (pollingInterrupt) {
                ++reactor->pollSleeps;
                waitDataReactorIdle(reactor, 100);
            } else {
                /* Hybrid polling preserves a short active window for bursty
                 * I/O, then waits on the doorbell instead of burning a core. */
                if (reactor->idlePolls < 128) {
                    ++reactor->idlePolls;
                } else {
                    ++reactor->pollSleeps;
                    const std::uint32_t idle = reactor->idlePolls;
                    const auto delay = std::min<std::uint32_t>(100, 1U <<
                                                               std::min<std::uint32_t>(
                                                                   6, idle / 256));
                    waitDataReactorIdle(reactor, delay);
                }
            }
            if (reactor->stop.load(std::memory_order_acquire)) {
                const auto now = std::chrono::steady_clock::now();
                if (!drainDeadlineSet) {
                    drainDeadline = now + std::chrono::seconds(5);
                    drainDeadlineSet = true;
                } else if (now >= drainDeadline) {
                    log::error("data reactor %zu drain deadline exceeded with in-flight "
                               "states; abandoning them",
                               dataIndex - 1);
                    break;
                }
            }
        }

        for (auto *channel : reactor->channels) {
            spdk_put_io_channel(channel);
        }
        reactor->channels.clear();
        drainThreadMessages(reactor->spdk);
        if (reactor->wakeFd >= 0) {
            ::close(reactor->wakeFd);
            reactor->wakeFd = -1;
        }
        spdk_set_thread(nullptr);
        spdk_thread_destroy(reactor->spdk);
        reactor->spdk = nullptr;
    }

    /* Stop and join all reactors; safe to call from start() failure paths. */
    void shutdownReactors() {
        disableDataReactorPollers();
        /* Retire the doorbell table before stopping the reactors so no wake can
         * touch a reactor after its thread exits. */
        disableWakeTable();
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
        /* Safety net: fail any synchronous ingress entry the reactors could
         * not drain, then release the per-reactor ingress and holder storage. */
        for (std::size_t index = 0; index < dataIngress_.size(); ++index) {
            if (dataIngress_[index] == nullptr) {
                continue;
            }
            DataIoCall call{};
            while (dataIngress_[index]->tryPop(call)) {
                if (call.cb != nullptr) {
                    call.cb(call.cbCtx, -ECANCELED);
                }
            }
        }
        dataIngress_.clear();
        dataHolderPools_.clear();
        dataHolderPoolSize_.clear();
        dataHolderFree_.clear();
        wakeEntries_.reset();
        wakeEntryCount_ = 0;
        std::lock_guard<std::mutex> lock(mutex);
        reactors.clear();
    }

    struct ControlCall {
        Impl *impl;
        std::shared_ptr<ControlIo> io;

        ControlCall(Impl *implPtr, std::shared_ptr<ControlIo> ioPtr)
            : impl(implPtr), io(std::move(ioPtr)) {}
    };

    static void controlSubmitTrampoline(void *ctx) {
        auto *call = static_cast<ControlCall *>(ctx);
        Impl *impl = call->impl;
        /* Read the device before any move: argument evaluation order is
         * unspecified and a moved-from shared_ptr must not be dereferenced. */
        std::shared_ptr<ControlIo> io = std::move(call->io);
        const std::size_t device = io->device;
        delete call;
        impl->submitControlIo(device, std::move(io));
    }

    /* Async data-reactor I/O request. */
    struct DataIoCall {
        Impl *impl = nullptr;
        std::size_t reactorIndex = 0; /* index into reactors (data reactors 1..R) */
        std::size_t deviceIndex = 0;
        void *buffer = nullptr;
        std::uint64_t offsetBlocks = 0;
        std::uint64_t blocks = 0;
        bool write = false;
        void (*cb)(void *, int) = nullptr;
        void *cbCtx = nullptr;
    };

    struct DataIoHolder {
        Impl *impl = nullptr;
        void (*cb)(void *, int) = nullptr;
        void *cbCtx = nullptr;
        std::size_t deviceIndex = 0;
        std::size_t reactorIndex = 0;
        bool write = false;
        /* Pooled holders return to the owning reactor's free list on completion. */
        bool pooled = false;
        DataIoHolder *freeNext = nullptr;
        /* SPDK retains the iovec until the asynchronous completion. */
        struct iovec iov{};
    };

    static constexpr std::size_t kDataIngressDepth = 1024;
    static constexpr std::size_t kDataIngressBatch = 64;
    using DataIngress = BoundedMpscQueue<DataIoCall, kDataIngressDepth>;

    /* Per-data-reactor synchronous submission ingress (heap storage). */
    std::vector<std::unique_ptr<DataIngress>> dataIngress_;
    /* Per-data-reactor completion holder pool; reactor-thread private. */
    std::vector<std::unique_ptr<DataIoHolder[]>> dataHolderPools_;
    std::vector<std::size_t> dataHolderPoolSize_;
    std::vector<DataIoHolder *> dataHolderFree_;

    void initializeDataIngress(std::size_t reactorCount) {
        dataIngress_.clear();
        dataIngress_.resize(reactorCount);
        for (std::size_t index = 0; index < reactorCount; ++index) {
            dataIngress_[index] = std::make_unique<DataIngress>();
        }
    }

    void initializeDataHolderPools(std::size_t reactorCount) {
        dataHolderPools_.clear();
        dataHolderPoolSize_.clear();
        dataHolderFree_.clear();
        dataHolderPools_.resize(reactorCount);
        dataHolderPoolSize_.assign(reactorCount, 0);
        dataHolderFree_.assign(reactorCount, nullptr);
        for (std::size_t index = 0; index < reactorCount; ++index) {
            auto pool = std::unique_ptr<DataIoHolder[]>(
                new (std::nothrow) DataIoHolder[kDataIngressDepth]);
            if (pool == nullptr) {
                continue;
            }
            for (std::size_t slot = 0; slot < kDataIngressDepth; ++slot) {
                pool[slot].pooled = true;
                pool[slot].freeNext =
                    slot + 1 < kDataIngressDepth ? &pool[slot + 1] : nullptr;
            }
            dataHolderFree_[index] = &pool[0];
            dataHolderPoolSize_[index] = kDataIngressDepth;
            dataHolderPools_[index] = std::move(pool);
        }
    }

    DataIoHolder *acquireDataIoHolder(std::size_t reactorIndex) noexcept {
        if (reactorIndex < dataHolderFree_.size()) {
            DataIoHolder *holder = dataHolderFree_[reactorIndex];
            if (holder != nullptr) {
                dataHolderFree_[reactorIndex] = holder->freeNext;
                holder->freeNext = nullptr;
                return holder;
            }
        }
        /* Rare overflow: bounded by in-flight synchronous I/O, not the pool. */
        return new (std::nothrow) DataIoHolder();
    }

    void releaseDataIoHolder(DataIoHolder *holder) noexcept {
        if (holder == nullptr) {
            return;
        }
        if (!holder->pooled) {
            delete holder;
            return;
        }
        const std::size_t index = holder->reactorIndex;
        if (index < dataHolderFree_.size()) {
            holder->freeNext = dataHolderFree_[index];
            dataHolderFree_[index] = holder;
        }
    }

    /* Lock-free doorbell table: entries are published while the runtime is
     * active and retired under a reader gate, so a concurrent wake can never
     * touch a dying reactor (same protocol as the poller reader mask). */
    static constexpr std::uint64_t kWakeEnabledBit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t kWakeReaderMask = ~kWakeEnabledBit;
    std::atomic<std::uint64_t> wakeGateState_{0};
    std::unique_ptr<std::atomic<Reactor *>[]> wakeEntries_;
    std::size_t wakeEntryCount_ = 0;

    void enableWakeTable() noexcept {
        for (std::size_t index = 0; index < wakeEntryCount_; ++index) {
            wakeEntries_[index].store(reactors[index + 1].get(),
                                      std::memory_order_relaxed);
        }
        wakeGateState_.store(kWakeEnabledBit, std::memory_order_release);
    }

    void disableWakeTable() noexcept {
        wakeGateState_.fetch_and(kWakeReaderMask, std::memory_order_acq_rel);
        for (;;) {
            const std::uint64_t state = wakeGateState_.load(std::memory_order_acquire);
            if ((state & kWakeReaderMask) == 0) {
                break;
            }
            wakeGateState_.wait(state, std::memory_order_acquire);
        }
        for (std::size_t index = 0; index < wakeEntryCount_; ++index) {
            wakeEntries_[index].store(nullptr, std::memory_order_relaxed);
        }
    }

    /* Wakes one idle-waiting data reactor without taking the runtime mutex. */
    Result<void> wakeDataReactor(std::uint32_t index) noexcept {
        if (index >= wakeEntryCount_) {
            return makeError(Status::InvalidArgument, "data reactor index out of range");
        }
        for (;;) {
            std::uint64_t state = wakeGateState_.load(std::memory_order_acquire);
            if ((state & kWakeEnabledBit) == 0) {
                return makeError(Status::Busy, "SPDK runtime is not active");
            }
            if (wakeGateState_.compare_exchange_weak(state, state + 1,
                                                     std::memory_order_acquire,
                                                     std::memory_order_relaxed)) {
                break;
            }
        }
        Reactor *reactor = wakeEntries_[index].load(std::memory_order_acquire);
        if (reactor != nullptr) {
            wakeReactor(reactor);
        }
        const std::uint64_t previous = wakeGateState_.fetch_sub(1, std::memory_order_acq_rel);
        if ((previous & kWakeReaderMask) == 1) {
            wakeGateState_.notify_all();
        }
        return Result<void>(Status::Ok);
    }

    static void dataIoDone(struct spdk_bdev_io *bdevIo, bool success, void *arg) {
        auto *holder = static_cast<DataIoHolder *>(arg);
        const int rc = success ? 0 : dataIoCompletionErrno(bdevIo);
        spdk_bdev_free_io(bdevIo);
        if (success && holder->write && holder->impl != nullptr) {
            holder->impl->noteDataWriteComplete(holder->deviceIndex);
        }
        void (*cb)(void *, int) = holder->cb;
        void *cbCtx = holder->cbCtx;
        Impl *impl = holder->impl;
        /* Release before invoking the callback so a synchronous re-submit can
         * reuse the same holder. */
        impl->releaseDataIoHolder(holder);
        if (cb != nullptr) {
            cb(cbCtx, rc);
        }
    }

    void submitDataIo(const DataIoCall &call) {
        if (call.cb == nullptr || call.reactorIndex >= reactors.size() ||
            call.deviceIndex >= devices.size() ||
            reactors[call.reactorIndex]->spdk == nullptr ||
            call.deviceIndex >= reactors[call.reactorIndex]->channels.size() ||
            devices[call.deviceIndex].desc == nullptr || call.buffer == nullptr ||
            call.blocks == 0) {
            if (call.cb != nullptr) {
                call.cb(call.cbCtx, -EINVAL);
            }
            return;
        }
        spdk_bdev_desc *desc = devices[call.deviceIndex].desc;
        spdk_io_channel *channel =
            reactors[call.reactorIndex]->channels[call.deviceIndex];
        auto *holder = acquireDataIoHolder(call.reactorIndex - 1);
        if (holder == nullptr) {
            if (call.cb != nullptr) {
                call.cb(call.cbCtx, -ENOMEM);
            }
            return;
        }
        holder->impl = this;
        holder->cb = call.cb;
        holder->cbCtx = call.cbCtx;
        holder->deviceIndex = call.deviceIndex;
        holder->reactorIndex = call.reactorIndex - 1;
        holder->write = call.write;
        holder->freeNext = nullptr;
        holder->iov.iov_base = call.buffer;
        holder->iov.iov_len = call.blocks * devices[call.deviceIndex].blockSize;
        const int rc = call.write
                           ? spdk_bdev_writev_blocks(desc, channel, &holder->iov, 1,
                                                     call.offsetBlocks, call.blocks,
                                                     dataIoDone, holder)
                           : spdk_bdev_readv_blocks(desc, channel, &holder->iov, 1,
                                                    call.offsetBlocks, call.blocks,
                                                    dataIoDone, holder);
        if (rc != 0) {
            releaseDataIoHolder(holder);
            if (call.cb != nullptr) {
                call.cb(call.cbCtx, rc);
            }
        } else if (stats::enabled()) {
            /* Include the synchronous DataPath in accepted device I/O counts. */
            auto &counters = stats::counters();
            if (call.write) {
                counters.bdevDataWriteOps.fetch_add(1, std::memory_order_relaxed);
                counters.bdevDataWriteBytes.fetch_add(holder->iov.iov_len,
                                                      std::memory_order_relaxed);
            } else {
                counters.bdevDataReadOps.fetch_add(1, std::memory_order_relaxed);
                counters.bdevDataReadBytes.fetch_add(holder->iov.iov_len,
                                                     std::memory_order_relaxed);
            }
        }
    }

    /* Reactor-thread drain for the bounded synchronous ingress. */
    std::size_t drainDataIngress(std::size_t index) noexcept {
        if (index >= dataIngress_.size() || dataIngress_[index] == nullptr) {
            return 0;
        }
        DataIoCall call{};
        std::size_t drained = 0;
        while (drained < kDataIngressBatch && dataIngress_[index]->tryPop(call)) {
            submitDataIo(call);
            ++drained;
        }
        return drained;
    }

    bool dataIngressPending(std::size_t index) const noexcept {
        return index < dataIngress_.size() && dataIngress_[index] != nullptr &&
               dataIngress_[index]->hasPending();
    }

    /* Synchronous (non-actor) data submission used by DataPath. The common
     * path publishes into the reactor's bounded ingress; the historical SPDK
     * message path remains as a rare fallback when the ingress is full. */
    Result<void> submitSyncDataIo(std::uint32_t reactorIndex, std::size_t deviceIndex,
                                  void *buffer, std::uint64_t offsetBlocks,
                                  std::uint64_t blocks, bool write,
                                  void (*cb)(void *, int), void *cbCtx) {
        DataIoCall call{};
        call.impl = this;
        call.reactorIndex = reactorIndex + 1;
        call.deviceIndex = deviceIndex;
        call.buffer = buffer;
        call.offsetBlocks = offsetBlocks;
        call.blocks = blocks;
        call.write = write;
        call.cb = cb;
        call.cbCtx = cbCtx;
        /* Serialize with stop(): once stoppingFlag is set no new ingress entry
         * may be published, so the owning reactor drains everything accepted. */
        std::lock_guard<std::mutex> lock(mutex);
        if (!activeFlag.load(std::memory_order_acquire) ||
            stoppingFlag.load(std::memory_order_acquire)) {
            return makeError(Status::Busy, "SPDK runtime is not active");
        }
        if (reactorIndex >= dataIngress_.size() || reactorIndex + 1 >= reactors.size() ||
            deviceIndex >= devices.size()) {
            return makeError(Status::InvalidArgument, "data reactor or device out of range");
        }
        if (dataIngress_[reactorIndex]->tryPush(call)) {
            (void)wakeDataReactor(reactorIndex);
            return Result<void>(Status::Ok);
        }
        spdk_thread *thread = reactors[reactorIndex + 1]->spdk;
        if (thread == nullptr) {
            return makeError(Status::Busy, "data reactor is not ready");
        }
        auto *heapCall = new (std::nothrow) DataIoCall(call);
        if (heapCall == nullptr) {
            return makeError(Status::Busy, "data ingress is full");
        }
        if (spdk_thread_send_msg(thread, dataSubmitTrampoline, heapCall) != 0) {
            delete heapCall;
            return makeError(Status::Busy, "spdk_thread_send_msg failed");
        }
        (void)wakeDataReactor(reactorIndex);
        return Result<void>(Status::Ok);
    }

    static void dataSubmitTrampoline(void *ctx) {
        auto *call = static_cast<DataIoCall *>(ctx);
        Impl *impl = call->impl;
        impl->submitDataIo(*call);
        delete call;
    }

    /* State for the actor-local API. This object is placement-constructed in
     * DataIoRequest storage and never allocated by the hot path. */
    struct DataIoState {
        std::uint64_t magic = 0;
        Impl *impl = nullptr;
        Reactor *reactor = nullptr;
        DataIoRequest *request = nullptr;
        std::size_t deviceIndex = 0;
        std::uint64_t traceRequestId = 0;
        std::uint32_t traceChildIndex = 0;
        spdk_bdev_desc *desc = nullptr;
        spdk_bdev *bdev = nullptr;
        spdk_io_channel *channel = nullptr;
        void *buffer = nullptr;
        std::uint64_t offsetBlocks = 0;
        std::uint64_t blocks = 0;
        std::size_t bytes = 0;
        std::uint32_t blockSize = 0;
        struct iovec iov{};
        bool write = false;
        DataIoCompletion cb = nullptr;
        void *cbCtx = nullptr;
        spdk_bdev_io_wait_entry waitEntry{};
        DataIoState *activeNext = nullptr;
        DataIoState *activePrev = nullptr;
        DataIoState *retryNext = nullptr;
        DataIoState *completionNext = nullptr;
        int completionRc = 0;
        bool activeLinked = false;
        bool waitingForIo = false;
        bool retryQueued = false;
        bool completionQueued = false;
        bool terminal = false;
    };

    static constexpr std::uint64_t kDataIoStateMagic = 0x5532465344415441ULL;
    static constexpr unsigned kImmediateRetryLimit = 8;
    static_assert(sizeof(DataIoState) <= DataIoRequest::kStorageBytes,
                  "DataIoRequest storage is too small for the SPDK state");

    static DataIoState *dataIoState(DataIoRequest &request) noexcept {
        return reinterpret_cast<DataIoState *>(request.storage);
    }

    void linkDataIo(DataIoState *state) noexcept {
        Reactor *reactor = state->reactor;
        state->activePrev = nullptr;
        state->activeNext = reactor->activeHead;
        if (reactor->activeHead != nullptr) {
            reactor->activeHead->activePrev = state;
        }
        reactor->activeHead = state;
        state->activeLinked = true;
    }

    void unlinkDataIo(DataIoState *state) noexcept {
        if (!state->activeLinked) {
            return;
        }
        Reactor *reactor = state->reactor;
        if (state->activePrev != nullptr) {
            state->activePrev->activeNext = state->activeNext;
        } else {
            reactor->activeHead = state->activeNext;
        }
        if (state->activeNext != nullptr) {
            state->activeNext->activePrev = state->activePrev;
        }
        state->activePrev = nullptr;
        state->activeNext = nullptr;
        state->activeLinked = false;
    }

    void enqueueDataIoRetry(DataIoState *state) noexcept {
        if (state->terminal || state->completionQueued || state->retryQueued) {
            return;
        }
        state->retryQueued = true;
        state->retryNext = nullptr;
        if (state->reactor->retryTail == nullptr) {
            state->reactor->retryHead = state;
        } else {
            state->reactor->retryTail->retryNext = state;
        }
        state->reactor->retryTail = state;
    }

    void enqueueDataIoCompletion(DataIoState *state, int rc) noexcept {
        if (state->terminal || state->completionQueued) {
            return;
        }
        state->completionRc = rc == 0 ? 0 : (rc < 0 ? rc : -rc);
        state->completionQueued = true;
        state->completionNext = nullptr;
        if (state->reactor->completionTail == nullptr) {
            state->reactor->completionHead = state;
        } else {
            state->reactor->completionTail->completionNext = state;
        }
        state->reactor->completionTail = state;
    }

    void finishDataIo(DataIoState *state) noexcept {
        if (state->terminal) {
            return;
        }
        state->terminal = true;
        unlinkDataIo(state);
        DataIoRequest *request = state->request;
        const DataIoCompletion cb = state->cb;
        void *const cbCtx = state->cbCtx;
        const int rc = state->completionRc;
        const std::uint64_t traceRequestId = state->traceRequestId;
        const std::uint32_t traceChildIndex = state->traceChildIndex;
        trace::event(trace::EventId::BdevComplete,
                     static_cast<std::uint64_t>(rc < 0 ? -static_cast<std::int64_t>(rc) : rc),
                     state->bytes, state->write ? 1 : 0, state->offsetBlocks);
        trace::event(trace::EventId::RequestBdevComplete, traceRequestId,
                     traceChildIndex,
                     static_cast<std::uint64_t>(
                         rc < 0 ? -static_cast<std::int64_t>(rc) : rc),
                     state->bytes);
        request->stateTag = 0;
        request->traceRequestId = 0;
        request->traceChildIndex = 0;
        state->~DataIoState();
        /* The caller supplied callback is already on the owning reactor; it
         * must remain nonblocking and must not enter frontend code. */
        cb(cbCtx, rc);
    }

    void drainDataIoRetries(Reactor *reactor) noexcept {
        DataIoState *state = reactor->retryHead;
        reactor->retryHead = nullptr;
        reactor->retryTail = nullptr;
        while (state != nullptr) {
            DataIoState *next = state->retryNext;
            state->retryNext = nullptr;
            state->retryQueued = false;
            if (!state->terminal && !state->completionQueued) {
                trySubmitDataIo(state);
            }
            state = next;
        }
    }

    void drainDataIoCompletions(Reactor *reactor) noexcept {
        DataIoState *state = reactor->completionHead;
        reactor->completionHead = nullptr;
        reactor->completionTail = nullptr;
        while (state != nullptr) {
            DataIoState *next = state->completionNext;
            state->completionNext = nullptr;
            state->completionQueued = false;
            finishDataIo(state);
            state = next;
        }
    }

    static void dataIoWait(void *ctx) {
        auto *state = static_cast<DataIoState *>(ctx);
        if (state->terminal || !state->waitingForIo) {
            return;
        }
        state->waitingForIo = false;
        /* SPDK invokes this on the same reactor. Defer the retry until the
         * reactor loop, so submission never recurses through SPDK's waitq. */
        state->impl->enqueueDataIoRetry(state);
    }

    static void actorDataIoDone(struct spdk_bdev_io *bdevIo, bool success, void *arg) {
        auto *state = static_cast<DataIoState *>(arg);
        const int rc = success ? 0 : dataIoCompletionErrno(bdevIo);
        spdk_bdev_free_io(bdevIo);
        if (success && state->write) {
            state->impl->noteDataWriteComplete(state->deviceIndex);
        }
        if (state->write && stats::enabled()) {
            stats::counters().bdevDataWriteOps.fetch_add(1, std::memory_order_relaxed);
            stats::counters().bdevDataWriteBytes.fetch_add(state->bytes,
                                                            std::memory_order_relaxed);
        } else if (!state->write && stats::enabled()) {
            stats::counters().dataReactorReadComplete.fetch_add(1,
                                                                 std::memory_order_relaxed);
        }
        /* Do not invoke the caller from the SPDK callback stack. */
        state->impl->enqueueDataIoCompletion(state, rc);
    }

    /* Submit or wait one actor-local request. All calls happen on one data
     * reactor, so the request state needs no synchronization. */
    void trySubmitDataIo(DataIoState *state) noexcept {
        state->iov.iov_base = state->buffer;
        state->iov.iov_len = state->bytes;
        for (unsigned attempt = 0; attempt <= kImmediateRetryLimit; ++attempt) {
            trace::event(trace::EventId::BdevSubmit, state->offsetBlocks, state->blocks,
                         state->write ? 1 : 0, attempt);
            trace::event(trace::EventId::RequestBdevSubmit, state->traceRequestId,
                         state->traceChildIndex, state->offsetBlocks, state->blocks);
            const int rc = state->write
                               ? spdk_bdev_writev_blocks(state->desc, state->channel, &state->iov,
                                                         1,
                                                         state->offsetBlocks, state->blocks,
                                                         actorDataIoDone, state)
                               : spdk_bdev_readv_blocks(state->desc, state->channel, &state->iov,
                                                        1,
                                                        state->offsetBlocks, state->blocks,
                                                        actorDataIoDone, state);
            if (rc == 0) {
                if (state->write && stats::enabled()) {
                    stats::counters().bdevDataWriteOps.fetch_add(
                        1, std::memory_order_relaxed);
                    stats::counters().bdevDataWriteBytes.fetch_add(
                        state->bytes, std::memory_order_relaxed);
                } else if (!state->write && stats::enabled()) {
                    stats::counters().dataReactorReadSubmit.fetch_add(
                        1, std::memory_order_relaxed);
                    stats::counters().bdevDataBlockBytes.store(
                        state->blockSize, std::memory_order_relaxed);
                    stats::counters().bdevDataReadOps.fetch_add(
                        1, std::memory_order_relaxed);
                    stats::counters().bdevDataReadBytes.fetch_add(
                        state->bytes, std::memory_order_relaxed);
                    std::uint64_t depth = 0;
                    for (auto *active = state->reactor->activeHead; active != nullptr;
                         active = active->activeNext) {
                        ++depth;
                    }
                    stats::counters().dataReactorQueueDepthSum.fetch_add(
                        depth, std::memory_order_relaxed);
                    auto &maximum = stats::counters().dataReactorQueueDepthMax;
                    auto previous = maximum.load(std::memory_order_relaxed);
                    while (previous < depth &&
                           !maximum.compare_exchange_weak(previous, depth,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
                    }
                }
                return;
            }
            if (rc != -ENOMEM) {
                trace::event(trace::EventId::BackendErr,
                             static_cast<std::uint64_t>(
                                 rc < 0 ? -static_cast<std::int64_t>(rc) : rc),
                             state->write ? 1 : 0, state->offsetBlocks);
                enqueueDataIoCompletion(state, rc);
                return;
            }

            state->waitEntry = {};
            state->waitEntry.bdev = state->bdev;
            state->waitEntry.cb_fn = dataIoWait;
            state->waitEntry.cb_arg = state;
            state->waitEntry.dep_unblock = false;
            state->waitingForIo = true;
            const int waitRc =
                spdk_bdev_queue_io_wait(state->bdev, state->channel, &state->waitEntry);
            if (waitRc == 0) {
                return;
            }
            state->waitingForIo = false;
            if (state->retryQueued) {
                /* A bdev may have released an I/O while the wait entry was
                 * being installed. Let the already queued retry win. */
                return;
            }
            if (waitRc != -EINVAL || attempt == kImmediateRetryLimit) {
                enqueueDataIoCompletion(state, waitRc);
                return;
            }
            /* -EINVAL means a bdev I/O became available between the failed
             * submit and queue_io_wait; retry on this same reactor. */
        }
    }

    Result<void> submitDataIoOnReactor(std::uint32_t reactorIndex,
                                       std::size_t deviceIndex, DataIoRequest &request,
                                       void *buffer, std::uint64_t offsetBlocks,
                                       std::uint64_t blocks, bool write,
                                       DataIoCompletion cb, void *cbCtx) {
        if (!activeFlag.load(std::memory_order_acquire)) {
            return makeError(Status::Busy, "SPDK runtime is not active");
        }
        if (reactorIndex >= reactorCount.load(std::memory_order_acquire) ||
            reactorIndex + 1 >= reactors.size()) {
            return makeError(Status::InvalidArgument, "data reactor index out of range");
        }
        Reactor *reactor = reactors[reactorIndex + 1].get();
        if (reactor->spdk == nullptr || spdk_get_thread() != reactor->spdk) {
            return makeError(Status::InvalidState,
                             "actor-local data I/O called from the wrong reactor");
        }
        if (deviceIndex >= devices.size()) {
            return makeError(Status::InvalidArgument, "data device index out of range");
        }
        DeviceState &device = devices[deviceIndex];
        if (device.bdev == nullptr || device.desc == nullptr ||
            deviceIndex >= reactor->channels.size() ||
            reactor->channels[deviceIndex] == nullptr) {
            return makeError(Status::InvalidState, "data device channel is not ready");
        }
        if (request.stateTag != 0) {
            return makeError(Status::Busy, "data I/O request storage is already in use");
        }
        if (buffer == nullptr || cb == nullptr || blocks == 0 || device.blockSize == 0) {
            return makeError(Status::InvalidArgument, "invalid actor-local data I/O arguments");
        }
        if (offsetBlocks > device.numBlocks || blocks > device.numBlocks - offsetBlocks) {
            return makeError(Status::InvalidArgument, "data I/O range exceeds device capacity");
        }
        if (blocks > std::numeric_limits<std::size_t>::max() / device.blockSize) {
            return makeError(Status::InvalidArgument, "data I/O byte length overflows size_t");
        }

        auto *state = dataIoState(request);
        ::new (static_cast<void *>(state)) DataIoState{};
        request.stateTag = kDataIoStateMagic;
        state->magic = kDataIoStateMagic;
        state->impl = this;
        state->reactor = reactor;
        state->request = &request;
        state->deviceIndex = deviceIndex;
        state->traceRequestId = request.traceRequestId;
        state->traceChildIndex = request.traceChildIndex;
        state->desc = device.desc;
        state->bdev = device.bdev;
        state->channel = reactor->channels[deviceIndex];
        state->buffer = buffer;
        state->offsetBlocks = offsetBlocks;
        state->blocks = blocks;
        state->bytes = static_cast<std::size_t>(blocks) * device.blockSize;
        state->blockSize = device.blockSize;
        state->write = write;
        state->cb = cb;
        state->cbCtx = cbCtx;
        linkDataIo(state);
        trySubmitDataIo(state);
        return Result<void>(Status::Ok);
    }

    /* Blocking control-path block I/O helper shared by the public methods. */
    Result<void> controlIo(ControlIo::Op op, std::size_t index, std::uint64_t offsetBlocks,
                           std::span<std::byte> readTarget,
                           std::span<const std::byte> writeSource) {
        if (!activeFlag.load(std::memory_order_acquire) ||
            stoppingFlag.load(std::memory_order_acquire)) {
            return makeError(Status::Busy, "SPDK runtime is not active");
        }
        if (index >= devices.size() || devices[index].desc == nullptr || reactors.empty() ||
            reactors[0]->spdk == nullptr) {
            return makeError(Status::InvalidArgument, "device index out of range");
        }
        if (op == ControlIo::Op::Flush) {
            test_hooks::crashIfRequested("data_flush");
        }
        if (op == ControlIo::Op::Flush && testFlushFailureRequested(index)) {
            return makeError(Status::IoError,
                             "test-injected data/control flush failure on device " +
                                 std::to_string(index));
        }
        /* Coverage check: skip the device flush only when no data write has
         * completed since the last flush on this device. Injection hooks above
         * still fire, so crash/failure tests keep their trigger points. */
        std::uint64_t flushTarget = 0;
        if (op == ControlIo::Op::Flush) {
            flushTarget = dataWrittenSeq[index].load(std::memory_order_acquire);
            if (dataFlushedSeq[index].load(std::memory_order_acquire) >= flushTarget) {
                stats::counters().deviceFlushSkips.fetch_add(1, std::memory_order_relaxed);
                return Result<void>(Status::Ok);
            }
        }

        auto io = std::make_shared<ControlIo>();
        io->op = op;
        io->device = index;
        io->offsetBlocks = offsetBlocks;
        io->readTarget = readTarget;
        io->writeSource = writeSource;

        if (op != ControlIo::Op::Flush) {
            const std::size_t size = op == ControlIo::Op::Read ? readTarget.size()
                                                               : writeSource.size();
            if (size == 0) {
                return Result<void>(Status::Ok);
            }
            const std::uint32_t blockSize = devices[index].blockSize;
            if (blockSize == 0 || size % blockSize != 0) {
                return makeError(Status::InvalidArgument,
                                 "buffer size must be a multiple of the device block size");
            }
            if (size > (1U << 20)) {
                return makeError(Status::InvalidArgument, "control I/O is limited to 1 MiB");
            }
            io->blocks = size / blockSize;
            io->dma = spdk_dma_zmalloc(size, 4096, nullptr);
            if (io->dma == nullptr) {
                return makeError(Status::NoMemory, "DMA allocation failed");
            }
            if (op == ControlIo::Op::Write) {
                std::memcpy(io->dma, writeSource.data(), size);
            }
        } else {
            io->blocks = devices[index].numBlocks;
        }

        auto *call = new ControlCall(this, io);
        if (spdk_thread_send_msg(reactors[0]->spdk, controlSubmitTrampoline, call) != 0) {
            delete call;
            if (io->dma != nullptr) {
                spdk_dma_free(io->dma);
                io->dma = nullptr;
            }
            return makeError(Status::Busy, "spdk_thread_send_msg failed");
        }
        wakeReactor(reactors[0].get());
        if (op == ControlIo::Op::Flush) {
            stats::counters().deviceFlushes.fetch_add(1, std::memory_order_relaxed);
        }

        std::unique_lock<std::mutex> lock(io->mutex);
        const auto timeout = std::chrono::milliseconds(config.metadataWaitTimeoutMs);
        if (!io->cv.wait_for(lock, timeout, [&io]() { return io->done; })) {
            io->abandoned = true;
            return makeError(Status::TimedOut, "control I/O timed out");
        }
        const int rc = io->rc;
        lock.unlock();
        if (rc == 0) {
            if (op == ControlIo::Op::Flush) {
                publishDataFlushed(index, flushTarget);
            } else if (op == ControlIo::Op::Write) {
                /* Metadata/journal writes also need flush coverage. */
                noteDataWriteComplete(index);
            }
            return Result<void>(Status::Ok);
        }
        return makeError(Status::IoError, "control I/O failed rc=" + std::to_string(rc));
    }

    /* Submits several control writes together and waits for all of them; the
     * metadata reactor keeps them in flight concurrently so the device can
     * overlap the round trips. Each range must be block-aligned and <= 1 MiB.
     * Logical batches larger than one device batch (checkpoint snapshots can
     * be many megabytes) are split into bounded sequential chunks; a crash
     * between chunks leaves a partial record that the existing CRC/generation
     * validation rejects. */
    Result<void> controlWriteBatch(std::size_t index,
                                   std::span<const ControlWriteRange> writes) {
        std::size_t offset = 0;
        while (offset < writes.size()) {
            const std::size_t count =
                std::min(kMaxControlBatchWrites, writes.size() - offset);
            auto chunk = controlWriteBatchChunk(index, writes.subspan(offset, count));
            if (!chunk.ok()) {
                return chunk;
            }
            offset += count;
        }
        return Result<void>(Status::Ok);
    }

    /* Submits one bounded control write batch; see controlWriteBatch. */
    Result<void> controlWriteBatchChunk(std::size_t index,
                                        std::span<const ControlWriteRange> writes) {
        if (!activeFlag.load(std::memory_order_acquire) ||
            stoppingFlag.load(std::memory_order_acquire)) {
            return makeError(Status::Busy, "SPDK runtime is not active");
        }
        if (index >= devices.size() || devices[index].desc == nullptr || reactors.empty() ||
            reactors[0]->spdk == nullptr) {
            return makeError(Status::InvalidArgument, "device index out of range");
        }
        if (writes.empty()) {
            return Result<void>(Status::Ok);
        }
        if (writes.size() > kMaxControlBatchWrites) {
            return makeError(Status::InvalidArgument, "control write batch is too large");
        }
        const std::uint32_t blockSize = devices[index].blockSize;
        std::vector<std::shared_ptr<ControlIo>> pending;
        pending.reserve(writes.size());
        for (const ControlWriteRange &range : writes) {
            if (range.buffer.empty()) {
                continue;
            }
            if (blockSize == 0 || range.buffer.size() % blockSize != 0) {
                return makeError(Status::InvalidArgument,
                                 "buffer size must be a multiple of the device block size");
            }
            if (range.buffer.size() > (1U << 20)) {
                return makeError(Status::InvalidArgument, "control I/O is limited to 1 MiB");
            }
            auto io = std::make_shared<ControlIo>();
            io->op = ControlIo::Op::Write;
            io->device = index;
            io->offsetBlocks = range.offsetBlocks;
            io->blocks = range.buffer.size() / blockSize;
            io->dma = spdk_dma_zmalloc(range.buffer.size(), 4096, nullptr);
            if (io->dma == nullptr) {
                return makeError(Status::NoMemory, "DMA allocation failed");
            }
            std::memcpy(io->dma, range.buffer.data(), range.buffer.size());
            auto *call = new ControlCall(this, io);
            if (spdk_thread_send_msg(reactors[0]->spdk, controlSubmitTrampoline, call) != 0) {
                delete call;
                spdk_dma_free(io->dma);
                io->dma = nullptr;
                return makeError(Status::Busy, "spdk_thread_send_msg failed");
            }
            pending.push_back(std::move(io));
        }
        if (pending.empty()) {
            return Result<void>(Status::Ok);
        }
        wakeReactor(reactors[0].get());

        Result<void> result(Status::Ok);
        for (auto &io : pending) {
            std::unique_lock<std::mutex> lock(io->mutex);
            const auto timeout = std::chrono::milliseconds(config.metadataWaitTimeoutMs);
            if (!io->cv.wait_for(lock, timeout, [&io]() { return io->done; })) {
                io->abandoned = true;
                if (result.ok()) {
                    result = makeError(Status::TimedOut, "control I/O timed out");
                }
                continue;
            }
            const int rc = io->rc;
            lock.unlock();
            if (rc != 0) {
                if (result.ok()) {
                    result = makeError(Status::IoError,
                                       "control I/O failed rc=" + std::to_string(rc));
                }
            } else {
                /* Metadata/journal writes also need flush coverage. */
                noteDataWriteComplete(index);
            }
        }
        return result;
    }

    static constexpr std::size_t kMaxControlBatchWrites = 8;
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

    const NumaTopologySnapshot topology = snapshotNumaTopology(config);
    log::info("numa topology snapshot: %s", topology.summary().c_str());
    if (config.numa.policy == "local-only") {
        if (!topology.available) {
            return makeError(Status::InvalidState,
                             "local-only NUMA policy requires readable sysfs topology");
        }
        for (const auto &device : topology.devices) {
            if (device.node < 0 ||
                std::find(topology.onlineNodes.begin(), topology.onlineNodes.end(),
                          static_cast<std::uint32_t>(device.node)) == topology.onlineNodes.end()) {
                return makeError(Status::InvalidState,
                                 "local-only NUMA policy cannot resolve device '" +
                                     device.id + "' NUMA node");
            }
        }
    }
    if (config.numa.deviceNuma != "auto") {
        auto requested = parseNumaIdList(config.numa.deviceNuma);
        if (!requested.ok() || requested.value().size() != 1 || !topology.available) {
            return makeError(Status::InvalidState,
                             "explicit device_numa requires a readable NUMA topology");
        }
        const std::uint32_t requestedNode = requested.value().front();
        for (const auto &device : topology.devices) {
            if (device.node != static_cast<std::int32_t>(requestedNode)) {
                return makeError(Status::InvalidState,
                                 "device '" + device.id + "' is not on requested NUMA node");
            }
        }
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
    if (impl.dataWrittenSeq.size() != impl.devices.size()) {
        impl.dataWrittenSeq = std::vector<std::atomic<std::uint64_t>>(impl.devices.size());
        impl.dataFlushedSeq = std::vector<std::atomic<std::uint64_t>>(impl.devices.size());
    }
    for (std::size_t i = 0; i < impl.devices.size(); ++i) {
        impl.dataWrittenSeq[i].store(0, std::memory_order_relaxed);
        impl.dataFlushedSeq[i].store(0, std::memory_order_relaxed);
    }
    impl.reactorCount.store(dataReactors, std::memory_order_release);
    impl.startDone = false;
    impl.startResult = 0;
    impl.readyCount = 0;
    impl.stoppingFlag.store(false, std::memory_order_release);

    impl.reactors.clear();
    impl.reactors.push_back(std::make_shared<Impl::Reactor>());
    for (std::uint32_t i = 0; i < dataReactors; ++i) {
        auto reactor = std::make_shared<Impl::Reactor>();
        reactor->ingressSlot = i;
        impl.reactors.push_back(std::move(reactor));
    }
    impl.configureReactorPlacement(topology, dataReactors);
    /* Synchronous ingress, completion holders and the doorbell table must be
     * ready before the data reactors start polling. */
    impl.initializeDataIngress(dataReactors);
    impl.initializeDataHolderPools(dataReactors);
    impl.wakeEntryCount_ = dataReactors;
    impl.wakeEntries_ = std::make_unique<std::atomic<Impl::Reactor *>[]>(dataReactors);
    impl.enableWakeTable();

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
        if (!impl.activeFlag.load(std::memory_order_acquire) ||
            impl.stoppingFlag.load(std::memory_order_acquire)) {
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
    (void)impl.wakeDataReactor(index);
    return Result<void>(Status::Ok);
}

Result<void> SpdkRuntime::wakeDataReactor(std::uint32_t index) noexcept {
    if (impl_ == nullptr) {
        return makeError(Status::InvalidState, "SPDK runtime is unavailable");
    }
    return impl_->wakeDataReactor(index);
}

std::uint64_t SpdkRuntime::dataReactorThreadId(std::uint32_t index) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index + 1 >= impl_->reactors.size()) {
        return 0;
    }
    return impl_->reactors[index + 1]->osTid.load(std::memory_order_acquire);
}

std::uint32_t SpdkRuntime::dataReactorForDevice(std::size_t deviceIndex) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (deviceIndex >= impl_->deviceReactors.size()) {
        return 0;
    }
    return impl_->deviceReactors[deviceIndex];
}

std::int32_t SpdkRuntime::dataReactorNumaNode(std::uint32_t index) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index + 1 >= impl_->reactors.size() || impl_->reactors[index + 1] == nullptr) {
        return -1;
    }
    return impl_->reactors[index + 1]->numaNode;
}

Result<void> SpdkRuntime::setDataReactorPoller(std::uint32_t index,
                                               DataReactorPoller callback,
                                               void *context) {
    if (callback == nullptr) {
        return makeError(Status::InvalidArgument, "data reactor poller callback is null");
    }
    auto &impl = *impl_;
    std::shared_ptr<Impl::Reactor> reactor;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.activeFlag.load(std::memory_order_acquire) ||
            impl.stoppingFlag.load(std::memory_order_acquire)) {
            return makeError(Status::Busy, "SPDK runtime is not active");
        }
        if (index + 1 >= impl.reactors.size() ||
            impl.reactors[index + 1]->spdk == nullptr) {
            return makeError(Status::InvalidArgument, "data reactor index out of range");
        }
        reactor = impl.reactors[index + 1];
    }

    std::unique_lock<std::mutex> lock(reactor->pollerMutex);
    if (reactor->pollerStopping) {
        return makeError(Status::Busy, "data reactor poller is stopping");
    }
    /* Each data reactor has one engine-owned actor pump. Replacing a live
     * registration would orphan its caller-owned context during shutdown. */
    if (reactor->poller != nullptr) {
        if (reactor->poller == callback && reactor->pollerContext == context) {
            return Result<void>(Status::Ok);
        }
        return makeError(Status::Busy, "data reactor poller is already registered");
    }
    if ((reactor->pollerState.load(std::memory_order_acquire) & kPollerReaderMask) != 0 &&
        spdk_get_thread() != reactor->spdk) {
        for (;;) {
            const std::uint64_t state =
                reactor->pollerState.load(std::memory_order_acquire);
            if ((state & kPollerReaderMask) == 0 || reactor->pollerStopping) {
                break;
            }
            reactor->pollerState.wait(state, std::memory_order_acquire);
        }
        if (reactor->pollerStopping) {
            return makeError(Status::Busy, "data reactor poller is stopping");
        }
    }
    reactor->poller = callback;
    reactor->pollerContext = context;
    reactor->pollerState.fetch_or(kPollerEnabledBit, std::memory_order_release);
    return Result<void>(Status::Ok);
}

Result<void> SpdkRuntime::clearDataReactorPoller(std::uint32_t index, void *context) {
    auto &impl = *impl_;
    std::shared_ptr<Impl::Reactor> reactor;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (index + 1 >= impl.reactors.size()) {
            return makeError(Status::InvalidArgument, "data reactor index out of range");
        }
        reactor = impl.reactors[index + 1];
    }

    std::unique_lock<std::mutex> lock(reactor->pollerMutex);
    if (reactor->poller == nullptr) {
        return Result<void>(Status::Ok);
    }
    if (reactor->pollerContext != context) {
        return makeError(Status::InvalidState, "data reactor poller context does not match");
    }
    const std::uint64_t previousState = reactor->pollerState.fetch_and(
        kPollerReaderMask, std::memory_order_acq_rel);
    if ((previousState & kPollerReaderMask) != 0 &&
        spdk_get_thread() != reactor->spdk) {
        for (;;) {
            const std::uint64_t state =
                reactor->pollerState.load(std::memory_order_acquire);
            if ((state & kPollerReaderMask) == 0) {
                break;
            }
            reactor->pollerState.wait(state, std::memory_order_acquire);
        }
    }
    reactor->poller = nullptr;
    reactor->pollerContext = nullptr;
    return Result<void>(Status::Ok);
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

std::uint64_t SpdkRuntime::deviceCapacityBytes(std::size_t index) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index >= impl_->devices.size()) {
        return 0;
    }
    return impl_->devices[index].numBlocks * impl_->devices[index].blockSize;
}

std::uint32_t SpdkRuntime::deviceBlockSize(std::size_t index) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index >= impl_->devices.size()) {
        return 0;
    }
    return impl_->devices[index].blockSize;
}

Result<void> SpdkRuntime::controlRead(std::size_t index, std::uint64_t offsetBlocks,
                                      std::span<std::byte> buffer) {
    return impl_->controlIo(ControlIo::Op::Read, index, offsetBlocks, buffer, {});
}

Result<void> SpdkRuntime::controlWrite(std::size_t index, std::uint64_t offsetBlocks,
                                       std::span<const std::byte> buffer) {
    return impl_->controlIo(ControlIo::Op::Write, index, offsetBlocks, {}, buffer);
}

Result<void> SpdkRuntime::controlWriteBatch(std::size_t index,
                                            std::span<const ControlWriteRange> writes) {
    return impl_->controlWriteBatch(index, writes);
}

Result<void> SpdkRuntime::controlFlush(std::size_t index) {
    return impl_->controlIo(ControlIo::Op::Flush, index, 0, {}, {});
}

bool SpdkRuntime::dataFlushPending() const noexcept {
    return impl_->dataFlushPending();
}

Result<void> SpdkRuntime::submitDataRead(std::uint32_t reactorIndex,
                                         std::size_t deviceIndex, void *buffer,
                                         std::uint64_t offsetBlocks, std::uint64_t blocks,
                                         void (*cb)(void *, int), void *cbCtx) {
    if (impl_ == nullptr) {
        return makeError(Status::InvalidState, "SPDK runtime is unavailable");
    }
    return impl_->submitSyncDataIo(reactorIndex, deviceIndex, buffer, offsetBlocks, blocks,
                                   false, cb, cbCtx);
}

Result<void> SpdkRuntime::submitDataWrite(std::uint32_t reactorIndex,
                                          std::size_t deviceIndex, const void *buffer,
                                          std::uint64_t offsetBlocks, std::uint64_t blocks,
                                          void (*cb)(void *, int), void *cbCtx) {
    if (impl_ == nullptr) {
        return makeError(Status::InvalidState, "SPDK runtime is unavailable");
    }
    return impl_->submitSyncDataIo(reactorIndex, deviceIndex,
                                   const_cast<void *>(buffer), offsetBlocks, blocks, true,
                                   cb, cbCtx);
}

Result<void> SpdkRuntime::submitDataReadOnReactor(std::uint32_t reactorIndex,
                                                 std::size_t deviceIndex,
                                                 DataIoRequest &request, void *buffer,
                                                 std::uint64_t offsetBlocks,
                                                 std::uint64_t blocks, DataIoCompletion cb,
                                                 void *cbCtx) {
    return impl_->submitDataIoOnReactor(reactorIndex, deviceIndex, request, buffer,
                                        offsetBlocks, blocks, false, cb, cbCtx);
}

Result<void> SpdkRuntime::submitDataWriteOnReactor(std::uint32_t reactorIndex,
                                                  std::size_t deviceIndex,
                                                  DataIoRequest &request, const void *buffer,
                                                  std::uint64_t offsetBlocks,
                                                  std::uint64_t blocks, DataIoCompletion cb,
                                                  void *cbCtx) {
    return impl_->submitDataIoOnReactor(reactorIndex, deviceIndex, request,
                                        const_cast<void *>(buffer), offsetBlocks, blocks, true,
                                        cb, cbCtx);
}

bool SpdkRuntime::isDmaCapable(const void *buffer, std::size_t bytes) const noexcept {
    if (buffer == nullptr || bytes == 0) {
        return false;
    }
    /* SPDK builds PRP lists from the payload address and rejects anything that
     * is not dword aligned; classify those spans as bounce-only instead of
     * letting the device submission fail. */
    if ((reinterpret_cast<std::uintptr_t>(buffer) & 0x3U) != 0) {
        return false;
    }
    std::uint64_t size = bytes;
    const std::uint64_t physical = spdk_vtophys(buffer, &size);
    return physical != SPDK_VTOPHYS_ERROR && size >= bytes;
}

#else /* !UUSE_WITH_SPDK */

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

Result<void> SpdkRuntime::wakeDataReactor(std::uint32_t index) noexcept {
    (void)index;
    return Result<void>(Status::Ok);
}
std::uint64_t SpdkRuntime::dataReactorThreadId(std::uint32_t index) const {
    (void)index;
    return 0;
}
std::uint32_t SpdkRuntime::dataReactorForDevice(std::size_t index) const noexcept {
    (void)index;
    return 0;
}
std::int32_t SpdkRuntime::dataReactorNumaNode(std::uint32_t index) const noexcept {
    (void)index;
    return -1;
}
Result<void> SpdkRuntime::setDataReactorPoller(std::uint32_t index,
                                               DataReactorPoller callback,
                                               void *context) {
    (void)index;
    (void)callback;
    (void)context;
    return makeError(Status::NotSupported, "core built without SPDK support");
}
Result<void> SpdkRuntime::clearDataReactorPoller(std::uint32_t index, void *context) {
    (void)index;
    (void)context;
    return makeError(Status::NotSupported, "core built without SPDK support");
}
std::size_t SpdkRuntime::channelCount() const noexcept { return 0; }
std::int64_t SpdkRuntime::ownerPid() const noexcept { return 0; }

std::uint64_t SpdkRuntime::deviceCapacityBytes(std::size_t index) const noexcept {
    (void)index;
    return 0;
}
std::uint32_t SpdkRuntime::deviceBlockSize(std::size_t index) const noexcept {
    (void)index;
    return 0;
}
Result<void> SpdkRuntime::controlRead(std::size_t index, std::uint64_t offsetBlocks,
                                      std::span<std::byte> buffer) {
    (void)index;
    (void)offsetBlocks;
    (void)buffer;
    return makeError(Status::NotSupported, "core built without SPDK support");
}
Result<void> SpdkRuntime::controlWrite(std::size_t index, std::uint64_t offsetBlocks,
                                       std::span<const std::byte> buffer) {
    (void)index;
    (void)offsetBlocks;
    (void)buffer;
    return makeError(Status::NotSupported, "core built without SPDK support");
}
Result<void> SpdkRuntime::controlWriteBatch(std::size_t index,
                                            std::span<const ControlWriteRange> writes) {
    (void)index;
    (void)writes;
    return makeError(Status::NotSupported, "core built without SPDK support");
}

Result<void> SpdkRuntime::controlFlush(std::size_t index) {
    (void)index;
    return makeError(Status::NotSupported, "core built without SPDK support");
}

bool SpdkRuntime::dataFlushPending() const noexcept { return false; }

Result<void> SpdkRuntime::submitDataRead(std::uint32_t reactorIndex,
                                         std::size_t deviceIndex, void *buffer,
                                         std::uint64_t offsetBlocks, std::uint64_t blocks,
                                         void (*cb)(void *, int), void *cbCtx) {
    (void)reactorIndex;
    (void)deviceIndex;
    (void)buffer;
    (void)offsetBlocks;
    (void)blocks;
    (void)cb;
    (void)cbCtx;
    return makeError(Status::NotSupported, "core built without SPDK support");
}

Result<void> SpdkRuntime::submitDataWrite(std::uint32_t reactorIndex,
                                          std::size_t deviceIndex, const void *buffer,
                                          std::uint64_t offsetBlocks, std::uint64_t blocks,
                                          void (*cb)(void *, int), void *cbCtx) {
    (void)reactorIndex;
    (void)deviceIndex;
    (void)buffer;
    (void)offsetBlocks;
    (void)blocks;
    (void)cb;
    (void)cbCtx;
    return makeError(Status::NotSupported, "core built without SPDK support");
}

Result<void> SpdkRuntime::submitDataReadOnReactor(std::uint32_t reactorIndex,
                                                 std::size_t deviceIndex,
                                                 DataIoRequest &request, void *buffer,
                                                 std::uint64_t offsetBlocks,
                                                 std::uint64_t blocks, DataIoCompletion cb,
                                                 void *cbCtx) {
    (void)reactorIndex;
    (void)deviceIndex;
    (void)request;
    (void)buffer;
    (void)offsetBlocks;
    (void)blocks;
    (void)cb;
    (void)cbCtx;
    return makeError(Status::NotSupported, "core built without SPDK support");
}

Result<void> SpdkRuntime::submitDataWriteOnReactor(std::uint32_t reactorIndex,
                                                  std::size_t deviceIndex,
                                                  DataIoRequest &request, const void *buffer,
                                                  std::uint64_t offsetBlocks,
                                                  std::uint64_t blocks, DataIoCompletion cb,
                                                  void *cbCtx) {
    (void)reactorIndex;
    (void)deviceIndex;
    (void)request;
    (void)buffer;
    (void)offsetBlocks;
    (void)blocks;
    (void)cb;
    (void)cbCtx;
    return makeError(Status::NotSupported, "core built without SPDK support");
}

bool SpdkRuntime::isDmaCapable(const void *buffer, std::size_t bytes) const noexcept {
    (void)buffer;
    (void)bytes;
    return false;
}

#endif /* UUSE_WITH_SPDK */

} // namespace uuse
