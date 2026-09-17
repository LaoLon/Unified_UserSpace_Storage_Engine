/* HFS 2.0 fio external engine. The first adapter keeps fio's completion
 * contract synchronous while the core grows its reactor-backed polling path. */

#include <cerrno>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "fio.h"
#include "optgroup.h"

/* fio's C helpers define min/max macros that must not leak into C++ headers. */
#undef min
#undef max

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/file.hpp"

#if defined(HFS_WITH_SPDK)
#include <spdk/env.h>
#endif

namespace {

struct FioHfsOptions {
    void *padding = nullptr; /* fio requires the first option offset to be non-zero. */
    char *config = nullptr;
    unsigned int direct = 1;
    unsigned int preallocate = 1;
    unsigned int clientDepth = 32;
};

struct FioHfsFile {
    std::shared_ptr<hfs::File> file;
};

struct FioHfsPending {
    io_u *io = nullptr;
    std::shared_ptr<hfs::File> file;
};

struct FioHfsThread {
    std::shared_ptr<hfs::Engine> engine;
    std::shared_ptr<hfs::Client> client;
    std::size_t maxOutstanding = 32;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::unique_ptr<FioHfsPending>> pending;
    std::deque<FioHfsPending *> workQueue;
    std::vector<std::thread> workers;
    bool stopping = false;
    std::vector<io_u *> completed;
    std::vector<io_u *> ready;
};

/* Runs a bounded worker for one FIO thread instead of creating one thread per I/O. */
void fioHfsWorker(FioHfsThread *state);

/* FIO numjobs share one process lock while retaining one Client per worker. */
std::mutex processMutex;
std::shared_ptr<hfs::Engine> processEngine;
std::size_t processUsers = 0;
std::string processConfigPath;

/* Preserve the parsed engine configuration for fio worker clones. */
int fioHfsConfigOption(void *, const char *value) {
    if (value == nullptr || value[0] == '\0') {
        return 1;
    }
    std::lock_guard<std::mutex> lock(processMutex);
    processConfigPath = value;
    return 0;
}

int fioError(hfs::Status status) {
    switch (status) {
    case hfs::Status::Busy:
    case hfs::Status::TryAgain:
        return EAGAIN;
    case hfs::Status::NoMemory:
        return ENOMEM;
    case hfs::Status::NoSpace:
        return ENOSPC;
    case hfs::Status::NotFound:
        return ENOENT;
    case hfs::Status::AlreadyExists:
        return EEXIST;
    case hfs::Status::InvalidArgument:
        return EINVAL;
    case hfs::Status::NotSupported:
        return ENOTSUP;
    case hfs::Status::Cancelled:
        return ECANCELED;
    case hfs::Status::TimedOut:
        return ETIMEDOUT;
    case hfs::Status::IoError:
        return EIO;
    default:
        return EIO;
    }
}

/* Keep diagnostics inside the module so C++ name mangling cannot affect fio symbols. */
void fioLog(const char *message) {
    std::fputs(message, stderr);
}

/* Allocate fio's complete requested buffer from the SPDK DMA heap. */
int fioHfsIomemAlloc(struct thread_data *td, size_t totalMem) {
    if (td == nullptr || totalMem == 0) {
        if (td != nullptr) td->orig_buffer = nullptr;
        return totalMem == 0 ? 0 : 1;
    }
#if defined(HFS_WITH_SPDK)
    td->orig_buffer = static_cast<char *>(spdk_dma_zmalloc(totalMem, 4096, nullptr));
    if (td->orig_buffer == nullptr) {
        fioLog("fio-hfs: complete DMA arena allocation failed\n");
        return 1;
    }
    td->orig_buffer_size = totalMem;
    return 0;
#else
    (void)totalMem;
    td->orig_buffer = nullptr;
    fioLog("fio-hfs: DMA arena requires an SPDK-enabled build\n");
    return 1;
#endif
}

/* Release only the single arena owned by fio for this worker. */
void fioHfsIomemFree(struct thread_data *td) {
    if (td == nullptr || td->orig_buffer == nullptr) {
        return;
    }
#if defined(HFS_WITH_SPDK)
    spdk_dma_free(td->orig_buffer);
#endif
    td->orig_buffer = nullptr;
    td->orig_buffer_size = 0;
}

std::string fioPath(const char *name) {
    if (name == nullptr || name[0] == '\0') {
        return {};
    }
    std::string path(name);
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return path;
}

/* Keep engine options statically initialized like fio's external skeleton. */
static struct fio_option options[5] = {
    {
        .name = "hfs_config",
        .lname = "HFS .conf path",
        .type = FIO_OPT_STR_STORE,
        .off1 = offsetof(FioHfsOptions, config),
        .cb = reinterpret_cast<void *>(fioHfsConfigOption),
        .help = "HFS 2.0 .conf configuration path",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "hfs_direct",
        .lname = "HFS direct mode",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(FioHfsOptions, direct),
        .help = "Use HFS direct data path",
        .def = "1",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "hfs_preallocate",
        .lname = "HFS preallocation",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(FioHfsOptions, preallocate),
        .help = "Allocate HFS extents during open_file",
        .def = "1",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "hfs_client_depth",
        .lname = "HFS client depth",
        .type = FIO_OPT_INT,
        .off1 = offsetof(FioHfsOptions, clientDepth),
        .help = "HFS client queue depth",
        .def = "32",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = nullptr,
    },
};

int fioHfsInit(struct thread_data *td) {
    auto *options = static_cast<FioHfsOptions *>(td->eo);
    const char *configPath = options == nullptr ? nullptr : options->config;
    std::string callbackConfig;
    if (configPath == nullptr || configPath[0] == '\0') {
        std::lock_guard<std::mutex> lock(processMutex);
        callbackConfig = processConfigPath;
        configPath = callbackConfig.c_str();
    }
    if (configPath == nullptr || configPath[0] == '\0') {
        configPath = std::getenv("HFS_CONFIG");
    }
    if (configPath == nullptr || configPath[0] == '\0') {
        fioLog("fio-hfs: hfs_config is required\n");
        return 1;
    }
    auto config = hfs::parseConfigFile(configPath);
    if (!config.ok()) {
        fioLog("fio-hfs: config parse failed\n");
        return 1;
    }
    std::shared_ptr<hfs::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(processMutex);
        if (processEngine == nullptr) {
            auto created = hfs::Engine::create(config.value());
            if (!created.ok()) {
                fioLog("fio-hfs: engine create failed\n");
                return 1;
            }
            engine = std::shared_ptr<hfs::Engine>(std::move(created).value());
            auto mountedTask = engine->mount().wait();
            if (!mountedTask.ok() || !mountedTask.value().ok()) {
                fioLog("fio-hfs: engine mount failed\n");
                return 1;
            }
            processEngine = engine;
        } else {
            engine = processEngine;
        }
        ++processUsers;
    }
    const std::uint32_t clientDepth = options == nullptr ? 32 : options->clientDepth;
    auto client = hfs::Client::create(*engine, hfs::ClientOptions{clientDepth});
    if (!client.ok()) {
        fioLog("fio-hfs: client create failed\n");
        std::lock_guard<std::mutex> lock(processMutex);
        if (processUsers != 0) {
            --processUsers;
        }
        if (processUsers == 0) {
            processEngine->stop().wait();
            processEngine.reset();
        }
        return 1;
    }
    auto *state = new FioHfsThread;
    state->engine = std::move(engine);
    state->client = std::move(client).value();
    state->maxOutstanding = clientDepth == 0 ? 1 : clientDepth;
    try {
        /* Keep a small fixed pool per fio worker to bound host-thread fanout. */
        const std::size_t workerCount = std::min<std::size_t>(16, state->maxOutstanding);
        state->workers.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            state->workers.emplace_back(fioHfsWorker, state);
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->stopping = true;
        }
        state->condition.notify_all();
        for (auto &worker : state->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        state->client->close().wait();
        state->client.reset();
        {
            std::lock_guard<std::mutex> lock(processMutex);
            if (processUsers != 0) {
                --processUsers;
            }
            if (processUsers == 0 && processEngine != nullptr) {
                processEngine->stop().wait();
                processEngine.reset();
            }
        }
        delete state;
        fioLog("fio-hfs: worker pool create failed\n");
        return 1;
    }
    td->io_ops_data = state;
    return 0;
}

