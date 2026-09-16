#include "core/actor.hpp"

#include "common/trace.hpp"
#include "core/shard_ring.hpp"

namespace hfs {

ShardActor::ShardActor(ShardRing &ring, Step step, void *ctx, std::size_t batchSize)
    : ring_(ring), step_(step), ctx_(ctx), batch_(batchSize == 0 ? 1 : batchSize) {}

std::size_t ShardActor::runOnce() noexcept {
    std::size_t processed = 0;
    for (;;) {
        const std::size_t count = ring_.drain(batch_.data(), batch_.size());
        if (count == 0) {
            break;
        }
        trace::event(trace::EventId::ActorBatch, count);
        for (std::size_t i = 0; i < count; ++i) {
            step_(ctx_, batch_[i]);
        }
        processed += count;
    }

    /* Lost-wake avoidance: a producer may publish between the empty drain and
     * clearing the doorbell, so clear first and re-check. */
    ring_.clearDoorbell();
    if (!ring_.empty()) {
        ring_.armDoorbell();
        const std::size_t count = ring_.drain(batch_.data(), batch_.size());
        if (count > 0) {
            trace::event(trace::EventId::ActorBatch, count);
            for (std::size_t i = 0; i < count; ++i) {
                step_(ctx_, batch_[i]);
            }
            processed += count;
        }
    }
    processed_ += processed;
    return processed;
}

std::size_t ShardActor::processed() const noexcept { return processed_; }

std::size_t ShardActor::batchSize() const noexcept { return batch_.size(); }

} // namespace hfs
