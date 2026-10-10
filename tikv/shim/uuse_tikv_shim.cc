/* C ABI shim over the UUSE core for TiKV frontends (Shard-116).
 *
 * v1 uses one shared path-op Client serialized by a mutex; file data
 * operations go through uuse::File coroutines. Per-thread clients and the
 * worker-direct path are Shard-118 work. The shared client is created and
 * closed on a dedicated frontend thread owned by the engine state, because the
 * core requires close() to run on the client's frontend thread while the last
 * handle can be dropped by any thread (Shard-118 lifecycle test case 2).
 *
 * Lifecycle: the engine state is reference counted. File handles hold a
 * reference, so dropping the frontend filesystem before its readers/writers
 * (raft-engine does this) cannot stop the engine while files are still open;
 * the engine closes with the last handle.
 *
 * Note: Task<Result<T>>::wait() yields a nested Result<Result<T>> (task level
 * + operation level); every call unwraps both layers. */

#include "uuse_tikv_shim.h"
#include "uuse_tikv_shim.hpp"

#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "uuse/config.hpp"
#include "uuse/core.hpp"
#include "uuse/file.hpp"
#include "uuse/types.hpp"

namespace {

/* Maps a core status to a negative errno-style code for the C ABI. */
int toErrno(uuse::Status status) {
    switch (status) {
    case uuse::Status::Ok: return 0;
    case uuse::Status::InvalidArgument: return -EINVAL;
    case uuse::Status::NotFound: return -ENOENT;
    case uuse::Status::AlreadyExists: return -EEXIST;
    case uuse::Status::NoSpace: return -ENOSPC;
    case uuse::Status::NoMemory: return -ENOMEM;
    case uuse::Status::Busy: return -EBUSY;
    case uuse::Status::TryAgain: return -EAGAIN;
    case uuse::Status::IoError: return -EIO;
    case uuse::Status::Cancelled: return -ECANCELED;
    case uuse::Status::TimedOut: return -ETIMEDOUT;
    case uuse::Status::ForkedProcess: return -ECHILD;
    case uuse::Status::Corrupt: return -EIO;
    case uuse::Status::NotSupported: return -ENOTSUP;
    case uuse::Status::InvalidState: return -EINVAL;
    }
    return -EIO;
}

/* Unwraps a nested Result<Result<T>> produced by Task<T>::wait(). */
template <typename T>
int unwrap(uuse::Result<uuse::Result<T>> &&outer, T *out) {
    if (!outer.ok()) return toErrno(outer.status());
    auto inner = std::move(outer).value();
    if (!inner.ok()) return toErrno(inner.status());
    if (out != nullptr) *out = std::move(inner).value();
    return 0;
}

/* Void specialization. */
int unwrap(uuse::Result<uuse::Result<void>> &&outer) {
    if (!outer.ok()) return toErrno(outer.status());
    auto inner = std::move(outer).value();
    return inner.ok() ? 0 : toErrno(inner.status());
}

uuse::OpenFlags toOpenFlags(unsigned flags) {
    uuse::OpenFlags out = uuse::OpenFlags::None;
    if (flags & UUSE_TIKV_OPEN_CREATE) out = out | uuse::OpenFlags::Create;
    if (flags & UUSE_TIKV_OPEN_TRUNCATE) out = out | uuse::OpenFlags::Truncate;
    if (flags & UUSE_TIKV_OPEN_EXCLUSIVE) out = out | uuse::OpenFlags::Exclusive;
    return out;
}

} // namespace

/* Reference-counted engine state shared by the engine handle and file
 * handles; the engine stops when the last reference goes away.  The shared
 * client is created and closed on a dedicated frontend thread owned by this
 * state (core contract: close() must run on the client's frontend thread),
 * while path ops from callers are serialized by `mutex`. */
struct UuseEngineState {
    std::shared_ptr<uuse::Engine> engine;
    std::shared_ptr<uuse::Client> client;
    std::mutex mutex; /* serializes the shared path-op client */

    std::thread clientThread; /* frontend thread of `client` */
    std::mutex closeMutex;
    std::condition_variable closeCv;
    bool closeRequested = false;

    /* Starts the client thread and creates the shared client on it.  Returns 0
     * or a negative errno. */
    int startClient() {
        std::mutex readyMutex;
        std::condition_variable readyCv;
        bool ready = false;
        int rc = 0;
        clientThread = std::thread([this, &readyMutex, &readyCv, &ready, &rc]() {
            auto created = uuse::Client::create(*engine);
            if (created.ok()) {
                client = std::move(created).value();
            } else {
                rc = toErrno(created.status());
            }
            {
                std::lock_guard<std::mutex> lock(readyMutex);
                ready = true;
                /* Notify under the lock: the opener destroys the stack condvar
                 * right after wait() returns. */
                readyCv.notify_all();
            }
            /* Stay alive so the client can be closed on its own thread. */
            std::unique_lock<std::mutex> lock(closeMutex);
            closeCv.wait(lock, [this] { return closeRequested; });
            if (client) {
                (void)unwrap(client->close().wait());
                client.reset();
            }
        });
        std::unique_lock<std::mutex> lock(readyMutex);
        readyCv.wait(lock, [&ready] { return ready; });
        return rc;
    }

