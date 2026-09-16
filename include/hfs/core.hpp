#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "hfs/config.hpp"
#include "hfs/export.hpp"
#include "hfs/superblock.hpp"
#include "hfs/task.hpp"
#include "hfs/types.hpp"

namespace hfs {

class File;      /* Phase 2+ */
class Operation; /* Phase 2+ */

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

    /* Runtime diagnostics used by acceptance tests. */
    std::uint32_t dataReactorCount() const noexcept;
    std::size_t channelCount() const noexcept;
    Result<void> postToDataReactor(std::uint32_t index, std::function<void()> fn);
    std::uint64_t dataReactorThreadId(std::uint32_t index) const;

private:
    explicit Engine(Config config);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* Client is the per-thread/frontend completion sink. */
class HFS_API Client {
public:
    static Result<std::shared_ptr<Client>> create(Engine &engine);
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    Task<Result<void>> close();

private:
    explicit Client(Engine &engine);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* ABI major recorded in the build package (HFS_CORE_ABI_MAJOR). */
HFS_API std::uint32_t coreAbiMajor() noexcept;

} // namespace hfs
