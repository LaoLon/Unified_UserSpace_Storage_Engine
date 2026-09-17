#include "metadata/group_commit.hpp"

#include <limits>

#include "common/trace.hpp"

namespace hfs::metadata {

Result<std::uint64_t> GroupCommitCoordinator::enqueue(std::vector<JournalRecord> records,
                                                      std::uint64_t epoch) {
    if (records.empty()) {
        return makeError<std::uint64_t>(Status::InvalidArgument,
                                        "cannot queue an empty metadata request");
    }
    if (!pending_.empty() && epoch < pending_.back().epoch) {
        return makeError<std::uint64_t>(Status::InvalidArgument,
                                        "metadata epochs must be ordered");
    }
    if (epoch <= stats_.durableEpoch) {
        return makeError<std::uint64_t>(Status::InvalidArgument,
                                        "metadata epoch is already durable");
    }
    const std::uint64_t requestId = nextRequestId_++;
    pending_.push_back(Request{std::move(records), epoch, requestId});
    ++stats_.requests;
    return requestId;
}

Result<std::uint64_t> GroupCommitCoordinator::sync(std::uint64_t targetEpoch) {
    if (targetEpoch < stats_.durableEpoch) {
        return makeError<std::uint64_t>(Status::InvalidArgument,
                                        "sync target epoch is behind durable epoch");
    }
    std::size_t requestCount = 0;
    std::size_t recordCount = 0;
    std::uint64_t lastEpoch = stats_.durableEpoch;
    for (const Request &request : pending_) {
        if (request.epoch > targetEpoch) {
            break;
        }
        if (recordCount > std::numeric_limits<std::size_t>::max() - request.records.size()) {
            return makeError<std::uint64_t>(Status::NoMemory, "metadata batch is too large");
        }
        ++requestCount;
        recordCount += request.records.size();
        lastEpoch = request.epoch;
    }
    if (requestCount == 0) {
        return stats_.durableEpoch;
    }

    std::vector<JournalRecord> batch;
    batch.reserve(recordCount);
    for (std::size_t i = 0; i < requestCount; ++i) {
        batch.insert(batch.end(), pending_[i].records.begin(), pending_[i].records.end());
    }

    trace::event(trace::EventId::MetaTxBegin, lastEpoch, requestCount, batch.size());
    auto appended = journal_.append(batch, false);
    if (!appended.ok()) {
        return makeError<std::uint64_t>(appended.status(), appended.message());
    }
    auto flushed = journal_.flush();
    if (!flushed.ok()) {
        return makeError<std::uint64_t>(flushed.status(), flushed.message());
    }
    for (std::size_t i = 0; i < requestCount; ++i) {
        pending_.pop_front();
    }
    stats_.transactions += 1;
    stats_.flushes += 1;
    stats_.lastBatchRequests = requestCount;
    stats_.lastBatchRecords = recordCount;
    stats_.durableEpoch = lastEpoch;
    trace::event(trace::EventId::MetaTxCommit, appended.value(), requestCount, recordCount);
    trace::event(trace::EventId::JournalFlush, lastEpoch);
    trace::event(trace::EventId::EpochPublish, lastEpoch);
    return stats_.durableEpoch;
}

} // namespace hfs::metadata
