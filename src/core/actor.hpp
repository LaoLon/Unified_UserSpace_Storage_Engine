#pragma once

#include <cstddef>
#include <vector>

#include "core/operation.hpp"

namespace hfs {

class ShardRing;

/* Single-actor consumer loop over a ShardRing. The step callback owns the
 * operation lifetime policy (release/complete) chosen by the shard. */
class ShardActor {
public:
    using Step = void (*)(void *ctx, Operation *operation);

    ShardActor(ShardRing &ring, Step step, void *ctx, std::size_t batchSize);

    /* Drain and process batches until the ring is empty; returns the number of
     * operations processed. Handles the doorbell lost-wake window. */
    std::size_t runOnce() noexcept;

    std::size_t processed() const noexcept;
    std::size_t batchSize() const noexcept;

private:
    ShardRing &ring_;
    Step step_ = nullptr;
    void *ctx_ = nullptr;
    std::vector<Operation *> batch_;
    std::size_t processed_ = 0;
};

} // namespace hfs
