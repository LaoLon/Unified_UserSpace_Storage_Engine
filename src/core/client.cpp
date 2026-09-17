#include "hfs/core.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "common/log.hpp"
#include "core/data_plane.hpp"
#include "metadata/inode_map.hpp"
#include "metadata/namespace.hpp"

namespace hfs {

/* Counts as a metadata user for as long as a client (or any of its open
 * files) is alive, so Engine::stop() cannot tear down the namespace below
 * live handles (hfs2.0.md §14.3). */
struct MetadataLease {
    Engine *engine = nullptr;

    MetadataLease() = default;
    explicit MetadataLease(Engine &value) : engine(&value) { engine->retainMetadataUser(); }
    ~MetadataLease() {
        if (engine != nullptr) {
            engine->releaseMetadataUser();
        }
    }
    MetadataLease(const MetadataLease &) = delete;
    MetadataLease &operator=(const MetadataLease &) = delete;
};

namespace {

/* Shared client state so open File handles keep working after Client::close
 * (the engine must still outlive them). */
struct ClientState {
    Engine *engine = nullptr;
    metadata::Namespace *namespaceState = nullptr;
    DataPlane *dataPlane = nullptr;
    ClientOptions options;
    std::shared_ptr<MetadataLease> lease;
    std::atomic<std::uint32_t> openFiles{0};
    std::atomic<bool> closed{false};
};

/* Rebuilds the per-inode extent map view for one File handle. */
Result<metadata::InodeExtentMap> extentMapFor(const ClientState &state, FileHandle handle) {
    auto descriptors = state.namespaceState->extents(handle);
    if (!descriptors.ok()) {
        return makeError<metadata::InodeExtentMap>(descriptors.status(),
                                                   descriptors.message());
    }
    metadata::InodeExtentMap map;
    map.configure(state.namespaceState->extentBytes());
    auto restored = map.restore(descriptors.value());
    if (!restored.ok()) {
        return makeError<metadata::InodeExtentMap>(restored.status(), restored.message());
    }
    return map;
}

Result<void> validateClient(const ClientState &state) {
    if (state.engine == nullptr || !state.engine->processValid()) {
        return makeError(Status::ForkedProcess, "engine belongs to another process");
    }
    if (state.closed.load(std::memory_order_acquire)) {
        return makeError(Status::InvalidState, "client is closed");
    }
    if (!state.engine->mounted() || state.namespaceState == nullptr) {
        return makeError(Status::InvalidState, "engine is not mounted");
    }
    return Result<void>(Status::Ok);
}

} // namespace

struct Client::Impl {
    std::shared_ptr<ClientState> state;
};

struct File::Impl {
    std::shared_ptr<ClientState> state;
    FileHandle handle;

