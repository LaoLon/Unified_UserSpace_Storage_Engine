#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "common/trace.hpp"
#include "core/scheduler.hpp"
#include "test_support.hpp"

namespace {

using hfs::OperationKind;
using hfs::SchedulerNode;

SchedulerNode makeNode(std::uint64_t start, std::uint64_t end, OperationKind kind,
                       bool barrier = false) {
    SchedulerNode node;
    node.start = start;
    node.end = end;
    node.kind = kind;
    node.inodeBarrier = barrier;
    return node;
}

struct AdmissionLog {
    std::vector<SchedulerNode *> admitted;

    static void callback(void *ctx, SchedulerNode &node) {
        static_cast<AdmissionLog *>(ctx)->admitted.push_back(&node);
    }

    std::size_t drain(std::vector<SchedulerNode *> &out) {
        const std::size_t count = admitted.size();
        out.insert(out.end(), admitted.begin(), admitted.end());
        admitted.clear();
        return count;
    }
};

bool pairConflicts(const SchedulerNode &left, const SchedulerNode &right) {
    if (left.inodeBarrier || right.inodeBarrier) {
        return true;
    }
    const bool overlap = left.start < right.end && right.start < left.end;
    const bool shared =
        left.kind == OperationKind::Read && right.kind == OperationKind::Read;
    return overlap && !shared;
}

} // namespace

