#include "hfs/core.hpp"

#include <atomic>
#include <memory>

#include "common/log.hpp"
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
    ClientOptions options;
    std::shared_ptr<MetadataLease> lease;
    std::atomic<std::uint32_t> openFiles{0};
    std::atomic<bool> closed{false};
};

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
    (void)buffers;
    (void)offset;
    (void)stop;
    if (!impl_->state->engine->processValid()) {
        co_return Result<ReadResult>(Status::ForkedProcess,
                                     "engine belongs to another process");
    }
    co_return makeError<ReadResult>(Status::NotSupported,
                                    "file read lands with the allocator (Shard-77)");
}

Task<Result<WriteResult>> File::write(std::span<const IoVec> buffers, std::uint64_t offset,
                                      std::stop_token stop) {
    (void)buffers;
    (void)offset;
    (void)stop;
    if (!impl_->state->engine->processValid()) {
        co_return Result<WriteResult>(Status::ForkedProcess,
                                      "engine belongs to another process");
    }
    co_return makeError<WriteResult>(Status::NotSupported,
                                     "file write lands with the allocator (Shard-77)");
}

Task<Result<WriteResult>> File::append(std::span<const IoVec> buffers,
                                       std::stop_token stop) {
    (void)buffers;
    (void)stop;
    if (!impl_->state->engine->processValid()) {
        co_return Result<WriteResult>(Status::ForkedProcess,
                                      "engine belongs to another process");
    }
    co_return makeError<WriteResult>(Status::NotSupported,
                                     "file append lands with the allocator (Shard-77)");
}

Task<Result<void>> File::allocate(std::uint64_t offset, std::uint64_t length) {
    (void)offset;
    (void)length;
    if (!impl_->state->engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    co_return makeError(Status::NotSupported,
                        "file allocate lands with the allocator (Shard-77)");
}

Task<Result<void>> File::truncate(std::uint64_t size) {
    (void)size;
    if (!impl_->state->engine->processValid()) {
        co_return Result<void>(Status::ForkedProcess, "engine belongs to another process");
    }
    co_return makeError(Status::NotSupported,
                        "file truncate lands with the allocator (Shard-77)");
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