    ~UuseEngineState() {
        {
            std::lock_guard<std::mutex> lock(closeMutex);
            closeRequested = true;
        }
        closeCv.notify_all();
        if (clientThread.joinable()) {
            clientThread.join();
        }
        if (engine) {
            (void)unwrap(engine->drain().wait());
            (void)unwrap(engine->stop().wait());
            engine.reset();
        }
    }
};

namespace {

/* Process-wide singleton per .conf: kvdb (RocksDB env) and raft-engine share
 * one UUSE engine. Entries are weak so a fully released engine can be
 * recreated later. */
std::mutex gEngineRegistryMutex;
std::unordered_map<std::string, std::weak_ptr<UuseEngineState>> gEngineRegistry;

} // namespace

struct uuse_tikv_engine {
    std::shared_ptr<UuseEngineState> state;
};

struct uuse_tikv_file {
    std::shared_ptr<UuseEngineState> state;
    std::shared_ptr<uuse::File> file;
};

namespace {

long long doReadWrite(uuse_tikv_file *file, const void *writeData, void *readBuf,
                      size_t len, uint64_t offset, bool isWrite) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    if (len == 0) return 0;
    auto *bytes = reinterpret_cast<std::byte *>(
        isWrite ? const_cast<void *>(writeData) : readBuf);
    if (isWrite) {
        uuse::IoVec vec{bytes, len};
        auto outer = file->file->write(std::span<const uuse::IoVec>(&vec, 1), offset).wait();
        uuse::WriteResult value{};
        const int rc = unwrap(std::move(outer), &value);
        return rc != 0 ? rc : static_cast<long long>(value.bytes);
    }
    uuse::IoVec vec{bytes, len};
    auto outer = file->file->read(std::span<uuse::IoVec>(&vec, 1), offset).wait();
    uuse::ReadResult value{};
    const int rc = unwrap(std::move(outer), &value);
    return rc != 0 ? rc : static_cast<long long>(value.bytes);
}

} // namespace

extern "C" {

int uuse_tikv_engine_open(const char *confPath, uuse_tikv_engine **out) {
    if (confPath == nullptr || out == nullptr) return -EINVAL;
    const std::string key(confPath);
    std::shared_ptr<UuseEngineState> state;
    {
        std::lock_guard<std::mutex> lock(gEngineRegistryMutex);
        auto it = gEngineRegistry.find(key);
        if (it != gEngineRegistry.end()) state = it->second.lock();
        if (state == nullptr) {
            auto config = uuse::parseConfigFile(confPath);
            if (!config.ok()) return toErrno(config.status());
            auto engine = uuse::Engine::create(config.value());
            if (!engine.ok()) return toErrno(engine.status());
            const int mountRc = unwrap(engine.value()->mount().wait());
            if (mountRc != 0) return mountRc;
            state = std::make_shared<UuseEngineState>();
            state->engine = std::shared_ptr<uuse::Engine>(std::move(engine).value());
            const int clientRc = state->startClient();
            if (clientRc != 0) return clientRc;
            gEngineRegistry[key] = state;
        }
    }
    auto *handle = new uuse_tikv_engine();
    handle->state = std::move(state);
    *out = handle;
    return 0;
}

void uuse_tikv_engine_close(uuse_tikv_engine *engine) {
    if (engine == nullptr) return;
    /* Releasing the owner reference is enough: open file handles keep the
     * state (and the engine) alive until they are closed. */
    engine->state.reset();
    delete engine;
}

int uuse_tikv_open(uuse_tikv_engine *engine, const char *path, unsigned flags,
                   uuse_tikv_file **out) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr || out == nullptr) {
        return -EINVAL;
    }
    auto state = engine->state;
    std::lock_guard<std::mutex> lock(state->mutex);
    std::shared_ptr<uuse::File> file;
    const int rc = unwrap(state->client->open(path, toOpenFlags(flags)).wait(), &file);
    if (rc != 0) return rc;
    auto *handle = new uuse_tikv_file();
    handle->state = std::move(state);
    handle->file = std::move(file);
    *out = handle;
    return 0;
}

void uuse_tikv_close(uuse_tikv_file *file) {
    if (file == nullptr) return;
    file->file.reset();
    file->state.reset();
    delete file;
}