void fioHfsCleanup(struct thread_data *td) {
    auto *state = static_cast<FioHfsThread *>(td->io_ops_data);
    if (state == nullptr) {
        return;
    }
    /* Stop and join the bounded worker pool before releasing the engine. */
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stopping = true;
        state->ready.clear();
        state->completed.clear();
    }
    state->condition.notify_all();
    for (auto &worker : state->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    state->workers.clear();
    state->pending.clear();
    state->workQueue.clear();
    if (state->client != nullptr) {
        state->client->close().wait();
    }
    state->client.reset();
    {
        std::lock_guard<std::mutex> lock(processMutex);
        if (processUsers != 0) {
            --processUsers;
        }
        if (processUsers == 0 && processEngine != nullptr) {
            processEngine->stop().wait();
            processEngine.reset();
        }
    }
    delete state;
    td->io_ops_data = nullptr;
}

int fioHfsOpen(struct thread_data *td, struct fio_file *fioFile) {
    auto *state = static_cast<FioHfsThread *>(td->io_ops_data);
    if (state == nullptr || state->client == nullptr) {
        return EINVAL;
    }
    auto opened = state->client->open(fioPath(fioFile->file_name), hfs::OpenFlags::Create).wait();
    if (!opened.ok()) {
        return fioError(opened.status());
    }
    auto *file = new FioHfsFile;
    auto openedResult = std::move(opened).value();
    if (!openedResult.ok()) {
        delete file;
        return fioError(openedResult.status());
    }
    file->file = std::move(openedResult).value();
    auto stat = file->file->stat();
    if (!stat.ok()) {
        delete file;
        return fioError(stat.status());
    }
    auto *options = static_cast<FioHfsOptions *>(td->eo);
    const std::uint64_t requestedSize = static_cast<std::uint64_t>(td->o.size);
    if ((options == nullptr || options->preallocate != 0) && requestedSize != 0) {
        auto allocated = file->file->allocate(0, requestedSize).wait();
        if (!allocated.ok() || !allocated.value().ok()) {
            delete file;
            return allocated.ok() ? fioError(allocated.value().status()) : EIO;
        }
        stat = file->file->stat();
        if (!stat.ok()) {
            delete file;
            return fioError(stat.status());
        }
    }
    fioFile->engine_data = file;
    fioFile->real_file_size = stat.value().logicalSize;
    fio_file_set_size_known(fioFile);
    return 0;
}

