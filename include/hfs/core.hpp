#pragma once

#include <cstdint>
#include <memory>

#include "hfs/config.hpp"
#include "hfs/export.hpp"
#include "hfs/task.hpp"
#include "hfs/types.hpp"

namespace hfs {

class File;      /* Phase 2+ */
class Operation; /* Phase 2+ */

/* Engine owns the parsed config, the process lock and (Phase 1+) the SPDK
 * runtime and device set. */
class HFS_API Engine {
public:
    static Result<std::unique_ptr<Engine>> create(const Config &config);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /* Lifecycle coroutines; the caller keeps the engine alive until they are
     * consumed. Shard-69/70 wire the SPDK runtime and the real mount. */
    Task<Result<void>> mount();
    Task<Result<void>> drain();
    Task<Result<void>> stop();

    const Config &config() const noexcept;
    bool mounted() const noexcept;

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
