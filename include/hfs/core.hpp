#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "hfs/async.hpp"
#include "hfs/config.hpp"
#include "hfs/export.hpp"
#include "hfs/file.hpp"
#include "hfs/journal.hpp"
#include "hfs/superblock.hpp"
#include "hfs/task.hpp"
#include "hfs/types.hpp"

namespace hfs {

namespace metadata {
class Namespace;
}

class DataPlane;
class AsyncDataService;

/* Internal lease that keeps the engine from stopping while client metadata
 * users (clients and open files) are still alive (hfs2.0.md §14.3). */
struct MetadataLease;

class Operation; /* Phase 2+ */

/* Options for one frontend client instance. */
struct ClientOptions {
    std::uint32_t maxOpenFiles = 0; /* 0 = only the namespace limits apply */
    std::uint32_t maxOutstanding = 32; /* bounded accepted async requests */
};

/* Options for the destructive volume format. */
struct FormatOptions {
    bool confirm = false;
};

/* Superblock A/B slots read back from one device (tools/acceptance). */
struct SuperblockSlots {
    bool validA = false;
    bool validB = false;
    Hfs2Superblock slotA{};
    Hfs2Superblock slotB{};
};

/* Scan-only metadata evidence collected without applying journal records. */
struct MetadataJournalScan {
    std::size_t deviceIndex = 0;
    Journal::Stats stats{};
    double usageRatio = 0.0;
    std::string error;
};

/* Engine owns the parsed config, the process lock and (Phase 1+) the SPDK
 * runtime and device set. */
class HFS_API Engine {
public:
    static Result<std::unique_ptr<Engine>> create(const Config &config);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /* Lifecycle coroutines; the caller keeps the engine alive until they are
     * consumed. Shard-70 wires the superblock format/mount on top of the SPDK
     * runtime. */
    Task<Result<void>> format(const FormatOptions &options);
    Task<Result<void>> mount();
    Task<Result<void>> drain();
    Task<Result<void>> stop();

    const Config &config() const noexcept;
    bool mounted() const noexcept;

    /* False after fork(); all core APIs must then fail with ForkedProcess. */
    bool processValid() const noexcept;

    /* Volume information from the last successful format/mount. */
    bool formatted() const noexcept;
    std::array<std::uint8_t, 16> volumeUuid() const noexcept;
    std::vector<DeviceGeometry> deviceGeometries() const;
    /* Returns allocatable data bytes across the configured data devices. */
    std::uint64_t freeDataBytes() const noexcept;
    /* Acquires/releases a DMA buffer from the engine-owned shared arena. */
    Result<std::span<std::byte>> acquireDmaBuffer(std::size_t size,
                                                   std::uint32_t timeoutMs);
    void releaseDmaBuffer(void *buffer, std::size_t size) noexcept;

    /* Control-path device block I/O (format/mount/tools); requires an active
     * runtime and a buffer that is a multiple of the device block size. */
    Result<void> readDeviceBlocks(std::size_t deviceIndex, std::uint64_t offsetBlocks,
                                  std::span<std::byte> buffer);
    Result<void> writeDeviceBlocks(std::size_t deviceIndex, std::uint64_t offsetBlocks,
                                   std::span<const std::byte> buffer);
    Result<void> flushDevice(std::size_t deviceIndex);
    std::uint32_t deviceBlockSize(std::size_t deviceIndex) const noexcept;
    std::uint64_t deviceCapacityBytes(std::size_t deviceIndex) const noexcept;

    /* Raw superblock slots of one device (requires an active runtime). */
    Result<SuperblockSlots> readSuperblockSlots(std::size_t deviceIndex) const;

    /* Scans every metadata journal for hfsctl check after a mount failure. */
    Result<std::vector<MetadataJournalScan>> scanMetadataJournals();

    /* Runtime diagnostics used by acceptance tests. */
    std::uint32_t dataReactorCount() const noexcept;
    std::size_t channelCount() const noexcept;
    Result<void> postToDataReactor(std::uint32_t index, std::function<void()> fn);
    std::uint64_t dataReactorThreadId(std::uint32_t index) const;

private:
    explicit Engine(Config config);
    friend class Client;
    friend class File;
    friend struct MetadataLease;
    /* Internal: mounted namespace state used by the frontend API. */
    metadata::Namespace *metadataNamespace() const noexcept;
    /* Internal: engine-owned data plane used by File I/O. */
    DataPlane *dataPlane() const noexcept;
    /* Internal: engine-owned actor service used by async Client submissions. */
    AsyncDataService *asyncDataService() const noexcept;
    void retainMetadataUser() noexcept;
    void releaseMetadataUser() noexcept;
    /* Flushes data, checkpoints metadata, then publishes the durable epoch. */
    Result<void> syncMetadata(std::uint64_t epoch);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* Client is the per-thread/frontend completion sink and path API entry. */
class HFS_API Client {
public:
    static Result<std::shared_ptr<Client>> create(Engine &engine,
                                                  const ClientOptions &options = {});
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    Task<Result<void>> close();

    /* Submits one contiguous asynchronous read or fixed-size overwrite. */
    Result<IoHandle> submitRead(const std::shared_ptr<File> &file, IoVec buffer,
                                std::uint64_t offset, CompletionToken token);
    Result<IoHandle> submitWrite(const std::shared_ptr<File> &file, IoVec buffer,
                                 std::uint64_t offset, CompletionToken token);

    /* Drains frontend completions and releases one Client credit per event. */
    PollResult poll(std::span<CompletionEvent> events, std::size_t min,
                    std::size_t max, std::chrono::microseconds timeout);
    std::size_t outstanding() const noexcept;
    std::size_t maxOutstanding() const noexcept;

    /* Path API; data operations on the returned File land with Shard-77. */
    Task<Result<std::shared_ptr<File>>> open(std::string_view path,
                                             OpenFlags flags = OpenFlags::None);
    Task<Result<void>> remove(std::string_view path);
    Task<Result<void>> rename(std::string_view from, std::string_view to);
    Task<Result<void>> link(std::string_view from, std::string_view to);
    Task<Result<DirectoryPage>> getChildren(std::string_view path,
                                            std::string_view cursor,
                                            std::size_t maxEntries);

private:
    explicit Client(Engine &engine, const ClientOptions &options);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* ABI major recorded in the build package (HFS_CORE_ABI_MAJOR). */
HFS_API std::uint32_t coreAbiMajor() noexcept;

} // namespace hfs