int uuse_tikv_remove(uuse_tikv_engine *engine, const char *path) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr) return -EINVAL;
    std::lock_guard<std::mutex> lock(engine->state->mutex);
    return unwrap(engine->state->client->remove(path).wait());
}

int uuse_tikv_rename(uuse_tikv_engine *engine, const char *from, const char *to) {
    if (engine == nullptr || engine->state == nullptr || from == nullptr || to == nullptr) {
        return -EINVAL;
    }
    std::lock_guard<std::mutex> lock(engine->state->mutex);
    return unwrap(engine->state->client->rename(from, to).wait());
}

int uuse_tikv_create_dirs(uuse_tikv_engine *engine, const char *path) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr) return -EINVAL;
    std::lock_guard<std::mutex> lock(engine->state->mutex);
    return unwrap(engine->state->client->createDirectories(path).wait());
}

long long uuse_tikv_read_dir(uuse_tikv_engine *engine, const char *path,
                             char *buf, size_t bufLen) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr) return -EINVAL;
    std::lock_guard<std::mutex> lock(engine->state->mutex);
    std::vector<std::string> names;
    std::string cursor;
    for (;;) {
        auto outer = engine->state->client->getChildren(path, cursor, 256).wait();
        if (!outer.ok()) return toErrno(outer.status());
        auto inner = std::move(outer).value();
        if (!inner.ok()) return toErrno(inner.status());
        auto page = std::move(inner).value();
        for (const auto &entry : page.entries) {
            if (!entry.directory) names.push_back(entry.name);
        }
        if (page.nextCursor.empty()) break;
        cursor = page.nextCursor;
    }
    size_t needed = 0;
    for (const auto &name : names) needed += name.size() + 1;
    if (buf == nullptr || bufLen < needed) return static_cast<long long>(needed);
    size_t offset = 0;
    for (const auto &name : names) {
        std::memcpy(buf + offset, name.data(), name.size());
        offset += name.size();
        buf[offset++] = '\0';
    }
    return 0;
}

int uuse_tikv_exists(uuse_tikv_engine *engine, const char *path, int *exists) {
    if (engine == nullptr || engine->state == nullptr || path == nullptr || exists == nullptr) {
        return -EINVAL;
    }
    std::lock_guard<std::mutex> lock(engine->state->mutex);
    auto outer = engine->state->client->stat(path).wait();
    if (!outer.ok()) return toErrno(outer.status());
    auto inner = std::move(outer).value();
    if (inner.ok()) {
        *exists = 1;
        return 0;
    }
    if (inner.status() == uuse::Status::NotFound) {
        *exists = 0;
        return 0;
    }
    return toErrno(inner.status());
}

long long uuse_tikv_pread(uuse_tikv_file *file, void *buf, size_t len,
                          uint64_t offset) {
    return doReadWrite(file, nullptr, buf, len, offset, false);
}

long long uuse_tikv_pwrite(uuse_tikv_file *file, const void *buf, size_t len,
                           uint64_t offset) {
    return doReadWrite(file, buf, nullptr, len, offset, true);
}

long long uuse_tikv_append(uuse_tikv_file *file, const void *buf, size_t len) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    if (len == 0) return 0;
    uuse::IoVec vec{reinterpret_cast<std::byte *>(const_cast<void *>(buf)), len};
    auto outer = file->file->append(std::span<const uuse::IoVec>(&vec, 1)).wait();
    uuse::WriteResult value{};
    const int rc = unwrap(std::move(outer), &value);
    return rc != 0 ? rc : static_cast<long long>(value.bytes);
}

int uuse_tikv_truncate(uuse_tikv_file *file, uint64_t size) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    return unwrap(file->file->truncate(size).wait());
}

int uuse_tikv_allocate(uuse_tikv_file *file, uint64_t offset, uint64_t len) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    return unwrap(file->file->allocate(offset, len).wait());
}

int uuse_tikv_sync(uuse_tikv_file *file) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    return unwrap(file->file->sync().wait());
}

long long uuse_tikv_file_size(uuse_tikv_file *file) {
    if (file == nullptr || file->file == nullptr) return -EINVAL;
    auto stat = file->file->stat();
    if (!stat.ok()) return toErrno(stat.status());
    return static_cast<long long>(stat.value().logicalSize);
}

} // extern "C"

std::shared_ptr<uuse::Engine> uuse_tikv_engine_shared_engine(uuse_tikv_engine *engine) {
    return engine != nullptr && engine->state != nullptr ? engine->state->engine : nullptr;
}

std::shared_ptr<uuse::Client> uuse_tikv_engine_shared_client(uuse_tikv_engine *engine) {
    return engine != nullptr && engine->state != nullptr ? engine->state->client : nullptr;
}