int main() {
    /* 1. overlapping reads share the inode */
    {
        hfs::InodeDependencyScheduler scheduler;
        AdmissionLog log;
        scheduler.setAdmitCallback(AdmissionLog::callback, &log);
        auto read1 = makeNode(0, 10, OperationKind::Read);
        auto read2 = makeNode(5, 15, OperationKind::Read);
        HFS_TEST_CHECK(scheduler.submit(read1));
        HFS_TEST_CHECK(scheduler.submit(read2));
        HFS_TEST_CHECK(log.admitted.size() == 2);
        HFS_TEST_CHECK(scheduler.activeCount() == 2);
        scheduler.complete(read1);
        scheduler.complete(read2);
        HFS_TEST_CHECK(scheduler.activeCount() == 0 && scheduler.pendingCount() == 0);
    }

    /* 2. overlapping writes queue in FIFO order */
    {
        hfs::InodeDependencyScheduler scheduler;
        AdmissionLog log;
        scheduler.setAdmitCallback(AdmissionLog::callback, &log);
        auto write1 = makeNode(0, 10, OperationKind::Write);
        auto write2 = makeNode(5, 15, OperationKind::Write);
        auto write3 = makeNode(7, 9, OperationKind::Write);
        HFS_TEST_CHECK(scheduler.submit(write1));
        HFS_TEST_CHECK(!scheduler.submit(write2));
        HFS_TEST_CHECK(!scheduler.submit(write3));
        HFS_TEST_CHECK(scheduler.pendingCount() == 2);
        scheduler.complete(write1);
        HFS_TEST_CHECK(write2.admitted && !write3.admitted);
        scheduler.complete(write2);
        HFS_TEST_CHECK(write3.admitted);
        scheduler.complete(write3);
        HFS_TEST_CHECK(log.admitted.size() == 3);
        HFS_TEST_CHECK(log.admitted[0] == &write1 && log.admitted[1] == &write2 &&
                       log.admitted[2] == &write3);
        const auto stats = scheduler.stats();
        HFS_TEST_CHECK(stats.admits == 3 && stats.queued == 2 && stats.completes == 3);
        HFS_TEST_CHECK(stats.dependencyWaits >= 2);
        HFS_TEST_CHECK(stats.pendingPeak == 2 && stats.activePeak == 1);
    }

    /* 3. adjacent and disjoint writes run in parallel */
    {
        hfs::InodeDependencyScheduler scheduler;
        AdmissionLog log;
        scheduler.setAdmitCallback(AdmissionLog::callback, &log);
        auto write1 = makeNode(0, 10, OperationKind::Write);
        auto write2 = makeNode(10, 20, OperationKind::Write);
        auto write3 = makeNode(100, 110, OperationKind::Write);
        HFS_TEST_CHECK(scheduler.submit(write1));
        HFS_TEST_CHECK(scheduler.submit(write2));
        HFS_TEST_CHECK(scheduler.submit(write3));
        HFS_TEST_CHECK(scheduler.activeCount() == 3);
        scheduler.complete(write1);
        scheduler.complete(write2);
        scheduler.complete(write3);
    }

    /* 4. read/write conflicts in both directions */
    {
        hfs::InodeDependencyScheduler scheduler;
        AdmissionLog log;
        scheduler.setAdmitCallback(AdmissionLog::callback, &log);
        auto read = makeNode(0, 10, OperationKind::Read);
        auto write = makeNode(5, 15, OperationKind::Write);
        HFS_TEST_CHECK(scheduler.submit(read));
        HFS_TEST_CHECK(!scheduler.submit(write));
        scheduler.complete(read);
        HFS_TEST_CHECK(write.admitted);
        scheduler.complete(write);
    }
    {
        hfs::InodeDependencyScheduler scheduler;
        AdmissionLog log;
        scheduler.setAdmitCallback(AdmissionLog::callback, &log);
        auto write = makeNode(0, 10, OperationKind::Write);
        auto read = makeNode(5, 15, OperationKind::Read);
        HFS_TEST_CHECK(scheduler.submit(write));
        HFS_TEST_CHECK(!scheduler.submit(read));
        scheduler.complete(write);
        HFS_TEST_CHECK(read.admitted);
        scheduler.complete(read);
    }

    /* 5. inode barrier waits for active work and blocks later nodes */
    {
        hfs::InodeDependencyScheduler scheduler;
        AdmissionLog log;
        scheduler.setAdmitCallback(AdmissionLog::callback, &log);
        auto read = makeNode(0, 10, OperationKind::Read);
        auto barrier = makeNode(0, 0, OperationKind::Sync, true);
        auto write = makeNode(100, 110, OperationKind::Write);
        HFS_TEST_CHECK(scheduler.submit(read));
        HFS_TEST_CHECK(!scheduler.submit(barrier));
        HFS_TEST_CHECK(!scheduler.submit(write)); /* must not pass the barrier */
        scheduler.complete(read);
        HFS_TEST_CHECK(barrier.admitted && !write.admitted);
        scheduler.complete(barrier);
        HFS_TEST_CHECK(write.admitted);
        scheduler.complete(write);
        HFS_TEST_CHECK(log.admitted.size() == 3);
    }

    /* 6. admitted barrier blocks everything until completion */
    {
        hfs::InodeDependencyScheduler scheduler;
        AdmissionLog log;
        scheduler.setAdmitCallback(AdmissionLog::callback, &log);
        auto barrier = makeNode(0, 0, OperationKind::Sync, true);
        auto read = makeNode(0, 10, OperationKind::Read);
        HFS_TEST_CHECK(scheduler.submit(barrier));
        HFS_TEST_CHECK(!scheduler.submit(read));
        scheduler.complete(barrier);
        HFS_TEST_CHECK(read.admitted);
        scheduler.complete(read);
    }

    /* 7. tracepoints fire when tracing is enabled */
    {
        hfs::InodeDependencyScheduler scheduler;
        AdmissionLog log;
        scheduler.setAdmitCallback(AdmissionLog::callback, &log);
        hfs::trace::setEnabled(true);
        const std::uint64_t before = hfs::trace::recorded();
        auto read = makeNode(0, 10, OperationKind::Read);
        auto write = makeNode(5, 15, OperationKind::Write);
        HFS_TEST_CHECK(scheduler.submit(read));
        HFS_TEST_CHECK(!scheduler.submit(write));
        scheduler.complete(read);
        scheduler.complete(write);
        HFS_TEST_CHECK(hfs::trace::recorded() > before);
        hfs::trace::setEnabled(false);
    }

    /* 8. file -> shard mapping */
    {
        bool distinct = false;
        std::uint32_t first = hfs::shardForFile(1, 4);
        for (std::uint64_t id = 0; id < 256; ++id) {
            const std::uint32_t shard = hfs::shardForFile(id, 4);
            HFS_TEST_CHECK(shard < 4);
            HFS_TEST_CHECK(hfs::shardForFile(id, 4) == shard);
            if (shard != first) {
                distinct = true;
            }
        }
        HFS_TEST_CHECK(distinct);
        HFS_TEST_CHECK(hfs::shardForFile(7, 0) == 0);
    }

    /* 9. 32-client stress: admission order preserves the conflict invariant */
    {
        constexpr std::size_t kTotal = 4096;
        hfs::InodeDependencyScheduler scheduler;
        struct StressCtx {
            std::vector<SchedulerNode *> admitted;
            std::vector<SchedulerNode *> *active = nullptr;
            bool violated = false;
        };
        StressCtx ctx;
        std::vector<SchedulerNode *> active;
        ctx.active = &active;
        scheduler.setAdmitCallback(
            [](void *raw, SchedulerNode &node) {
                auto *context = static_cast<StressCtx *>(raw);
                for (SchedulerNode *other : *context->active) {
                    if (pairConflicts(node, *other)) {
                        context->violated = true;
                    }
                }
                for (SchedulerNode *other : context->admitted) {
                    if (pairConflicts(node, *other)) {
                        context->violated = true;
                    }
                }
                context->admitted.push_back(&node);
            },
            &ctx);

        std::vector<SchedulerNode> nodes(kTotal);
        std::mt19937_64 rng(20260916);
        auto pullAdmissions = [&ctx, &active]() {
            for (SchedulerNode *node : ctx.admitted) {
                active.push_back(node);
            }
            ctx.admitted.clear();
        };
        std::size_t next = 0;
        std::size_t completed = 0;
        std::size_t guard = 0;
        const std::size_t guardLimit = 64 * kTotal;
        constexpr std::size_t kWindow = 32;
        while (completed < kTotal && guard++ < guardLimit) {
            if (next < kTotal && active.size() < kWindow) {
                SchedulerNode &node = nodes[next];
                const std::uint64_t start = rng() % 256;
                const std::uint64_t length = 1 + rng() % 16;
                const bool barrier = (rng() % 100) == 0;
                const bool read = !barrier && (rng() % 100) < 70;
                node = makeNode(start, start + length,
                                read ? OperationKind::Read : OperationKind::Write, barrier);
                (void)scheduler.submit(node);
                ++next;
            } else if (!active.empty()) {
                SchedulerNode *node = active.front();
                active.erase(active.begin());
                scheduler.complete(*node);
                ++completed;
            } else {
                /* no submissions left and no active work: pending must be empty */
                HFS_TEST_CHECK(next == kTotal && scheduler.pendingCount() == 0);
                break;
            }
            pullAdmissions();
        }
        HFS_TEST_CHECK(guard < guardLimit);
        while (!active.empty()) {
            SchedulerNode *node = active.front();
            active.erase(active.begin());
            scheduler.complete(*node);
            ++completed;
            pullAdmissions();
        }
        HFS_TEST_CHECK(completed == kTotal);
        HFS_TEST_CHECK(!ctx.violated);
        HFS_TEST_CHECK(scheduler.activeCount() == 0);
        HFS_TEST_CHECK(scheduler.pendingCount() == 0);
        const auto stats = scheduler.stats();
        HFS_TEST_CHECK(stats.admits == kTotal);
        HFS_TEST_CHECK(stats.completes == kTotal);
        HFS_TEST_CHECK(stats.pendingPeak > 0);
        HFS_TEST_CHECK(stats.dependencyWaits > 0);
    }

    /* 8. trigger check for the unbalanced interval index (review P1-6):
     * monotonically increasing offsets with a deep active window are the worst
     * case for a degenerate BST. The per-file active set in production is
     * bounded by concurrent operations on one file, so this only documents
     * the cost; it must not blow up. */
    {
        constexpr std::size_t kTotal = 5000;
        constexpr std::size_t kWindow = 4096;
        hfs::InodeDependencyScheduler scheduler;
        AdmissionLog log;
        scheduler.setAdmitCallback(AdmissionLog::callback, &log);
        std::vector<SchedulerNode> nodes(kTotal);
        std::size_t next = 0;
        std::size_t completed = 0;
        const auto start = std::chrono::steady_clock::now();
        while (completed < kTotal) {
            if (next < kTotal && log.admitted.size() < kWindow) {
                nodes[next] = makeNode(next * 64, next * 64 + 64, OperationKind::Write);
                (void)scheduler.submit(nodes[next]);
                ++next;
                continue;
            }
            if (!log.admitted.empty()) {
                SchedulerNode *node = log.admitted.back();
                log.admitted.pop_back();
                scheduler.complete(*node);
                ++completed;
                continue;
            }
            HFS_TEST_CHECK(false); /* no runnable node and nothing pending */
            break;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        HFS_TEST_CHECK(scheduler.activeCount() == 0);
        HFS_TEST_CHECK(scheduler.pendingCount() == 0);
        std::printf("scheduler monotonic: total=%zu window=%zu ns_per_node=%.0f\n",
                    kTotal, kWindow,
                    static_cast<double>(elapsed) / static_cast<double>(kTotal));
        HFS_TEST_CHECK(elapsed / static_cast<long long>(kTotal) < 100000);
    }

    std::printf("scheduler_test: ok\n");
    return 0;
}
