#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <limits>
#include <string>
#include <vector>

#include "uuse/journal.hpp"
#include "metadata/allocator.hpp"
#include "metadata/inode_map.hpp"
#include "metadata/namespace.hpp"
#include "test_support.hpp"

using namespace uuse;
using namespace uuse::metadata;

namespace {

JournalRecord allocRecord(JournalRecordType type, std::uint64_t start,
                          std::uint64_t count) {
    JournalRecord record;
    record.type = type;
    record.objectId = 0; /* device id */
    record.arg0 = start;
    record.arg1 = count;
    return record;
}

JournalRecord setSizeRecord(std::uint64_t inodeId, std::uint64_t size) {
    JournalRecord record;
    record.type = JournalRecordType::SetSize;
    record.objectId = inodeId;
    record.arg0 = size;
    return record;
}

std::uint64_t fixedClock(void *) { return 1; }

/* Record application shared by the replay assertions (the journal transport
 * itself is covered by the journal tests). */
Result<void> applyAllocatorRecord(Allocator &allocator, const JournalRecord &record,
                                  std::map<std::uint64_t, std::uint64_t> &sizes) {
    switch (record.type) {
    case JournalRecordType::Reserve:
        return allocator.markReserved(record.arg0, record.arg1);
    case JournalRecordType::Alloc:
        return allocator.commit(record.arg0, record.arg1);
    case JournalRecordType::Free:
        return allocator.release(record.arg0, record.arg1);
    case JournalRecordType::SetSize:
        sizes[record.objectId] = record.arg0;
        return Result<void>(Status::Ok);
    default:
        return Result<void>(Status::Ok);
    }
}

} // namespace

