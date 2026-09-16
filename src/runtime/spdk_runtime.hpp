#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "hfs/config.hpp"
#include "hfs/export.hpp"
#include "hfs/types.hpp"

namespace hfs {

/* Process-wide SPDK runtime (env, reactors, bdev descriptors and per-reactor
 * io channels). Engine mount/stop switch it between Active and EnvReady
 * without re-initializing DPDK; the env lives until process exit.
 * Shard-69 owns this layer; Shard-70+ add the real superblock mount. */
class HFS_API SpdkRuntime {
public:
    static SpdkRuntime &instance() noexcept;

    /* Initialize env/subsystems on first use, open the configured bdev set,
     * then start the metadata and data reactors. */
    Result<void> start(const Config &config);
    /* Stop reactors, release channels/descriptors and return to EnvReady. */
    Result<void> stop();

    bool active() const noexcept;
    std::uint32_t dataReactorCount() const noexcept;
    std::size_t deviceCount() const noexcept;
    std::string deviceName(std::size_t index) const;

    /* Control-path doorbell onto a data reactor (spdk_thread_send_msg). */
    Result<void> postToDataReactor(std::uint32_t index, std::function<void()> fn);
    std::uint64_t dataReactorThreadId(std::uint32_t index) const;

    /* Acceptance/diagnostic counters. */
    std::size_t channelCount() const noexcept;
    std::int64_t ownerPid() const noexcept;

    /* Device geometry of the opened bdev set. */
    std::uint64_t deviceCapacityBytes(std::size_t index) const noexcept;
    std::uint32_t deviceBlockSize(std::size_t index) const noexcept;

    /* Control-path block I/O on the metadata reactor (format/mount/tools).
     * The caller blocks; buffers are bounced through DMA memory. */
    Result<void> controlRead(std::size_t index, std::uint64_t offsetBlocks,
                             std::span<std::byte> buffer);
    Result<void> controlWrite(std::size_t index, std::uint64_t offsetBlocks,
                              std::span<const std::byte> buffer);
    Result<void> controlFlush(std::size_t index);

    /* Async data-reactor block I/O; the callback runs on the reactor thread. */
    Result<void> submitDataRead(std::uint32_t reactorIndex, std::size_t deviceIndex,
                                void *buffer, std::uint64_t offsetBlocks,
                                std::uint64_t blocks, void (*cb)(void *, int),
                                void *cbCtx);
    Result<void> submitDataWrite(std::uint32_t reactorIndex, std::size_t deviceIndex,
                                 const void *buffer, std::uint64_t offsetBlocks,
                                 std::uint64_t blocks, void (*cb)(void *, int),
                                 void *cbCtx);

    /* True when the buffer is DMA-mappable through SPDK. */
    bool isDmaCapable(const void *buffer, std::size_t bytes) const noexcept;

private:
    SpdkRuntime();
    ~SpdkRuntime();
    SpdkRuntime(const SpdkRuntime &) = delete;
    SpdkRuntime &operator=(const SpdkRuntime &) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hfs