    ~Impl() {
        if (state != nullptr) {
            if (state->namespaceState != nullptr && handle.valid()) {
                state->namespaceState->close(handle);
            }
            state->openFiles.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
};

Client::Client(Engine &engine, const ClientOptions &options) : impl_(std::make_unique<Impl>()) {
    impl_->state = std::make_shared<ClientState>();
    impl_->state->engine = &engine;
    impl_->state->namespaceState = engine.metadataNamespace();
    impl_->state->dataPlane = engine.dataPlane();
    impl_->state->options = options;
    impl_->state->lease = std::make_shared<MetadataLease>(engine);
}

Client::~Client() = default;

Result<std::shared_ptr<Client>> Client::create(Engine &engine, const ClientOptions &options) {
    if (!engine.processValid()) {
        return makeError<std::shared_ptr<Client>>(Status::ForkedProcess,
                                                  "engine belongs to another process");
    }
    if (!engine.mounted() || engine.metadataNamespace() == nullptr) {
        return makeError<std::shared_ptr<Client>>(Status::InvalidState,
                                                  "engine is not mounted");
    }
    return std::shared_ptr<Client>(new Client(engine, options));
}

Task<Result<void>> Client::close() {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    state.closed.store(true, std::memory_order_release);
    log::info("client closed: open files=%u",
              state.openFiles.load(std::memory_order_relaxed));
    co_return Result<void>(Status::Ok);
}

Task<Result<std::shared_ptr<File>>> Client::open(std::string_view path, OpenFlags flags) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return makeError<std::shared_ptr<File>>(valid.status(), valid.message());
    }
    Result<FileHandle> handle =
        hasFlag(flags, OpenFlags::Create)
            ? state.namespaceState->create(path, hasFlag(flags, OpenFlags::Exclusive),
                                           hasFlag(flags, OpenFlags::Truncate),
                                           hasFlag(flags, OpenFlags::Directory))
            : state.namespaceState->open(path);
    if (!handle.ok()) {
        co_return makeError<std::shared_ptr<File>>(handle.status(), handle.message());
    }
    const bool wantDirectory = hasFlag(flags, OpenFlags::Directory);
    auto stat = state.namespaceState->stat(handle.value());
    if (!stat.ok() || stat.value().directory != wantDirectory) {
        state.namespaceState->close(handle.value());
        co_return makeError<std::shared_ptr<File>>(Status::InvalidArgument,
                                                   "open flags do not match the path type");
    }
    const std::uint32_t maxFiles = state.options.maxOpenFiles;
    if (maxFiles != 0 && state.openFiles.load(std::memory_order_relaxed) >= maxFiles) {
        state.namespaceState->close(handle.value());
        co_return makeError<std::shared_ptr<File>>(Status::NoMemory,
                                                   "client open file limit reached");
    }
    state.openFiles.fetch_add(1, std::memory_order_acq_rel);
    auto fileImpl = std::make_unique<File::Impl>();
    fileImpl->state = impl_->state;
    fileImpl->handle = handle.value();
    co_return std::shared_ptr<File>(new File(std::move(fileImpl)));
}

Task<Result<void>> Client::remove(std::string_view path) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return makeError(valid.status(), valid.message());
    }
    co_return state.namespaceState->remove(path);
}

Task<Result<void>> Client::rename(std::string_view from, std::string_view to) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return makeError(valid.status(), valid.message());
    }
    co_return state.namespaceState->rename(from, to);
}

/* Creates a namespace hard link while preserving one shared inode and extent map. */
Task<Result<void>> Client::link(std::string_view from, std::string_view to) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return Result<void>(valid.status(), valid.message());
    }
    co_return state.namespaceState->link(from, to);
}

Task<Result<DirectoryPage>> Client::getChildren(std::string_view path,
                                                std::string_view cursor,
                                                std::size_t maxEntries) {
    auto &state = *impl_->state;
    auto valid = validateClient(state);
    if (!valid.ok()) {
        co_return makeError<DirectoryPage>(valid.status(), valid.message());
    }
    co_return state.namespaceState->getChildren(path, cursor, maxEntries);
}

File::File(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

File::~File() = default;

FileHandle File::handle() const noexcept { return impl_->handle; }

Result<FileStat> File::stat() const {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        return makeError<FileStat>(Status::ForkedProcess,
                                   "engine belongs to another process");
    }
    if (state.namespaceState == nullptr) {
        return makeError<FileStat>(Status::InvalidState, "engine is not mounted");
    }
    return state.namespaceState->stat(impl_->handle);
}