int fioHfsClose(struct thread_data *, struct fio_file *fioFile) {
    auto *file = static_cast<FioHfsFile *>(fioFile->engine_data);
    delete file;
    fioFile->engine_data = nullptr;
    return 0;
}

/* HFS owns cache coherency; fio has no host file descriptor to invalidate. */
int fioHfsInvalidate(struct thread_data *, struct fio_file *) { return 0; }

/* Fio needs a logical extent before it schedules the first request. */
int fioHfsGetFileSize(struct thread_data *td, struct fio_file *fioFile) {
    const std::uint64_t size = static_cast<std::uint64_t>(td->o.size);
    fioFile->real_file_size = size == 0 ? 4096 : size;
    return 0;
}

/* Executes one FIO request without crossing the caller's queue callback. */
void executeFioIo(io_u *io, const std::shared_ptr<hfs::File> &file) {
    try {
        hfs::IoVec buffer{static_cast<std::byte *>(io->xfer_buf), io->xfer_buflen};
        if (io->ddir == DDIR_READ) {
            auto taskResult = file->read(std::span<hfs::IoVec>(&buffer, 1), io->offset).wait();
            if (!taskResult.ok() || !taskResult.value().ok()) {
                io->error = taskResult.ok() ? fioError(taskResult.value().status()) : EIO;
                io->resid = io->xfer_buflen;
            } else {
                io->error = 0;
                io->resid = io->xfer_buflen - taskResult.value().value().bytes;
            }
        } else if (io->ddir == DDIR_WRITE) {
            auto taskResult =
                file->write(std::span<const hfs::IoVec>(&buffer, 1), io->offset).wait();
            if (!taskResult.ok() || !taskResult.value().ok()) {
                io->error = taskResult.ok() ? fioError(taskResult.value().status()) : EIO;
                io->resid = io->xfer_buflen;
            } else {
                io->error = 0;
                io->resid = io->xfer_buflen - taskResult.value().value().bytes;
            }
        } else {
            io->error = EOPNOTSUPP;
            io->resid = io->xfer_buflen;
        }
    } catch (...) {
        io->error = EIO;
        io->resid = io->xfer_buflen;
    }
}

