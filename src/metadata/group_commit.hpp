#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "uuse/journal.hpp"
#include "uuse/export.hpp"
#include "uuse/types.hpp"

namespace uuse::metadata {

/* Batches ordered metadata requests into one journal transaction and one
 * device flush. The metadata actor is the single caller in production. */
class UUSE_API GroupCommitCoordinator {
public:
    struct Stats {
        std::uint64_t requests = 0;
        std::uint64_t transactions = 0;
        std::uint64_t flushes = 0;
        std::uint64_t lastBatchRequests = 0;
        std::uint64_t lastBatchRecords = 0;
        std::uint64_t durableEpoch = 0;
    };

    explicit GroupCommitCoordinator(Journal &journal) noexcept : journal_(journal) {}

    /* Queues one non-empty metadata request at a monotonically increasing epoch. */
    Result<std::uint64_t> enqueue(std::vector<JournalRecord> records,
                                  std::uint64_t epoch);
    /* Commits all queued requests up to targetEpoch as one durable batch. */
    Result<std::uint64_t> sync(std::uint64_t targetEpoch);

    const Stats &stats() const noexcept { return stats_; }
    std::uint64_t durableEpoch() const noexcept { return stats_.durableEpoch; }

private:
    struct Request {
        std::vector<JournalRecord> records;
        std::uint64_t epoch = 0;
        std::uint64_t requestId = 0;
    };

    Journal &journal_;
    std::deque<Request> pending_;
    std::uint64_t nextRequestId_ = 1;
    Stats stats_;
};

} // namespace uuse::metadata