Task<Result<ReadResult>> File::read(std::span<IoVec> buffers, std::uint64_t offset,
                                    std::stop_token stop) {
    (void)stop;
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<ReadResult>(Status::ForkedProcess,
                                     "engine belongs to another process");
    }
    if (state.namespaceState == nullptr || state.dataPlane == nullptr) {
        co_return Result<ReadResult>(Status::InvalidState, "engine is not mounted");
    }
    std::size_t total = 0;
    for (const IoVec &vec : buffers) {
        if (vec.size != 0 && vec.data == nullptr) {
            co_return makeError<ReadResult>(Status::InvalidArgument, "null iovec buffer");
        }
        total += vec.size;
    }
    if (total == 0) {
        co_return ReadResult{0};
    }
    auto stat = state.namespaceState->stat(impl_->handle);
    if (!stat.ok()) {
        co_return makeError<ReadResult>(stat.status(), stat.message());
    }
    const std::uint64_t size = stat.value().logicalSize;
    if (offset >= size) {
        co_return ReadResult{0};
    }
    const std::uint64_t length = std::min<std::uint64_t>(total, size - offset);
    auto map = extentMapFor(state, impl_->handle);
    if (!map.ok()) {
        co_return makeError<ReadResult>(map.status(), map.message());
    }
    std::uint64_t cursor = offset;
    std::uint64_t remaining = length;
    for (const IoVec &vec : buffers) {
        if (remaining == 0) {
            break;
        }
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(vec.size, remaining));
        auto read = state.dataPlane->read(map.value(),
                                          std::span<std::byte>(vec.data, take), cursor,
                                          take);
        if (!read.ok()) {
            co_return makeError<ReadResult>(read.status(), read.message());
        }
        cursor += take;
        remaining -= take;
    }
    co_return ReadResult{length};
}

Task<Result<WriteResult>> File::write(std::span<const IoVec> buffers, std::uint64_t offset,
                                      std::stop_token stop) {
    (void)stop;
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<WriteResult>(Status::ForkedProcess,
                                      "engine belongs to another process");
    }
    if (state.namespaceState == nullptr || state.dataPlane == nullptr) {
        co_return Result<WriteResult>(Status::InvalidState, "engine is not mounted");
    }
    std::size_t total = 0;
    for (const IoVec &vec : buffers) {
        if (vec.size != 0 && vec.data == nullptr) {
            co_return makeError<WriteResult>(Status::InvalidArgument, "null iovec buffer");
        }
        if (vec.size > std::numeric_limits<std::size_t>::max() - total) {
            co_return makeError<WriteResult>(Status::InvalidArgument,
                                             "iovec byte count overflows");
        }
        total += vec.size;
    }
    if (total == 0) {
        co_return WriteResult{0};
    }
    if (offset > std::numeric_limits<std::uint64_t>::max() - total) {
        co_return makeError<WriteResult>(Status::InvalidArgument,
                                         "write range overflows");
    }

    /* Allocate missing extents, zero-initialize them, then commit. Extents are
     * invisible until COMMITTED (hfs2.0.md §9.4). */
    auto reservations = state.namespaceState->reserveExtents(impl_->handle, offset, total);
    if (!reservations.ok()) {
        co_return makeError<WriteResult>(reservations.status(), reservations.message());
    }
    if (!reservations.value().empty()) {
        std::vector<metadata::ExtentDescriptor> descriptors;
        descriptors.reserve(reservations.value().size());
        for (const metadata::ExtentAllocation &allocation : reservations.value()) {
            metadata::ExtentDescriptor descriptor;
            descriptor.logicalExtent = allocation.logicalExtent;
            descriptor.physicalExtent =
                state.namespaceState->deviceStartExtent(
                    static_cast<std::uint32_t>(allocation.run.deviceId)) +
                allocation.run.start;
            descriptor.count = allocation.run.count;
            descriptor.deviceId = static_cast<std::uint32_t>(allocation.run.deviceId);
            descriptors.push_back(descriptor);
        }
        auto zeroed = state.dataPlane->zeroExtents(descriptors,
                                                   state.namespaceState->extentBytes());
        if (!zeroed.ok()) {
            state.namespaceState->releaseExtents(impl_->handle, reservations.value());
            co_return makeError<WriteResult>(zeroed.status(), zeroed.message());
        }
        auto committed =
            state.namespaceState->commitExtents(impl_->handle, reservations.value());
        if (!committed.ok()) {
            state.namespaceState->releaseExtents(impl_->handle, reservations.value());
            co_return makeError<WriteResult>(committed.status(), committed.message());
        }
    }

    auto map = extentMapFor(state, impl_->handle);
    if (!map.ok()) {
        co_return makeError<WriteResult>(map.status(), map.message());
    }
    std::uint64_t cursor = offset;
    for (const IoVec &vec : buffers) {
        if (vec.size == 0) {
            continue;
        }
        auto written = state.dataPlane->write(
            map.value(), std::span<const std::byte>(vec.data, vec.size), cursor, vec.size);
        if (!written.ok()) {
            co_return makeError<WriteResult>(written.status(), written.message());
        }
        cursor += vec.size;
    }
    auto resized = state.namespaceState->setSize(impl_->handle, cursor);
    if (!resized.ok()) {
        co_return makeError<WriteResult>(resized.status(), resized.message());
    }
    co_return WriteResult{total};
}

