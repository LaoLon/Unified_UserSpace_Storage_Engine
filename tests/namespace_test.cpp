#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "metadata/inode.hpp"
#include "metadata/namespace.hpp"
#include "test_support.hpp"

using namespace uuse;
using namespace uuse::metadata;

namespace {

std::uint64_t gNowNs = 1'000'000'000ULL;

/* Reads this process's resident set size in KiB (Linux /proc). */
std::uint64_t residentSetKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::strtoull(line.c_str() + 6, nullptr, 10);
        }
    }
    return 0;
}

std::uint64_t fakeClock(void *) { return gNowNs; }

NamespaceConfig testConfig(std::uint64_t maxInodes = 4096,
                           std::uint64_t maxNamespaceBytes = 4ULL << 20) {
    NamespaceConfig config;
    config.maxInodes = maxInodes;
    config.maxNamespaceBytes = maxNamespaceBytes;
    config.bytesPerInode = 1024;
    config.bytesPerDirectoryEntry = 64;
    return config;
}

} // namespace

int main() {
    /* 1. path rules and create/open semantics */
    {
        auto created = Namespace::create(testConfig(), fakeClock, nullptr);
        UUSE_TEST_CHECK(created.ok());
        Namespace &ns = *created.value();

        auto root = ns.open("/");
        UUSE_TEST_CHECK(root.ok());
        auto rootStat = ns.stat(root.value());
        UUSE_TEST_CHECK(rootStat.ok() && rootStat.value().directory);

        UUSE_TEST_CHECK(!ns.open("relative").ok());
        UUSE_TEST_CHECK(ns.open("relative").status() == Status::InvalidArgument);
        UUSE_TEST_CHECK(ns.open("/a/../b").status() == Status::InvalidArgument);
        UUSE_TEST_CHECK(ns.open("/a/").status() == Status::InvalidArgument);
        UUSE_TEST_CHECK(ns.open("/nope").status() == Status::NotFound);
        UUSE_TEST_CHECK(ns.create("/missing/file", false, false, false).status() ==
                       Status::NotFound);

        auto dir = ns.create("/dir", false, false, true);
        UUSE_TEST_CHECK(dir.ok());
        UUSE_TEST_CHECK(ns.create("/dir", true, false, true).status() ==
                       Status::AlreadyExists);
        auto file = ns.create("/dir/file", false, false, false);
        UUSE_TEST_CHECK(file.ok());
        UUSE_TEST_CHECK(ns.create("/dir/file", true, false, false).status() ==
                       Status::AlreadyExists);
        UUSE_TEST_CHECK(ns.create("/dir/file", false, false, true).status() ==
                       Status::AlreadyExists);
        auto overwrite = ns.create("/dir/file", false, true, false);
        UUSE_TEST_CHECK(overwrite.ok());
        UUSE_TEST_CHECK(overwrite.value().inodeId == file.value().inodeId);
        UUSE_TEST_CHECK(overwrite.value().generation == file.value().generation);
        UUSE_TEST_CHECK(ns.close(overwrite.value()).ok());
        UUSE_TEST_CHECK(ns.close(root.value()).ok());
    }

    /* 2. injected clock drives mtime/ctime */
    {
        auto created = Namespace::create(testConfig(), fakeClock, nullptr);
        Namespace &ns = *created.value();
        gNowNs = 100;
        auto handle = ns.create("/t", false, false, false);
        UUSE_TEST_CHECK(handle.ok());
        auto initial = ns.stat(handle.value());
        UUSE_TEST_CHECK(initial.value().mtimeNs == 100 && initial.value().ctimeNs == 100);

        gNowNs = 200;
        UUSE_TEST_CHECK(ns.setTimes(handle.value(), 150, 160).ok());
        auto explicitTimes = ns.stat(handle.value());
        UUSE_TEST_CHECK(explicitTimes.value().mtimeNs == 150);
        UUSE_TEST_CHECK(explicitTimes.value().ctimeNs == 160);

        gNowNs = 300;
        UUSE_TEST_CHECK(ns.rename("/t", "/t2").ok());
        auto renamed = ns.stat("/t2");
        UUSE_TEST_CHECK(renamed.value().mtimeNs == 150);
        UUSE_TEST_CHECK(renamed.value().ctimeNs == 300);
        UUSE_TEST_CHECK(!ns.stat("/t").ok());
    }

    /* 3. handle generation: stale handles never resolve to a reused inode */
    {
        auto created = Namespace::create(testConfig(), fakeClock, nullptr);
        Namespace &ns = *created.value();
        auto first = ns.create("/orphan", false, false, false);
        UUSE_TEST_CHECK(first.ok());
        UUSE_TEST_CHECK(ns.remove("/orphan").ok());
        UUSE_TEST_CHECK(ns.stat(first.value()).ok()); /* open handle survives */
        UUSE_TEST_CHECK(ns.stats().orphanInodes == 1);
        UUSE_TEST_CHECK(ns.close(first.value()).ok());
        UUSE_TEST_CHECK(ns.stat(first.value()).status() == Status::NotFound);
        /* the inode was freed: a second close sees a stale handle */
        UUSE_TEST_CHECK(ns.close(first.value()).status() == Status::NotFound);

        auto reused = ns.create("/reuse", false, false, false);
        UUSE_TEST_CHECK(reused.ok());
        UUSE_TEST_CHECK(reused.value().inodeId == first.value().inodeId);
        UUSE_TEST_CHECK(reused.value().generation != first.value().generation);
        UUSE_TEST_CHECK(ns.stat(first.value()).status() == Status::NotFound);
        UUSE_TEST_CHECK(ns.stat(reused.value()).ok());

        /* a live inode rejects double close with InvalidArgument */
        auto second = ns.create("/double", false, false, false);
        UUSE_TEST_CHECK(second.ok());
        UUSE_TEST_CHECK(ns.close(second.value()).ok());
        UUSE_TEST_CHECK(ns.close(second.value()).status() == Status::InvalidArgument);
    }

    /* 4. remove rules */
    {
        auto created = Namespace::create(testConfig(), fakeClock, nullptr);
        Namespace &ns = *created.value();
        UUSE_TEST_CHECK(ns.create("/d", false, false, true).ok());
        auto child = ns.create("/d/a", false, false, false);
        UUSE_TEST_CHECK(child.ok());
        UUSE_TEST_CHECK(ns.remove("/").status() == Status::InvalidArgument);
        UUSE_TEST_CHECK(ns.remove("/d").status() == Status::InvalidArgument);
        UUSE_TEST_CHECK(ns.remove("/missing").status() == Status::NotFound);
        UUSE_TEST_CHECK(ns.remove("/d/a").ok());
        UUSE_TEST_CHECK(ns.stat(child.value()).ok()); /* open handle survives removal */
        UUSE_TEST_CHECK(ns.close(child.value()).ok());
        UUSE_TEST_CHECK(ns.stat(child.value()).status() == Status::NotFound);
        UUSE_TEST_CHECK(ns.remove("/d").ok());
    }

    /* 5. rename is atomic replace with type and cycle checks */
    {
        auto created = Namespace::create(testConfig(), fakeClock, nullptr);
        Namespace &ns = *created.value();
        auto source = ns.create("/r1", false, false, false);
        UUSE_TEST_CHECK(source.ok());
        UUSE_TEST_CHECK(ns.rename("/r1", "/r2").ok());
        UUSE_TEST_CHECK(ns.rename("/r1", "/r3").status() == Status::NotFound);
        UUSE_TEST_CHECK(ns.rename("/missing", "/x").status() == Status::NotFound);
        UUSE_TEST_CHECK(ns.rename("/r2", "/missing/x").status() == Status::NotFound);
        UUSE_TEST_CHECK(ns.rename("/", "/x").status() == Status::InvalidArgument);

        auto replaced = ns.create("/r3", false, false, false);
        UUSE_TEST_CHECK(replaced.ok());
        UUSE_TEST_CHECK(ns.rename("/r2", "/r3").ok());
        auto afterReplace = ns.stat("/r3");
        UUSE_TEST_CHECK(afterReplace.ok());
        UUSE_TEST_CHECK(afterReplace.value().inodeId == source.value().inodeId);
        UUSE_TEST_CHECK(ns.stat(replaced.value()).ok()); /* replaced inode still open */
        UUSE_TEST_CHECK(ns.close(replaced.value()).ok());
        UUSE_TEST_CHECK(ns.stat(replaced.value()).status() == Status::NotFound);

        UUSE_TEST_CHECK(ns.create("/dir_target", false, false, true).ok());
        UUSE_TEST_CHECK(ns.rename("/r3", "/dir_target").status() == Status::AlreadyExists);

        UUSE_TEST_CHECK(ns.create("/md", false, false, true).ok());
        UUSE_TEST_CHECK(ns.create("/md/b", false, false, true).ok());
        UUSE_TEST_CHECK(ns.create("/md/b/x", false, false, false).ok());
        UUSE_TEST_CHECK(ns.rename("/md", "/md/b/sub").status() == Status::InvalidArgument);
        UUSE_TEST_CHECK(ns.rename("/md/b/x", "/md/b/y").ok());
        UUSE_TEST_CHECK(ns.rename("/md", "/md2").ok());
        auto moved = ns.stat("/md2/b/y");
        UUSE_TEST_CHECK(moved.ok());
    }

    /* 6. ordered getChildren pagination */
    {
        auto created = Namespace::create(testConfig(), fakeClock, nullptr);
        Namespace &ns = *created.value();
        UUSE_TEST_CHECK(ns.create("/g", false, false, true).ok());
        for (const char *name : {"/g/c", "/g/a", "/g/b", "/g/d"}) {
            UUSE_TEST_CHECK(ns.create(name, false, false, false).ok());
        }
        auto page1 = ns.getChildren("/g", "", 2);
        UUSE_TEST_CHECK(page1.ok() && page1.value().entries.size() == 2);
        UUSE_TEST_CHECK(page1.value().entries[0].name == "a");
        UUSE_TEST_CHECK(page1.value().entries[1].name == "b");
        UUSE_TEST_CHECK(page1.value().nextCursor == "b");

        auto page2 = ns.getChildren("/g", page1.value().nextCursor, 2);
        UUSE_TEST_CHECK(page2.ok() && page2.value().entries.size() == 2);
        UUSE_TEST_CHECK(page2.value().entries[0].name == "c");
        UUSE_TEST_CHECK(page2.value().entries[1].name == "d");
        /* the page consumed the last entry: iteration is complete */
        UUSE_TEST_CHECK(page2.value().nextCursor.empty());

        auto page3 = ns.getChildren("/g", "d", 2);
        UUSE_TEST_CHECK(page3.ok() && page3.value().entries.empty());
        UUSE_TEST_CHECK(page3.value().nextCursor.empty());
        UUSE_TEST_CHECK(ns.getChildren("/g", "", 0).status() == Status::InvalidArgument);

        UUSE_TEST_CHECK(ns.create("/plain", false, false, false).ok());
        UUSE_TEST_CHECK(ns.getChildren("/plain", "", 4).status() == Status::NotFound);
    }

    /* 7. inode and namespace byte limits fail with NoMemory */
    {
        auto created = Namespace::create(testConfig(4, 1ULL << 20), fakeClock, nullptr);
        Namespace &ns = *created.value();
        auto a = ns.create("/a", false, false, false);
        auto b = ns.create("/b", false, false, false);
        auto c = ns.create("/c", false, false, false);
        UUSE_TEST_CHECK(a.ok() && b.ok() && c.ok());
        auto over = ns.create("/d", false, false, false);
        UUSE_TEST_CHECK(!over.ok() && over.status() == Status::NoMemory);
        UUSE_TEST_CHECK(ns.stats().limitRejects == 1);

        UUSE_TEST_CHECK(ns.close(a.value()).ok());
        UUSE_TEST_CHECK(ns.remove("/a").ok());
        auto reopened = ns.open("/a");
        UUSE_TEST_CHECK(reopened.status() == Status::NotFound);
        UUSE_TEST_CHECK(ns.create("/e", false, false, false).ok());
        UUSE_TEST_CHECK(ns.close(b.value()).ok());
        UUSE_TEST_CHECK(ns.close(c.value()).ok());
    }
    {
        /* budget: root inode + one file inode + entry (name + per-entry bytes) */
        NamespaceConfig config = testConfig(1024, 2 * 1024 + 64 + 5);
        auto created = Namespace::create(config, fakeClock, nullptr);
        Namespace &ns = *created.value();
        auto tooLong = ns.create("/abcdef", false, false, false);
        UUSE_TEST_CHECK(!tooLong.ok() && tooLong.status() == Status::NoMemory);
        UUSE_TEST_CHECK(ns.create("/abcde", false, false, false).ok());
    }

    /* 8. checkpoint snapshot round-trip and corruption rejection */
    {
        NamespaceConfig config = testConfig(1024, 8ULL << 20);
        config.extentBytes = 4096;
        config.deviceExtents = {16};
        config.deviceStartExtents = {0};
        auto source = Namespace::create(config, fakeClock, nullptr);
        UUSE_TEST_CHECK(source.ok());
        Namespace &sourceNs = *source.value();
        UUSE_TEST_CHECK(sourceNs.freeDataBytes() == 16 * config.extentBytes);
        auto file = sourceNs.create("/snapshot", false, false, false);
        UUSE_TEST_CHECK(file.ok());
        UUSE_TEST_CHECK(sourceNs.link("/snapshot", "/snapshot-link").ok());
        std::vector<std::byte> snapshot;
        UUSE_TEST_CHECK(sourceNs.saveSnapshot(17, snapshot).ok());
        UUSE_TEST_CHECK(sourceNs.snapshotTxid() == 17);

        auto restored = Namespace::create(config, fakeClock, nullptr);
        UUSE_TEST_CHECK(restored.ok());
        Namespace &restoredNs = *restored.value();
        UUSE_TEST_CHECK(restoredNs.loadSnapshot(snapshot).ok());
        auto first = restoredNs.stat("/snapshot");
        auto second = restoredNs.stat("/snapshot-link");
        UUSE_TEST_CHECK(first.ok() && second.ok());
        UUSE_TEST_CHECK(first.value().inodeId == second.value().inodeId);
        UUSE_TEST_CHECK(first.value().linkCount == 2);
        UUSE_TEST_CHECK(restoredNs.snapshotTxid() == 17);
        snapshot.back() ^= std::byte{0x01};
        UUSE_TEST_CHECK(restoredNs.loadSnapshot(snapshot).status() == Status::Corrupt);
    }

    /* 9. concurrent creators with a directory reader (TSAN target) */
    {
        auto created = Namespace::create(testConfig(8192, 8ULL << 20), fakeClock, nullptr);
        UUSE_TEST_CHECK(created.ok());
        Namespace &ns = *created.value();
        UUSE_TEST_CHECK(ns.create("/conc", false, false, true).ok());
        constexpr int kThreads = 4;
        constexpr int kPerThread = 128;
        std::atomic<bool> readerDone{false};
        std::thread reader([&]() {
            while (!readerDone.load(std::memory_order_relaxed)) {
                auto page = ns.getChildren("/conc", "", 8);
                if (!page.ok()) {
                    std::fprintf(stderr, "reader getChildren failed\n");
                    return;
                }
            }
        });
        std::vector<std::thread> writers;
        for (int t = 0; t < kThreads; ++t) {
            writers.emplace_back([&ns, t]() {
                for (int i = 0; i < kPerThread; ++i) {
                    const std::string path = "/conc/t" + std::to_string(t) + "_" +
                                             std::to_string(i);
                    if (!ns.create(path, true, false, false).ok()) {
                        std::fprintf(stderr, "create failed: %s\n", path.c_str());
                        return;
                    }
                }
            });
        }
        for (auto &writer : writers) {
            writer.join();
        }
        readerDone.store(true, std::memory_order_relaxed);
        reader.join();

        std::size_t total = 0;
        std::string cursor;
        for (;;) {
            auto page = ns.getChildren("/conc", cursor, 100);
            UUSE_TEST_CHECK(page.ok());
            total += page.value().entries.size();
            if (page.value().nextCursor.empty()) {
                break;
            }
            cursor = page.value().nextCursor;
        }
        UUSE_TEST_CHECK(total == kThreads * kPerThread);
        UUSE_TEST_CHECK(ns.stats().creates == kThreads * kPerThread + 1);
    }

    /* 9. RocksDB-style create/rename/remove churn */
    {
        auto created = Namespace::create(testConfig(8192, 8ULL << 20), fakeClock, nullptr);
        Namespace &ns = *created.value();
        UUSE_TEST_CHECK(ns.create("/db", false, false, true).ok());
        constexpr int kFiles = 1000;
        for (int i = 1; i <= kFiles; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "/db/%06d.sst", i);
            UUSE_TEST_CHECK(ns.create(name, true, false, false).ok());
        }
        for (int i = 1; i <= kFiles; i += 2) {
            char from[64];
            char to[64];
            std::snprintf(from, sizeof(from), "/db/%06d.sst", i);
            std::snprintf(to, sizeof(to), "/db/%06d.sst.moved", i);
            UUSE_TEST_CHECK(ns.rename(from, to).ok());
        }
        for (int i = 3; i <= kFiles; i += 6) {
            char name[64];
            std::snprintf(name, sizeof(name), "/db/%06d.sst.moved", i);
            UUSE_TEST_CHECK(ns.remove(name).ok());
        }
        std::size_t total = 0;
        std::string cursor;
        for (;;) {
            auto page = ns.getChildren("/db", cursor, 64);
            UUSE_TEST_CHECK(page.ok());
            total += page.value().entries.size();
            if (page.value().nextCursor.empty()) {
                break;
            }
            cursor = page.value().nextCursor;
        }
        /* removed set: odd names divisible by 3 (i % 6 == 3) */
        const std::size_t removedCount =
            static_cast<std::size_t>(kFiles / 6) + (kFiles % 6 >= 3 ? 1 : 0);
        const auto stats = ns.stats();
        UUSE_TEST_CHECK(stats.renames == kFiles / 2);
        UUSE_TEST_CHECK(stats.removes == removedCount);
        UUSE_TEST_CHECK(total == static_cast<std::size_t>(kFiles) - removedCount);
        std::printf("namespace stats: inodes=%llu bytes=%llu creates=%llu renames=%llu\n",
                    static_cast<unsigned long long>(stats.inodes),
                    static_cast<unsigned long long>(stats.namespaceBytes),
                    static_cast<unsigned long long>(stats.creates),
                    static_cast<unsigned long long>(stats.renames));
    }

    /* Logical and durable epochs are monotonic and publish atomically. */
    {
        auto created = Namespace::create(testConfig(64, 1ULL << 20), fakeClock, nullptr);
        UUSE_TEST_CHECK(created.ok());
        Namespace &ns = *created.value();
        auto handle = ns.create("/epoch", true, false, false);
        UUSE_TEST_CHECK(handle.ok());
        const auto logical = ns.logicalEpoch();
        UUSE_TEST_CHECK(logical > 0 && ns.durableEpoch() == 0);
        UUSE_TEST_CHECK(!ns.publishDurableEpoch(logical + 1).ok());
        UUSE_TEST_CHECK(ns.publishDurableEpoch(logical).ok());
        UUSE_TEST_CHECK(ns.durableEpoch() == logical);
        auto stat = ns.stat(handle.value());
        UUSE_TEST_CHECK(stat.ok() && stat.value().logicalSize == stat.value().durableSize);
        UUSE_TEST_CHECK(ns.setSize(handle.value(), 4096).ok());
        const auto future = ns.logicalEpoch();
        UUSE_TEST_CHECK(future > logical);
        stat = ns.stat(handle.value());
        UUSE_TEST_CHECK(stat.ok() && stat.value().logicalSize == 4096 &&
                       stat.value().durableSize == 0);
        UUSE_TEST_CHECK(ns.publishDurableEpoch(logical).ok());
        stat = ns.stat(handle.value());
        UUSE_TEST_CHECK(stat.ok() && stat.value().durableSize == 0);
        UUSE_TEST_CHECK(ns.publishDurableEpoch(future).ok());
        stat = ns.stat(handle.value());
        UUSE_TEST_CHECK(stat.ok() && stat.value().durableSize == 4096);
        UUSE_TEST_CHECK(!ns.publishDurableEpoch(logical - 1).ok());
    }

    /* Per-inode data version: only the file's own data-map changes invalidate
     * the frontend snapshot; rename/link touches must not. */
    {
        auto created = Namespace::create(testConfig(64, 1ULL << 20), fakeClock, nullptr);
        UUSE_TEST_CHECK(created.ok());
        Namespace &ns = *created.value();
        auto first = ns.create("/v1", true, false, false);
        auto second = ns.create("/v2", true, false, false);
        UUSE_TEST_CHECK(first.ok() && second.ok());

        const std::uint64_t firstBefore = ns.dataVersionFast(first.value());
        const std::uint64_t secondBefore = ns.dataVersionFast(second.value());
        UUSE_TEST_CHECK(ns.setSize(first.value(), 4096).ok());
        UUSE_TEST_CHECK(ns.dataVersionFast(first.value()) != firstBefore);
        UUSE_TEST_CHECK(ns.dataVersionFast(second.value()) == secondBefore);

        const std::uint64_t firstAfterSize = ns.dataVersionFast(first.value());
        UUSE_TEST_CHECK(ns.rename("/v1", "/v1-renamed").ok());
        UUSE_TEST_CHECK(ns.dataVersionFast(first.value()) == firstAfterSize);

        /* prepare stages only the dirty inode; rollback restores it. */
        auto statFirst = ns.stat(first.value());
        UUSE_TEST_CHECK(statFirst.ok() && statFirst.value().durableSize == 0);
        const std::uint64_t epoch = ns.logicalEpoch();
        UUSE_TEST_CHECK(ns.prepareDurableEpoch(epoch).ok());
        statFirst = ns.stat(first.value());
        UUSE_TEST_CHECK(statFirst.ok() && statFirst.value().durableSize == 4096);
        UUSE_TEST_CHECK(!ns.prepareDurableEpoch(epoch).ok()); /* already staged */
        ns.rollbackPreparedDurableEpoch();
        statFirst = ns.stat(first.value());
        UUSE_TEST_CHECK(statFirst.ok() && statFirst.value().durableSize == 0);
        UUSE_TEST_CHECK(ns.publishDurableEpoch(epoch).ok());
        statFirst = ns.stat(first.value());
        const auto statSecond = ns.stat(second.value());
        UUSE_TEST_CHECK(statFirst.ok() && statFirst.value().durableSize == 4096);
        UUSE_TEST_CHECK(statSecond.ok() && statSecond.value().durableSize == 0);
    }

    /* Durable publish cost follows the dirty set, not the inode count. */
    {
        const auto measurePublish = [](std::uint64_t inodeCount, bool &ok) -> long long {
            auto created = Namespace::create(testConfig(inodeCount + 8, 64ULL << 20),
                                             fakeClock, nullptr);
            if (!created.ok()) {
                ok = false;
                return 0;
            }
            Namespace &ns = *created.value();
            if (!ns.create("/f", false, false, true).ok()) {
                ok = false;
                return 0;
            }
            for (std::uint64_t index = 0; index < inodeCount; ++index) {
                char name[64];
                std::snprintf(name, sizeof(name), "/f/%llu",
                              static_cast<unsigned long long>(index));
                if (!ns.create(name, true, false, false).ok()) {
                    ok = false;
                    return 0;
                }
            }
            auto handle = ns.create("/f/dirty", true, false, false);
            if (!handle.ok()) {
                ok = false;
                return 0;
            }
            constexpr int kPublishes = 100;
            const auto start = std::chrono::steady_clock::now();
            for (int iteration = 0; iteration < kPublishes; ++iteration) {
                const bool grown =
                    ns.setSize(handle.value(), 4096 + static_cast<std::uint64_t>(iteration))
                        .ok();
                const bool published = ns.publishDurableEpoch(ns.logicalEpoch()).ok();
                if (!grown || !published) {
                    ok = false;
                    return 0;
                }
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            return elapsed / kPublishes;
        };
        bool smallOk = true;
        bool largeOk = true;
        const auto small = measurePublish(1000, smallOk);
        const auto large = measurePublish(20000, largeOk);
        UUSE_TEST_CHECK(smallOk && largeOk);
        std::printf("namespace durable publish: ns/publish small_inodes=%lld "
                    "large_inodes=%lld\n",
                    static_cast<long long>(small), static_cast<long long>(large));
        UUSE_TEST_CHECK(large < 1000000); /* sanity: must not be O(inode count^2) */
    }

    /* P1-5 footprint: 1M inode namespace memory (design §17.1 budget
     * <=1 KiB/active inode, excluding the DMA arena). */
    {
        NamespaceConfig config = testConfig(1000000 + 1024, 2ULL << 30);
        config.extentBytes = 1ULL << 20;
        config.deviceExtents = {500000};
        config.deviceStartExtents = {0};
        auto created = Namespace::create(config, fakeClock, nullptr);
        UUSE_TEST_CHECK(created.ok());
        Namespace &ns = *created.value();
        UUSE_TEST_CHECK(ns.create("/d", false, false, /*directory=*/true).ok());
        const std::uint64_t before = residentSetKb();
        constexpr std::uint64_t kFiles = 1000000;
        for (std::uint64_t index = 0; index < kFiles; ++index) {
            char name[32];
            std::snprintf(name, sizeof(name), "/d/f%07llu",
                          static_cast<unsigned long long>(index));
            auto handle = ns.create(name, false, false, false);
            if (!handle.ok()) {
                std::printf("footprint create failed at %llu: status=%u message=%s\n",
                            static_cast<unsigned long long>(index),
                            static_cast<unsigned>(handle.status()),
                            handle.message().c_str());
                UUSE_TEST_CHECK(false);
                return 1;
            }
        }
        const std::uint64_t after = residentSetKb();
        const double perInodeBytes =
            static_cast<double>(after - before) * 1024.0 / static_cast<double>(kFiles);
        std::printf("namespace footprint: inodes=%llu rss_delta_kb=%llu "
                    "per_inode_bytes=%.0f\n",
                    static_cast<unsigned long long>(kFiles),
                    static_cast<unsigned long long>(after - before), perInodeBytes);
        UUSE_TEST_CHECK(perInodeBytes < 1024.0); /* design §17.1 budget */
    }

    std::printf("namespace_test: ok\n");
    return 0;
}
