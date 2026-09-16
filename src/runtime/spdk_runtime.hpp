#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

private:
    SpdkRuntime();
    ~SpdkRuntime();
    SpdkRuntime(const SpdkRuntime &) = delete;
    SpdkRuntime &operator=(const SpdkRuntime &) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hfs