Task<Result<WriteResult>> File::append(std::span<const IoVec> buffers,
                                       std::stop_token stop) {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<WriteResult>(Status::ForkedProcess,
                                      "engine belongs to another process");
    }
    if (state.namespaceState == nullptr) {
        co_return Result<WriteResult>(Status::InvalidState, "engine is not mounted");
    }
    auto stat = state.namespaceState->stat(impl_->handle);
    if (!stat.ok()) {
        co_return makeError<WriteResult>(stat.status(), stat.message());
    }
    auto outer = co_await write(buffers, stat.value().logicalSize, stop);
    if (!outer.ok()) {
        co_return makeError<WriteResult>(outer.status(), outer.message());
    }
    co_return outer.value();
}

Task<Result<void>> File::allocate(std::uint64_t offset, std::uint64_t length) {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (state.namespaceState == nullptr || state.dataPlane == nullptr) {
        co_return Result<void>(Status::InvalidState, "engine is not mounted");
    }
    if (length == 0) {
        co_return Result<void>(Status::Ok);
    }
    if (offset > std::numeric_limits<std::uint64_t>::max() - length) {
        co_return Result<void>(Status::InvalidArgument, "allocation range overflows");
    }
    auto reservations = state.namespaceState->reserveExtents(impl_->handle, offset, length);
    if (!reservations.ok()) {
        co_return makeError(reservations.status(), reservations.message());
    }
    if (!reservations.value().empty()) {
        std::vector<metadata::ExtentDescriptor> descriptors;
        descriptors.reserve(reservations.value().size());
        for (const metadata::ExtentAllocation &allocation : reservations.value()) {
            metadata::ExtentDescriptor descriptor;
            descriptor.logicalExtent = allocation.logicalExtent;
            descriptor.physicalExtent =
                state.namespaceState->deviceStartExtent(
                    static_cast<std::uint32_t>(allocation.run.deviceId)) +
                allocation.run.start;
            descriptor.count = allocation.run.count;
            descriptor.deviceId = static_cast<std::uint32_t>(allocation.run.deviceId);
            descriptors.push_back(descriptor);
        }
        auto zeroed = state.dataPlane->zeroExtents(descriptors,
                                                   state.namespaceState->extentBytes());
        if (!zeroed.ok()) {
            state.namespaceState->releaseExtents(impl_->handle, reservations.value());
            co_return makeError(zeroed.status(), zeroed.message());
        }
        auto committed =
            state.namespaceState->commitExtents(impl_->handle, reservations.value());
        if (!committed.ok()) {
            state.namespaceState->releaseExtents(impl_->handle, reservations.value());
            co_return makeError(committed.status(), committed.message());
        }
    }
    co_return state.namespaceState->setSize(impl_->handle, offset + length);
}

Task<Result<void>> File::truncate(std::uint64_t size) {
    auto &state = *impl_->state;
    if (state.engine == nullptr || !state.engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    if (state.namespaceState == nullptr) {
        co_return Result<void>(Status::InvalidState, "engine is not mounted");
    }
    co_return state.namespaceState->truncateFile(impl_->handle, size);
}

Task<Result<void>> File::datasync() {
    if (!impl_->state->engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    co_return makeError(Status::NotSupported,
                        "file datasync lands with the metadata epoch path (Shard-78)");
}

Task<Result<void>> File::sync() {
    if (!impl_->state->engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    co_return makeError(Status::NotSupported,
                        "file sync lands with the metadata epoch path (Shard-78)");
}

} // namespace hfs