int main() {
    /* 1. reserve/commit/release conservation and run merging */
    {
        auto created = Allocator::create({0, 1000, 0, 64});
        UUSE_TEST_CHECK(created.ok());
        Allocator &alloc = *created.value();

        auto run = alloc.reserve(64);
        UUSE_TEST_CHECK(run.ok() && run.value().start == 0 && run.value().count == 64);
        UUSE_TEST_CHECK(alloc.stateAt(0) == ExtentState::Reserved);
        UUSE_TEST_CHECK(alloc.commit(0, 64).ok());
        UUSE_TEST_CHECK(alloc.stateAt(63) == ExtentState::Committed);
        UUSE_TEST_CHECK(alloc.stats().committed == 64);
        UUSE_TEST_CHECK(alloc.stats().free == 936);
        UUSE_TEST_CHECK(alloc.verify());

        auto single = alloc.reserve(1);
        UUSE_TEST_CHECK(single.ok() && single.value().start == 64);
        UUSE_TEST_CHECK(alloc.release(64, 1).ok());
        UUSE_TEST_CHECK(alloc.stats().freeRuns == 1);
        UUSE_TEST_CHECK(alloc.verify());

        UUSE_TEST_CHECK(!alloc.reserve(65).ok());
        UUSE_TEST_CHECK(alloc.reserve(65).status() == Status::InvalidArgument);
        UUSE_TEST_CHECK(alloc.stateAt(1000) == ExtentState::Free);
    }

    /* 2. capacity ceiling and ENOSPC consistency */
    {
        auto created = Allocator::create({0, 100, 50, 8});
        UUSE_TEST_CHECK(created.ok());
        Allocator &alloc = *created.value();
        for (int i = 0; i < 6; ++i) {
            auto run = alloc.reserve(8);
            UUSE_TEST_CHECK(run.ok());
            UUSE_TEST_CHECK(alloc.commit(run.value().start, run.value().count).ok());
        }
        UUSE_TEST_CHECK(alloc.stats().committed == 48);
        UUSE_TEST_CHECK(alloc.reserve(8).status() == Status::NoSpace);
        UUSE_TEST_CHECK(alloc.verify());

        auto scattered = alloc.reserveScattered(2);
        UUSE_TEST_CHECK(scattered.ok());
        UUSE_TEST_CHECK(alloc.stats().reserved + alloc.stats().committed == 50);
        auto over = alloc.reserve(1);
        UUSE_TEST_CHECK(!over.ok() && over.status() == Status::NoSpace);
        for (const ExtentRun &part : scattered.value()) {
            UUSE_TEST_CHECK(alloc.release(part.start, part.count).ok());
        }
        UUSE_TEST_CHECK(alloc.release(0, 48).ok());
        UUSE_TEST_CHECK(alloc.stats().free == 100);
        UUSE_TEST_CHECK(alloc.verify());
    }

    /* 3. scattered reservation limit and atomic rollback */
    {
        auto created = Allocator::create({0, 10, 0, 4});
        UUSE_TEST_CHECK(created.ok());
        Allocator &alloc = *created.value();
        auto first = alloc.reserve(4);
        auto second = alloc.reserve(4);
        UUSE_TEST_CHECK(first.ok() && second.ok());
        UUSE_TEST_CHECK(alloc.commit(first.value().start, 4).ok());
        UUSE_TEST_CHECK(alloc.commit(second.value().start, 4).ok());
        UUSE_TEST_CHECK(alloc.release(4, 2).ok());
        UUSE_TEST_CHECK(alloc.stats().free == 4);
        UUSE_TEST_CHECK(alloc.stats().freeRuns == 2);

        auto failing = alloc.reserveScattered(7);
        UUSE_TEST_CHECK(!failing.ok() && failing.status() == Status::NoSpace);
        /* the failed request must not leave partial reservations behind */
        UUSE_TEST_CHECK(alloc.stats().free == 4);
        UUSE_TEST_CHECK(alloc.stats().reserved == 0);
        UUSE_TEST_CHECK(alloc.verify());
    }

    /* 4. snapshot round-trip and corruption handling */
    {
        auto source = Allocator::create({0, 128, 0, 16});
        UUSE_TEST_CHECK(source.ok());
        Allocator &alloc = *source.value();
        UUSE_TEST_CHECK(alloc.markReserved(10, 3).ok());
        UUSE_TEST_CHECK(alloc.commit(10, 3).ok());
        UUSE_TEST_CHECK(alloc.markReserved(40, 5).ok());
        auto blob = alloc.serialize();

        auto loaded = Allocator::create({0, 128, 0, 16});
        UUSE_TEST_CHECK(loaded.ok());
        UUSE_TEST_CHECK(loaded.value()->deserialize(blob).ok());
        UUSE_TEST_CHECK(loaded.value()->verify());
        for (std::uint64_t i = 0; i < 128; ++i) {
            UUSE_TEST_CHECK(loaded.value()->stateAt(i) == alloc.stateAt(i));
        }
        UUSE_TEST_CHECK(loaded.value()->stats().committed == 3);
        UUSE_TEST_CHECK(loaded.value()->stats().reserved == 5);

        auto corrupted = blob;
        corrupted[Allocator::kSnapshotHeaderBytes + 5] ^= std::byte{0xFF};
        auto bad = Allocator::create({0, 128, 0, 16});
        UUSE_TEST_CHECK(bad.ok());
        auto result = bad.value()->deserialize(corrupted);
        UUSE_TEST_CHECK(!result.ok() && result.status() == Status::Corrupt);
        UUSE_TEST_CHECK(bad.value()->verify());
    }

    /* 5. replay reclaims reserved-but-never-committed extents */
    {
        auto source = Allocator::create({0, 64, 0, 16});
        UUSE_TEST_CHECK(source.ok());
        Allocator &alloc = *source.value();
        UUSE_TEST_CHECK(alloc.markReserved(0, 4).ok());
        UUSE_TEST_CHECK(alloc.commit(0, 4).ok());
        UUSE_TEST_CHECK(alloc.markReserved(20, 6).ok());
        UUSE_TEST_CHECK(alloc.stats().reserved == 6);
        auto blob = alloc.serialize();
        auto loaded = Allocator::create({0, 64, 0, 16});
        UUSE_TEST_CHECK(loaded.ok() && loaded.value()->deserialize(blob).ok());
        UUSE_TEST_CHECK(loaded.value()->reclaimReserved().ok());
        UUSE_TEST_CHECK(loaded.value()->stats().reserved == 0);
        UUSE_TEST_CHECK(loaded.value()->stateAt(20) == ExtentState::Free);
        UUSE_TEST_CHECK(loaded.value()->stats().committed == 4);
        UUSE_TEST_CHECK(loaded.value()->verify());
    }

    /* 6. placement policy */
    {
        const std::uint64_t free3[] = {10, 40, 40};
        PlacementPicker weighted(PlacementPolicy::CapacityWeighted, 3);
        UUSE_TEST_CHECK(weighted.pick(free3) == 1);
        PlacementPicker roundRobin(PlacementPolicy::RoundRobin, 3);
        UUSE_TEST_CHECK(roundRobin.pick(free3) == 0);
        UUSE_TEST_CHECK(roundRobin.pick(free3) == 1);
        UUSE_TEST_CHECK(roundRobin.pick(free3) == 2);
        UUSE_TEST_CHECK(roundRobin.pick(free3) == 0);
        UUSE_TEST_CHECK(weighted.pick({}) == 0);
    }

    /* 7. per-inode extent map lookup, truncate and restore */
    {
        InodeExtentMap map;
        map.configure(1ULL << 20);
        const ExtentDescriptor extents[] = {
            {0, 100, 2, 0},
            {4, 500, 3, 1},
        };
        UUSE_TEST_CHECK(map.addExtents(extents).ok());
        UUSE_TEST_CHECK(map.mappedExtents() == 5);
        UUSE_TEST_CHECK(map.logicalExtents() == 7);

        std::array<ExtentView, 8> views{};
        auto lookup = map.lookup(1ULL << 20, 4ULL << 20, views);
        UUSE_TEST_CHECK(lookup.ok() && lookup.value() == 2);
        UUSE_TEST_CHECK(views[0].logicalOffset == 1ULL << 20);
        UUSE_TEST_CHECK(views[0].length == 1ULL << 20);
        UUSE_TEST_CHECK(views[0].physicalByte == (101ULL << 20));
        UUSE_TEST_CHECK(views[0].deviceId == 0);
        UUSE_TEST_CHECK(views[1].logicalOffset == 4ULL << 20);
        UUSE_TEST_CHECK(views[1].physicalByte == 500ULL << 20);
        UUSE_TEST_CHECK(views[1].deviceId == 1);

        auto clipped = map.lookup((1ULL << 20) + 4096, 1ULL << 20, views);
        UUSE_TEST_CHECK(clipped.ok() && clipped.value() == 1);
        UUSE_TEST_CHECK(views[0].logicalOffset == (1ULL << 20) + 4096);
        UUSE_TEST_CHECK(views[0].length == (1ULL << 20) - 4096);
        UUSE_TEST_CHECK(views[0].physicalByte == (101ULL << 20) + 4096);

        /* hole between extent 2 and extent 4 is not reported */
        auto hole = map.lookup(2ULL << 20, 1ULL << 20, views);
        UUSE_TEST_CHECK(hole.ok() && hole.value() == 0);

        const ExtentDescriptor overlap[] = {{1, 900, 1, 0}};
        UUSE_TEST_CHECK(!map.addExtents(overlap).ok());

        auto restored = map.snapshot();
        InodeExtentMap copy;
        copy.configure(1ULL << 20);
        UUSE_TEST_CHECK(copy.restore(restored).ok());
        UUSE_TEST_CHECK(copy.mappedExtents() == map.mappedExtents());

        UUSE_TEST_CHECK(map.removeFrom(4).ok());
        UUSE_TEST_CHECK(map.mappedExtents() == 2);
        UUSE_TEST_CHECK(map.logicalExtents() == 2);
        UUSE_TEST_CHECK(map.removeFrom(1).ok());
        UUSE_TEST_CHECK(map.mappedExtents() == 1);
        UUSE_TEST_CHECK(map.empty() == false);
        UUSE_TEST_CHECK(map.removeFrom(0).ok());
        UUSE_TEST_CHECK(map.empty());
    }

    /* 8. ALLOC/FREE/RESERVE record semantics replayed over a checkpoint */
    {
        const std::vector<JournalRecord> stream = {
            allocRecord(JournalRecordType::Reserve, 0, 4),
            allocRecord(JournalRecordType::Alloc, 0, 4),
            allocRecord(JournalRecordType::Reserve, 64, 2),
            allocRecord(JournalRecordType::Alloc, 64, 2),
            allocRecord(JournalRecordType::Reserve, 128, 3),
            allocRecord(JournalRecordType::Alloc, 128, 3),
            allocRecord(JournalRecordType::Free, 64, 2),
            setSizeRecord(7, 4096),
        };

        auto allocatorResult = Allocator::create({0, 1024, 0, 64});
        UUSE_TEST_CHECK(allocatorResult.ok());
        Allocator &allocator = *allocatorResult.value();
        std::map<std::uint64_t, std::uint64_t> sizes;
        for (std::size_t i = 0; i < 4; ++i) {
            UUSE_TEST_CHECK(applyAllocatorRecord(allocator, stream[i], sizes).ok());
        }
        const std::vector<std::byte> checkpoint = allocator.serialize();

        /* mount: restore the checkpoint snapshot, then replay the tail */
        auto mountedResult = Allocator::create({0, 1024, 0, 64});
        UUSE_TEST_CHECK(mountedResult.ok());
        Allocator &mounted = *mountedResult.value();
        UUSE_TEST_CHECK(mounted.deserialize(checkpoint).ok());
        std::map<std::uint64_t, std::uint64_t> mountedSizes;
        for (std::size_t i = 4; i < stream.size(); ++i) {
            UUSE_TEST_CHECK(applyAllocatorRecord(mounted, stream[i], mountedSizes).ok());
        }

        UUSE_TEST_CHECK(mounted.verify());
        UUSE_TEST_CHECK(mounted.stateAt(0) == ExtentState::Committed);
        UUSE_TEST_CHECK(mounted.stateAt(64) == ExtentState::Free);
        UUSE_TEST_CHECK(mounted.stateAt(128) == ExtentState::Committed);
        UUSE_TEST_CHECK(mounted.stats().committed == 7);
        UUSE_TEST_CHECK(mounted.stats().free == 1017);
        UUSE_TEST_CHECK(mountedSizes[7] == 4096);

        /* replaying the whole stream again ends in the same state */
        for (const JournalRecord &record : stream) {
            UUSE_TEST_CHECK(applyAllocatorRecord(mounted, record, mountedSizes).ok());
        }
        UUSE_TEST_CHECK(mounted.verify());
        UUSE_TEST_CHECK(mounted.stats().committed == 7);
        UUSE_TEST_CHECK(mounted.stats().free == 1017);

        /* RESERVED runs seen in a snapshot are reclaimed at mount */
        auto reservedSnapshotResult = Allocator::create({0, 1024, 0, 64});
        UUSE_TEST_CHECK(reservedSnapshotResult.ok());
        Allocator &reservedSnapshot = *reservedSnapshotResult.value();
        UUSE_TEST_CHECK(reservedSnapshot.markReserved(200, 4).ok());
        const auto reservedBlob = reservedSnapshot.serialize();
        auto recoveryResult = Allocator::create({0, 1024, 0, 64});
        UUSE_TEST_CHECK(recoveryResult.ok());
        Allocator &recovery = *recoveryResult.value();
        UUSE_TEST_CHECK(recovery.deserialize(reservedBlob).ok());
        UUSE_TEST_CHECK(recovery.reclaimReserved().ok());
        UUSE_TEST_CHECK(recovery.stateAt(200) == ExtentState::Free);
        UUSE_TEST_CHECK(recovery.verify());
    }

    /* 9. namespace allocation, truncate and ENOSPC conservation */
    {
        NamespaceConfig config;
        config.maxInodes = 64;
        config.maxNamespaceBytes = 1ULL << 20;
        config.extentBytes = 4096; /* one metadata block per extent for the test */
        config.deviceExtents = {16};
        config.deviceStartExtents = {2}; /* data region base in extent units */
        config.maxReserveExtents = 4;
        config.capacityOverrideExtents = 4;
        auto created = Namespace::create(config, fixedClock, nullptr);
        UUSE_TEST_CHECK(created.ok());
        Namespace &ns = *created.value();
        auto handle = ns.create("/f", false, false, false);
        UUSE_TEST_CHECK(handle.ok());

        /* reserve + commit three extents, then read the committed map back */
        auto reservations = ns.reserveExtents(handle.value(), 0, 3 * 4096);
        UUSE_TEST_CHECK(reservations.ok() && reservations.value().size() == 1);
        UUSE_TEST_CHECK(reservations.value()[0].run.start == 0);
        UUSE_TEST_CHECK(reservations.value()[0].logicalExtent == 0);
        UUSE_TEST_CHECK(ns.commitExtents(handle.value(), reservations.value()).ok());
        auto descriptors = ns.extents(handle.value());
        UUSE_TEST_CHECK(descriptors.ok() && descriptors.value().size() == 1);
        UUSE_TEST_CHECK(descriptors.value()[0].physicalExtent == 2);
        UUSE_TEST_CHECK(descriptors.value()[0].count == 3);
        UUSE_TEST_CHECK(ns.stats().allocatedExtents == 3);

        /* an overlapping reservation only covers the unmapped tail */
        auto more = ns.reserveExtents(handle.value(), 2 * 4096, 2 * 4096);
        UUSE_TEST_CHECK(more.ok() && more.value().size() == 1);
        UUSE_TEST_CHECK(more.value()[0].logicalExtent == 3);
        UUSE_TEST_CHECK(more.value()[0].run.count == 1);
        UUSE_TEST_CHECK(ns.commitExtents(handle.value(), more.value()).ok());
        UUSE_TEST_CHECK(ns.extents(handle.value()).value().size() == 2);

        /* shrink releases the tail extents; unmapped extents stay invisible */
        UUSE_TEST_CHECK(ns.truncateFile(handle.value(), 4096).ok());
        auto shrunk = ns.extents(handle.value());
        UUSE_TEST_CHECK(shrunk.ok() && shrunk.value().size() == 1);
        UUSE_TEST_CHECK(shrunk.value()[0].count == 1);
        UUSE_TEST_CHECK(ns.stats().freedExtents == 3);
        UUSE_TEST_CHECK(ns.setSize(handle.value(), 2 * 4096).ok());
        UUSE_TEST_CHECK(ns.stat(handle.value()).value().logicalSize == 2 * 4096);

        /* capacity override (4 usable extents) yields NoSpace without leaks */
        auto over = ns.reserveExtents(handle.value(), 8 * 4096, 8 * 4096);
        UUSE_TEST_CHECK(!over.ok() && over.status() == Status::NoSpace);
        UUSE_TEST_CHECK(ns.extents(handle.value()).value().size() == 1);
        auto refill = ns.reserveExtents(handle.value(), 8 * 4096, 3 * 4096);
        UUSE_TEST_CHECK(refill.ok());
        UUSE_TEST_CHECK(ns.commitExtents(handle.value(), refill.value()).ok());
        auto full = ns.reserveExtents(handle.value(), 32 * 4096, 4096);
        UUSE_TEST_CHECK(!full.ok() && full.status() == Status::NoSpace);
        UUSE_TEST_CHECK(ns.truncateFile(handle.value(), 4096).ok());
        auto after = ns.reserveExtents(handle.value(), 8 * 4096, 2 * 4096);
        UUSE_TEST_CHECK(after.ok());
        UUSE_TEST_CHECK(ns.releaseExtents(handle.value(), after.value()).ok());

        /* error-path release keeps the allocator conservative */
        auto temporary = ns.reserveExtents(handle.value(), 12 * 4096, 2 * 4096);
        UUSE_TEST_CHECK(temporary.ok());
        UUSE_TEST_CHECK(ns.releaseExtents(handle.value(), temporary.value()).ok());
        auto retry = ns.reserveExtents(handle.value(), 12 * 4096, 2 * 4096);
        UUSE_TEST_CHECK(retry.ok());
        UUSE_TEST_CHECK(ns.releaseExtents(handle.value(), retry.value()).ok());
        UUSE_TEST_CHECK(ns.extents(handle.value()).value().size() == 1);

        /* A stale overlapping reservation must not leak committed allocator space. */
        auto firstPending = ns.reserveExtents(handle.value(), 16 * 4096, 4096);
        auto secondPending = ns.reserveExtents(handle.value(), 16 * 4096, 4096);
        UUSE_TEST_CHECK(firstPending.ok() && secondPending.ok());
        const auto freeBeforeCommit = ns.stats().allocatedExtents;
        UUSE_TEST_CHECK(ns.commitExtents(handle.value(), firstPending.value()).ok());
        UUSE_TEST_CHECK(!ns.commitExtents(handle.value(), secondPending.value()).ok());
        UUSE_TEST_CHECK(ns.stats().allocatedExtents == freeBeforeCommit + 1);
        UUSE_TEST_CHECK(ns.extents(handle.value()).value().back().logicalExtent == 16);

        /* Range arithmetic must fail closed instead of wrapping. */
        auto overflow = ns.reserveExtents(
            handle.value(), std::numeric_limits<std::uint64_t>::max() - 1, 8);
        UUSE_TEST_CHECK(!overflow.ok() && overflow.status() == Status::InvalidArgument);
    }

    /* 9b. unlink while open reclaims extents only at the last close */
    {
        NamespaceConfig config;
        config.maxInodes = 64;
        config.maxNamespaceBytes = 1ULL << 20;
        config.extentBytes = 4096;
        config.deviceExtents = {16};
        config.deviceStartExtents = {2};
        config.maxReserveExtents = 4;
        config.capacityOverrideExtents = 4;
        auto created = Namespace::create(config, fixedClock, nullptr);
        UUSE_TEST_CHECK(created.ok());
        Namespace &ns = *created.value();
        auto handle = ns.create("/open-unlinked", false, false, false);
        UUSE_TEST_CHECK(handle.ok());
        const std::uint64_t freeBefore = ns.freeDataBytes();
        auto reservations = ns.reserveExtents(handle.value(), 0, 2 * 4096);
        UUSE_TEST_CHECK(reservations.ok());
        UUSE_TEST_CHECK(ns.commitExtents(handle.value(), reservations.value()).ok());
        UUSE_TEST_CHECK(ns.freeDataBytes() < freeBefore);

        UUSE_TEST_CHECK(ns.remove("/open-unlinked").ok());
        UUSE_TEST_CHECK(ns.stats().orphanInodes == 1);
        /* The orphan keeps its extents while a handle is still open. */
        UUSE_TEST_CHECK(ns.freeDataBytes() < freeBefore);
        UUSE_TEST_CHECK(ns.close(handle.value()).ok());
        /* The last close frees the unlinked inode and returns its extents. */
        UUSE_TEST_CHECK(ns.stats().orphanInodes == 0);
        UUSE_TEST_CHECK(ns.freeDataBytes() == freeBefore);
    }

    /* 10. multi-device placement keeps every run device-local */
    {
        NamespaceConfig config;
        config.maxInodes = 64;
        config.maxNamespaceBytes = 1ULL << 20;
        config.extentBytes = 4096;
        config.deviceExtents = {4, 4, 4};
        config.deviceStartExtents = {10, 20, 30};
        config.maxReserveExtents = 4;
        config.capacityOverrideExtents = 2;
        config.placement = PlacementPolicy::RoundRobin;
        auto created = Namespace::create(config, fixedClock, nullptr);
        UUSE_TEST_CHECK(created.ok());
        Namespace &ns = *created.value();
        auto handle = ns.create("/spread", false, false, false);
        UUSE_TEST_CHECK(handle.ok());

        auto reservations = ns.reserveExtents(handle.value(), 0, 5 * 4096);
        UUSE_TEST_CHECK(reservations.ok() && reservations.value().size() == 3);
        UUSE_TEST_CHECK(reservations.value()[0].run.deviceId == 0);
        UUSE_TEST_CHECK(reservations.value()[1].run.deviceId == 1);
        UUSE_TEST_CHECK(reservations.value()[2].run.deviceId == 2);
        for (const ExtentAllocation &allocation : reservations.value()) {
            UUSE_TEST_CHECK(allocation.run.count > 0);
            UUSE_TEST_CHECK(allocation.run.count <= config.maxReserveExtents);
        }
        UUSE_TEST_CHECK(ns.commitExtents(handle.value(), reservations.value()).ok());
        auto descriptors = ns.extents(handle.value());
        UUSE_TEST_CHECK(descriptors.ok() && descriptors.value().size() == 3);
        UUSE_TEST_CHECK(descriptors.value()[0].deviceId == 0);
        UUSE_TEST_CHECK(descriptors.value()[0].physicalExtent >= 10);
        UUSE_TEST_CHECK(descriptors.value()[0].physicalExtent + descriptors.value()[0].count <=
                       14);
        UUSE_TEST_CHECK(descriptors.value()[1].deviceId == 1);
        UUSE_TEST_CHECK(descriptors.value()[1].physicalExtent >= 20);
        UUSE_TEST_CHECK(descriptors.value()[2].deviceId == 2);
        UUSE_TEST_CHECK(descriptors.value()[2].physicalExtent >= 30);

        /* capacity-weighted placement drains the device with more free room */
        auto weightedConfig = config;
        weightedConfig.deviceExtents = {2, 8};
        weightedConfig.deviceStartExtents = {10, 20};
        weightedConfig.capacityOverrideExtents = 8;
        weightedConfig.placement = PlacementPolicy::CapacityWeighted;
        auto weighted = Namespace::create(weightedConfig, fixedClock, nullptr);
        UUSE_TEST_CHECK(weighted.ok());
        auto weightedHandle = weighted.value()->create("/w", false, false, false);
        UUSE_TEST_CHECK(weightedHandle.ok());
        auto weightedRuns =
            weighted.value()->reserveExtents(weightedHandle.value(), 0, 5 * 4096);
        UUSE_TEST_CHECK(weightedRuns.ok());
        for (const ExtentAllocation &allocation : weightedRuns.value()) {
            UUSE_TEST_CHECK(allocation.run.deviceId == 1);
        }
    }

    std::printf("allocator_test: ok\n");
    return 0;
}