/* Drains queued requests using the bounded pool and publishes completions. */
void fioHfsWorker(FioHfsThread *state) {
    while (true) {
        FioHfsPending *request = nullptr;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->condition.wait(lock, [state] {
                return state->stopping || !state->workQueue.empty();
            });
            if (state->stopping && state->workQueue.empty()) {
                return;
            }
            request = state->workQueue.front();
            state->workQueue.pop_front();
        }
        executeFioIo(request->io, request->file);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->completed.push_back(request->io);
        }
        state->condition.notify_all();
    }
}

enum fio_q_status fioHfsQueue(struct thread_data *td, struct io_u *io) {
    auto *state = static_cast<FioHfsThread *>(td->io_ops_data);
    auto *file = io->file == nullptr ? nullptr
                                     : static_cast<FioHfsFile *>(io->file->engine_data);
    if (state == nullptr || file == nullptr || file->file == nullptr) {
        io->error = EINVAL;
        return FIO_Q_COMPLETED;
    }
    auto request = std::make_unique<FioHfsPending>();
    request->io = io;
    request->file = file->file;
    auto *requestPtr = request.get();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->pending.size() >= state->maxOutstanding) {
            return FIO_Q_BUSY;
        }
        state->pending.push_back(std::move(request));
        state->workQueue.push_back(requestPtr);
    }
    state->condition.notify_one();
    return FIO_Q_QUEUED;
}

int fioHfsGetevents(struct thread_data *td, unsigned int min, unsigned int max,
                    const struct timespec *) {
    auto *state = static_cast<FioHfsThread *>(td->io_ops_data);
    if (state == nullptr || max == 0) {
        return 0;
    }
    std::vector<std::unique_ptr<FioHfsPending>> finished;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->condition.wait(lock, [state, min] {
            return state->completed.size() >= min;
        });
        const unsigned int take = static_cast<unsigned int>(
            std::min<std::size_t>(max, state->completed.size()));
        state->ready.assign(state->completed.begin(), state->completed.begin() + take);
        state->completed.erase(state->completed.begin(), state->completed.begin() + take);
        for (unsigned int index = 0; index < take; ++index) {
            auto found = std::find_if(
                state->pending.begin(), state->pending.end(),
                [io = state->ready[index]](const auto &entry) { return entry->io == io; });
            if (found != state->pending.end()) {
                finished.push_back(std::move(*found));
                state->pending.erase(found);
            }
        }
    }
    return static_cast<int>(state->ready.size());
}

struct io_u *fioHfsEvent(struct thread_data *td, int event) {
    auto *state = static_cast<FioHfsThread *>(td->io_ops_data);
    if (state == nullptr || event < 0 || static_cast<std::size_t>(event) >= state->ready.size()) {
        return nullptr;
    }
    return state->ready[static_cast<std::size_t>(event)];
}

} // namespace

extern "C" struct ioengine_ops ioengine = {
    .name = "hfs",
    .version = FIO_IOOPS_VERSION,
    .flags = FIO_NOEXTEND | FIO_MEMALIGN,
    .init = fioHfsInit,
    .queue = fioHfsQueue,
    .getevents = fioHfsGetevents,
    .event = fioHfsEvent,
    .cleanup = fioHfsCleanup,
    .open_file = fioHfsOpen,
    .close_file = fioHfsClose,
    .invalidate = fioHfsInvalidate,
    .get_file_size = fioHfsGetFileSize,
    .iomem_alloc = fioHfsIomemAlloc,
    .iomem_free = fioHfsIomemFree,
    .option_struct_size = sizeof(FioHfsOptions),
    .options = options,
};
